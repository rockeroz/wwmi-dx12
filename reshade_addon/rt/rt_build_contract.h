#pragma once
#include <cstdint>
#include <vector>
#include <utility>

namespace rtm {
// Properties DXR forbids changing during a BLAS update. Buffer addresses and vertex
// positions may change; index contents must not (index_generation identifies them).
struct GeometryContract {
    uint32_t flags = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t index_format = 0;
    uint32_t vertex_format = 0;
    uint64_t vertex_stride = 0;
    uint64_t index_generation = 0;
    bool has_transform = false;
    bool operator==(const GeometryContract&) const = default;
};

struct BuildContract {
    std::vector<GeometryContract> geometries;
    bool built = false;

    bool NeedsBuild(const std::vector<GeometryContract>& next, bool requested_build, uint32_t updates) const
    {
        return !built || requested_build || updates >= 300 || geometries != next;
    }
    // Called only after every allocation succeeded and the build was recorded.
    void Commit(std::vector<GeometryContract> next)
    {
        geometries = std::move(next);
        built = true;
    }
};
} // namespace rtm
