#pragma once
#include <atomic>
#include <memory>

namespace rtm {
struct Completion {
    enum State { pending, complete, abandoned, failed };
    std::atomic<State> state{ pending };
    bool Ready() const { return state.load(std::memory_order_acquire) == complete; }
};
using CompletionPtr = std::shared_ptr<Completion>;
} // namespace rtm
