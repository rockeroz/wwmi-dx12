// rt_registry.h -- the RT module's bookkeeping, free of D3D12 / ReShade so it can be tested anywhere.
//
// A "ring" is the GPU range a character's BLAS triangles are read from: with ray tracing on, the game's
// GPU skin cache writes the posed positions there, and the raster draws of that character read the SAME
// range as vertex buffer 0 (observed: FLOAT3, stride 12, same VB/IB as the BLAS). A ring is learned only
// when BOTH sides agree:
//   - a BLAS triangle geometry: vertex VA, index VA at the geometry's start, index count;
//   - a draw the core matched to a mod component: vb0 VA, IB VA + first_index * index width, index count.
// Either may arrive first (BLAS builds and raster draws are recorded on different lists/threads), so both
// sides are kept for a while and matched from whichever arrives second.
//
// Nothing here proves bone layout, GPU order or ownership beyond that address/count agreement. Rings are
// observations for the M1 "observe" mode and for the core's existing RT diagnostics (RtIsSkinOut...).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace rtm {

struct BlasGeom {
    uint64_t vb = 0;      // vertex VA (start of the geometry's positions)
    uint64_t ib = 0;      // index VA at the geometry's first index
    uint32_t vertex_count = 0;
    uint32_t vertex_stride = 0;
    uint32_t vertex_format = 0; // reshade::api::format value (6 = R32G32B32_FLOAT)
    uint32_t index_count = 0;
    uint32_t index_width = 0;   // 2 or 4; 0 = unindexed / unknown (never matched)
    uint32_t geometry = 0;      // its position in the build's input array
    uint32_t geometry_count = 0;
    uint64_t dest = 0;          // destination AS (diagnostic only)
    uint32_t frame = 0;
    bool     transformed = false;
};

struct DrawNote {
    int      mod = -1;
    int      comp = -1;
    bool     identified = false; // the core's definitive identification (IB hash, texture, ...), not first+count alone
    uint64_t vb0 = 0;            // bound VB0 VA (vertex_offset already applied by the caller)
    uint32_t vb0_bytes = 0;
    uint32_t vb0_stride = 0;
    uint64_t ib_at = 0; // IB VA + first_index * index width
    uint32_t index_count = 0;
    uint32_t frame = 0;
};

struct Ring {
    uint64_t va = 0;
    uint64_t bytes = 0;
    int      mod = -1;
    bool     weak = true; // learned only from a first+count match, never from an identified draw
    uint32_t learned_frame = 0;
    uint32_t confirmed_frame = 0;
    uint32_t geometries = 0; // distinct (ib, count) pairs seen reading this ring
};

inline bool Overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    if (!an || !bn) return false;
    const uint64_t ae = (a > UINT64_MAX - an) ? UINT64_MAX : a + an;
    const uint64_t be = (b > UINT64_MAX - bn) ? UINT64_MAX : b + bn;
    return a < be && b < ae;
}

// What a learned/refreshed ring means to the caller (to log it once, or count it).
struct RingEvent {
    Ring ring;
    bool is_new = false;
    BlasGeom geom;
    DrawNote draw;
};

class Registry {
public:
    static constexpr size_t   kMaxGeoms = 4096;
    static constexpr size_t   kMaxDraws = 2048;
    static constexpr size_t   kMaxRings = 64;
    static constexpr uint32_t kKeepFrames = 240;    // how long one side waits for the other
    static constexpr uint32_t kRingExpire = 1800;   // a ring nobody confirmed for this long is dropped

    // A BLAS build. Returns the rings this build learned or confirmed.
    std::vector<RingEvent> NoteBlas(const std::vector<BlasGeom>& geoms, uint32_t frame)
    {
        std::vector<RingEvent> out;
        Expire(frame);
        for (const BlasGeom& g : geoms) {
            if (!Usable(g)) continue;
            const Key k{ g.ib, g.index_count };
            if (geoms_.size() >= kMaxGeoms && !geoms_.count(k)) EvictOldest(geoms_);
            geoms_[k] = g;
            geoms_[k].frame = frame;
            if (geom_vbs_.size() < kMaxGeoms || geom_vbs_.count(g.vb)) geom_vbs_[g.vb] = frame;
            auto d = draws_.find(k);
            if (d != draws_.end() && Agree(g, d->second)) Learn(g, d->second, frame, out);
        }
        return out;
    }

    // A draw the core matched to a mod component. Returns the ring it learned or confirmed, if any.
    std::optional<RingEvent> NoteDraw(const DrawNote& in, uint32_t frame)
    {
        if (!in.vb0 || !in.ib_at || !in.index_count || in.mod < 0) return std::nullopt;
        Expire(frame);
        const Key k{ in.ib_at, in.index_count };
        auto      cur = draws_.find(k);
        // An identified note is never replaced by a weaker one for the same (ib, count) in the same window.
        if (cur == draws_.end() || in.identified || !cur->second.identified || frame - cur->second.frame > kKeepFrames) {
            if (draws_.size() >= kMaxDraws && cur == draws_.end()) EvictOldest(draws_);
            draws_[k] = in;
            draws_[k].frame = frame;
        }
        const auto g = geoms_.find(k);
        if (g == geoms_.end() || !Agree(g->second, in)) return std::nullopt;
        std::vector<RingEvent> out;
        Learn(g->second, in, frame, out);
        if (out.empty()) return std::nullopt;
        return out.front();
    }

    // Public API mirror of rt_hooks.h.
    void NoteRing(uint64_t va, uint64_t bytes, int mod, bool weak, uint32_t frame)
    {
        if (!va || !bytes) return;
        for (Ring& r : rings_)
            if (r.va == va) {
                r.bytes = (std::max)(r.bytes, bytes);
                if (mod >= 0 && (!weak || r.weak || r.mod < 0)) r.mod = mod;
                r.weak = r.weak && weak;
                r.confirmed_frame = frame;
                return;
            }
        if (rings_.size() >= kMaxRings) {
            auto oldest = std::min_element(rings_.begin(), rings_.end(),
                [](const Ring& a, const Ring& b) { return a.confirmed_frame < b.confirmed_frame; });
            rings_.erase(oldest);
        }
        Ring r;
        r.va = va;
        r.bytes = bytes;
        r.mod = mod;
        r.weak = weak;
        r.learned_frame = r.confirmed_frame = frame;
        rings_.push_back(r);
        ++gen_;
    }
    bool IsSkinOut(uint64_t va, uint64_t bytes) const
    {
        for (const Ring& r : rings_)
            if (Overlaps(r.va, r.bytes, va, bytes ? bytes : 1)) return true;
        return false;
    }
    int SkinOutMod(uint64_t va, uint64_t bytes, int* overlap_mod) const
    {
        if (overlap_mod) *overlap_mod = -1;
        for (const Ring& r : rings_) {
            if (r.va == va && r.bytes == bytes) return r.mod;
            if (overlap_mod && *overlap_mod < 0 && Overlaps(r.va, r.bytes, va, bytes ? bytes : 1)) *overlap_mod = r.mod;
        }
        return -1;
    }
    const Ring* RingAt(uint64_t va, uint64_t bytes) const
    {
        for (const Ring& r : rings_)
            if (Overlaps(r.va, r.bytes, va, bytes ? bytes : 1)) return &r;
        return nullptr;
    }
    void Forget(uint64_t va, uint64_t bytes)
    {
        if (rings_.empty()) return;
        const size_t before = rings_.size();
        rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
                         [&](const Ring& r) { return Overlaps(r.va, r.bytes, va, bytes); }),
            rings_.end());
        // Pending BLAS geometries / draws are not scanned here (this runs on every buffer destruction): they
        // expire after kKeepFrames, and a stale one still needs a matching other side to learn anything.
        if (rings_.size() != before) ++gen_;
        PruneRingGeoms();
    }
    size_t                   Count() const { return rings_.size(); }
    uint32_t                 Gen() const { return gen_; }
    const std::vector<Ring>& Rings() const { return rings_; }
    size_t                   PendingGeoms() const { return geoms_.size(); }
    // Was this VA the vertex base of a recent BLAS triangle geometry?
    bool HasGeomVb(uint64_t vb) const { return vb && geom_vbs_.count(vb) != 0; }
    size_t                   PendingDraws() const { return draws_.size(); }

    void Expire(uint32_t frame)
    {
        if (frame == last_expire_) return;
        last_expire_ = frame;
        const size_t before = rings_.size();
        rings_.erase(std::remove_if(rings_.begin(), rings_.end(),
                         [&](const Ring& r) { return frame - r.confirmed_frame > kRingExpire; }),
            rings_.end());
        if (rings_.size() != before) ++gen_;
        DropOld(geoms_, frame);
        DropOld(draws_, frame);
        for (auto it = geom_vbs_.begin(); it != geom_vbs_.end();)
            it = (frame - it->second > kKeepFrames) ? geom_vbs_.erase(it) : std::next(it);
        PruneRingGeoms();
    }

private:
    struct Key {
        uint64_t ib;
        uint32_t count;
        bool     operator<(const Key& o) const { return ib != o.ib ? ib < o.ib : count < o.count; }
    };
    static bool Usable(const BlasGeom& g)
    {
        return g.vb && g.ib && g.vertex_count && g.vertex_stride && g.index_count && (g.index_width == 2 || g.index_width == 4) &&
            !g.transformed;
    }
    // Same index range was established by the key; the vertex side must agree too.
    static bool Agree(const BlasGeom& g, const DrawNote& d)
    {
        if (g.vb != d.vb0) return false;
        if (d.vb0_stride && d.vb0_stride != g.vertex_stride) return false;
        return true;
    }
    void Learn(const BlasGeom& g, const DrawNote& d, uint32_t frame, std::vector<RingEvent>& out)
    {
        const uint64_t bytes = uint64_t(g.vertex_count) * g.vertex_stride;
        const bool     existed = std::any_of(rings_.begin(), rings_.end(), [&](const Ring& r) { return r.va == g.vb; });
        NoteRing(g.vb, bytes, d.mod, !d.identified, frame);
        Ring* r = nullptr;
        for (Ring& x : rings_)
            if (x.va == g.vb) r = &x;
        if (!r) return;
        auto& seen = ring_geoms_[g.vb];
        if (std::find(seen.begin(), seen.end(), std::make_pair(g.ib, g.index_count)) == seen.end() && seen.size() < 64) {
            seen.emplace_back(g.ib, g.index_count);
            r->geometries = uint32_t(seen.size());
        }
        RingEvent e;
        e.ring = *r;
        e.is_new = !existed;
        e.geom = g;
        e.draw = d;
        out.push_back(e);
    }
    void PruneRingGeoms()
    {
        for (auto it = ring_geoms_.begin(); it != ring_geoms_.end();)
            it = std::none_of(rings_.begin(), rings_.end(), [&](const Ring& r) { return r.va == it->first; }) ? ring_geoms_.erase(it) : std::next(it);
    }
    template <class M>
    static void EvictOldest(M& m)
    {
        auto oldest = m.begin();
        for (auto it = m.begin(); it != m.end(); ++it)
            if (it->second.frame < oldest->second.frame) oldest = it;
        if (oldest != m.end()) m.erase(oldest);
    }
    template <class M>
    static void DropOld(M& m, uint32_t frame)
    {
        for (auto it = m.begin(); it != m.end();)
            it = (frame - it->second.frame > kKeepFrames) ? m.erase(it) : std::next(it);
    }

    std::map<Key, BlasGeom>                                             geoms_;
    std::map<Key, DrawNote>                                             draws_;
    std::vector<Ring>                                                   rings_;
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint32_t>>>     ring_geoms_;
    std::map<uint64_t, uint32_t>                                        geom_vbs_; // vertex base -> last frame seen
    uint32_t                                                            gen_ = 0;
    uint32_t                                                            last_expire_ = 0;
};

// ---- a BLAS build explained by a mod's component windows ------------------------------------------
// The mod's ini names each component by the ORIGINAL mesh's index window (match_first_index /
// match_index_count). A skinned character's BLAS has one triangle geometry per drawn section, all reading
// the same vertex base, each starting at IB + first * width. If one mod's windows explain EVERY geometry
// of a build -- same counts, same relative first indices -- the build is that character's, whatever its
// raster draws look like. One geometry alone is too weak unless its count is unique among all components.
struct CompWindow {
    int      mod = -1;
    int      comp = -1;
    uint32_t first = 0;
    uint32_t count = 0;
};
struct SignatureHit {
    int      mod = -1;
    uint64_t vb = 0;
    uint64_t bytes = 0;
    uint32_t geometries = 0;
    uint32_t base_first = 0; // the first index of the build's lowest index VA, per the mod's windows
};
inline std::vector<SignatureHit> MatchSignatures(const std::vector<BlasGeom>& geoms, const std::vector<CompWindow>& comps)
{
    std::vector<SignatureHit> hits;
    if (comps.empty()) return hits;
    // group usable geometries by vertex base
    std::map<uint64_t, std::vector<const BlasGeom*>> by_vb;
    for (const BlasGeom& g : geoms)
        if (g.vb && g.ib && g.index_count && (g.index_width == 2 || g.index_width == 4) && !g.transformed && g.vertex_stride)
            by_vb[g.vb].push_back(&g);
    for (const auto& [vb, list] : by_vb) {
        const uint32_t w = list.front()->index_width;
        uint64_t       ib0 = UINT64_MAX;
        bool           ok = true;
        for (const BlasGeom* g : list) {
            if (g->index_width != w) ok = false;
            ib0 = (std::min)(ib0, g->ib);
        }
        if (!ok) continue;
        std::vector<std::pair<uint64_t, uint32_t>> rel; // (first index relative to ib0, count)
        for (const BlasGeom* g : list) {
            const uint64_t d = g->ib - ib0;
            if (d % w) {
                ok = false;
                break;
            }
            rel.emplace_back(d / w, g->index_count);
        }
        if (!ok) continue;
        if (rel.size() == 1) { // a lone geometry: only if its count is unique among every component
            const uint32_t c = rel.front().second;
            int            owner = -1, n = 0;
            for (const CompWindow& cw : comps)
                if (cw.count == c) {
                    ++n;
                    owner = cw.mod;
                }
            if (n != 1 || c < 1024) continue;
            hits.push_back({ owner, vb, uint64_t(list.front()->vertex_count) * list.front()->vertex_stride, 1u, 0u });
            for (const CompWindow& cw : comps)
                if (cw.count == c) hits.back().base_first = cw.first;
            continue;
        }
        // the geometry at relative 0, aligned in turn to each component with its count, of each mod
        const auto anchor = std::find_if(rel.begin(), rel.end(), [](const auto& r) { return r.first == 0; });
        int        found = -1;
        uint32_t   found_base = 0;
        for (const CompWindow& a : comps) {
            if (a.count != anchor->second) continue;
            const uint64_t base = a.first;
            bool           all = true;
            for (const auto& [off, cnt] : rel) {
                const bool m = std::any_of(comps.begin(), comps.end(), [&](const CompWindow& c) {
                    return c.mod == a.mod && c.count == cnt && uint64_t(c.first) == base + off;
                });
                if (!m) {
                    all = false;
                    break;
                }
            }
            if (!all) continue;
            if (found >= 0 && found != a.mod) {
                found = -2; // two mods explain it: refuse
                break;
            }
            found = a.mod;
            found_base = uint32_t(base);
        }
        if (found >= 0)
            hits.push_back({ found, vb, uint64_t(list.front()->vertex_count) * list.front()->vertex_stride, uint32_t(rel.size()), found_base });
    }
    return hits;
}

// ---- bone-likeness of raw constant / buffer bytes -------------------------------------------------
// A 3x4 row-major affine matrix (12 floats) whose 3x3 part looks like a rotation (+ mild scale) and whose
// translation is finite and bounded. Camera, object and light matrices pass this too: a heuristic only.
inline bool LooksAffine3x4(const float* m)
{
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(m[i])) return false;
    float n[3];
    for (int r = 0; r < 3; ++r) {
        n[r] = std::sqrt(m[r * 4 + 0] * m[r * 4 + 0] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
        if (n[r] < 0.25f || n[r] > 4.0f) return false;
        if (std::fabs(m[r * 4 + 3]) > 1.0e5f) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = (m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2]) / (n[a] * n[b]);
            if (std::fabs(d) > 0.3f) return false;
        }
    return true;
}

struct BoneRun {
    uint32_t offset = 0; // bytes
    uint32_t count = 0;  // consecutive 48-byte matrices
};
// The longest run of consecutive 48-byte affine matrices, at any 16-byte-aligned start.
inline BoneRun BestBoneRun(const uint8_t* bytes, size_t n)
{
    BoneRun best;
    if (!bytes || n < 48) return best;
    for (size_t start = 0; start < 48 && start + 48 <= n; start += 16) {
        uint32_t run = 0;
        size_t   run_start = start;
        for (size_t at = start; at + 48 <= n; at += 48) {
            float m[12];
            std::memcpy(m, bytes + at, sizeof m);
            if (LooksAffine3x4(m)) {
                if (!run) run_start = at;
                if (++run > best.count) {
                    best.count = run;
                    best.offset = uint32_t(run_start);
                }
            } else
                run = 0;
        }
    }
    return best;
}

// How many 4-byte words differ between two snapshots of the same place (over the shorter length).
inline uint32_t ChangedWords(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    const size_t n = (std::min)(a.size(), b.size()) / 4;
    uint32_t     c = 0;
    for (size_t i = 0; i < n; ++i) c += std::memcmp(a.data() + i * 4, b.data() + i * 4, 4) != 0;
    return c;
}

// When the module looks at a dispatch's bindings. Looking costs a few hundred lock-free heap lookups, so:
//   - a compute shader seen binding a ring is looked at only on observation frames (IsObservationFrame);
//   - any other shader is judged again after the rings change (gen): kPerFrame of its dispatches per frame,
//     for kJudgeFrames frames, at a window that moves through the frame's first kWindow dispatches -- the
//     skin-cache shader is shared by every character and dispatch order repeats frame to frame, so a fixed
//     "first few" would never reach a character that is not dispatched first.
inline bool IsObservationFrame(uint32_t frame)
{
    const uint32_t m = frame % 1800u;
    return m == 0 || m == 1 || m == 30;
}
class CsJudge {
public:
    static constexpr uint32_t kPerFrame = 2;
    static constexpr uint32_t kJudgeFrames = 300;
    static constexpr uint32_t kWindow = 128;
    static constexpr size_t   kMaxShaders = 4096;
    // True = look at this dispatch's bindings now. `first_sight` = the rings changed since this shader was last judged.
    bool ShouldInspect(uint64_t cs, uint32_t ring_gen, uint32_t frame)
    {
        auto it = map_.find(cs);
        if (it == map_.end()) {
            if (map_.size() >= kMaxShaders) return false;
            it = map_.emplace(cs, Entry{}).first;
        }
        Entry& e = it->second;
        if (e.gen != ring_gen) {
            e.gen = ring_gen;
            e.since = frame;
        }
        if (e.frame != frame) {
            e.frame = frame;
            e.in_frame = 0;
        }
        const uint32_t idx = e.in_frame++; // this dispatch's position among the shader's dispatches this frame
        if (e.hits) return IsObservationFrame(frame) || frame - e.since < 2; // a new ring: look right away too
        if (frame - e.since >= kJudgeFrames) return false;
        const uint32_t off = ((frame - e.since) * kPerFrame) % kWindow;
        return idx >= off && idx < off + kPerFrame;
    }
    void Verdict(uint64_t cs, bool binds_ring)
    {
        auto it = map_.find(cs);
        if (it != map_.end() && binds_ring) ++it->second.hits;
    }
    uint32_t Hits(uint64_t cs) const
    {
        auto it = map_.find(cs);
        return it == map_.end() ? 0 : it->second.hits;
    }
    size_t SkinShaders() const
    {
        size_t n = 0;
        for (const auto& [cs, e] : map_) n += e.hits != 0;
        return n;
    }

private:
    struct Entry {
        uint32_t gen = UINT32_MAX;
        uint32_t since = 0;
        uint32_t frame = UINT32_MAX;
        uint32_t in_frame = 0;
        uint32_t hits = 0;
    };
    std::map<uint64_t, Entry> map_;
};

} // namespace rtm
