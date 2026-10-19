#include "tex_eager.h"
#include "../migoto/port_api.h"
#include "../config.h" // g_tex_deferred
#include "../../src/log.h"
#include <windows.h>
#include <d3d11.h>
#include <vector>

TexEagerWorker g_tex_eager;

bool TexEagerWorker::ReadyForDraw(int mod, const wchar_t* ns)
{
    if (!g_tex_deferred || mod < 0 || !ns || !*ns) return true;
    OnClaimed(mod, ns, "texture warmup before mod commands");
    return !Pending(mod);
}

std::atomic<uint8_t>& TexEagerWorker::Slot(int mod)
{
    Table* t = table_.load(std::memory_order_relaxed);
    if (!t || mod >= t->n) {
        int n = t ? t->n : 64;
        while (n <= mod) n *= 2;
        auto grown = std::make_unique<Table>();
        grown->v = std::make_unique<std::atomic<uint8_t>[]>(n);
        grown->n = n;
        for (int i = 0; i < n; ++i) grown->v[i].store(t && i < t->n ? t->v[i].load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
        t = grown.get();
        tables_.push_back(std::move(grown));
        table_.store(t, std::memory_order_release);
    }
    return t->v[mod];
}

void TexEagerWorker::OnClaimed(int mod, const wchar_t* ns, const char* why)
{
    if (mod < 0 || !ns || !*ns) return;
    unsigned gen = PortApi::LoadGeneration();
    if ((Flags(mod) & kQueued) && gen == gen_.load(std::memory_order_relaxed)) return;
    // Calls can originate inside an engine command. Always acquire engine ->
    // worker in that order, never worker -> engine on a different drawing thread.
    PortApi::EngineScope engine;
    gen = PortApi::LoadGeneration();
    Job job{ ns, why, mod, g_tex_deferred, gen, {} };
    {
        std::lock_guard l(mtx_);
        if (stop_) return;
        if (gen != gen_.load(std::memory_order_relaxed)) {
            gen_.store(gen, std::memory_order_relaxed);
            if (Table* t = table_.load(std::memory_order_relaxed))
                for (int i = 0; i < t->n; ++i) t->v[i].store(0, std::memory_order_relaxed);
        }
        std::atomic<uint8_t>& s = Slot(mod);
        if (s.load(std::memory_order_relaxed) & kQueued) return;
        // Marked here, on the caller's thread, so the draw this comes from loads nothing itself either.
        if (job.deferred)
            PortApi::BeginDeferredTextures(ns, job.texs);
        else
            PortApi::ForEachUnsubstantiatedTexture(ns, [](const wchar_t* fq, const wchar_t* fn, void* user) { static_cast<std::vector<PortApi::DeferredTexture>*>(user)->push_back({ fq, fn, 0, 0 }); }, &job.texs);
        s.store(job.texs.empty() ? kQueued : (uint8_t)(kQueued | (job.deferred ? kPending : 0)), std::memory_order_release);
        if (job.texs.empty()) return;
        queue_.push_back(std::move(job));
        if (g_tex_deferred)
            logger::write("TEXWARM mod=%d: queued %zu texture(s); keep the game's draw until background publication completes", mod, queue_.back().texs.size());
        if (!thread_.joinable()) thread_ = std::thread(&TexEagerWorker::Run, this);
    }
    cv_.notify_one();
}

void TexEagerWorker::Run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock l(mtx_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        const std::vector<PortApi::DeferredTexture>& texs = job.texs;
        LARGE_INTEGER                                f;
        LARGE_INTEGER                                t0;
        LARGE_INTEGER                                t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        unsigned made = 0;
        LONGLONG locked = 0;
        LONGLONG longest = 0;
        bool     stale = false;
        for (const PortApi::DeferredTexture& t : texs) {
            {
                std::lock_guard l(mtx_);
                if (stop_) return;
            }
            if (PortApi::LoadGeneration() != job.gen) {
                stale = true;
                break;
            }
            LARGE_INTEGER a;
            LARGE_INTEGER b;
            LARGE_INTEGER c;
            QueryPerformanceCounter(&a);
            if (job.deferred) {
                // A legacy copy path may already have loaded it. Do not create
                // another texture only to discard it at publication.
                if (!PortApi::DeferredTexturePending(t, job.gen)) continue;
                ID3D11Resource* res = PortApi::LoadDeferredTexture(t);
                QueryPerformanceCounter(&b);
                // Failure is also completed, so a missing file does not get
                // retried synchronously on the first mod draw. A reload retries.
                const bool accepted = PortApi::PublishDeferredTexture(t.fq_name.c_str(), res, job.gen);
                if (res && accepted) ++made;
                if (res && !accepted) res->Release();
                QueryPerformanceCounter(&c);
                locked += c.QuadPart - b.QuadPart;
                if (b.QuadPart - a.QuadPart > longest) longest = b.QuadPart - a.QuadPart;
            } else {
                PortApi::EngineScope engine;
                QueryPerformanceCounter(&b);
                if (PortApi::SubstantiateTexture(t.fq_name.c_str())) ++made;
                QueryPerformanceCounter(&c);
                locked += c.QuadPart - b.QuadPart;
                if (c.QuadPart - b.QuadPart > longest) longest = c.QuadPart - b.QuadPart;
            }
        }
        {
            std::lock_guard l(mtx_);
            if (job.gen == gen_.load(std::memory_order_relaxed)) Slot(job.mod).fetch_and((uint8_t)~kPending, std::memory_order_acq_rel);
        }
        QueryPerformanceCounter(&t1);
        const double ms = 1000.0 / (double)f.QuadPart;
        logger::write("TEXEAGER '%ls' (%s, %s): loaded %u of %zu texture(s) in %.0f ms, %.0f ms %s "
                      "(longest texture %.0f ms)%s",
            job.ns.c_str(), job.why, job.deferred ? "deferred" : "under the lock",
            made, texs.size(), (double)(t1.QuadPart - t0.QuadPart) * ms, (double)locked * ms,
            job.deferred ? "publishing (including engine-lock wait)" : "under the engine lock", (double)longest * ms,
            stale ? " -- the inis were reloaded meanwhile, the rest waits for the next claim" : "");
    }
}

void TexEagerWorker::Stop()
{
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
