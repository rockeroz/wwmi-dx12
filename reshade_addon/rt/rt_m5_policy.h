#pragma once
#include <cstdint>
#include <map>
#include <iterator>
#include <mutex>

namespace rtcritical {
// No address heuristics: these slots exist only because CreateDescriptorHeap
// allocated them in addition to the application-visible descriptor count.
inline uint32_t ReservationCapacity(uint32_t requested)
{
    constexpr uint32_t limit = 1000000, tail = 65536;
    return requested >= 65536 && requested <= limit - tail ? tail : 0;
}

class FreeRanges {
    std::map<uint32_t, uint32_t> free_;
public:
    FreeRanges(uint32_t first, uint32_t count) { if (count) free_.emplace(first, count); }
    bool Take(uint32_t count, uint32_t* first)
    {
        if (!count || !first) return false;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second < count) continue;
            *first = it->first;
            const uint32_t left = it->second - count;
            free_.erase(it);
            if (left) free_.emplace(*first + count, left);
            return true;
        }
        return false;
    }
    void Put(uint32_t first, uint32_t count)
    {
        if (!count) return;
        auto next = free_.lower_bound(first);
        if (next != free_.begin()) {
            auto previous = std::prev(next);
            if (uint64_t(previous->first) + previous->second == first) {
                first = previous->first;
                count += previous->second;
                free_.erase(previous);
            }
        }
        next = free_.lower_bound(first);
        if (next != free_.end() && uint64_t(first) + count == next->first) {
            count += next->second;
            free_.erase(next);
        }
        free_.emplace(first, count);
    }
};

// Publication has a single commit point. A timeout publishes the already-zeroed
// fallback; it must never allow a subsequent late refill to change GPU input.
class PublicationState {
public:
    enum State { pending, published, fallback };
private:
    State state_ = pending;
public:
    State Get() const { return state_; }
    bool Commit(bool real_block)
    {
        if (state_ != pending) return false;
        state_ = real_block ? published : fallback;
        return true;
    }
};
} // namespace rtcritical
