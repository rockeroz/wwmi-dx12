#pragma once
#include "../migoto/port_api.h" // DeferredTexture
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TexEagerWorker {
public:
    void OnClaimed(int mod, const wchar_t* ns, const char* why); // `why` is a literal
    // Poll only: retain the game's draw until this mod's textures are complete.
    // No mod command list may run yet, including one-time texture copies.
    bool ReadyForDraw(int mod, const wchar_t* ns);
    bool Pending(int mod) const { return (Flags(mod) & kPending) != 0; }
    void Stop();
    ~TexEagerWorker()
    {
        if (thread_.joinable()) thread_.detach();
    }

private:
    void Run();
    enum : uint8_t { kQueued = 1,
        kPending = 2 };
    struct Table {
        std::unique_ptr<std::atomic<uint8_t>[]> v;
        int                                     n;
    };
    uint8_t Flags(int mod) const
    {
        const Table* t = table_.load(std::memory_order_acquire);
        return (t && mod >= 0 && mod < t->n) ? t->v[mod].load(std::memory_order_acquire) : 0;
    }
    std::atomic<uint8_t>&               Slot(int mod); // under mtx_; grows the table, the old one is kept
    std::atomic<Table*>                 table_{ nullptr };
    std::vector<std::unique_ptr<Table>> tables_;   // under mtx_: every table ever published
    std::atomic<unsigned>               gen_{ 0 }; // written under mtx_: the ini load the flags belong to
    struct Job {
        std::wstring                          ns;
        const char*                           why;
        int                                   mod;
        bool                                  deferred;
        unsigned                              gen;
        std::vector<PortApi::DeferredTexture> texs;
    };
    std::deque<Job>         queue_; // under mtx_
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stop_ = false; // under mtx_
    std::thread             thread_;       // started under mtx_
};
extern TexEagerWorker g_tex_eager;
