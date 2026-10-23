#define NOMINMAX
#include "../reshade_addon/textures/tex_defer.h"
#include "../reshade_addon/config.h"
#include "../reshade_addon/mods/claim_state.h"
#include <d3d11.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>

// The 2026.10.4 background texture worker (TexDeferWorker), as the RT branch uses it: the real worker, driven through
// the port's deferral sink with CPU-only load callbacks. No files and no GPU are used.
// Ported from rt_tex_eager_test.cpp (the whole-namespace warmup worker it replaced).
static void Check(bool v, const char* why)
{
    if (!v) {
        std::cerr << "FAIL: " << why << '\n';
        std::exit(1);
    }
}
static std::mutex              test_mutex;
static std::condition_variable test_cv;
static std::atomic<unsigned>   generation{ 1 }, live_resources{ 0 };
static const std::thread::id   caller = std::this_thread::get_id();
struct Stage {
    unsigned loads = 0, published = 0, failures = 0, rejected = 0;
    bool     entered = false, release = false;
};
static std::map<unsigned, Stage>                         stages;
static std::map<std::pair<unsigned, std::wstring>, bool> pending;
struct Resource final : ID3D11Resource {
    std::atomic<ULONG> refs{ 1 };
    Resource() { ++live_resources; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto n = --refs;
        if (!n) {
            --live_resources;
            delete this;
        }
        return n;
    }
    void STDMETHODCALLTYPE    GetDevice(ID3D11Device** out) override { *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    void STDMETHODCALLTYPE    GetType(D3D11_RESOURCE_DIMENSION* out) override { *out = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE    SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE    GetEvictionPriority() override { return 0; }
};

std::deque<GeoMod> g_geomods; // mod 0 draws from namespace "mod"
namespace logger {
void write(const char*, ...) {}
} // namespace logger
static bool (*g_sink)(const PortApi::DeferredTexture&, void*) = nullptr;
static void* g_sink_user = nullptr;
namespace PortApi {
unsigned LoadGeneration() { return generation.load(); }
void     SetTextureDeferral(bool (*sink)(const DeferredTexture& t, void* user), void* user)
{
    g_sink = sink;
    g_sink_user = user;
}
bool DeferredTexturePending(const DeferredTexture& t, unsigned gen)
{
    std::lock_guard lock(test_mutex);
    return gen == generation.load() && pending[{ gen, t.fq_name }];
}
ID3D11Resource* LoadDeferredTexture(const DeferredTexture& t)
{
    Check(std::this_thread::get_id() != caller, "file loading must not run on the drawing caller");
    const unsigned   gen = unsigned(std::stoul(t.filename));
    std::unique_lock lock(test_mutex);
    auto&            s = stages[gen];
    ++s.loads;
    if (t.filename.find(L":first") != std::wstring::npos) {
        s.entered = true;
        test_cv.notify_all();
        test_cv.wait(lock, [&] { return s.release; });
    }
    return t.filename.find(L":missing") == std::wstring::npos ? new Resource : nullptr;
}
bool PublishDeferredTexture(const wchar_t* fq, ID3D11Resource* res, unsigned gen)
{
    std::lock_guard lock(test_mutex);
    if (gen != generation.load() || !pending[{ gen, fq }]) {
        ++stages[gen].rejected;
        return false;
    }
    pending[{ gen, fq }] = false;
    if (res) {
        ++stages[gen].published;
        res->Release(); // the [Resource] took it
    } else {
        ++stages[gen].failures;
    }
    return true;
}
} // namespace PortApi

// What the port does the first time a draw's slot asks for a .dds [Resource]: offer it, mark it pending if taken.
static bool Ask(const wchar_t* name)
{
    const unsigned     gen = generation.load();
    const std::wstring fq = std::wstring(L"Resource\\mod\\") + name;
    const PortApi::DeferredTexture t{ fq, std::to_wstring(gen) + L":" + name, 0, 0, false };
    Check(g_sink != nullptr, "the worker hands the port its sink");
    const bool taken = g_sink(t, g_sink_user);
    if (taken) {
        std::lock_guard lock(test_mutex);
        pending[{ gen, fq }] = true;
    }
    return taken;
}
static void Entered(unsigned gen)
{
    std::unique_lock lock(test_mutex);
    Check(test_cv.wait_for(lock, std::chrono::seconds(3), [&] { return stages[gen].entered; }), "worker starts the offered load");
}
static void Release(unsigned gen)
{
    {
        std::lock_guard lock(test_mutex);
        stages[gen].release = true;
    }
    test_cv.notify_all();
}
static void Idle(TexDeferWorker& worker)
{
    for (unsigned i = 0; i < 3000 && worker.Loading(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(!worker.Loading(), "every offered load finishes or is dropped");
}
int main()
{
    g_geomods.emplace_back();
    g_geomods.back().wcl_ns = L"mod";
    TexDeferWorker worker;
    worker.Start();
    Check(!worker.Pending(0), "nothing pending before a draw asks");

    // Generation 1: three textures asked for by the mod's first section run.
    Check(Ask(L"first"), "the first ask is taken by the background worker");
    Check(worker.Pending(0), "the drawing thread sees its mod loading as soon as it asked (draw gate holds)");
    Entered(1);
    Check(Ask(L"second") && Ask(L"missing"), "later asks are taken too");
    {
        std::lock_guard lock(test_mutex);
        pending[{ 1, L"Resource\\mod\\second" }] = false; // a copy already loaded it in the foreground (LoadDeferredNow)
    }
    for (unsigned i = 0; i < 16; ++i) Check(worker.Pending(0), "polling while loading keeps the mod pending");
    Release(1);
    Idle(worker);
    Check(!worker.Pending(0), "completed loads release the draw gate");
    {
        std::lock_guard lock(test_mutex);
        Check(stages[1].loads == 2, "a texture supplied meanwhile is not loaded again");
        Check(stages[1].published == 1 && stages[1].failures == 1, "a missing file is completed (null), not left pending");
        Check(!pending[{ 1, L"Resource\\mod\\missing" }], "a failed file needs no foreground retry");
    }

    // Generation 2 asked, then the inis reload (3) while its load runs.
    generation.store(2);
    Check(Ask(L"first"), "a reload asks again");
    Entered(2);
    generation.store(3);
    Check(Ask(L"first") && Ask(L"second"), "the new generation is taken independently");
    Release(2);
    Release(3);
    Idle(worker);
    Check(!worker.Pending(0), "the new generation's loads finish");
    {
        std::lock_guard lock(test_mutex);
        Check(stages[2].published == 0, "a load of a superseded generation is never published");
        Check(stages[3].published == 2, "the new generation publishes all it asked for");
    }

    g_tex_deferred = false;
    Check(!Ask(L"legacy"), "with A12 off nothing is taken: it loads where asked, as on DX11");
    worker.Stop();
    Check(live_resources == 0, "rejected and published resources are all released");
    std::cout << "rt_tex_defer_test: PASS (actual worker, CPU callbacks, no GPU)\n";
}
