#pragma once
// CPU-side trace storage, deliberately independent of Windows and ReShade.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rt_capture {
struct TraceBuffer {
    static constexpr size_t max_bytes = 8 * 1024 * 1024;
    static constexpr size_t max_events = 32768;
    std::vector<std::string> events;
    std::vector<uint64_t> sequences;
    size_t bytes = 0;
    uint64_t dropped = 0;

    bool append(std::string row, uint64_t sequence = 0)
    {
        if (events.size() >= max_events || row.size() > max_bytes - bytes) {
            ++dropped;
            return false;
        }
        bytes += row.size();
        events.push_back(std::move(row));
        sequences.push_back(sequence);
        return true;
    }
    void clear()
    {
        events.clear();
        sequences.clear();
        bytes = 0;
        dropped = 0;
    }
};

template <class Visit>
void visit_sequenced(const TraceBuffer& general, const TraceBuffer& priority, Visit&& visit)
{
    size_t a = 0, b = 0;
    while (a < general.events.size() || b < priority.events.size()) {
        const bool take_priority = a == general.events.size() ||
            (b < priority.events.size() && priority.sequences[b] < general.sequences[a]);
        visit(take_priority ? priority.events[b++] : general.events[a++]);
    }
}

inline std::string hex(uint64_t value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(18, '0');
    result[1] = 'x';
    for (unsigned i = 0; i < 16; ++i) {
        result[17 - i] = digits[value & 15];
        value >>= 4;
    }
    return result;
}

inline uint64_t fnv1a64(const void* data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
} // namespace rt_capture
