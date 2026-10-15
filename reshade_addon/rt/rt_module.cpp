// rt_module.cpp -- the RT module, milestone M1: OBSERVE. Built instead of rt_stub.cpp with WwmiRt=1.
//
// What M1 does while the game ray traces:
//   - keeps the core running instead of switching it off (RtModulePresent), but leaves every character
//     draw and dispatch the game's own (RtClaimNote / RtHoldVanilla): the mod is NOT drawn yet;
//   - learns each mod character's skin-cache output ring by pairing a BLAS triangle geometry with the
//     claimed raster draw that reads the same index range and the same vertex base (rt_registry.h);
//   - on compute dispatches that bind a ring, records what else is bound and snapshots the UPLOAD-heap
//     constants / buffers among them (the core's own CPU mapping, SnapshotGameCB), scoring bone-likeness;
//   - writes <Win64>\wwmi_dx12\rt_observe\rt_observe.log (+ bins\*.bin) and shows a section in the overlay.
// With RT off it does nothing and the mods draw as always. ini [wwmi-dx12] rt_mode=0 restores the old
// behaviour (the add-on switches itself off while the game ray traces).
//
// It does NOT: replace BLAS/TLAS, read DEFAULT-heap buffers, publish a palette or skin the mod. Those are M2+.
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../mods/layout_registry.h" // TableStartOf
#include "../mods/skeleton.h"        // SnapshotGameCB
#include "rt_hooks.h"
#include "rt_registry.h"
#include <cstdarg>
#include <map>
#include <share.h>
#include <tuple>

using namespace reshade::api;

namespace {

constexpr uint32_t kActiveFrames = 120;     // RT counts as on this long after the last BLAS build
constexpr uint32_t kSnapBytes = 16384;      // per binding snapshot
constexpr uint64_t kLogCap = 16ull << 20;   // rt_observe.log
constexpr uint64_t kBinCap = 64ull << 20;   // all bins together
constexpr uint32_t kBinFiles = 4000;
constexpr uint32_t kSummaryEvery = 600;     // frames

std::mutex            g_m; // registry, judge, sample state, draw-vb map
rtm::Registry         g_reg;
rtm::CsJudge          g_judge;
std::atomic<int>      g_mode{ -1 }; // -1 unread, 0 off, 1 observe
std::atomic<uint32_t> g_last_blas_frame{ 0 };
std::atomic<uint32_t> g_ring_count{ 0 };
std::atomic<uint32_t> g_ring_gen{ 0 };

struct Counters {
    std::atomic<uint64_t> blas_builds{ 0 }, blas_bottom{ 0 }, claims{ 0 }, claims_identified{ 0 }, claims_held{ 0 },
        ring_dispatches{ 0 }, pal_records{ 0 }, snapshots{ 0 }, snapshot_upload{ 0 }, bins{ 0 }, bin_bytes{ 0 },
        inspect_us{ 0 };
    std::atomic<uint32_t> best_bone_run{ 0 };
    std::atomic<uint64_t> best_bone_cs{ 0 };
    std::atomic<uint32_t> best_bone_param{ 0 };
} g_c;

int Mode()
{
    int m = g_mode.load(std::memory_order_relaxed);
    if (m >= 0) return m;
    if (g_addon_ini_path.empty()) return 1; // not read yet: observe, re-read later
    m = GetPrivateProfileIntW(L"wwmi-dx12", L"rt_mode", 1, g_addon_ini_path.c_str()) != 0 ? 1 : 0;
    g_mode.store(m, std::memory_order_relaxed);
    return m;
}
bool RtActive()
{
    const uint32_t last = g_last_blas_frame.load(std::memory_order_relaxed);
    return last && g_frame_id - last <= kActiveFrames;
}
void SyncAtomics() // g_m held
{
    g_ring_count.store(uint32_t(g_reg.Count()), std::memory_order_relaxed);
    g_ring_gen.store(g_reg.Gen(), std::memory_order_relaxed);
}

// ---- log + bins ---------------------------------------------------------------------------------
class ObserveLog {
public:
    void Line(_Printf_format_string_ const char* fmt, ...)
    {
        char    buf[1024];
        va_list a;
        va_start(a, fmt);
        int n = vsnprintf(buf, sizeof buf, fmt, a);
        va_end(a);
        if (n < 0) return;
        if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
        if (logger::enabled(1)) logger::write("%s", buf);
        std::lock_guard l(m_);
        if (!Open() || bytes_ >= kLogCap) return;
        char head[32];
        const int h = snprintf(head, sizeof head, "[f%u] ", g_frame_id);
        fwrite(head, 1, (size_t)h, f_);
        fwrite(buf, 1, (size_t)n, f_);
        fputc('\n', f_);
        fflush(f_);
        bytes_ += uint64_t(h + n + 1);
        if (bytes_ >= kLogCap) {
            fputs("[log cap reached]\n", f_);
            fflush(f_);
        }
    }
    // A raw snapshot next to the log. Returns the file's name ("" = not written).
    std::string Bin(const char* name, const void* data, size_t n)
    {
        std::lock_guard l(m_);
        if (!Open() || dir_.empty()) return {};
        if (g_c.bins.load() >= kBinFiles || g_c.bin_bytes.load() + n > kBinCap) return {};
        const std::wstring bins = dir_ + L"\\bins";
        CreateDirectoryW(bins.c_str(), nullptr);
        std::wstring wname(name, name + strlen(name));
        FILE*        f = _wfsopen((bins + L"\\" + wname).c_str(), L"wb", _SH_DENYWR);
        if (!f) return {};
        const size_t w = fwrite(data, 1, n, f);
        fclose(f);
        if (w != n) return {};
        g_c.bins.fetch_add(1);
        g_c.bin_bytes.fetch_add(n);
        return name;
    }
    std::wstring Dir()
    {
        std::lock_guard l(m_);
        return dir_;
    }

private:
    bool Open() // m_ held
    {
        if (f_) return true;
        if (tried_) return false;
        tried_ = true;
        if (g_data_dir.empty()) return false;
        CreateDirectoryW(g_data_dir.c_str(), nullptr);
        dir_ = g_data_dir + L"\\rt_observe";
        CreateDirectoryW(dir_.c_str(), nullptr);
        const std::wstring path = dir_ + L"\\rt_observe.log";
        MoveFileExW(path.c_str(), (path + L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
        f_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        if (!f_) return false;
        const int n = fprintf(f_, "wwmi-dx12 %s (build %s) RT module M1 observe; rt_mode=%d\n", WWMI_VERSION, WWMI_BUILD_SHA,
            g_mode.load());
        bytes_ = n > 0 ? uint64_t(n) : 0;
        fflush(f_);
        return true;
    }
    std::mutex   m_;
    FILE*        f_ = nullptr;
    bool         tried_ = false;
    uint64_t     bytes_ = 0;
    std::wstring dir_;
};
ObserveLog g_log;

void MaybeSummary()
{
    static std::atomic<uint32_t> s_last{ 0 };
    const uint32_t               f = g_frame_id;
    uint32_t                     last = s_last.load(std::memory_order_relaxed);
    if (last && f - last < kSummaryEvery) return;
    if (!s_last.compare_exchange_strong(last, f)) return;
    std::string rings;
    {
        std::lock_guard l(g_m);
        for (const rtm::Ring& r : g_reg.Rings()) {
            char b[160];
            snprintf(b, sizeof b, " [va=%llx bytes=%llu mod=%d%s geoms=%u seen=%uf]", (unsigned long long)r.va,
                (unsigned long long)r.bytes, r.mod, r.weak ? " weak" : "", r.geometries, f - r.confirmed_frame);
            rings += b;
        }
    }
    g_log.Line("RTSUM active=%d blas=%llu bottom=%llu claims=%llu identified=%llu held=%llu ring_dispatches=%llu "
               "pal_records=%llu snapshots=%llu upload=%llu bins=%llu best_bone_run=%u (cs=%016llx param=%u) "
               "inspect_us=%llu rings=%u:%s",
        RtActive() ? 1 : 0, (unsigned long long)g_c.blas_builds.load(), (unsigned long long)g_c.blas_bottom.load(),
        (unsigned long long)g_c.claims.load(), (unsigned long long)g_c.claims_identified.load(),
        (unsigned long long)g_c.claims_held.load(), (unsigned long long)g_c.ring_dispatches.load(),
        (unsigned long long)g_c.pal_records.load(), (unsigned long long)g_c.snapshots.load(),
        (unsigned long long)g_c.snapshot_upload.load(), (unsigned long long)g_c.bins.load(), g_c.best_bone_run.load(),
        (unsigned long long)g_c.best_bone_cs.load(), g_c.best_bone_param.load(),
        (unsigned long long)g_c.inspect_us.load(), g_ring_count.load(), rings.c_str());
}

uint64_t BufferVa(resource res, uint64_t offset) // ReShade DX12: a null resource carries the VA in `offset`
{
    if (!res.handle) return offset;
    auto* r = reinterpret_cast<ID3D12Resource*>(res.handle);
    return r->GetGPUVirtualAddress() + offset;
}
uint32_t IndexWidth(format f)
{
    return f == format::r32_uint ? 4u : f == format::r16_uint ? 2u : 0u;
}

void LogRing(const rtm::RingEvent& e, const char* via)
{
    g_log.Line("RTRING %s via %s: va=%llx bytes=%llu mod=%d comp=%d%s | blas geom %u/%u verts=%u stride=%u fmt=%u "
               "ib=%llx count=%u width=%u dest=%llx | draw vb0=%llx stride=%u",
        e.is_new ? "NEW" : "again", via, (unsigned long long)e.ring.va, (unsigned long long)e.ring.bytes, e.ring.mod, e.draw.comp,
        e.ring.weak ? " (first+count only)" : "", e.geom.geometry, e.geom.geometry_count, e.geom.vertex_count,
        e.geom.vertex_stride, e.geom.vertex_format, (unsigned long long)e.geom.ib, e.geom.index_count, e.geom.index_width,
        (unsigned long long)e.geom.dest, (unsigned long long)e.draw.vb0, e.draw.vb0_stride);
}

// ---- RTPAL sampling state ------------------------------------------------------------------------
struct SampleKey {
    uint64_t cs, ring;
    bool     operator<(const SampleKey& o) const { return cs != o.cs ? cs < o.cs : ring < o.ring; }
};
struct SampleState {
    uint32_t first = 0, frame = 0, in_frame = 0, records = 0;
};
std::map<SampleKey, SampleState> g_samples; // g_m

struct SnapKey {
    uint64_t cs, ring;
    uint32_t kind, param, slot, section;
    bool     operator<(const SnapKey& o) const
    {
        return std::tie(cs, ring, kind, param, slot, section) < std::tie(o.cs, o.ring, o.kind, o.param, o.slot, o.section);
    }
};
std::map<SnapKey, std::pair<uint32_t, std::vector<uint8_t>>> g_prev_snap; // g_m; frame + bytes, capped

bool SampleFrame(const SampleState& s, uint32_t f)
{
    const uint32_t age = f - s.first;
    return age == 0 || age == 1 || age == 30 || (age % kSummaryEvery) == 0;
}

std::map<int, RtDrawVb> g_draw_vb; // g_m

} // namespace

// ==== rt_hooks.h =================================================================================
bool RtModulePresent()
{
    return Mode() != 0;
}

bool RtHoldVanilla()
{
    return RtActive() && Mode() == 1;
}

bool RtClaimNote(const RtClaimInfo& in)
{
    if (!RtActive() || Mode() == 0) return false;
    g_c.claims.fetch_add(1, std::memory_order_relaxed);
    if (in.identified) g_c.claims_identified.fetch_add(1, std::memory_order_relaxed);
    const uint32_t width = in.index_bytes == 2 || in.index_bytes == 4 ? in.index_bytes : 0;
    if (width && in.vb0 && in.ib_va) {
        rtm::DrawNote d;
        d.mod = in.mod;
        d.comp = in.comp;
        d.identified = in.identified;
        const int64_t vbase = int64_t(in.vertex_offset) * int64_t(in.vb0_stride);
        d.vb0 = uint64_t(int64_t(in.vb0) + vbase);
        d.vb0_bytes = in.vb0_bytes;
        d.vb0_stride = in.vb0_stride;
        d.ib_at = in.ib_va + uint64_t(in.first_index) * width;
        d.index_count = in.index_count;
        std::optional<rtm::RingEvent> e;
        {
            std::lock_guard l(g_m);
            e = g_reg.NoteDraw(d, g_frame_id);
            SyncAtomics();
        }
        if (e && e->is_new) LogRing(*e, "draw");
    }
    MaybeSummary();
    // M1 never draws the mod while the game ray traces: the core's draw path would hand the mod's bind-pose
    // vertices to the game's pass-through VS (the skin cache already posed vb0).
    if (in.identified) g_c.claims_held.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RtOnBlasBuild(command_list*, acceleration_structure_type type, acceleration_structure_build_flags,
    uint32_t input_count, const acceleration_structure_build_input* inputs, resource_view dest,
    acceleration_structure_build_mode mode, unsigned long long)
{
    if (Mode() == 0) return false;
    g_last_blas_frame.store(g_frame_id, std::memory_order_relaxed);
    g_c.blas_builds.fetch_add(1, std::memory_order_relaxed);
    if (type != acceleration_structure_type::bottom_level || !inputs || !input_count) return false;
    g_c.blas_bottom.fetch_add(1, std::memory_order_relaxed);
    std::vector<rtm::BlasGeom> geoms;
    geoms.reserve(input_count);
    for (uint32_t i = 0; i < input_count && i < 256; ++i) {
        if (inputs[i].type != acceleration_structure_build_input_type::triangles) continue;
        const auto&   t = inputs[i].triangles;
        rtm::BlasGeom g;
        g.vb = BufferVa(t.vertex_buffer, t.vertex_offset);
        g.ib = (t.index_buffer.handle || t.index_offset) ? BufferVa(t.index_buffer, t.index_offset) : 0;
        g.vertex_count = t.vertex_count;
        g.vertex_stride = t.vertex_stride > UINT32_MAX ? 0 : uint32_t(t.vertex_stride);
        g.vertex_format = uint32_t(t.vertex_format);
        g.index_count = t.index_count;
        g.index_width = IndexWidth(t.index_format);
        g.geometry = i;
        g.geometry_count = input_count;
        g.dest = dest.handle;
        g.transformed = t.transform_buffer.handle != 0 || t.transform_offset != 0;
        geoms.push_back(g);
    }
    std::vector<rtm::RingEvent> ev;
    {
        std::lock_guard l(g_m);
        ev = g_reg.NoteBlas(geoms, g_frame_id);
        SyncAtomics();
    }
    for (const auto& e : ev)
        if (e.is_new) LogRing(e, mode == acceleration_structure_build_mode::update ? "blas-update" : "blas-build");
    MaybeSummary();
    return false; // M1: the game's own build always runs unchanged
}

void RtNoteSkinOut(uint64_t va, uint64_t bytes, int mod)
{
    std::lock_guard l(g_m);
    g_reg.NoteRing(va, bytes, mod, mod < 0, g_frame_id);
    SyncAtomics();
}
bool RtIsSkinOut(uint64_t va, uint64_t bytes)
{
    if (!g_ring_count.load(std::memory_order_relaxed)) return false;
    std::lock_guard l(g_m);
    return g_reg.IsSkinOut(va, bytes);
}
int RtSkinOutMod(uint64_t va, uint64_t bytes, int* overlap_mod)
{
    if (overlap_mod) *overlap_mod = -1;
    if (!g_ring_count.load(std::memory_order_relaxed)) return -1;
    std::lock_guard l(g_m);
    return g_reg.SkinOutMod(va, bytes, overlap_mod);
}
size_t RtSkinOutCount()
{
    return g_ring_count.load(std::memory_order_relaxed);
}
void RtForgetSkinOutRange(uint64_t va, uint64_t bytes)
{
    if (!g_ring_count.load(std::memory_order_relaxed) && !g_last_blas_frame.load(std::memory_order_relaxed)) return;
    std::lock_guard l(g_m);
    g_reg.Forget(va, bytes);
    SyncAtomics();
}
uint32_t RtSkinOutGen()
{
    return g_ring_gen.load(std::memory_order_relaxed);
}

void RtNoteDrawVb(int mod_id, int comp_idx, const RtDrawVb& v)
{
    std::lock_guard l(g_m);
    if (g_draw_vb.size() < 4096 || g_draw_vb.count(CompKey(mod_id, comp_idx))) g_draw_vb[CompKey(mod_id, comp_idx)] = v;
}
bool RtDrawVbOf(int mod_id, int comp_idx, RtDrawVb* out)
{
    std::lock_guard l(g_m);
    auto it = g_draw_vb.find(CompKey(mod_id, comp_idx));
    if (it == g_draw_vb.end()) return false;
    if (out) *out = it->second;
    return true;
}

// RTPAL (observe): a dispatch binding a ring; what else it binds, and what the UPLOAD ones hold.
void RtPalRecord(command_list* cmd_list, const CmdState& state, uint64_t cs_hash)
{
    if (!cmd_list || !g_ring_count.load(std::memory_order_relaxed) || Mode() == 0) return;
    const uint32_t gen = g_ring_gen.load(std::memory_order_relaxed);
    {
        std::lock_guard l(g_m);
        if (!g_judge.ShouldInspect(cs_hash, gen)) return;
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);

    struct Bound {
        uint32_t        kind; // 0 root CBV, 1 root SRV, 2 root UAV, 3 table slot
        uint32_t        param, slot;
        uint64_t        va;
        uint32_t        size;
        ID3D12Resource* res;
    };
    std::vector<Bound> bound;
    bound.reserve(64);
    for (uint32_t p = 0; p < 64; ++p) {
        const RootParam& rp = state.root[p];
        if (rp.va && (rp.desc_type == 2 || rp.desc_type == 3))
            bound.push_back({ rp.desc_type == 3 ? 2u : 1u, p, 0, rp.va, 0, rp.res });
    }
    {
        const CmdCbvBinds& cb = CmdCbv(cmd_list); // most recent root CBV per param (graphics + compute share it)
        for (uint32_t p = 0; p < 64; ++p)
            if (cb.binds[p].res && cb.binds[p].va) bound.push_back({ 0u, p, 0, cb.binds[p].va, cb.binds[p].size, cb.binds[p].res });
    }
    device* dev = cmd_list->get_device();
    for (uint32_t p = 0; p < 16; ++p) {
        const TableStart ts = TableStartOf(dev, state.root[p].compute_table());
        if (!ts) continue;
        for (uint32_t i = 0; i < 32; ++i) {
            ResInfo ri;
            if (!g_heap_to_res.find({ ts.heap, ts.base + i }, ri)) continue;
            if (ri.is_buffer && ri.cached_va) bound.push_back({ 3u, p, i, ri.cached_va, ri.size, ri.res });
        }
    }

    uint64_t ring_va = 0;
    int      ring_mod = -1;
    bool     sample = false;
    uint32_t section = 0;
    {
        std::lock_guard l(g_m);
        for (const Bound& b : bound)
            if (b.kind != 0)
                if (const rtm::Ring* r = g_reg.RingAt(b.va, b.size ? b.size : 1)) {
                    ring_va = r->va;
                    ring_mod = r->mod;
                    break;
                }
        g_judge.Verdict(cs_hash, ring_va != 0);
        if (ring_va) {
            SampleState& s = g_samples[{ cs_hash, ring_va }];
            if (!s.first) s.first = g_frame_id;
            if (s.frame != g_frame_id) {
                s.frame = g_frame_id;
                s.in_frame = 0;
            }
            section = s.in_frame++;
            sample = SampleFrame(s, g_frame_id) && section < 16 && g_samples.size() < 256;
            if (sample) ++s.records;
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    if (fq.QuadPart) g_c.inspect_us.fetch_add(uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart), std::memory_order_relaxed);
    if (!ring_va) return;
    g_c.ring_dispatches.fetch_add(1, std::memory_order_relaxed);
    if (!sample) return;
    g_c.pal_records.fetch_add(1, std::memory_order_relaxed);

    g_log.Line("RTPAL cs=%016llx ring=%llx mod=%d section=%u bound=%zu (snapshots: UPLOAD heap only, CPU-mapped at "
               "record time; root CBVs are the list's most recent per param)",
        (unsigned long long)cs_hash, (unsigned long long)ring_va, ring_mod, section, bound.size());
    static const char* kKind[] = { "cbv", "srv", "uav", "tbl" };
    std::vector<uint8_t> buf(kSnapBytes);
    for (const Bound& b : bound) {
        const bool is_ring = rtm::Overlaps(b.va, b.size ? b.size : 1, ring_va, 1);
        uint32_t   n = 0;
        // A table slot can outlive its resource (slots beyond what this shader reads), and a stale root bind could
        // too: only a buffer the resource tracker still knows as live is touched.
        const bool live = b.res && BufferLive(b.res);
        if (!is_ring && live) {
            const uint32_t want = b.size ? (b.size < kSnapBytes ? b.size : kSnapBytes) : kSnapBytes;
            n = SnapshotGameCB(b.res, b.va, buf.data(), want, true); // 0 = not an UPLOAD buffer (or unmappable)
        }
        g_c.snapshots.fetch_add(1, std::memory_order_relaxed);
        if (!n) {
            g_log.Line("  %s p%u s%u va=%llx size=%u%s", kKind[b.kind], b.param, b.slot, (unsigned long long)b.va, b.size,
                is_ring ? " <RING" : " (not CPU-readable: DEFAULT/other heap)");
            continue;
        }
        g_c.snapshot_upload.fetch_add(1, std::memory_order_relaxed);
        std::vector<uint8_t> snap(buf.begin(), buf.begin() + n);
        const rtm::BoneRun   run = rtm::BestBoneRun(snap.data(), snap.size());
        uint32_t             changed = UINT32_MAX, prev_frame = 0;
        {
            std::lock_guard l(g_m);
            auto&           prev = g_prev_snap[{ cs_hash, ring_va, b.kind, b.param, b.slot, section }];
            if (!prev.second.empty()) {
                changed = rtm::ChangedWords(prev.second, snap);
                prev_frame = prev.first;
            }
            if (g_prev_snap.size() <= 512) prev = { g_frame_id, snap };
        }
        if (run.count > g_c.best_bone_run.load()) {
            g_c.best_bone_run.store(run.count);
            g_c.best_bone_cs.store(cs_hash);
            g_c.best_bone_param.store(b.param);
        }
        char name[160];
        snprintf(name, sizeof name, "f%u_cs%016llx_r%llx_s%u_%s%u_%u.bin", g_frame_id, (unsigned long long)cs_hash,
            (unsigned long long)ring_va, section, kKind[b.kind], b.param, b.slot);
        const std::string written = g_log.Bin(name, snap.data(), snap.size());
        uint32_t          w[8] = {};
        memcpy(w, snap.data(), n < sizeof w ? n : sizeof w);
        char chg[48] = "first";
        if (changed != UINT32_MAX) snprintf(chg, sizeof chg, "%u/%u words vs f%u", changed, n / 4, prev_frame);
        g_log.Line("  %s p%u s%u va=%llx size=%u read=%u bone_run=%u@%u changed=%s head=%08x %08x %08x %08x %08x %08x %08x %08x %s",
            kKind[b.kind], b.param, b.slot, (unsigned long long)b.va, b.size, n, run.count, run.offset, chg, w[0], w[1], w[2], w[3],
            w[4], w[5], w[6], w[7], written.empty() ? "(no bin)" : written.c_str());
    }
    MaybeSummary();
}

// The core calls these only with its debug diagnostics on (dispatch_diag.cpp); M1 records through RtPalRecord.
bool RtBonesWindowOpen()
{
    return false;
}
void RtBonesLog(command_list*, const CmdState&, uint32_t groups_x, uint64_t cs_hash, bool learned)
{
    if (DiagOnce("RTBONES-M1", (int)(cs_hash ^ (cs_hash >> 32))))
        g_log.Line("RTBONES cs=%016llx groups_x=%u learned_skin_cs=%d (core diagnostic; bindings in RTPAL lines)",
            (unsigned long long)cs_hash, groups_x, learned ? 1 : 0);
}
void RtSkinReadDrain() {}
void RtSkinReadRecord(command_list*, const CmdState&, uint64_t, uint32_t) {}

// M2+: skinning the mod and publishing a palette. M1 holds every such draw (RtClaimNote), so these are not reached
// while the game ray traces; with RT off they must leave the draw alone.
VfRestore RtSkinDraw(command_list*, ID3D12GraphicsCommandList*, int, const WwmiMod::Component*, uint32_t, uint32_t, uint32_t,
    uint32_t, int32_t, uint64_t, uint64_t, int, bool)
{
    return {};
}
RtPalResult RtPalPublish(uint64_t, int, const WwmiMod::Component*, uint64_t)
{
    return {};
}

void RtDrawOverlay()
{
    const int m = Mode();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "RT module M1 (observe) -- rt_mode=%d (%s)", m,
        m ? "mods held back while the game ray traces" : "off: the add-on stops while the game ray traces");
    const uint32_t last = g_last_blas_frame.load();
    if (!last) {
        ImGui::TextDisabled("  ray tracing: no BLAS build seen yet");
        return;
    }
    ImGui::TextDisabled("  ray tracing: %s (last BLAS %u frames ago)   builds: %llu (bottom %llu)", RtActive() ? "ON" : "off",
        g_frame_id - last, (unsigned long long)g_c.blas_builds.load(), (unsigned long long)g_c.blas_bottom.load());
    ImGui::TextDisabled("  claims seen: %llu  identified: %llu  held vanilla: %llu", (unsigned long long)g_c.claims.load(),
        (unsigned long long)g_c.claims_identified.load(), (unsigned long long)g_c.claims_held.load());
    size_t pending_g = 0, pending_d = 0, skin_cs = 0;
    std::vector<rtm::Ring> rings;
    {
        std::lock_guard l(g_m);
        rings = g_reg.Rings();
        pending_g = g_reg.PendingGeoms();
        pending_d = g_reg.PendingDraws();
        skin_cs = g_judge.SkinShaders();
    }
    ImGui::TextDisabled("  rings: %zu  (BLAS geoms waiting %zu, draws waiting %zu)", rings.size(), pending_g, pending_d);
    for (const rtm::Ring& r : rings)
        ImGui::TextDisabled("    va=%llx %.2f MB mod=%d%s geoms=%u confirmed %u frames ago", (unsigned long long)r.va,
            double(r.bytes) / (1024.0 * 1024.0), r.mod, r.weak ? " (first+count only)" : "", r.geometries, g_frame_id - r.confirmed_frame);
    ImGui::TextDisabled("  compute shaders binding a ring: %zu  dispatches: %llu  sampled: %llu", skin_cs,
        (unsigned long long)g_c.ring_dispatches.load(), (unsigned long long)g_c.pal_records.load());
    ImGui::TextDisabled("  snapshots: %llu (UPLOAD %llu)  bins: %llu (%.1f MB)  best bone-like run: %u (cs %016llx p%u)",
        (unsigned long long)g_c.snapshots.load(), (unsigned long long)g_c.snapshot_upload.load(), (unsigned long long)g_c.bins.load(),
        double(g_c.bin_bytes.load()) / (1024.0 * 1024.0), g_c.best_bone_run.load(), (unsigned long long)g_c.best_bone_cs.load(),
        g_c.best_bone_param.load());
    ImGui::TextDisabled("  inspection cost so far: %.1f ms", double(g_c.inspect_us.load()) / 1000.0);
    const std::wstring dir = g_log.Dir();
    if (!dir.empty()) ImGui::TextDisabled("  log: %ls\\rt_observe.log", dir.c_str());
}
