#pragma once
// The mod lists' reload barrier. g_geomods, g_merged_comps and the maps built from them (g_first_to_comp, ...) are
// read without a lock by every game thread's callbacks: a reload of the mods (F10 / Reload mods) may only change them
// while no callback runs. Every callback of the game's device enters a ModsGate (OwnDevice in wwmi_addon.cpp, and the
// overlay); a reload pauses the gate, waits for the callbacks already inside to leave, changes the lists on its own
// thread, and resumes. Callbacks that arrive meanwhile wait at the gate -- they are not skipped, so the state the
// trackers follow (bindings, resources, lists) misses nothing.
//
// Cost on the hot path: one store to the thread's own cache line and one load (Dekker: both sequentially consistent,
// so either the reload sees the callback inside or the callback sees the pause). Nested callbacks on one thread count
// once. The reload's own thread is inside a callback too (present, or the frame-end submit): it never waits for itself.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace modsgate {
struct alignas(64) Slot {
    std::atomic<uint32_t> depth{ 0 }; // callbacks this thread is inside (nested ones too)
};
inline std::atomic<bool>       g_paused{ false };
inline std::mutex              g_m; // g_slots; g_cv's predicate
inline std::condition_variable g_cv;
inline std::vector<Slot*>      g_slots; // every thread that ever entered; never shrinks (a slot is 64 B)
inline Slot&                   Mine()
{
    thread_local Slot* s = [] {
        Slot*           p = new Slot();
        std::lock_guard l(g_m);
        g_slots.push_back(p);
        return p;
    }();
    return *s;
}
} // namespace modsgate

class ModsGate {
public:
    ModsGate()
        : s_(modsgate::Mine())
    {
        const uint32_t d = s_.depth.load(std::memory_order_relaxed);
        if (d) { // nested: this thread is already counted
            s_.depth.store(d + 1, std::memory_order_relaxed);
            return;
        }
        for (;;) {
            s_.depth.store(1, std::memory_order_seq_cst);
            if (!modsgate::g_paused.load(std::memory_order_seq_cst)) return;
            s_.depth.store(0, std::memory_order_seq_cst); // not inside while it waits: the reload does not wait for it
            std::unique_lock l(modsgate::g_m);
            modsgate::g_cv.wait(l, [] { return !modsgate::g_paused.load(std::memory_order_seq_cst); });
        }
    }
    ~ModsGate() { s_.depth.store(s_.depth.load(std::memory_order_relaxed) - 1, std::memory_order_release); }
    ModsGate(const ModsGate&) = delete;
    ModsGate& operator=(const ModsGate&) = delete;

private:
    modsgate::Slot& s_;
};

// Lets the callbacks waiting at the gate go on.
inline void ModsResume()
{
    {
        std::lock_guard l(modsgate::g_m);
        modsgate::g_paused.store(false, std::memory_order_seq_cst);
    }
    modsgate::g_cv.notify_all();
}

// Closes the gate and waits until no other thread is inside a callback. false (gate open again) when that takes
// longer than timeout_ms: a thread inside waits on something this one holds, so the reload is not done this time.
inline bool ModsPause(unsigned timeout_ms, unsigned* waited_ms = nullptr)
{
    {
        std::lock_guard l(modsgate::g_m);
        modsgate::g_paused.store(true, std::memory_order_seq_cst);
    }
    modsgate::Slot* const self = &modsgate::Mine();
    const auto            t0 = std::chrono::steady_clock::now();
    for (unsigned spin = 0;; ++spin) {
        bool busy = false;
        {
            std::lock_guard l(modsgate::g_m);
            for (modsgate::Slot* s : modsgate::g_slots)
                if (s != self && s->depth.load(std::memory_order_seq_cst)) {
                    busy = true;
                    break;
                }
        }
        const auto ms = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (waited_ms) *waited_ms = ms;
        if (!busy) return true;
        if (ms >= timeout_ms) {
            ModsResume();
            return false;
        }
        if (spin < 64)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

inline bool ModsPaused() { return modsgate::g_paused.load(std::memory_order_acquire); }
