#pragma once
#include <cstdint>
#include <vector>

namespace rtm {
class RecordPlan {
    struct Range { uint64_t first, count, owner; };
    std::vector<Range> ranges_;
public:
    bool Reserve(uint64_t first, uint64_t count, uint64_t owner, uint64_t total)
    {
        if (!count || first > total || count > total - first) return false;
        for (const auto& r : ranges_) {
            if (first == r.first && count == r.count && owner == r.owner) return true;
            if (first < r.first + r.count && r.first < first + count) return false;
        }
        ranges_.push_back({ first, count, owner });
        return true;
    }
    bool Contains(uint64_t record) const
    {
        for (const auto& r : ranges_)
            if (record >= r.first && record - r.first < r.count) return true;
        return false;
    }
};
} // namespace rtm
