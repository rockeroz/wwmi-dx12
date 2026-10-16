// rt_module.cpp -- the RT module, milestone M1: OBSERVE. Built instead of rt_stub.cpp with WwmiRT=observe.
//
// What M1 does while the game ray traces (a BLAS build or DispatchRays in the last kActiveFrames frames):
//   - keeps the core running instead of switching it off (RtModulePresent), but HOLDS every mod edit of the
//     game's frame (RtClaimNote / RtHoldVanilla): no mod geometry, no component hides, no draw / dispatch
//     overrides, no [Present] lists, no texture swaps (entering the hold gives every stamped slot back and
//     keeps it for retry when the hold ends, RtPresentTick). The game
//     renders vanilla; the mod is NOT drawn yet;
//   - learns each mod character's skin-cache output ring by pairing a BLAS triangle geometry with the
//     claimed raster draw that reads the same index range and vertex base (rt_registry.h);
//   - on observation frames, for compute dispatches that bind a ring, records what else is bound and
//     snapshots the UPLOAD-heap buffers among them (the core's CPU mapping, SnapshotGameCB, SEH-guarded),
//     scoring bone-likeness and change between observations;
//   - buffers its log and bins in memory and writes them from the present path:
//     <Win64>\wwmi_dx12\rt_observe\rt_observe.log and bins\*.bin; plus a section in the overlay.
// With RT off it holds nothing and the mods draw as always. ini [wwmi-dx12] rt_mode=0 restores the public
// build's behaviour (the add-on switches itself off while the game ray traces).
//
// It does NOT replace BLAS/TLAS, read DEFAULT-heap buffers, publish a palette or skin the mod: M2+.
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../mods/layout_registry.h" // TableStartOf
#include "../mods/skeleton.h"        // SnapshotGameCB
#include "../textures/texture_module.h" // TexHoldForRetry / TexRetryMissed
#include "rt_hooks.h"
#include "rt_registry.h"
#include <cstdarg>
#include <map>
#include <set>
#include <unordered_map>
#include <share.h>
#include <tuple>

using namespace reshade::api;

namespace {

constexpr int32_t  kActiveFrames = 120;         // RT counts as on this long after the last BLAS build / DispatchRays
constexpr uint32_t kSnapBytes = 16384;          // per binding snapshot
constexpr uint64_t kLogDetailCap = 12ull << 20; // RTPAL detail stops here...
constexpr uint64_t kLogCap = 16ull << 20;       // ...RTRING / RTSUM / hold lines continue to here
constexpr size_t   kPendingText = 1u << 20;     // log text buffered between presents
constexpr uint64_t kPendingBins = 2ull << 20;   // bin bytes buffered between presents (written at present)
constexpr uint64_t kBinCap = 64ull << 20;       // all bins together
constexpr uint32_t kBinFiles = 1500; // snapshots (shader dumps have their own budget)
constexpr uint32_t kSummaryEvery = 600; // frames
constexpr uint32_t kLinesPerDispatch = 40;
constexpr uint32_t kSectionsPerFrame = 16;

std::mutex            g_m; // registry, judge, sample state, draw-vb map
rtm::Registry         g_reg;
rtm::CsJudge          g_judge;
std::atomic<int>      g_mode{ -1 }; // -1 unread, 0 off, 1 observe
std::atomic<uint32_t> g_last_rt_frame{ 0 };
std::atomic<uint32_t> g_ring_count{ 0 };
std::atomic<uint32_t> g_ring_gen{ 0 };
std::atomic<size_t>   g_comps_n{ 0 };                  // loaded component windows (g_comps)
std::atomic<uint64_t> g_sig_rings{ 0 }, g_rt_draws{ 0 }; // rings learned from a BLAS signature; sampled draws

struct Counters {
    std::atomic<uint64_t> blas_builds{ 0 }, blas_bottom{ 0 }, rays{ 0 }, claims{ 0 }, claims_identified{ 0 }, claims_held{ 0 },
        ring_dispatches{ 0 }, pal_records{ 0 }, snapshots{ 0 }, snapshot_upload{ 0 }, snapshot_faults{ 0 }, bins{ 0 },
        bin_bytes{ 0 }, inspect_us{ 0 }, dropped_lines{ 0 }, dropped_bins{ 0 }, unstamped{ 0 }, skin_records{ 0 };
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
    const uint32_t last = g_last_rt_frame.load(std::memory_order_relaxed);
    // Signed: another thread may already stamp the next frame while this one still reads the previous id.
    return last && int32_t(g_frame_id - last) <= kActiveFrames;
}
void NoteRtFrame()
{
    g_last_rt_frame.store(g_frame_id ? g_frame_id : 1, std::memory_order_relaxed);
}
void SyncAtomics() // g_m held
{
    g_ring_count.store(uint32_t(g_reg.Count()), std::memory_order_relaxed);
    g_ring_gen.store(g_reg.Gen(), std::memory_order_relaxed);
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
// ReShade.log is written in every configuration (the core's own log only with its debug switch on): the
// module's must-know lines go there too, so a user can always tell whether the module is in the build.
void ReshadeLog(reshade::log::level lvl, _Printf_format_string_ const char* fmt, ...)
{
    char    buf[768];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    reshade::log::message(lvl, buf);
}
// Where the add-on keeps its data; <this add-on's folder>\wwmi_dx12 when the core has none (no wwmi_dir).
std::wstring DataDir()
{
    if (!g_data_dir.empty()) return g_data_dir;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&DataDir), &self))
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash + 1) + L"wwmi_dx12";
}

// ---- log + bins: buffered by the hooks, written by the present path ------------------------------
class ObserveLog {
public:
    // essential = RTRING / RTSUM / hold lines: also mirrored to the core's log, and kept after the detail cap.
    void Line(bool essential, _Printf_format_string_ const char* fmt, ...)
    {
        char    buf[1024];
        va_list a;
        va_start(a, fmt);
        int n = vsnprintf(buf, sizeof buf, fmt, a);
        va_end(a);
        if (n < 0) return;
        if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
        if (essential && logger::enabled(1)) logger::write("%s", buf);
        char      head[24];
        const int h = snprintf(head, sizeof head, "[f%u] ", g_frame_id);
        std::lock_guard l(m_);
        const uint64_t  cap = essential ? kLogCap : kLogDetailCap;
        if (written_ + text_.size() >= cap || text_.size() + size_t(h + n + 1) > kPendingText) {
            g_c.dropped_lines.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        text_.append(head, size_t(h));
        text_.append(buf, size_t(n));
        text_.push_back('\n');
    }
    // Queue a raw snapshot; returns the name it will be written under ("" = not queued).
    // priority = a shader dump: its own budget, never crowded out by snapshots.
    std::string Bin(const char* name, const uint8_t* data, size_t n, bool priority = false)
    {
        std::lock_guard l(m_);
        if (priority) {
            if (prio_files_ >= 3000 || prio_bytes_ + n > (96ull << 20)) {
                g_c.dropped_bins.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
            ++prio_files_;
            prio_bytes_ += n;
            bins_.emplace_back(name, std::vector<uint8_t>(data, data + n));
            return name;
        }
        if (g_c.bins.load() + bins_.size() >= kBinFiles || g_c.bin_bytes.load() + pending_bin_bytes_ + n > kBinCap ||
            pending_bin_bytes_ + n > kPendingBins) {
            g_c.dropped_bins.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        bins_.emplace_back(name, std::vector<uint8_t>(data, data + n));
        pending_bin_bytes_ += n;
        return name;
    }
    // Present path only.
    void Flush()
    {
        std::string                                               text;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> bins;
        {
            std::lock_guard l(m_);
            text.swap(text_);
            bins.swap(bins_);
            pending_bin_bytes_ = 0;
            written_ += text.size();
        }
        if (text.empty() && bins.empty()) return;
        if (!Open()) return;
        if (!text.empty()) {
            fwrite(text.data(), 1, text.size(), f_);
            fflush(f_);
        }
        if (!bins.empty()) {
            const std::wstring dir = dir_ + L"\\bins";
            CreateDirectoryW(dir.c_str(), nullptr);
            for (const auto& [name, data] : bins) {
                const std::wstring wname(name.begin(), name.end()); // ASCII names
                FILE*              f = _wfsopen((dir + L"\\" + wname).c_str(), L"wb", _SH_DENYWR);
                if (!f) continue;
                const size_t w = fwrite(data.data(), 1, data.size(), f);
                fclose(f);
                if (w != data.size()) continue;
                g_c.bins.fetch_add(1);
                g_c.bin_bytes.fetch_add(data.size());
            }
        }
    }
    std::wstring Dir() const { return dir_; } // present / overlay thread

private:
    bool Open() // present path only
    {
        if (f_) return true;
        if (tried_) return false;
        tried_ = true;
        const std::wstring data = DataDir();
        if (data.empty()) {
            ReshadeLog(reshade::log::level::warning, "wwmi-dx12 RT module: no data folder known -- rt_observe.log not written");
            return false;
        }
        CreateDirectoryW(data.c_str(), nullptr);
        dir_ = data + L"\\rt_observe";
        CreateDirectoryW(dir_.c_str(), nullptr);
        const std::wstring path = dir_ + L"\\rt_observe.log";
        MoveFileExW(path.c_str(), (path + L".prev").c_str(), MOVEFILE_REPLACE_EXISTING);
        f_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        if (!f_) {
            ReshadeLog(reshade::log::level::warning, "wwmi-dx12 RT module: cannot create %s (error %lu)", Utf8(path).c_str(),
                GetLastError());
            dir_.clear();
            return false;
        }
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module: writing %s", Utf8(path).c_str());
        fprintf(f_, "wwmi-dx12 %s (build %s) RT module M1 observe; rt_mode=%d\n", WWMI_VERSION, WWMI_BUILD_SHA, g_mode.load());
        fflush(f_);
        return true;
    }
    std::mutex                                                m_;
    std::string                                               text_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> bins_;
    uint64_t                                                  pending_bin_bytes_ = 0;
    uint32_t                                                  prio_files_ = 0;
    uint64_t                                                  prio_bytes_ = 0;
    uint64_t                                                  written_ = 0;
    FILE*                                                     f_ = nullptr;
    bool                                                      tried_ = false;
    std::wstring                                              dir_;
};
ObserveLog g_log;

void Summary()
{
    std::string rings;
    {
        std::lock_guard l(g_m);
        for (const rtm::Ring& r : g_reg.Rings()) {
            char b[160];
            snprintf(b, sizeof b, " [va=%llx bytes=%llu mod=%d%s geoms=%u seen=%uf]", (unsigned long long)r.va,
                (unsigned long long)r.bytes, r.mod, r.weak ? " weak" : "", r.geometries, g_frame_id - r.confirmed_frame);
            rings += b;
        }
    }
    g_log.Line(true,
        "RTSUM active=%d blas=%llu bottom=%llu rays=%llu claims=%llu identified=%llu held=%llu ring_dispatches=%llu "
        "pal_records=%llu snapshots=%llu upload=%llu faults=%llu bins=%llu dropped_lines=%llu dropped_bins=%llu "
        "unstamped=%llu best_bone_run=%u (cs=%016llx param=%u) inspect_us=%llu components=%zu sig_rings=%llu rt_draws=%llu skin_records=%llu rings=%u:%s",
        RtActive() ? 1 : 0, (unsigned long long)g_c.blas_builds.load(), (unsigned long long)g_c.blas_bottom.load(),
        (unsigned long long)g_c.rays.load(), (unsigned long long)g_c.claims.load(), (unsigned long long)g_c.claims_identified.load(),
        (unsigned long long)g_c.claims_held.load(), (unsigned long long)g_c.ring_dispatches.load(),
        (unsigned long long)g_c.pal_records.load(), (unsigned long long)g_c.snapshots.load(),
        (unsigned long long)g_c.snapshot_upload.load(), (unsigned long long)g_c.snapshot_faults.load(),
        (unsigned long long)g_c.bins.load(), (unsigned long long)g_c.dropped_lines.load(), (unsigned long long)g_c.dropped_bins.load(),
        (unsigned long long)g_c.unstamped.load(), g_c.best_bone_run.load(), (unsigned long long)g_c.best_bone_cs.load(),
        g_c.best_bone_param.load(), (unsigned long long)g_c.inspect_us.load(), g_comps_n.load(),
        (unsigned long long)g_sig_rings.load(), (unsigned long long)g_rt_draws.load(), (unsigned long long)g_c.skin_records.load(),
        g_ring_count.load(), rings.c_str());
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
    g_log.Line(true,
        "RTRING NEW via %s: va=%llx bytes=%llu mod=%d comp=%d%s | blas geom %u/%u verts=%u stride=%u fmt=%u ib=%llx count=%u "
        "width=%u dest=%llx | draw vb0=%llx stride=%u",
        via, (unsigned long long)e.ring.va, (unsigned long long)e.ring.bytes, e.ring.mod, e.draw.comp,
        e.ring.weak ? " (first+count only)" : "", e.geom.geometry, e.geom.geometry_count, e.geom.vertex_count, e.geom.vertex_stride,
        e.geom.vertex_format, (unsigned long long)e.geom.ib, e.geom.index_count, e.geom.index_width, (unsigned long long)e.geom.dest,
        (unsigned long long)e.draw.vb0, e.draw.vb0_stride);
}

// A buffer the game may destroy on another thread between the liveness check and the read: an access
// violation here must not take the game down. /EHa (the project's ExceptionHandling=Async) unwinds the core's
// locks inside SnapshotGameCB. MSVC refuses __try in a function with objects to unwind (C2712): none here.
uint32_t GuardedSnapshot(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t n)
{
#ifdef _MSC_VER
    __try {
        return SnapshotGameCB(res, va, dst, n, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_c.snapshot_faults.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
#else
    return SnapshotGameCB(res, va, dst, n, true);
#endif
}

// A descriptor-table slot: read through a private, immediately released mapping so a resource destroyed
// meanwhile can never leave a stale entry in the core's own mapping cache (SnapshotGameCB caches its maps).
uint32_t PrivateUploadRead(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t n)
{
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS      hf = {};
    if (FAILED(res->GetHeapProperties(&hp, &hf)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD) return 0;
    const uint64_t base = res->GetGPUVirtualAddress();
    const uint64_t width = res->GetDesc().Width;
    if (va < base || va - base >= width) return 0;
    const uint64_t off = va - base;
    const uint32_t m = (width - off) < n ? uint32_t(width - off) : n;
    D3D12_RANGE    none = { 0, 0 };
    void*          p = nullptr;
    if (FAILED(res->Map(0, &none, &p)) || !p) return 0;
    memcpy(dst, static_cast<const uint8_t*>(p) + off, m);
    res->Unmap(0, &none);
    return m;
}
uint32_t GuardedPrivateRead(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t n)
{
#ifdef _MSC_VER
    __try {
        return PrivateUploadRead(res, va, dst, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_c.snapshot_faults.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
#else
    return PrivateUploadRead(res, va, dst, n);
#endif
}

// ---- RTPAL sampling state (g_m) --------------------------------------------------------------------
struct SampleKey {
    uint64_t cs, ring;
    bool     operator<(const SampleKey& o) const { return cs != o.cs ? cs < o.cs : ring < o.ring; }
};
struct SampleState {
    uint32_t frame = 0, in_frame = 0, records = 0;
};
std::map<SampleKey, SampleState> g_samples;
constexpr size_t                 kMaxSampleKeys = 256;

struct SnapKey {
    uint64_t cs, ring;
    uint32_t kind, param, slot, section;
    bool     operator<(const SnapKey& o) const
    {
        return std::tie(cs, ring, kind, param, slot, section) < std::tie(o.cs, o.ring, o.kind, o.param, o.slot, o.section);
    }
};
std::map<SnapKey, std::pair<uint32_t, std::vector<uint8_t>>> g_prev_snap; // frame + bytes
constexpr size_t                                             kMaxPrevSnaps = 512;

std::map<int, RtDrawVb> g_draw_vb;

// Compute shader bytecode by the core's hash, copied at pipeline creation (any thread). g_shader_m.
std::mutex                                       g_shader_m;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cs_code;
uint64_t                                         g_cs_code_bytes = 0;
constexpr uint64_t                               kCsCodeCap = 64ull << 20;
std::set<uint64_t>                               g_cs_dumped; // g_shader_m
std::set<uint64_t>                               g_skin_cs;   // g_shader_m: entry point SkinCacheUpdateBatchCS (UE GPU skin cache)

bool IsSkinCs(uint64_t cs)
{
    std::lock_guard l(g_shader_m);
    return g_skin_cs.count(cs) != 0;
}

std::unordered_map<uint64_t, std::vector<uint8_t>> g_vs_code; // g_shader_m: vertex shaders, same budget

void DumpShader(uint64_t cs_hash, bool vertex = false) // any thread; once per hash
{
    std::vector<uint8_t> code;
    {
        std::lock_guard l(g_shader_m);
        if (!g_cs_dumped.insert(cs_hash ^ (vertex ? 0x5653000000000000ull : 0)).second) return;
        auto& store = vertex ? g_vs_code : g_cs_code;
        auto  it = store.find(cs_hash);
        if (it == store.end()) {
            g_log.Line(true, "RTSHADER %s=%016llx: bytecode not kept (created before the module saw it, or over budget)",
                vertex ? "vs" : "cs", (unsigned long long)cs_hash);
            return;
        }
        code = it->second;
    }
    char name[64];
    snprintf(name, sizeof name, "%s_%016llx.dxil", vertex ? "vs" : "cs", (unsigned long long)cs_hash);
    const std::string queued = g_log.Bin(name, code.data(), code.size(), true);
    g_log.Line(true, "RTSHADER %s=%016llx bytes=%zu -> %s", vertex ? "vs" : "cs", (unsigned long long)cs_hash, code.size(),
        queued.empty() ? "(not written: budget)" : queued.c_str());
}

// The loaded mods' component windows, copied on the present thread (where the core loads them) for the
// BLAS-signature match on other threads. g_m.
std::vector<rtm::CompWindow> g_comps;
std::set<std::tuple<uint64_t, uint32_t, uint32_t>> g_seen_draws; // (vb0, first, count) already logged
constexpr size_t                                   kMaxSeenDraws = 1024;

const rtm::CompWindow* CompAt(uint32_t first, uint32_t count) // g_m held
{
    for (const rtm::CompWindow& c : g_comps)
        if (c.first == first && c.count == count) return &c;
    return nullptr;
}

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

void RtNoteRays()
{
    if (Mode() == 0) return;
    NoteRtFrame();
    g_c.rays.fetch_add(1, std::memory_order_relaxed);
}

void RtPresentTick()
{
    static bool     s_held = false;
    static bool     s_started = false;
    static uint32_t s_last_summary = 0;
    if (!s_started) {
        s_started = true;
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module M1 (observe) is in this build; rt_mode=%d (%s)", Mode(),
            Mode() ? "mods held while the game ray traces" : "off: the add-on stops while the game ray traces");
        g_log.Line(true, "RTSTART module M1 observe, rt_mode=%d; waiting for ray tracing (BLAS builds / DispatchRays)", Mode());
    }
    {
        static size_t s_comps_seen = SIZE_MAX;
        static bool   s_loaded_seen = false;
        if (g_merged_comps.size() != s_comps_seen || g_merged_loaded != s_loaded_seen) {
            s_comps_seen = g_merged_comps.size();
            s_loaded_seen = g_merged_loaded;
            std::vector<rtm::CompWindow> comps;
            for (const MergedComp& c : g_merged_comps) comps.push_back({ c.mod_id, c.comp_idx, c.first_index, c.match_count });
            g_log.Line(true, "RTMODS merged_loaded=%d merged_enable=%d geomods=%zu components=%zu", g_merged_loaded ? 1 : 0,
                g_merged_enable ? 1 : 0, g_geomods.size(), comps.size());
            ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module: %zu mod(s), %zu component window(s) loaded", g_geomods.size(),
                comps.size());
            for (size_t gi = 0; gi < g_geomods.size() && gi < 16; ++gi) {
                const WwmiMod::Mod& mod = g_geomods[gi].mod;
                g_log.Line(true, "  geomod %zu ns=%s mesh_vertex_count=%u object_guid=%d comps=%zu resources=%zu", gi,
                    Utf8(g_geomods[gi].wcl_ns).c_str(), mod.mesh_vertex_count, mod.object_guid, mod.comps.size(), mod.resources.size());
                for (const WwmiMod::Component& wc : mod.comps) {
                    std::string draws;
                    for (const WwmiMod::SubDraw& d : wc.draws) {
                        char b[64];
                        snprintf(b, sizeof b, " %u@%u%s", d.count, d.start, d.cond.empty() ? "" : "?");
                        draws += b;
                    }
                    g_log.Line(true, "    comp %d %s match=%u+%u vg_offset=%u vg_count=%u remapped=%d remap_id=%d remap_vg=%u draws:%s", wc.index,
                        Utf8(wc.tail).c_str(), wc.match_first_index, wc.match_index_count, wc.vg_offset, wc.vg_count, wc.remapped ? 1 : 0,
                        wc.remap_id, wc.remap_vg, draws.c_str());
                }
                for (const WwmiMod::Resource& r : mod.resources)
                    g_log.Line(true, "    res %s stride=%u size=%u va=%llx file=%s", Utf8(r.name).c_str(), r.stride, r.size,
                        (unsigned long long)r.gpu_va, Utf8(r.filename).c_str());
            }
            for (size_t i = 0; i < g_merged_comps.size() && i < 96; ++i) {
                const MergedComp& c = g_merged_comps[i];
                const std::string ns = c.mod_id >= 0 && size_t(c.mod_id) < g_geomods.size() ? Utf8(g_geomods[c.mod_id].wcl_ns) : std::string();
                g_log.Line(true, "  mod=%d comp=%d first=%u count=%u remap_bones=%d hide=%d ns=%s", c.mod_id, c.comp_idx, c.first_index,
                    c.match_count, c.remap_size, c.hide ? 1 : 0, ns.c_str());
            }
            std::lock_guard l(g_m);
            g_comps.swap(comps);
            g_comps_n.store(g_comps.size(), std::memory_order_relaxed);
        }
    }
    const bool held = RtHoldVanilla();
    if (held && !s_held) {
        const UnstampTally t = UnstampAll(TexHoldForRetry); // as C1/C2 off does: the game's textures back, kept for retry
        g_c.unstamped.fetch_add(t.recorded, std::memory_order_relaxed);
        g_log.Line(true, "RTHOLD ON: the game ray traces -- mod held, %u stamped slot(s) given back (+%u in CPU-only heaps, %zu still stamped there)",
            t.recorded, t.cpu_only, t.cpu_only_stamped);
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module: the game ray traces -- mods held (vanilla), observing");
    } else if (!held && s_held) {
        TexRetryMissed("RT hold ended"); // what UnstampAll and the held RestampResource calls set aside
        g_log.Line(true, "RTHOLD OFF: no ray tracing for %d frames -- mods draw again, held texture swaps retried", kActiveFrames);
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module: ray tracing stopped -- mods draw again");
    }
    s_held = held;
    if (!s_last_summary || g_frame_id - s_last_summary >= kSummaryEvery) { // a heartbeat, ray tracing or not
        s_last_summary = g_frame_id;
        Summary();
    }
    g_log.Flush();
}

void RtObserveDraw(command_list* cmd_list, uint32_t index_count, uint32_t first_index, int32_t vertex_offset, uint64_t vs_hash)
{
    if (!cmd_list || !rtm::IsObservationFrame(g_frame_id) || !RtActive() || Mode() == 0) return;
    const CmdState& st = CmdGfx(cmd_list);
    const uint64_t  vb0 = st.vb[0].va;
    if (!vb0) return;
    const uint32_t stride = st.vb[0].stride;
    const uint64_t ib = st.ib.va;
    const uint32_t ib_w = st.ib.index_bytes;
    bool           fresh = false, ring = false;
    int            comp_mod = -1, comp_idx = -1;
    {
        std::lock_guard l(g_m);
        ring = g_reg.IsSkinOut(vb0, 1);
        if (!ring && !g_reg.HasGeomVb(vb0)) return;
        const auto key = std::make_tuple(vb0, first_index, index_count);
        if (!g_seen_draws.count(key) && g_seen_draws.size() < kMaxSeenDraws) {
            g_seen_draws.insert(key);
            fresh = true;
            if (const rtm::CompWindow* c = CompAt(first_index, index_count)) {
                comp_mod = c->mod;
                comp_idx = c->comp;
            }
        }
    }
    g_rt_draws.fetch_add(1, std::memory_order_relaxed);
    if (!fresh) return;
    g_log.Line(true, "RTDRAW vb0=%llx%s stride=%u ib=%llx width=%u first=%u count=%u vertex_offset=%d vs=%016llx -> %s mod=%d comp=%d",
        (unsigned long long)vb0, ring ? " (ring)" : " (BLAS vertex base)", stride, (unsigned long long)ib, ib_w, first_index, index_count,
        vertex_offset, (unsigned long long)vs_hash, comp_mod >= 0 ? "IS a component window" : "no component window", comp_mod, comp_idx);
}

// RTPSO: a held draw of a mod component, once per (VS, PSO, component): what the pass-through VS reads -- its input
// layout, every vertex buffer (ring = the skin cache's posed positions), root CBVs (UE's LocalVF / Primitive / View
// uniform buffers), root SRVs -- and its VS bytecode for offline disassembly.
void LogPassThroughDraw(const RtClaimInfo& in)
{
    static std::mutex                                       s_m;
    static std::set<std::tuple<uint64_t, uint64_t, int, int>> s_seen;
    const CmdState&                                         st = CmdGfx(in.cmd_list);
    const uint64_t                                          pso = st.gfx_pso;
    {
        std::lock_guard l(s_m);
        if (s_seen.size() >= 256 || !s_seen.insert({ in.vs_hash, pso, in.mod, in.comp }).second) return;
    }
    char layout[1024] = {};
    MigotoPort::DescribeInputLayout(pso, layout, sizeof layout);
    std::string vbs;
    for (unsigned i = 0; i < 8; ++i) {
        if (!st.vb[i].va) continue;
        bool ring = false;
        {
            std::lock_guard l(g_m);
            ring = g_reg.IsSkinOut(st.vb[i].va, 1);
        }
        char b[160];
        snprintf(b, sizeof b, " vb%u=%llx/%u/s%u%s", i, (unsigned long long)st.vb[i].va, st.vb[i].size, st.vb[i].stride, ring ? "(ring)" : "");
        vbs += b;
    }
    g_log.Line(true, "RTPSO mod=%d comp=%d first=%u count=%u vertex_offset=%d vs=%016llx pso=%llx ib=%llx w=%u layout=[%s]%s",
        in.mod, in.comp, in.first_index, in.index_count, in.vertex_offset, (unsigned long long)in.vs_hash, (unsigned long long)pso,
        (unsigned long long)st.ib.va, st.ib.index_bytes, layout, vbs.c_str());
    const CmdCbvBinds& cb = CmdCbv(in.cmd_list);
    std::string        roots;
    for (uint32_t p = 0; p < 64; ++p) {
        char b[96];
        if (cb.binds[p].va) {
            snprintf(b, sizeof b, " cbv%u=%llx/%u", p, (unsigned long long)cb.binds[p].va, cb.binds[p].size);
            roots += b;
        }
        if (st.root[p].va && (st.root[p].desc_type == 2 || st.root[p].desc_type == 3)) {
            snprintf(b, sizeof b, " %s%u=%llx", st.root[p].desc_type == 3 ? "uav" : "srv", p, (unsigned long long)st.root[p].va);
            roots += b;
        }
        if (st.root[p].table.handle) {
            snprintf(b, sizeof b, " tbl%u", p);
            roots += b;
        }
    }
    g_log.Line(true, "  roots:%s", roots.c_str());
    // small root CBVs in full (LocalVF / Primitive uniform buffers): 256 bytes each, priority bins
    std::vector<uint8_t> buf(256);
    for (uint32_t p = 0; p < 64; ++p) {
        const RootCbvBind& r = cb.binds[p];
        if (!r.res || !r.va || !BufferLive(r.res)) continue;
        const uint32_t n = GuardedSnapshot(r.res, r.va, buf.data(), 256);
        if (!n) continue;
        char name[128];
        snprintf(name, sizeof name, "pso_m%d_c%d_vs%016llx_cbv%u.bin", in.mod, in.comp, (unsigned long long)in.vs_hash, p);
        g_log.Bin(name, buf.data(), n, true);
    }
    DumpShader(in.vs_hash, true);
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
    if (in.cmd_list && in.identified) LogPassThroughDraw(in);
    // M1 never draws the mod while the game ray traces: the core would hand the mod's bind-pose vertices to the
    // game's pass-through VS (with RT on, the skin cache has already posed vb0).
    if (in.identified) g_c.claims_held.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RtOnBlasBuild(command_list*, acceleration_structure_type type, acceleration_structure_build_flags,
    uint32_t input_count, const acceleration_structure_build_input* inputs, resource_view dest,
    acceleration_structure_build_mode mode, unsigned long long)
{
    if (Mode() == 0) return false;
    NoteRtFrame();
    g_c.blas_builds.fetch_add(1, std::memory_order_relaxed);
    if (type != acceleration_structure_type::bottom_level || !inputs || !input_count) return false;
    g_c.blas_bottom.fetch_add(1, std::memory_order_relaxed);
    std::vector<rtm::BlasGeom> geoms;
    geoms.reserve(input_count < 256 ? input_count : 256);
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
    std::vector<rtm::RingEvent>    ev;
    std::vector<rtm::SignatureHit> fresh;
    {
        std::lock_guard l(g_m);
        ev = g_reg.NoteBlas(geoms, g_frame_id);
        if (!g_comps.empty())
            for (const rtm::SignatureHit& h : rtm::MatchSignatures(geoms, g_comps)) {
                const bool known = g_reg.RingAt(h.vb, 1) != nullptr;
                g_reg.NoteRing(h.vb, h.bytes, h.mod, false, g_frame_id); // learns or confirms
                if (!known) fresh.push_back(h);
            }
        SyncAtomics();
    }
    for (const auto& e : ev)
        if (e.is_new) LogRing(e, mode == acceleration_structure_build_mode::update ? "blas-update" : "blas-build");
    for (const rtm::SignatureHit& h : fresh) {
        g_sig_rings.fetch_add(1, std::memory_order_relaxed);
        g_log.Line(true, "RTRING NEW via blas-signature: va=%llx bytes=%llu mod=%d geometries=%u (of %u in the build) first index at "
                         "the lowest IB=%u dest=%llx",
            (unsigned long long)h.vb, (unsigned long long)h.bytes, h.mod, h.geometries, input_count, h.base_first,
            (unsigned long long)dest.handle);
    }
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
    if (!g_ring_count.load(std::memory_order_relaxed)) return; // every buffer destruction comes through here
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
    const int       key = CompKey(mod_id, comp_idx);
    if (g_draw_vb.size() < 4096 || g_draw_vb.count(key)) g_draw_vb[key] = v;
}
bool RtDrawVbOf(int mod_id, int comp_idx, RtDrawVb* out)
{
    std::lock_guard l(g_m);
    auto            it = g_draw_vb.find(CompKey(mod_id, comp_idx));
    if (it == g_draw_vb.end()) return false;
    if (out) *out = it->second;
    return true;
}

void RtNoteVertexShader(uint64_t vs_hash, const void* code, size_t code_size)
{
    if (!vs_hash || !code || !code_size || code_size > (4u << 20) || Mode() == 0) return;
    std::lock_guard l(g_shader_m);
    if (g_vs_code.count(vs_hash) || g_cs_code_bytes + code_size > kCsCodeCap) return;
    const auto* p = static_cast<const uint8_t*>(code);
    g_vs_code.emplace(vs_hash, std::vector<uint8_t>(p, p + code_size));
    g_cs_code_bytes += code_size;
}

void RtNoteShader(uint64_t cs_hash, const void* code, size_t code_size)
{
    if (!cs_hash || !code || !code_size || code_size > (4u << 20) || Mode() == 0) return;
    std::lock_guard l(g_shader_m);
    if (g_cs_code.count(cs_hash) || g_cs_code_bytes + code_size > kCsCodeCap) return;
    const auto* p = static_cast<const uint8_t*>(code);
    g_cs_code.emplace(cs_hash, std::vector<uint8_t>(p, p + code_size));
    g_cs_code_bytes += code_size;
    // The entry name is stored as plain text in the container's PSV0 string table.
    static const char kSkin[] = "SkinCacheUpdateBatchCS";
    if (std::search(p, p + code_size, kSkin, kSkin + sizeof kSkin - 1) != p + code_size) g_skin_cs.insert(cs_hash);
}

// RTPAL (observe): a dispatch binding a ring; what else it binds, and what the UPLOAD ones hold.
void RtPalRecord(command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x, uint32_t groups_y,
    uint32_t groups_z)
{
    if (!cmd_list || !g_ring_count.load(std::memory_order_relaxed) || Mode() == 0) return;
    const uint32_t frame = g_frame_id;
    const uint32_t gen = g_ring_gen.load(std::memory_order_relaxed);
    {
        std::lock_guard l(g_m);
        if (!g_judge.ShouldInspect(cs_hash, gen, frame)) return;
    }
    const bool is_skin = IsSkinCs(cs_hash); // g_shader_m, never under g_m
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);

    struct Bound {
        uint32_t        kind; // 0 root CBV, 1 root SRV, 2 root UAV, 3 table slot
        uint32_t        param, slot;
        uint64_t        va;
        uint32_t        size;
        ID3D12Resource* res;
        uint32_t        age = 0;  // table slots: frames since the descriptor was written
        uint64_t        view = 0; // table slots: the descriptor's view handle
    };
    std::vector<Bound> bound;
    bound.reserve(96);
    for (uint32_t p = 0; p < 64; ++p) {
        const RootParam& rp = state.root[p];
        if (rp.va && (rp.desc_type == 2 || rp.desc_type == 3)) bound.push_back({ rp.desc_type == 3 ? 2u : 1u, p, 0, rp.va, 0, rp.res });
    }
    {
        const CmdCbvBinds& cb = CmdCbv(cmd_list); // most recent root CBV per param this recording (graphics + compute share it)
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
            if (ri.is_buffer && ri.cached_va) bound.push_back({ 3u, p, i, ri.cached_va, ri.size, ri.res, frame - ri.wrote_frame, ri.view_handle });
        }
    }

    uint64_t        ring_va = 0, ring_bytes = 0;
    int             ring_mod = -1;
    bool            sample = false, first_hit = false;
    uint32_t        section = 0;
    const Bound*    ring_bind = nullptr;
    {
        std::lock_guard l(g_m);
        for (const Bound& b : bound)
            if (b.kind != 0)
                if (const rtm::Ring* r = g_reg.RingAt(b.va, b.size ? b.size : 1)) {
                    ring_va = r->va;
                    ring_bytes = r->bytes;
                    ring_mod = r->mod;
                    ring_bind = &b;
                    break;
                }
        const uint32_t hits_before = g_judge.Hits(cs_hash);
        g_judge.Verdict(cs_hash, ring_va != 0);
        first_hit = ring_va && hits_before == 0;
        if (ring_va) {
            auto it = g_samples.find({ cs_hash, ring_va });
            if (it == g_samples.end() && g_samples.size() < kMaxSampleKeys)
                it = g_samples.emplace(SampleKey{ cs_hash, ring_va }, SampleState{}).first;
            if (it != g_samples.end()) {
                SampleState& s = it->second;
                if (s.frame != frame) {
                    s.frame = frame;
                    s.in_frame = 0;
                }
                section = s.in_frame++;
                // Skin cache: every section on observation frames. Anything else binding a ring: two records only.
                sample = is_skin ? section < 48u : (section < kSectionsPerFrame && s.records < 2);
                if (sample && s.in_frame == 1) ++s.records;
            }
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    if (fq.QuadPart) g_c.inspect_us.fetch_add(uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart), std::memory_order_relaxed);
    if (!ring_va) return;
    if (first_hit) DumpShader(cs_hash);
    g_c.ring_dispatches.fetch_add(1, std::memory_order_relaxed);
    // Does this dispatch WRITE the ring? Evidence: the ring's resource was transitioned to UNORDERED_ACCESS on
    // this list (this frame or the previous one), and/or the core's last tracked state for it includes UAV.
    bool barrier_uav = false, state_uav = false;
    if (ring_bind && ring_bind->res) {
        const uint64_t rh = reinterpret_cast<uint64_t>(ring_bind->res);
        if (const CmdBlock* blk = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list)))
            for (const UavNote& n : blk->uav_recent)
                if (n.res == rh && n.frame + 1 >= g_frame_id) barrier_uav = true;
        const uint32_t st = ResourceStateOf(ring_bind->res);
        state_uav = st != MigotoPort::kResStateUnknown && (st & uint32_t(resource_usage::unordered_access)) != 0;
    }
    if (sample && is_skin) {
        // RTSKIN: UE's GPU skin cache, one dispatch per mesh section. Every table slot with its VIEW range (the
        // bone matrices are a typed view into a shared per-frame upload buffer: the view offset IS the section's
        // palette), the UPLOAD ones read at that range; root CBVs read in full up to 256 bytes ($Globals carries the
        // section's start/count/offsets). All bins go to the priority budget.
        g_c.skin_records.fetch_add(1, std::memory_order_relaxed);
        g_log.Line(false, "RTSKIN cs=%016llx ring=%llx mod=%d section=%u groups=%u (x64 threads = %u vertices max) ring_at=%s%u.%u",
            (unsigned long long)cs_hash, (unsigned long long)ring_va, ring_mod, section, groups_x, groups_x * 64u,
            ring_bind ? (ring_bind->kind == 3 ? "tbl" : "root") : "?", ring_bind ? ring_bind->param : 0u, ring_bind ? ring_bind->slot : 0u);
        device* dv = cmd_list->get_device();
        std::vector<uint8_t> sbuf(kSnapBytes);
        for (const Bound& b : bound) {
            const bool is_ring = rtm::Overlaps(b.va, b.size ? b.size : 1, ring_va, ring_bytes);
            if (b.kind == 3) {
                if ((b.age > 2 || b.slot >= 16) && !is_ring) continue; // stale or far slot of a bindless table
                const resource_view_desc vd = b.view ? dv->get_resource_view_desc(resource_view{ b.view }) : resource_view_desc{};
                const uint32_t           elem = vd.format != format::unknown ? format_row_pitch(vd.format, 1) : 0u;
                const bool               structured = vd.type == resource_view_type::buffer && vd.format == format::unknown && vd.buffer.structured.stride;
                // ReShade's pinned D3D12 conversion keeps FirstElement / NumElements for typed and raw views.
                uint64_t off = 0, bytes = 0;
                if (structured) {
                    off = vd.buffer.offset * vd.buffer.structured.stride;
                    bytes = uint64_t(vd.buffer.structured.count) * vd.buffer.structured.stride;
                } else if (elem) {
                    off = vd.buffer.offset * elem;
                    bytes = vd.buffer.size == UINT64_MAX ? 0 : vd.buffer.size * elem;
                }
                std::string queued;
                uint32_t    n = 0;
                rtm::BoneRun run;
                if (!is_ring && b.res && BufferLive(b.res) && off < b.size) {
                    const uint32_t want = uint32_t((std::min)(bytes ? bytes : uint64_t(kSnapBytes), uint64_t(kSnapBytes)));
                    n = GuardedPrivateRead(b.res, b.va + off, sbuf.data(), want);
                    if (n) {
                        run = rtm::BestBoneRun(sbuf.data(), n);
                        char name[160];
                        snprintf(name, sizeof name, "skin_f%u_cs%016llx_r%llx_s%u_tbl%u_%u.bin", frame, (unsigned long long)cs_hash,
                            (unsigned long long)ring_va, section, b.param, b.slot);
                        queued = g_log.Bin(name, sbuf.data(), n, true);
                    }
                }
                g_log.Line(false, "  tbl p%u s%u va=%llx res_bytes=%u view type=%u fmt=%u raw_off=%llu raw_size=%llu elem=%u -> off=%llu bytes=%llu%s read=%u bone_run=%u@%u %s",
                    b.param, b.slot, (unsigned long long)b.va, b.size, uint32_t(vd.type), uint32_t(vd.format), (unsigned long long)vd.buffer.offset,
                    (unsigned long long)vd.buffer.size, elem, (unsigned long long)off, (unsigned long long)bytes, is_ring ? " <RING" : "", n,
                    run.count, run.offset, queued.empty() ? "" : queued.c_str());
            } else if (b.kind == 0 && b.res && BufferLive(b.res)) {
                const uint32_t want = b.size && b.size < 256 ? b.size : 256;
                const uint32_t n = GuardedSnapshot(b.res, b.va, sbuf.data(), want);
                if (!n) continue;
                uint32_t w[8] = {};
                memcpy(w, sbuf.data(), n < sizeof w ? n : sizeof w);
                char name[160];
                snprintf(name, sizeof name, "skin_f%u_cs%016llx_r%llx_s%u_cbv%u.bin", frame, (unsigned long long)cs_hash,
                    (unsigned long long)ring_va, section, b.param);
                const std::string queued = g_log.Bin(name, sbuf.data(), n, true);
                g_log.Line(false, "  cbv p%u va=%llx size=%u read=%u words=%08x %08x %08x %08x %08x %08x %08x %08x %s", b.param,
                    (unsigned long long)b.va, b.size, n, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], queued.c_str());
            } else if (b.kind != 0)
                g_log.Line(false, "  %s p%u va=%llx%s", b.kind == 2 ? "uav" : "srv", b.param, (unsigned long long)b.va, is_ring ? " <RING" : "");
        }
        return;
    }
    if (!sample) return;
    g_c.pal_records.fetch_add(1, std::memory_order_relaxed);

    // Snapshot first, then log the most telling lines: the ring itself, then readable buffers, then the rest.
    struct Seen {
        const Bound*         b;
        bool                 is_ring;
        std::vector<uint8_t> bytes;
    };
    std::vector<Seen>    seen;
    std::vector<uint8_t> buf(kSnapBytes);
    seen.reserve(bound.size());
    for (const Bound& b : bound) {
        Seen s{ &b, rtm::Overlaps(b.va, b.size ? b.size : 1, ring_va, ring_bytes), {} };
        // A table slot can outlive its resource (slots beyond what this shader reads), and so could a stale bind:
        // only a buffer the resource tracker still knows as live is touched, every read is SEH-guarded, and a
        // table slot is read only if its descriptor was written within the last two frames, through a private map.
        if (!s.is_ring && b.res && BufferLive(b.res) && (b.kind != 3 || b.age <= 2)) {
            const uint32_t want = b.size ? (b.size < kSnapBytes ? b.size : kSnapBytes) : kSnapBytes;
            const uint32_t n = b.kind == 3 ? GuardedPrivateRead(b.res, b.va, buf.data(), want)
                                           : GuardedSnapshot(b.res, b.va, buf.data(), want); // 0 = not UPLOAD / unmappable
            if (n) s.bytes.assign(buf.begin(), buf.begin() + n);
        }
        g_c.snapshots.fetch_add(1, std::memory_order_relaxed);
        if (!s.bytes.empty()) g_c.snapshot_upload.fetch_add(1, std::memory_order_relaxed);
        seen.push_back(std::move(s));
    }
    std::stable_sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) {
        const int ra = a.is_ring ? 0 : !a.bytes.empty() ? 1 : 2, rb = b.is_ring ? 0 : !b.bytes.empty() ? 1 : 2;
        return ra < rb;
    });

    g_log.Line(false,
        "RTPAL cs=%016llx ring=%llx mod=%d section=%u groups=%u,%u,%u ring_at=%s%u.%u ring_uav_barrier=%d ring_state_uav=%d bound=%zu "
        "readable=%llu (UPLOAD heap only, CPU-mapped at record time; root CBVs = this recording's most recent per param; section "
        "= dispatch order within the frame)",
        (unsigned long long)cs_hash, (unsigned long long)ring_va, ring_mod, section, groups_x, groups_y, groups_z,
        ring_bind ? (ring_bind->kind == 3 ? "tbl" : ring_bind->kind == 2 ? "uav" : "srv") : "?", ring_bind ? ring_bind->param : 0u,
        ring_bind ? ring_bind->slot : 0u, barrier_uav ? 1 : 0, state_uav ? 1 : 0, seen.size(),
        (unsigned long long)std::count_if(seen.begin(), seen.end(), [](const Seen& s) { return !s.bytes.empty(); }));
    static const char* kKind[] = { "cbv", "srv", "uav", "tbl" };
    uint32_t           lines = 0, unlisted = 0;
    for (const Seen& s : seen) {
        const Bound& b = *s.b;
        if (lines >= kLinesPerDispatch) {
            ++unlisted;
            continue;
        }
        ++lines;
        if (s.bytes.empty()) {
            if (b.kind == 3 && !s.is_ring && b.age > 2)
                g_log.Line(false, "  tbl p%u s%u va=%llx size=%u (descriptor written %u frames ago: not read)", b.param, b.slot,
                    (unsigned long long)b.va, b.size, b.age);
            else
                g_log.Line(false, "  %s p%u s%u va=%llx size=%u%s", kKind[b.kind], b.param, b.slot, (unsigned long long)b.va, b.size,
                    s.is_ring ? " <RING" : " (not CPU-readable: DEFAULT/other heap, or not live)");
            continue;
        }
        const rtm::BoneRun run = rtm::BestBoneRun(s.bytes.data(), s.bytes.size());
        uint32_t           changed = UINT32_MAX, prev_frame = 0;
        {
            std::lock_guard l(g_m);
            const SnapKey   key{ cs_hash, ring_va, b.kind, b.param, b.slot, section };
            auto            it = g_prev_snap.find(key);
            if (it != g_prev_snap.end()) {
                changed = rtm::ChangedWords(it->second.second, s.bytes);
                prev_frame = it->second.first;
                it->second = { frame, s.bytes };
            } else if (g_prev_snap.size() < kMaxPrevSnaps)
                g_prev_snap.emplace(key, std::make_pair(frame, s.bytes));
        }
        if (run.count > g_c.best_bone_run.load()) {
            g_c.best_bone_run.store(run.count);
            g_c.best_bone_cs.store(cs_hash);
            g_c.best_bone_param.store(b.param);
        }
        char name[160];
        snprintf(name, sizeof name, "f%u_cs%016llx_r%llx_s%u_%s%u_%u.bin", frame, (unsigned long long)cs_hash,
            (unsigned long long)ring_va, section, kKind[b.kind], b.param, b.slot);
        const std::string queued = g_log.Bin(name, s.bytes.data(), s.bytes.size());
        uint32_t          w[8] = {};
        memcpy(w, s.bytes.data(), s.bytes.size() < sizeof w ? s.bytes.size() : sizeof w);
        char chg[48] = "first";
        if (changed != UINT32_MAX) snprintf(chg, sizeof chg, "%u/%zu words vs f%u", changed, s.bytes.size() / 4, prev_frame);
        g_log.Line(false, "  %s p%u s%u va=%llx size=%u read=%zu bone_run=%u@%u changed=%s head=%08x %08x %08x %08x %08x %08x %08x %08x %s",
            kKind[b.kind], b.param, b.slot, (unsigned long long)b.va, b.size, s.bytes.size(), run.count, run.offset, chg, w[0], w[1],
            w[2], w[3], w[4], w[5], w[6], w[7], queued.empty() ? "(no bin)" : queued.c_str());
    }
    if (unlisted) g_log.Line(false, "  ... %u more binding(s) not listed (no CPU-readable content)", unlisted);
}

// The core calls these only with its debug diagnostics on (dispatch_diag.cpp); M1 records through RtPalRecord.
bool RtBonesWindowOpen()
{
    return false;
}
void RtBonesLog(command_list*, const CmdState&, uint32_t groups_x, uint64_t cs_hash, bool learned)
{
    if (DiagOnce("RTBONES-M1", (int)(cs_hash ^ (cs_hash >> 32))))
        g_log.Line(false, "RTBONES cs=%016llx groups_x=%u learned_skin_cs=%d (core diagnostic; bindings in RTPAL lines)",
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
    const uint32_t last = g_last_rt_frame.load();
    if (!last) {
        ImGui::TextDisabled("  ray tracing: not seen yet (no BLAS build or DispatchRays)");
        const std::wstring d = g_log.Dir();
        ImGui::TextDisabled("  log: %ls", d.empty() ? L"not open (see ReShade.log)" : (d + L"\\rt_observe.log").c_str());
        return;
    }
    ImGui::TextDisabled("  ray tracing: %s (last seen %d frames ago)   BLAS builds: %llu (bottom %llu)  DispatchRays: %llu",
        RtActive() ? "ON -- mods held" : "off", int32_t(g_frame_id - last), (unsigned long long)g_c.blas_builds.load(),
        (unsigned long long)g_c.blas_bottom.load(), (unsigned long long)g_c.rays.load());
    ImGui::TextDisabled("  claims seen: %llu  identified: %llu  held vanilla: %llu  texture slots given back: %llu",
        (unsigned long long)g_c.claims.load(), (unsigned long long)g_c.claims_identified.load(),
        (unsigned long long)g_c.claims_held.load(), (unsigned long long)g_c.unstamped.load());
    size_t                 pending_g = 0, pending_d = 0, skin_cs = 0;
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
    ImGui::TextDisabled("  snapshots: %llu (UPLOAD %llu, faults %llu)  bins: %llu (%.1f MB)  best bone-like run: %u (cs %016llx p%u)",
        (unsigned long long)g_c.snapshots.load(), (unsigned long long)g_c.snapshot_upload.load(),
        (unsigned long long)g_c.snapshot_faults.load(), (unsigned long long)g_c.bins.load(),
        double(g_c.bin_bytes.load()) / (1024.0 * 1024.0), g_c.best_bone_run.load(), (unsigned long long)g_c.best_bone_cs.load(),
        g_c.best_bone_param.load());
    ImGui::TextDisabled("  inspection cost so far: %.1f ms   log lines dropped: %llu  bins dropped: %llu",
        double(g_c.inspect_us.load()) / 1000.0, (unsigned long long)g_c.dropped_lines.load(), (unsigned long long)g_c.dropped_bins.load());
    const std::wstring dir = g_log.Dir();
    if (!dir.empty()) ImGui::TextDisabled("  log: %ls\\rt_observe.log", dir.c_str());
    else ImGui::TextDisabled("  log: not open (see ReShade.log)");
}
