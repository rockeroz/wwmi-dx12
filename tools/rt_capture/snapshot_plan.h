#pragma once
#include <algorithm>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace rt_capture {
struct Window { uint32_t first = 0, count = 0; };
struct Target {
    uint32_t vertices = 0;
    std::vector<Window> windows;
};
inline std::optional<Target> read_target(std::istream& in)
{
    std::string magic;
    uint64_t vertices = 0, count = 0;
    if (!(in >> magic >> vertices >> count) || magic != "WWMI_RT_TARGET_V1" ||
        !vertices || vertices > UINT32_MAX || !count || count > 64) return {};
    Target result{ uint32_t(vertices), {} };
    uint64_t end = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t first = 0, n = 0;
        if (!(in >> first >> n) || !n || first < end || first > UINT32_MAX || n > UINT32_MAX - first) return {};
        result.windows.push_back({ uint32_t(first), uint32_t(n) });
        end = first + n;
    }
    if (in >> magic) return {}; // no silently ignored configuration
    return result;
}
struct GeometrySlice {
    uint64_t vertices = 0, indices = 0;
    uint32_t vertex_count = 0, index_count = 0, stride = 0, vertex_format = 0, index_format = 0;
    bool transformed = false;
};
struct Ring { uint64_t vertices = 0, indices = 0; uint32_t index_bytes = 0; };
inline std::optional<Ring> match_ring(const Target& target, const std::vector<GeometrySlice>& geometries)
{
    if (target.windows.empty() || target.windows.size() != geometries.size()) return {};
    const auto& first = geometries.front();
    const uint32_t width = first.index_format == 42 ? 4u : first.index_format == 57 ? 2u : 0u;
    if (!width || !first.vertices || first.vertices > UINT64_MAX - uint64_t(target.vertices) * 12) return {};
    const uint64_t offset = uint64_t(target.windows.front().first) * width;
    if (first.indices <= offset) return {};
    Ring ring{ first.vertices, first.indices - offset, width };
    for (size_t i = 0; i < geometries.size(); ++i) {
        const auto& g = geometries[i]; const auto& w = target.windows[i];
        const uint64_t extent = uint64_t(w.first + w.count) * width;
        if (g.transformed || g.vertices != ring.vertices || g.vertex_count != target.vertices ||
            g.stride != 12 || g.vertex_format != 6 || g.index_format != first.index_format ||
            g.index_count != w.count || ring.indices > UINT64_MAX - extent ||
            g.indices != ring.indices + uint64_t(w.first) * width) return {};
    }
    return ring;
}
inline uint64_t snapshot_bytes(uint64_t resource_bytes, uint64_t offset, uint64_t requested = 16384)
{
    return offset < resource_bytes ? (std::min)({ resource_bytes - offset, requested, uint64_t(16384) }) : 0;
}
// ReShade's pinned native DX12 typed/raw SRV/UAV conversion preserves element
// units, unlike CBV and structured views. Convert explicitly, with overflow checks.
inline bool typed_bytes(uint64_t first, uint64_t count, uint32_t element, uint64_t& offset, uint64_t& bytes)
{
    if (!element || first > UINT64_MAX / element || count > UINT64_MAX / element) return false;
    offset = first * element; bytes = count * element;
    return offset <= UINT64_MAX - bytes;
}
} // namespace rt_capture
