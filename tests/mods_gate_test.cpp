#define NOMINMAX
#include "../reshade_addon/mods/mods_gate.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

// The mod reload barrier (mods/mods_gate.h): game threads enter and leave callbacks without pause while a reload
// swaps a list they read; no reader is ever inside while it changes, nested callbacks count once, the reload never
// waits for its own thread, and a reader that does not leave makes the pause give up instead of hanging.
static void Check(bool v, const char* why)
{
    if (!v) {
        std::cerr << "FAIL: " << why << '\n';
        std::exit(1);
    }
}

static std::atomic<bool>     g_mutating{ false };
static std::atomic<bool>     g_stop{ false };
static std::atomic<unsigned> g_inside_while_mutating{ 0 };
static std::atomic<uint64_t> g_entries{ 0 };
static std::vector<int>*     g_list = nullptr; // read inside the gate, reallocated by the "reload"

static void Reader(int id)
{
    uint64_t sum = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        ModsGate gate;
        if (g_mutating.load(std::memory_order_seq_cst)) g_inside_while_mutating.fetch_add(1);
        {
            ModsGate nested; // a callback inside a callback: never waits
            const std::vector<int>& l = *g_list;
            for (size_t i = 0; i < l.size(); i += 7) sum += (uint64_t)l[i];
        }
        if (g_mutating.load(std::memory_order_seq_cst)) g_inside_while_mutating.fetch_add(1);
        g_entries.fetch_add(1, std::memory_order_relaxed);
        if ((id & 1) == 0) std::this_thread::yield();
    }
    if (sum == 42) std::cout << ""; // keep the reads
}

int main()
{
    g_list = new std::vector<int>(1000, 1);
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back(Reader, i);

    unsigned paused = 0;
    for (int round = 0; round < 300; ++round) {
        ModsGate self; // the reload runs inside a callback of its own (present): it must not wait for itself
        unsigned waited = 0;
        Check(ModsPause(2000, &waited), "the readers leave within the timeout");
        g_mutating.store(true, std::memory_order_seq_cst);
        for (int k = 0; k < 64; ++k) g_list->push_back(round); // reallocates while no reader is inside
        std::vector<int>* old = g_list;
        g_list = new std::vector<int>(*old);
        delete old;
        g_mutating.store(false, std::memory_order_seq_cst);
        ModsResume();
        ++paused;
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    const uint64_t before = g_entries.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(g_entries.load() > before, "readers go on after the last resume");
    Check(g_inside_while_mutating.load() == 0, "no reader is inside a callback while the lists change");

    // A reader that does not leave: the pause gives up after its timeout and opens the gate again.
    std::atomic<bool> hold{ true }, in{ false };
    std::thread       stuck([&] {
        ModsGate gate;
        in.store(true);
        while (hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!in.load()) std::this_thread::yield();
    unsigned waited = 0;
    Check(!ModsPause(100, &waited), "a reader that stays inside makes the pause give up");
    Check(waited >= 100, "it gave up only after the timeout");
    Check(!ModsPaused(), "the gate is open again after giving up");
    const uint64_t after_giveup = g_entries.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(g_entries.load() > after_giveup, "readers were not left waiting by the failed pause");
    hold.store(false);
    stuck.join();

    g_stop.store(true);
    for (auto& t : readers) t.join();
    std::cout << "mods_gate_test: PASS (" << paused << " pauses, " << g_entries.load() << " callbacks)\n";
    return 0;
}
