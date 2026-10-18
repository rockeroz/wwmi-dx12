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
//
// M2 (rt_mode=2, the default of this build; rt_skin.cpp): instead of holding, a claimed draw that reads a skin-cache
// ring is drawn with the mod -- its vertices posed by our own compute pass with the palette the game's skin cache
// used for that section this frame, through the game's pass-through VS. A claim that cannot be drawn that way
// (mesh not loaded yet, no palette for the section, a non-ring draw) stays the game's own, as in M1.
// rt_mode=1 is M1 (observe and hold), rt_mode=0 the public build's behaviour.
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../instrumentation/removal_pin.h"
#include "../../src/d3d12_buffer.h"
#include "../mods/layout_registry.h" // TableStartOf
#include "../mods/skeleton.h"        // SnapshotGameCB
#include "../textures/texture_module.h" // TexHoldForRetry / TexRetryMissed
#include "rt_hooks.h"
#include "rt_registry.h"
#include "rt_skin.h"
#include <cstdarg>
#include <map>
#include <set>
#include <unordered_map>
#include <share.h>
#include <tuple>
#include <wrl/client.h>
#include <intrin.h>

using namespace reshade::api;
using Microsoft::WRL::ComPtr;

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
std::atomic<int>      g_mode{ -1 }; // -1 unread, 0 off, 1 observe (hold), 2 draw (M2)
std::atomic<uint32_t> g_last_rt_frame{ 0 };
std::atomic<uint32_t> g_ring_count{ 0 };
std::atomic<uint32_t> g_ring_gen{ 0 };
std::atomic<size_t>   g_comps_n{ 0 };                  // loaded component windows (g_comps)
std::atomic<uint64_t> g_sig_rings{ 0 }, g_rt_draws{ 0 }; // rings learned from a BLAS signature; sampled draws

struct Counters {
    std::atomic<uint64_t> blas_builds{ 0 }, blas_bottom{ 0 }, rays{ 0 }, claims{ 0 }, claims_identified{ 0 }, claims_held{ 0 },
        ring_dispatches{ 0 }, pal_records{ 0 }, snapshots{ 0 }, snapshot_upload{ 0 }, snapshot_faults{ 0 }, bins{ 0 },
        bin_bytes{ 0 }, inspect_us{ 0 }, dropped_lines{ 0 }, dropped_bins{ 0 }, unstamped{ 0 }, skin_records{ 0 }, claims_drawn{ 0 },
        sections{ 0 }, section_misses{ 0 }, m2_drawn{ 0 }, m2_not_drawn{ 0 };
    std::atomic<uint32_t> best_bone_run{ 0 };
    std::atomic<uint64_t> best_bone_cs{ 0 };
    std::atomic<uint32_t> best_bone_param{ 0 };
} g_c;

int Mode()
{
    int m = g_mode.load(std::memory_order_relaxed);
    if (m >= 0) return m;
    if (g_addon_ini_path.empty()) return 2; // not read yet: the default, re-read later
    const int v = int(GetPrivateProfileIntW(L"wwmi-dx12", L"rt_mode", 2, g_addon_ini_path.c_str()));
    m = v <= 0 ? 0 : v >= 2 ? 2 : 1;
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
        fprintf(f_, "wwmi-dx12 %s (build %s) RT module M2; rt_mode=%d\n", WWMI_VERSION, WWMI_BUILD_SHA, g_mode.load());
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
std::mutex                      g_held_m;
std::map<std::string, uint64_t> g_held_why; // g_held_m: M2 holds by reason (RTSUM)

const char* ModeText(int m)
{
    return m == 2 ? "M2: mods drawn with the game's skin cache palettes while the game ray traces"
         : m == 1 ? "M1: mods held (vanilla) while the game ray traces, observing"
                  : "off: the add-on stops while the game ray traces";
}

void M3Summary();

void Summary()
{
    std::string m2 = Mode() == 2 ? rtskin::Status() : std::string("m2 off");
    {
        std::lock_guard l(g_held_m);
        for (const auto& [why, n] : g_held_why) m2 += " | held " + std::to_string(n) + ": " + why;
    }
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
        "unstamped=%llu best_bone_run=%u (cs=%016llx param=%u) inspect_us=%llu components=%zu sig_rings=%llu rt_draws=%llu skin_records=%llu "
        "claims_drawn=%llu sections=%llu section_misses=%llu m2_drawn=%llu m2_not_drawn=%llu | %s | rings=%u:%s",
        RtActive() ? 1 : 0, (unsigned long long)g_c.blas_builds.load(), (unsigned long long)g_c.blas_bottom.load(),
        (unsigned long long)g_c.rays.load(), (unsigned long long)g_c.claims.load(), (unsigned long long)g_c.claims_identified.load(),
        (unsigned long long)g_c.claims_held.load(), (unsigned long long)g_c.ring_dispatches.load(),
        (unsigned long long)g_c.pal_records.load(), (unsigned long long)g_c.snapshots.load(),
        (unsigned long long)g_c.snapshot_upload.load(), (unsigned long long)g_c.snapshot_faults.load(),
        (unsigned long long)g_c.bins.load(), (unsigned long long)g_c.dropped_lines.load(), (unsigned long long)g_c.dropped_bins.load(),
        (unsigned long long)g_c.unstamped.load(), g_c.best_bone_run.load(), (unsigned long long)g_c.best_bone_cs.load(),
        g_c.best_bone_param.load(), (unsigned long long)g_c.inspect_us.load(), g_comps_n.load(),
        (unsigned long long)g_sig_rings.load(), (unsigned long long)g_rt_draws.load(), (unsigned long long)g_c.skin_records.load(),
        (unsigned long long)g_c.claims_drawn.load(), (unsigned long long)g_c.sections.load(), (unsigned long long)g_c.section_misses.load(),
        (unsigned long long)g_c.m2_drawn.load(), (unsigned long long)g_c.m2_not_drawn.load(), m2.c_str(), g_ring_count.load(), rings.c_str());
    if (Mode() == 2) {
        for (const std::string& line : rtskin::PoseLines()) g_log.Line(true, "%s", line.c_str());
        M3Summary(); // its own lines (RTSUM is cut at 1024 characters)
    }
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

std::atomic<bool> g_palfmt_told{ false }, g_globals_told{ false };

// M2: why a claim stayed the game's (one line per mod, component and reason; counted in RTSUM's held).
void NoteHeld(int mod, int comp, const char* why)
{
    static std::set<std::tuple<int, int, const char*>> s_seen;
    {
        std::lock_guard l(g_held_m);
        ++g_held_why[why];
        if (s_seen.size() >= 512 || !s_seen.insert({ mod, comp, why }).second) return;
    }
    g_log.Line(true, "RTM2 held mod=%d comp=%d (drawn by the game, vanilla): %s", mod, comp, why);
}

// M2: every skin cache dispatch of a ring, every frame -- its bone palette (t0: the view's window into the frame's
// upload buffer) and its tangent output (u0), in dispatch order = section order (rtskin::NoteSection).
void NoteSkinSection(command_list* cmd_list, const CmdState& state, uint64_t cs_hash)
{
    thread_local uint64_t t_cs = 0;
    thread_local bool     t_skin = false;
    if (cs_hash != t_cs) {
        t_cs = cs_hash;
        t_skin = IsSkinCs(cs_hash);
    }
    if (!t_skin) return;
    // as observed, when the root signature has no map: tables p0 (SRVs) / p2 (UAVs), $Globals root CBV p3
    RegBind t0{ 0, 0, true }, u0{ 2, 0, true }, u1{ 2, 1, true }, b0{ 3, 0, false };
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(CmdCmpLayout(cmd_list));
        if (it != g_layout_regmap.end() && it->second.built && it->second.t_cs[0].param != 0xFF && it->second.u_cs[1].param != 0xFF) {
            t0 = it->second.t_cs[0];
            u0 = it->second.u_cs[0];
            u1 = it->second.u_cs[1];
            if (it->second.b_cs[0].param != 0xFF) b0 = it->second.b_cs[0];
        }
    }
    device* dev = cmd_list->get_device();
    auto    resolve = [&](const RegBind& b, ResInfo& ri) -> bool {
        if (b.param == 0xFF || b.param >= 64) return false;
        if (!b.is_table) {
            ri = ResInfo{};
            ri.cached_va = state.root[b.param].va;
            ri.res = state.root[b.param].res;
            return ri.cached_va != 0;
        }
        const TableStart ts = TableStartOf(dev, state.root[b.param].compute_table());
        return ts && g_heap_to_res.find({ ts.heap, ts.base + b.offset }, ri) && ri.cached_va;
    };
    ResInfo rring{}, rpal{}, rtan{};
    if (!resolve(u1, rring)) return;
    const uint64_t ring = rring.cached_va; // the buffer the draws bind at VB0
    uint32_t       expected = 0;
    {
        std::lock_guard l(g_m);
        if (const rtm::Ring* r = g_reg.RingAt(ring, rring.size ? rring.size : 1)) expected = r->geometries;
    }
    // $Globals word 1 = SkinCacheStart (the section's first vertex in the ring) in every variant seen (ef8d.., e031..,
    // 8210..)
    uint32_t start = UINT32_MAX;
    if (!b0.is_table && b0.param < 64) {
        const RootCbvBind rc = CmdCbv(cmd_list).binds[b0.param];
        uint32_t          w[2] = {};
        if (rc.res && rc.va && BufferLive(rc.res) && SnapshotGameCB(rc.res, rc.va, reinterpret_cast<uint8_t*>(w), sizeof w, true) == sizeof w)
            start = w[1];
    }
    if (start == UINT32_MAX) {
        g_c.section_misses.fetch_add(1, std::memory_order_relaxed);
        if (!g_globals_told.exchange(true)) g_log.Line(true, "RTM2 skin cache $Globals not readable (p%u): sections not recorded", b0.param);
        return;
    }
    uint64_t        pal = 0, pal_off = 0;
    uint32_t        bones = 0;
    ID3D12Resource* pal_res = nullptr;
    if (resolve(t0, rpal)) {
        pal_res = rpal.res;
        if (!t0.is_table) {
            pal = rpal.cached_va; // a root SRV: the address is the palette's
            bones = 256;
            if (pal_res) pal_off = pal - pal_res->GetGPUVirtualAddress();
        } else if (rpal.view_handle) {
            const resource_view_desc vd = dev->get_resource_view_desc(resource_view{ rpal.view_handle });
            if (vd.type == resource_view_type::buffer && (vd.format == format::r32g32b32a32_float || vd.format == format::r32g32b32a32_uint ||
                                                             vd.format == format::r32g32b32a32_typeless)) {
                pal = rpal.cached_va + vd.buffer.offset * 16;
                pal_off = (pal_res ? rpal.cached_va - pal_res->GetGPUVirtualAddress() : 0) + vd.buffer.offset * 16;
                bones = vd.buffer.size == UINT64_MAX ? 0u : uint32_t((std::min)(vd.buffer.size / 3, uint64_t(1024)));
            } else if (!g_palfmt_told.exchange(true))
                g_log.Line(true, "RTM2 skin cache t0 view is not float4 (type %u fmt %u): sections not recorded", uint32_t(vd.type),
                    uint32_t(vd.format));
        }
    }
    if (!pal || !bones) {
        g_c.section_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t tan_va = resolve(u0, rtan) ? rtan.cached_va : 0;
    rtskin::NoteSection(ring, rring.size, start, pal, bones, tan_va, expected, g_frame_id, pal_res, pal_off);
    g_c.sections.fetch_add(1, std::memory_order_relaxed);
}

// ---- M3 observe: the character's BLAS, the TLAS instances that name it, the hit group records they use ----------
struct CharBlas {
    uint64_t              ring = 0;
    uint32_t              frame = 0, build_flags = 0, mode = 0;
    std::vector<uint64_t> ibs, vbs;
    std::vector<uint32_t> tris, verts, flags, index_width;
};
struct CharInst {
    uint32_t index = 0, id = 0, mask = 0, contrib = 0, flags = 0;
    uint64_t blas = 0;
};
std::mutex                   g_as_m; // the three below
std::map<uint64_t, CharBlas> g_char_blas; // BLAS dest VA -> what it was built from
std::vector<CharInst>        g_char_inst;  // the last TLAS's instances naming one
uint32_t                     g_tlas_logged = 0, g_rays_logged = 0;
std::atomic<bool>            g_have_char_inst{ false };

// Buffer copies into DEFAULT memory (the shader tables live there): where each region was filled from.
struct CopyRec {
    uint64_t        dst_va = 0, size = 0, src_va = 0;
    ID3D12Resource* src = nullptr;
    uint32_t        frame = 0;
    const void*     cl = nullptr; // the recording it is in
    uint64_t        gen = 0;
    bool            executed = false; // ...submitted already (ExecuteCommandLists): too late to change what it copies
    uint32_t        seq = 0;          // recording order (shared with the TLAS builds)
};
std::atomic<uint32_t> g_rec_seq{ 0 };
std::mutex g_copy_m;
CopyRec    g_copies[512];
uint32_t   g_copy_next = 0;

// The buffer holding a GPU address (resource handle when ReShade gave one, else the core's address map).
bool ResOfVa(resource r, uint64_t off, ID3D12Resource** res, uint64_t* va)
{
    *va = BufferVa(r, off);
    *res = r.handle ? reinterpret_cast<ID3D12Resource*>(r.handle) : nullptr;
    if (*res) return true;
    std::lock_guard l(g_mem_mutex);
    for (const auto& [base, ri] : g_va_to_res)
        if (ri.res && *va >= base && *va < base + (ri.size ? ri.size : 1)) {
            *res = ri.res;
            return true;
        }
    return false;
}
int HeapTypeOf(ID3D12Resource* res)
{
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS      hf = {};
    return res && SUCCEEDED(res->GetHeapProperties(&hp, &hf)) ? int(hp.Type) : -1;
}

void ObserveTlas(acceleration_structure_build_flags flags, const acceleration_structure_build_input& in, resource_view dest,
    acceleration_structure_build_mode mode)
{
    const uint32_t frame = g_frame_id;
    {
        std::lock_guard l(g_as_m);
        if (g_char_blas.empty()) return;
        const bool want = g_tlas_logged < 6 || (rtm::IsObservationFrame(frame) && g_tlas_logged < 40);
        if (!want) return;
        ++g_tlas_logged;
    }
    ID3D12Resource* res = nullptr;
    uint64_t        va = 0;
    const bool      found = ResOfVa(in.instances.buffer, in.instances.offset, &res, &va);
    const int       heap = HeapTypeOf(res);
    const uint32_t  count = in.instances.count;
    g_log.Line(true, "RTTLAS f=%u instances=%u va=%llx res=%p heap=%d array_of_pointers=%d build_flags=%x mode=%u dest=%llx", frame, count,
        (unsigned long long)va, (void*)res, found ? heap : -2, in.instances.array_of_pointers ? 1 : 0, uint32_t(flags), uint32_t(mode),
        (unsigned long long)dest.handle);
    if (!found || in.instances.array_of_pointers || !count) return;
    const uint32_t       n = (std::min)(count, 16384u);
    std::vector<uint8_t> buf(size_t(n) * 64);
    const uint32_t       got = heap == int(D3D12_HEAP_TYPE_UPLOAD) ? GuardedPrivateRead(res, va, buf.data(), uint32_t(buf.size())) : 0;
    if (!got) {
        g_log.Line(true, "  instances not CPU-readable (heap %d): not inspected", heap);
        return;
    }
    std::vector<CharInst> hits;
    std::lock_guard       l(g_as_m);
    for (uint32_t i = 0; i < got / 64; ++i) {
        const uint8_t* d = buf.data() + size_t(i) * 64;
        uint64_t       as = 0;
        uint32_t       w12 = 0, w13 = 0;
        memcpy(&as, d + 56, 8);
        memcpy(&w12, d + 48, 4);
        memcpy(&w13, d + 52, 4);
        if (!g_char_blas.count(as)) continue;
        CharInst ci;
        ci.index = i;
        ci.id = w12 & 0xffffff;
        ci.mask = w12 >> 24;
        ci.contrib = w13 & 0xffffff;
        ci.flags = w13 >> 24;
        ci.blas = as;
        hits.push_back(ci);
        float t[12];
        memcpy(t, d, 48);
        if (hits.size() <= 8)
            g_log.Line(true, "  char instance #%u blas=%llx id=%u mask=%02x hitgroup_contrib=%u flags=%02x translation=(%.2f %.2f %.2f)", i,
                (unsigned long long)as, ci.id, ci.mask, ci.contrib, ci.flags, t[3], t[7], t[11]);
    }
    g_log.Line(true, "  %zu instance(s) name a mod character's BLAS (of %u read)", hits.size(), got / 64);
    if (!hits.empty()) {
        g_char_inst = hits;
        g_have_char_inst.store(true, std::memory_order_relaxed);
    }
}

void ObserveRays(resource rg_res, uint64_t rg_off, uint64_t rg_size, resource miss_res, uint64_t miss_off, uint64_t miss_size,
    uint64_t miss_stride, resource hit_res, uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride, uint32_t w, uint32_t h, uint32_t d)
{
    const uint32_t        frame = g_frame_id;
    std::vector<CharInst> insts;
    std::map<uint64_t, CharBlas> blas;
    {
        std::lock_guard l(g_as_m);
        if (g_char_inst.empty()) return;
        const bool want = g_rays_logged < 8 || (rtm::IsObservationFrame(frame) && g_rays_logged < 48);
        if (!want) return;
        ++g_rays_logged;
        insts = g_char_inst;
        blas = g_char_blas;
    }
    ID3D12Resource *rg = nullptr, *ms = nullptr, *hg = nullptr;
    uint64_t        rg_va = 0, ms_va = 0, hg_va = 0;
    ResOfVa(rg_res, rg_off, &rg, &rg_va);
    ResOfVa(miss_res, miss_off, &ms, &ms_va);
    const bool have_hit = ResOfVa(hit_res, hit_off, &hg, &hg_va);
    const int  hheap = HeapTypeOf(hg);
    g_log.Line(true, "RTRAYS f=%u %ux%ux%u raygen=%llx/%llu (heap %d) miss=%llx/%llu stride %llu (heap %d) hit=%llx/%llu stride %llu (heap %d)",
        frame, w, h, d, (unsigned long long)rg_va, (unsigned long long)rg_size, HeapTypeOf(rg), (unsigned long long)ms_va,
        (unsigned long long)miss_size, (unsigned long long)miss_stride, HeapTypeOf(ms), (unsigned long long)hg_va, (unsigned long long)hit_size,
        (unsigned long long)hit_stride, have_hit ? hheap : -2);
    if (!have_hit || !hit_stride) return;
    // where the records can be read: the table itself (UPLOAD), or the UPLOAD buffer a copy filled it from
    ID3D12Resource* rd = hheap == int(D3D12_HEAP_TYPE_UPLOAD) ? hg : nullptr;
    uint64_t        rd_va = hg_va;
    if (!rd) {
        CopyRec c{};
        {
            std::lock_guard l(g_copy_m);
            for (const CopyRec& r : g_copies)
                if (r.src && hg_va >= r.dst_va && hg_va + hit_size <= r.dst_va + r.size && frame - r.frame <= 2 && r.frame >= c.frame) c = r;
        }
        if (!c.src) {
            g_log.Line(true, "  hit table: no buffer copy filled it (in the last 2 frames) -- records not readable");
            return;
        }
        const int sheap = HeapTypeOf(c.src);
        g_log.Line(true, "  hit table filled by a copy of %llu bytes from %llx (heap %d) at f%u", (unsigned long long)c.size,
            (unsigned long long)c.src_va, sheap, c.frame);
        if (sheap != int(D3D12_HEAP_TYPE_UPLOAD)) return;
        rd = c.src;
        rd_va = c.src_va + (hg_va - c.dst_va);
    }
    std::set<uint32_t> done;
    for (const CharInst& ci : insts) {
        if (!done.insert(ci.contrib).second || done.size() > 3) continue;
        const uint64_t       first = uint64_t(ci.contrib) * hit_stride;
        if (first >= hit_size) {
            g_log.Line(true, "  instance contrib %u beyond the hit table (%llu records)", ci.contrib, (unsigned long long)(hit_size / hit_stride));
            continue;
        }
        const uint32_t       want = uint32_t((std::min)(hit_size - first, uint64_t(64) * hit_stride));
        std::vector<uint8_t> rec(want);
        const uint32_t       got = GuardedPrivateRead(rd, rd_va + first, rec.data(), want);
        if (!got) continue;
        char name[96];
        snprintf(name, sizeof name, "sbt_f%u_contrib%u_stride%llu.bin", frame, ci.contrib, (unsigned long long)hit_stride);
        g_log.Bin(name, rec.data(), got, true);
        g_log.Line(true, "  hit records from contrib %u: %u bytes -> %s", ci.contrib, got, name);
        for (uint32_t r = 0; r < 2 && (r + 1) * hit_stride <= got; ++r) {
            std::string hex;
            for (uint32_t o = 0; o < hit_stride && o < 128; o += 4) {
                uint32_t v;
                memcpy(&v, rec.data() + r * hit_stride + o, 4);
                char b[12];
                snprintf(b, sizeof b, " %08x", v);
                hex += b;
            }
            g_log.Line(true, "    record %u:%s", ci.contrib + r, hex.c_str());
        }
        // root arguments that are the character's own buffers (its BLAS's index / vertex buffers)
        const auto bi = blas.find(ci.blas);
        uint32_t   matches = 0;
        for (uint32_t o = 0; o + 8 <= got && matches < 24; o += 8) {
            uint64_t v;
            memcpy(&v, rec.data() + o, 8);
            if (!v || bi == blas.end()) continue;
            for (size_t g = 0; g < bi->second.ibs.size(); ++g) {
                const char* what = nullptr;
                if (v == bi->second.ibs[g]) what = "IB";
                else if (v == bi->second.vbs[g]) what = "VB";
                else if (v >= bi->second.ring && v < bi->second.ring + 0x1000000 && bi->second.ring) what = "near ring";
                if (!what) continue;
                g_log.Line(true, "    record %u +%u = %llx: geometry %zu %s", ci.contrib + o / uint32_t(hit_stride), o % uint32_t(hit_stride),
                    (unsigned long long)v, g, what);
                ++matches;
                break;
            }
        }
        if (!matches) g_log.Line(true, "    no record holds the character's IB / VB addresses");
    }
}

// ---- M3: the mod's own BLAS in the character's place: named by its TLAS instance and its hit group records ----------
// Facts this rests on (rt_observe.log, 2a398de): the character BLAS is built (then updated every frame) from the skin
// cache ring, one geometry per mesh section, flags ALLOW_UPDATE | PREFER_FAST_BUILD; the TLAS instances are read from
// an UPLOAD buffer; the hit group table (DEFAULT) is filled by a copy from UPLOAD in the frame; a character's records
// are InstanceContributionToHitGroupIndex + 3 * geometry + ray type, 128 bytes: shader identifier (32), then UE's
// hit group system parameters -- +32 index buffer (the BLAS's geometry 0 IB), +40 vertex buffer (the ring), +48
// config (index stride | vertex stride << 8), +52 the geometry's offset in the index buffer (bytes) -- and nothing
// else (no material). So the hit shaders read the index / vertex buffers the records name, with the primitive index
// of the BLAS they hit: a BLAS of ours goes in only together with records naming our buffers. The game writes the
// TLAS instances after recording (even after submitting) the TLAS build, so the instances are never written: right
// before DispatchRays (its list unsubmitted, the hit table's copy from UPLOAD in it) the TLAS is rebuilt in place from
// a GPU copy of the game's final instances with our BLAS addresses over the character's, and the records are patched
// in the copy's UPLOAD source -- both or neither.
struct ModBlas {
    uint32_t                                                geoms = 0;
    uint64_t                                                game_ib = 0; // the game's geometry 0 IB (its records' base)
    std::vector<uint64_t>                                   game_off;    // each geometry's offset from it
    uint32_t                                                game_iw = 0;
    ComPtr<ID3D12Resource>                                  blas, scratch, ib;
    uint64_t                                                blas_cap = 0, scratch_cap = 0;
    std::vector<uint64_t>                                   vb;
    std::vector<uint32_t>                                   verts, ib_off, ib_cnt, gflags;
    std::vector<int>                                        pose_mod, pose_slot; // M4: whose pose each geometry is (-1 none)
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> sig;     // the topology built: ranges per geometry
    std::vector<ID3D12Resource*>                            sig_idx; // ...of which mod index buffer
    uint32_t                                                built = 0, updates = 0, builds = 0;
    uint32_t                                                topo_frame = 0; // when the topology (index buffer) last changed
    bool                                                    need_full = true; // the next build must be a full one (contract)
    std::vector<uint32_t>                                   built_verts, built_flags, built_cnt; // what the BLAS was built from
    bool                                                    ok = false;
    // where the TLAS instance naming it was last found / written
    uint32_t              redirected = 0;
    uint32_t contrib = UINT32_MAX;          // its instance's hit group contribution (last TLAS that named it)
    uint32_t patched = 0;                   // frame a TLAS build last got our BLAS in its instance
    std::vector<uint32_t> redirected_idx; // the instances our last TLAS rebuild put it in
    uint32_t              redirected_seq = 0; // ...of which TLAS (its build's sequence number)
    uint64_t              rays_live = 0, rays_covered = 0; // ray dispatches it was live for / every instance ours then
    uint64_t              rays_found = 0;                   // ...of which its instance was found in the TLAS
};
struct TlasRec {
    const void*     cl = nullptr;
    uint64_t        gen = 0;
    uint32_t        frame = 0, count = 0;
    ID3D12Resource* res = nullptr;
    uint64_t        va = 0;
    bool            executed = false; // its list was submitted
    uint32_t        seq = 0;
    uint32_t        flags = 0; // its build flags
    uint64_t        dest = 0;  // the TLAS it builds
    uint64_t        gate = 0;  // the gate value its list waits for (0 = not gated)
    bool            decided = false; // the rays came before its list was submitted: written then, never gated
};
std::mutex                                           g_m3_m; // everything below
std::map<uint64_t, ModBlas>                          g_m3;   // the game's BLAS (dest VA) -> ours
std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> g_m3_grave; // retired buffers, the frame they left
TlasRec                                              g_m3_tlas[8];
uint32_t                                             g_m3_tlas_next = 0;
std::map<std::string, uint64_t>                      g_m3_why; // refusals by reason
uint64_t                                             g_m3_builds = 0, g_m3_updates = 0, g_m3_redirects = 0;
std::map<std::string, uint64_t>                      g_m3_full_why; // full builds of our BLAS by reason
std::atomic<bool>                                    g_m3_any{ false };
std::atomic<int>                                     g_m3_on{ -1 };
ComPtr<ID3D12Device5>                                g_dev5;

// rt_m3: 0 off, 1 full (our BLAS in a rebuilt TLAS, records patched), 2 our BLAS built only (nothing redirected),
// 3 the TLAS rebuilt in place from the game's own instances, unchanged (no BLAS of ours in it, no record touched).
// 2 and 3 isolate a hang: our BLAS work alone, the in-place rebuild alone.
int M3Mode()
{
    int v = g_m3_on.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    if (g_addon_ini_path.empty()) return 0; // not read yet: nothing until it is
    v = int(GetPrivateProfileIntW(L"wwmi-dx12", L"rt_m3", 1, g_addon_ini_path.c_str()));
    if (v < 0 || v > 3) v = 0;
    g_m3_on.store(v, std::memory_order_relaxed);
    return v;
}
bool M3On()
{
    return M3Mode() != 0;
}

void M3Refuse(const char* why) // g_m3_m held
{
    uint64_t& n = g_m3_why[why];
    if (n++ == 0 || n == 100 || n == 10000) g_log.Line(true, "RTM3 refused (%llu): %s", (unsigned long long)n, why);
}

void M3Retire(ComPtr<ID3D12Resource>& r) // g_m3_m held: the GPU may still read it for a few frames
{
    if (r) g_m3_grave.push_back({ r, g_frame_id });
    r.Reset();
}

// Write into a game UPLOAD buffer (its CPU-visible memory, read by the GPU when the list runs).
uint8_t* MapUpload(ID3D12Resource* res, uint64_t va, uint64_t n)
{
    if (!res || HeapTypeOf(res) != int(D3D12_HEAP_TYPE_UPLOAD)) return nullptr;
    const uint64_t base = res->GetGPUVirtualAddress(), width = res->GetDesc().Width;
    if (va < base || va - base + n > width) return nullptr;
    D3D12_RANGE none = { 0, 0 };
    void*       p = nullptr;
    if (FAILED(res->Map(0, &none, &p)) || !p) return nullptr;
    return static_cast<uint8_t*>(p) + (va - base);
}
void UnmapUpload(ID3D12Resource* res)
{
    res->Unmap(0, nullptr);
}
bool GuardedCopy(void* dst, const void* src, size_t n)
{
#ifdef _MSC_VER
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    memcpy(dst, src, n);
    return true;
#endif
}

// A DRED breadcrumb context on the game's list (a device removal names what of ours ran last).
void M3Marker(ID3D12GraphicsCommandList* cl, const wchar_t* what)
{
    cl->SetMarker(0, what, UINT((wcslen(what) + 1) * sizeof(wchar_t)));
}

// The game builds / updates a character BLAS from a ring the module poses a mod for: build ours next to it, from the
// raster's last poses of the mod's components (one geometry per section, as the game's).
void M3OnCharBlas(command_list* cmd_list, uint64_t game, uint64_t ring, const acceleration_structure_build_input* inputs, uint32_t n,
    acceleration_structure_build_mode mode)
{
    if (!M3On() || !cmd_list || !n || n > 64) return;
    const uint32_t frame = g_frame_id;
    std::vector<uint64_t> gib(n);
    std::vector<uint32_t> gflags(n);
    uint32_t              iw = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (inputs[i].type != acceleration_structure_build_input_type::triangles) return;
        const auto& t = inputs[i].triangles;
        if (!t.index_buffer.handle && !t.index_offset) return;
        gib[i] = BufferVa(t.index_buffer, t.index_offset);
        gflags[i] = uint32_t(inputs[i].flags);
        iw = IndexWidth(t.index_format);
    }
    std::vector<rtskin::GeomPose> poses(n);
    uint32_t                      have = 0;
    for (uint32_t g = 0; g < n; ++g) {
        rtskin::GeomPose p;
        if (!rtskin::PoseFor(ring, int(g), frame, &p)) continue;
        bool fits = p.idx && p.pos_va && p.verts;
        for (const auto& [s2, c] : p.ranges) fits = fits && c && uint64_t(s2) + c <= p.indices && c % 3 == 0;
        if (!fits) continue;
        poses[g] = std::move(p);
        ++have;
    }
    std::lock_guard l(g_m3_m);
    // retired buffers the GPU has long finished with
    for (auto it = g_m3_grave.begin(); it != g_m3_grave.end();)
        it = frame - it->second > 600 ? g_m3_grave.erase(it) : std::next(it);
    for (auto it = g_m3.begin(); it != g_m3.end();) // characters gone (their BLAS not built for a long time)
        if (it->first != game && it->second.built && frame - it->second.built > 600) {
            M3Retire(it->second.blas);
            M3Retire(it->second.scratch);
            M3Retire(it->second.ib);
            it = g_m3.erase(it);
        } else
            ++it;
    if (!have) {
        const auto it = g_m3.find(game);
        if (it != g_m3.end()) it->second.ok = false;
        M3Refuse("no pose of the mod for any of the character's sections (not drawn lately)");
        return;
    }
    ModBlas& mb = g_m3[game];
    mb.ok = false;
    // any return before the build below leaves the BLAS as it was; the next build is then a full one
    const bool need_full = mb.need_full;
    mb.need_full = true;
    if (!g_dev5 && (!g_d3d12_device || FAILED(g_d3d12_device->QueryInterface(IID_PPV_ARGS(&g_dev5))))) {
        M3Refuse("no ID3D12Device5");
        return;
    }
    ComPtr<ID3D12GraphicsCommandList4> cl;
    if (FAILED(reinterpret_cast<IUnknown*>(cmd_list->get_native())->QueryInterface(IID_PPV_ARGS(&cl))) || !cl) {
        M3Refuse("the list is not an ID3D12GraphicsCommandList4");
        return;
    }
    // topology: the index ranges of each geometry (a section the mod does not draw: one degenerate triangle)
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> sig(n);
    std::vector<ID3D12Resource*>                            sig_idx(n, nullptr);
    uint64_t                                                any_vb = 0;
    uint32_t                                                any_verts = 0;
    for (uint32_t g = 0; g < n; ++g)
        if (poses[g].mod >= 0) {
            sig[g] = poses[g].ranges;
            sig_idx[g] = poses[g].idx;
            any_vb = poses[g].pos_va;
            any_verts = poses[g].verts;
        }
    const bool topo = !mb.blas || !mb.ib || mb.geoms != n || sig != mb.sig || sig_idx != mb.sig_idx;
    if (topo) {
        std::vector<uint32_t> off(n), cnt(n);
        uint64_t              total = 0;
        for (uint32_t g = 0; g < n; ++g) {
            uint32_t c = 0;
            for (const auto& r : sig[g]) c += r.second;
            off[g] = uint32_t(total * 4);
            cnt[g] = c ? c : 3;
            total += cnt[g];
        }
        // zero-filled (committed): the degenerate triangles are indices 0, 0, 0; and as many zero indices again as the
        // game's geometry has past our last one, so a read with the game's primitive indices stays in the buffer
        uint64_t game_bytes = 0;
        for (uint32_t g = 0; g < n; ++g) game_bytes += uint64_t(inputs[g].triangles.index_count) * (iw ? iw : 4) * 2;
        ComPtr<ID3D12Resource> ib;
        if (total > (64u << 20) || FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, (total * 4 + game_bytes + 255) & ~uint64_t(255),
                                       D3D12_RESOURCE_STATE_COPY_DEST, &ib))) {
            M3Refuse("could not create the index buffer");
            return;
        }
        for (uint32_t g = 0; g < n; ++g) {
            uint64_t o = off[g];
            for (const auto& [s2, c] : sig[g]) {
                cl->CopyBufferRegion(ib.Get(), o, sig_idx[g], uint64_t(s2) * 4, uint64_t(c) * 4);
                o += uint64_t(c) * 4;
            }
        }
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = ib.Get();
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        br.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &br);
        M3Retire(mb.ib);
        mb.ib = ib;
        mb.ib_off = off;
        mb.ib_cnt = cnt;
        mb.sig = sig;
        mb.sig_idx = sig_idx;
        mb.topo_frame = frame;
    }
    mb.geoms = n;
    mb.game_ib = gib[0];
    mb.game_off.resize(n);
    for (uint32_t g = 0; g < n; ++g) mb.game_off[g] = gib[g] - gib[0];
    mb.game_iw = iw;
    mb.gflags = gflags;
    mb.vb.resize(n);
    mb.verts.resize(n);
    mb.pose_mod.assign(n, -1);
    mb.pose_slot.assign(n, -1);
    for (uint32_t g = 0; g < n; ++g)
        if (poses[g].mod >= 0) {
            mb.pose_mod[g] = poses[g].mod;
            mb.pose_slot[g] = poses[g].slot;
        }
    for (uint32_t g = 0; g < n; ++g) {
        mb.vb[g] = poses[g].mod >= 0 ? poses[g].pos_va : any_vb;
        mb.verts[g] = poses[g].mod >= 0 ? poses[g].verts : any_verts;
    }
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> gd(n);
    const uint64_t                              ib_va = mb.ib->GetGPUVirtualAddress();
    for (uint32_t g = 0; g < n; ++g) {
        D3D12_RAYTRACING_GEOMETRY_DESC& d = gd[g];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = D3D12_RAYTRACING_GEOMETRY_FLAGS(mb.gflags[g]);
        d.Triangles.Transform3x4 = 0;
        d.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.IndexCount = mb.ib_cnt[g];
        d.Triangles.VertexCount = mb.verts[g];
        d.Triangles.IndexBuffer = ib_va + mb.ib_off[g];
        d.Triangles.VertexBuffer.StartAddress = mb.vb[g];
        d.Triangles.VertexBuffer.StrideInBytes = 12;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs = n;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = gd.data();
    // an update keeps every geometry's index / vertex count and flags (DXR update constraints): anything else, a full build
    std::vector<uint32_t> cur_cnt(n), cur_verts(n), cur_flags(n);
    for (uint32_t g = 0; g < n; ++g) {
        cur_cnt[g] = mb.ib_cnt[g];
        cur_verts[g] = mb.verts[g];
        cur_flags[g] = mb.gflags[g];
    }
    // rt_m3_update = 0: every build a full one (before this, every build was, by a bug: need_full was set before it was read)
    static const bool s_updates = g_addon_ini_path.empty() || GetPrivateProfileIntW(L"wwmi-dx12", L"rt_m3_update", 1, g_addon_ini_path.c_str()) != 0;
    // (the game's own build mode does not matter: ours has its own topology, counts and history)
    const char* full_why = !s_updates                                                                     ? "rt_m3_update=0"
                           : topo                                                                         ? "topology changed"
                           : need_full                                                                    ? "previous build skipped"
                           : !mb.built                                                                    ? "first build"
                           : mb.updates >= 300                                                            ? "300 updates in a row"
                           : cur_cnt != mb.built_cnt || cur_verts != mb.built_verts || cur_flags != mb.built_flags ? "counts / flags changed"
                                                                                                          : nullptr;
    (void)mode;
    const bool full = full_why != nullptr;
    if (full) ++g_m3_full_why[full_why];
    if (full) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
        g_dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
        if (!pi.ResultDataMaxSizeInBytes) {
            M3Refuse("prebuild info returned no size");
            return;
        }
        if (pi.ResultDataMaxSizeInBytes > mb.blas_cap) {
            ComPtr<ID3D12Resource> b;
            const uint64_t         cap = (pi.ResultDataMaxSizeInBytes + pi.ResultDataMaxSizeInBytes / 4 + 65535) & ~uint64_t(65535);
            if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, cap, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &b,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
                M3Refuse("could not create the BLAS buffer");
                return;
            }
            M3Retire(mb.blas);
            mb.blas = b;
            mb.blas_cap = cap;
        }
        const uint64_t need = (std::max)(pi.ScratchDataSizeInBytes, pi.UpdateScratchDataSizeInBytes);
        if (need > mb.scratch_cap) {
            ComPtr<ID3D12Resource> s;
            const uint64_t         cap = (need + need / 4 + 65535) & ~uint64_t(65535);
            if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, cap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
                M3Refuse("could not create the scratch buffer");
                return;
            }
            M3Retire(mb.scratch);
            mb.scratch = s;
            mb.scratch_cap = cap;
        }
    } else
        in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    D3D12_RESOURCE_BARRIER uav[2]{};
    uav[0].Type = uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav[0].UAV.pResource = mb.blas.Get();
    uav[1].UAV.pResource = mb.scratch.Get();
    M3Marker(cl.Get(), L"wwmi rt-m3: our character BLAS build");
    cl->ResourceBarrier(2, uav); // a build of an earlier list still writing either
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.DestAccelerationStructureData = mb.blas->GetGPUVirtualAddress();
    bd.SourceAccelerationStructureData = full ? 0 : bd.DestAccelerationStructureData;
    bd.ScratchAccelerationStructureData = mb.scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    cl->ResourceBarrier(1, &uav[0]); // complete before the TLAS build reads it
    RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "rt-m3 blas");
    mb.updates = full ? 0 : mb.updates + 1;
    mb.built = frame;
    mb.ok = true;
    mb.need_full = false;
    mb.built_cnt = cur_cnt;
    mb.built_verts = cur_verts;
    mb.built_flags = cur_flags;
    g_m3_any.store(true, std::memory_order_relaxed);
    if (full) {
        ++g_m3_builds;
        if (mb.builds++ < 3 || topo) {
            std::string geo;
            for (uint32_t g = 0; g < n && geo.size() < 900; ++g) {
                char b[96];
                snprintf(b, sizeof b, " [%u: mod %d, %u tris (game %llu), %zu range(s)]", g, poses[g].mod, mb.ib_cnt[g] / 3,
                    (unsigned long long)((g + 1 < n ? gib[g + 1] - gib[g] : 0) / (iw ? iw * 3 : 1)), sig[g].size());
                geo += b;
            }
            g_log.Line(true, "RTM3 blas %s game=%llx ours=%llx (%llu bytes) ring=%llx geometries=%u%s", topo ? "built (new topology)" : "rebuilt",
                (unsigned long long)game, (unsigned long long)mb.blas->GetGPUVirtualAddress(), (unsigned long long)mb.blas_cap,
                (unsigned long long)ring, n, geo.c_str());
        }
    } else
        ++g_m3_updates;
}

// Every instance naming a character's BLAS (the game's or ours), from one full scan of each TLAS's instances shared by
// all characters (the 3f348bb/11c9692 tests: the instance order changes from one TLAS to the next, so the hints of the
// last one missed ~85 % of the time). g_m3_m held.
uint32_t                                          g_scan_seq = 0;   // the TLAS (sequence) scanned
std::unordered_map<uint64_t, std::vector<uint32_t>> g_scan_hits;    // BLAS address -> instance indices naming it
uint64_t                                          g_scan_n = 0, g_scan_us = 0, g_scan_max_us = 0, g_scan_slow = 0;

// The instances' BLAS addresses (+56 of each 64-byte descriptor) in the game's UPLOAD memory (write-combined: a plain
// read is one uncached transaction per load). One 16-byte streaming load per instance (+48..+63) fetches its line in a
// single transaction. No C++ objects here (SEH). Returns the hits written, or UINT32_MAX on a fault.
uint32_t ScanAsStream(const uint8_t* p, uint32_t count, const uint64_t* keys, uint32_t nkeys, uint32_t* hit_idx, uint64_t* hit_as, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < cap; ++i) {
        const uint8_t* q = p + size_t(i) * 64 + 48;
        uint64_t       as;
#if defined(_MSC_VER) && !defined(__clang__)
        const __m128i v = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(q));
        as = uint64_t(_mm_cvtsi128_si64(_mm_unpackhi_epi64(v, v)));
#else
        memcpy(&as, q + 8, 8);
#endif
        uint32_t lo = 0, hi = nkeys;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (keys[mid] < as) lo = mid + 1;
            else hi = mid;
        }
        if (lo < nkeys && keys[lo] == as) {
            hit_idx[n] = i;
            hit_as[n] = as;
            ++n;
        }
    }
    return n;
}
uint32_t GuardedScanAs(const uint8_t* p, uint32_t count, const uint64_t* keys, uint32_t nkeys, uint32_t* hit_idx, uint64_t* hit_as, uint32_t cap)
{
#ifdef _MSC_VER
    __try {
        return ScanAsStream(p, count, keys, nkeys, hit_idx, hit_as, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT32_MAX;
    }
#else
    return ScanAsStream(p, count, keys, nkeys, hit_idx, hit_as, cap);
#endif
}

void M3ScanTlas(ID3D12Resource* res, uint64_t va, uint32_t count, uint32_t seq)
{
    g_scan_seq = seq;
    g_scan_hits.clear();
    std::vector<uint64_t> keys;
    for (const auto& [game, mb] : g_m3)
        if (mb.blas) {
            keys.push_back(game);
            keys.push_back(mb.blas->GetGPUVirtualAddress());
        }
    if (keys.empty()) return;
    std::sort(keys.begin(), keys.end());
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    constexpr uint32_t    kCap = 256;
    uint32_t              hit_idx[kCap];
    uint64_t              hit_as[kCap];
    uint32_t              got = UINT32_MAX;
    const uint8_t*        p = MapUpload(res, va, uint64_t(count) * 64);
    if (p) {
        if ((reinterpret_cast<uintptr_t>(p) & 15) == 0) got = GuardedScanAs(p, count, keys.data(), uint32_t(keys.size()), hit_idx, hit_as, kCap);
        UnmapUpload(res);
    }
    if (got == UINT32_MAX) {
        // not mappable / unaligned / faulted: plain chunked reads
        got = 0;
        std::vector<uint8_t> buf(1024 * 64);
        for (uint32_t i = 0; i < count && got < kCap; i += 1024) {
            const uint32_t k = (std::min)(1024u, count - i);
            const uint32_t r = GuardedPrivateRead(res, va + uint64_t(i) * 64, buf.data(), k * 64);
            for (uint32_t j = 0; j < r / 64 && got < kCap; ++j) {
                uint64_t as;
                memcpy(&as, buf.data() + size_t(j) * 64 + 56, 8);
                if (std::binary_search(keys.begin(), keys.end(), as)) {
                    hit_idx[got] = i + j;
                    hit_as[got] = as;
                    ++got;
                }
            }
            if (r < k * 64) break;
        }
        ++g_scan_slow;
    }
    for (uint32_t h = 0; h < got; ++h) {
        auto& v = g_scan_hits[hit_as[h]];
        if (v.size() < 4) v.push_back(hit_idx[h]);
    }
    QueryPerformanceCounter(&t1);
    const uint64_t us = uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
    ++g_scan_n;
    g_scan_us += us;
    g_scan_max_us = (std::max)(g_scan_max_us, us);
}

std::vector<uint32_t> FindInstances(const TlasRec& t, uint64_t a, uint64_t b)
{
    if (g_scan_seq != t.seq) M3ScanTlas(t.res, t.va, t.count, t.seq);
    std::vector<uint32_t> out;
    for (uint64_t k : { a, b }) {
        const auto it = g_scan_hits.find(k);
        if (it == g_scan_hits.end()) continue;
        for (uint32_t i : it->second)
            if (out.size() < 4 && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    }
    return out;
}

constexpr uint32_t kM3Window = 2;  // frames a TLAS build's redirect stays in force (a TLAS may serve the next frame too)

// The hit group table's records as the CPU can still change them: the table itself when it is UPLOAD, or the UPLOAD
// source of the copy that fills it, recorded in a list not submitted yet. g_m3_m held.
bool M3HitSource(command_list* cmd_list, resource hit_res, uint64_t hit_off, uint64_t hit_size, uint32_t frame, ID3D12Resource** rd,
    uint64_t* rd_va, const char** why, CopyRec* copy)
{
    ID3D12Resource* hg = nullptr;
    uint64_t        hg_va = 0;
    if (!ResOfVa(hit_res, hit_off, &hg, &hg_va)) {
        *why = "the hit group table's buffer is unknown";
        return false;
    }
    if (HeapTypeOf(hg) == int(D3D12_HEAP_TYPE_UPLOAD)) {
        *rd = hg;
        *rd_va = hg_va;
        return true;
    }
    CopyRec c{};
    {
        std::lock_guard l2(g_copy_m);
        // the newest copy into the table recorded in the rays' own list (observed: the game fills it there, just before)
        const uint64_t gen = CmdGen(cmd_list);
        for (const CopyRec& r : g_copies)
            if (r.src && r.cl == cmd_list && r.gen == gen && hg_va >= r.dst_va && hg_va + hit_size <= r.dst_va + r.size &&
                int32_t(frame - r.frame) <= 1 && (!c.src || r.seq > c.seq))
                c = r;
    }
    if (copy) *copy = c;
    if (!c.src) {
        *why = "no copy in the rays' list filled the hit group table";
        return false;
    }
    if (HeapTypeOf(c.src) != int(D3D12_HEAP_TYPE_UPLOAD)) {
        *why = "the hit group table was not filled from UPLOAD memory";
        return false;
    }
    if (c.executed) {
        *why = "the copy that fills the hit group table was already submitted";
        return false;
    }
    *rd = c.src;
    *rd_va = c.src_va + (hg_va - c.dst_va);
    return true;
}

// The 3-per-geometry records from `contrib`, read once: `orig` as they are, `mine` with our buffers. false = some record
// is neither the character's (for its geometry) nor already ours. g_m3_m held.
struct RecTx {
    ID3D12Resource*      rd = nullptr;
    uint64_t             va = 0;
    std::vector<uint8_t> orig, mine;
    bool                 same = false; // already ours: nothing to write
};
bool M3Records(ModBlas& mb, ID3D12Resource* rd, uint64_t rd_va, uint64_t hit_size, uint64_t stride, uint32_t contrib, RecTx* tx)
{
    const uint64_t nrec = uint64_t(mb.geoms) * 3;
    if (!mb.ib || !nrec || stride < 64 || (uint64_t(contrib) + nrec) * stride > hit_size) return false;
    std::vector<uint8_t> recs(size_t(nrec * stride));
    if (GuardedPrivateRead(rd, rd_va + uint64_t(contrib) * stride, recs.data(), uint32_t(recs.size())) != recs.size()) return false;
    std::vector<uint8_t> orig = recs;
    const uint64_t       ib_va = mb.ib->GetGPUVirtualAddress();
    bool                 same = true;
    for (uint64_t i = 0; i < nrec; ++i) {
        uint8_t*       r = recs.data() + i * stride;
        const uint32_t g = uint32_t(i / 3);
        uint64_t       ib, vb;
        uint32_t       cfg, off;
        memcpy(&ib, r + 32, 8);
        memcpy(&vb, r + 40, 8);
        memcpy(&cfg, r + 48, 4);
        memcpy(&off, r + 52, 4);
        const bool is_game = ib == mb.game_ib && off == mb.game_off[g] && (cfg & 0xff) == mb.game_iw && ((cfg >> 8) & 0xff) == 12 &&
                             rtskin::RingKey(vb) != 0;
        const bool is_ours = ib == ib_va && off == mb.ib_off[g] && (cfg & 0xff) == 4;
        if (!is_game && !is_ours) {
            static std::atomic<uint32_t> s_dump{ 0 };
            if (s_dump.fetch_add(1) < 8)
                g_log.Line(true, "RTM3 record %llu (contrib %u, geometry %u) is not the character's: ib=%llx (want %llx) vb=%llx cfg=%x off=%x (want %llx)",
                    (unsigned long long)(contrib + i), contrib, g, (unsigned long long)ib, (unsigned long long)mb.game_ib, (unsigned long long)vb,
                    cfg, off, (unsigned long long)mb.game_off[g]);
            return false;
        }
        same = same && is_ours && vb == mb.vb[g];
        { // M4: the hit group identifiers characters' records run (once each)
            static std::set<uint64_t> s_ids;
            uint64_t                  id8;
            memcpy(&id8, r, 8);
            if (s_ids.size() < 32 && s_ids.insert(id8).second) {
                std::string tail;
                for (uint64_t o = 56; o < stride && o < 128; o += 8) {
                    uint64_t v;
                    memcpy(&v, r + o, 8);
                    char e[24];
                    snprintf(e, sizeof e, " %llx", (unsigned long long)v);
                    tail += e;
                }
                g_log.Line(true, "RTM4 character record id=%02x%02x%02x%02x%02x%02x%02x%02x (ray type %u, geometry %u, contrib %u) +56..:%s", r[0], r[1],
                    r[2], r[3], r[4], r[5], r[6], r[7], uint32_t(i % 3), g, contrib, tail.c_str());
            }
        }
        memcpy(r + 32, &ib_va, 8);
        memcpy(r + 40, &mb.vb[g], 8);
        cfg = (cfg & 0xffff0000u) | (12u << 8) | 4u;
        memcpy(r + 48, &cfg, 4);
        memcpy(r + 52, &mb.ib_off[g], 4);
    }
    if (tx) {
        tx->rd = rd;
        tx->va = rd_va + uint64_t(contrib) * stride;
        tx->orig = std::move(orig);
        tx->mine = std::move(recs);
        tx->same = same;
    }
    return true;
}
bool M3WriteRecs(const RecTx& tx, bool mine)
{
    if (tx.same && mine) return true;
    const std::vector<uint8_t>& bytes = mine ? tx.mine : tx.orig;
    uint8_t*                    p = MapUpload(tx.rd, tx.va, bytes.size());
    if (!p) return false;
    const bool ok = GuardedCopy(p, bytes.data(), bytes.size());
    UnmapUpload(tx.rd);
    return ok;
}
// All the prepared records ours, or (one write failing) every one written put back: never a mix. g_m3_m held.
bool M3CommitRecs(const std::vector<RecTx>& txs)
{
    size_t done = 0;
    for (; done < txs.size(); ++done)
        if (!M3WriteRecs(txs[done], true)) break;
    if (done == txs.size()) return true;
    bool back = true;
    for (size_t i = 0; i < done; ++i) back = M3WriteRecs(txs[i], false) && back;
    if (!back) {
        static std::atomic<uint32_t> s_warn{ 0 };
        if (s_warn.fetch_add(1) < 16) g_log.Line(true, "RTM3 WARNING: a record write failed and the written ones could not all be put back");
    }
    return false;
}

// A TLAS build: recorded (the game writes its instances later; they are final by the time the rays are recorded).
void M3NoteTlas(command_list* cmd_list, const acceleration_structure_build_input& in, uint32_t flags, uint64_t dest)
{
    if (!M3On() || !g_m3_any.load(std::memory_order_relaxed) || in.instances.array_of_pointers || !in.instances.count || !dest) return;
    ID3D12Resource* res = nullptr;
    uint64_t        va = 0;
    if (!ResOfVa(in.instances.buffer, in.instances.offset, &res, &va) || HeapTypeOf(res) != int(D3D12_HEAP_TYPE_UPLOAD)) return;
    std::lock_guard l(g_m3_m);
    const uint32_t  slot = ++g_m3_tlas_next;
    TlasRec&        t = g_m3_tlas[slot & 7];
    t = TlasRec{};
    t.seq = g_rec_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    t.cl = cmd_list;
    t.gen = CmdGen(cmd_list);
    t.frame = g_frame_id;
    t.res = res;
    t.va = va;
    t.count = in.instances.count;
    t.flags = flags;
    t.dest = dest;
}

// Our copies of a TLAS's instances and what rebuilding it needs (g_m3_m). A ring of kM3Slots: a slot is used once per
// rebuild (at most two a frame), so when it comes round again its last use is frames old -- finished on the GPU and its
// buffers decayed to COMMON (they decay when the ExecuteCommandLists that used them completes).
constexpr uint32_t kM3Slots = 6;
struct RebuildSlot {
    ComPtr<ID3D12Resource> inst, scratch;
    uint64_t               inst_cap = 0, scratch_cap = 0;
    uint32_t               frame = 0;
};
RebuildSlot        g_m3_slot[kM3Slots];
uint32_t           g_m3_slot_next = 0;
ComPtr<ID3D12Resource> g_m3_up;
uint8_t*           g_m3_up_map = nullptr;
uint32_t           g_m3_up_next = 0;
constexpr uint32_t kM3UpBytes = 64 * 1024; // 8-byte BLAS addresses: a handful a frame, so an entry is reused thousands of
                                           // frames later (the GPU is a few frames behind at most)
uint32_t           g_m3_rebuilt_seq = 0; // the TLAS last rebuilt with our BLAS in it
uint64_t           g_m3_rebuilds = 0, g_m3_restores = 0;

// Rebuild the TLAS `t` in place, in the rays' list right before DispatchRays, from the game's (final) instances with
// `patches` (instance index -> BLAS address) applied. false = nothing recorded. g_m3_m held.
bool M3RebuildTlas(command_list* cmd_list, const TlasRec& t, const std::vector<std::pair<uint32_t, uint64_t>>& patches, const char** why)
{
    ComPtr<ID3D12GraphicsCommandList4> cl;
    if (FAILED(reinterpret_cast<IUnknown*>(cmd_list->get_native())->QueryInterface(IID_PPV_ARGS(&cl))) || !cl) {
        *why = "the rays' list is not an ID3D12GraphicsCommandList4";
        return false;
    }
    if (!g_dev5 && (!g_d3d12_device || FAILED(g_d3d12_device->QueryInterface(IID_PPV_ARGS(&g_dev5))))) {
        *why = "no ID3D12Device5";
        return false;
    }
    const uint64_t bytes = uint64_t(t.count) * 64;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(t.flags & ~uint32_t(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE));
    in.NumDescs = t.count;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
    g_dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
    // the game's TLAS buffer must hold what this build writes (same inputs as its own build: it does)
    ID3D12Resource* dres = nullptr;
    uint64_t        dva = 0;
    if (!ResOfVa(resource{ 0 }, t.dest, &dres, &dva) || !dres || dva - dres->GetGPUVirtualAddress() + pi.ResultDataMaxSizeInBytes > dres->GetDesc().Width) {
        *why = "the TLAS buffer is unknown or too small for the rebuild";
        return false;
    }
    RebuildSlot& sl = g_m3_slot[g_m3_slot_next % kM3Slots];
    if (sl.frame && int32_t(g_frame_id - sl.frame) < 2) {
        *why = "every rebuild slot was used in the last two frames";
        return false;
    }
    if (bytes > sl.inst_cap) {
        ComPtr<ID3D12Resource> b;
        const uint64_t         cap = (bytes + bytes / 4 + 65535) & ~uint64_t(65535);
        if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, cap, D3D12_RESOURCE_STATE_COMMON, &b))) {
            *why = "could not create the instance buffer";
            return false;
        }
        M3Retire(sl.inst);
        sl.inst = b;
        sl.inst_cap = cap;
    }
    if (pi.ScratchDataSizeInBytes > sl.scratch_cap) {
        ComPtr<ID3D12Resource> s;
        const uint64_t         cap = (pi.ScratchDataSizeInBytes + pi.ScratchDataSizeInBytes / 4 + 65535) & ~uint64_t(65535);
        if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, cap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &s,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) {
            *why = "could not create the TLAS scratch buffer";
            return false;
        }
        M3Retire(sl.scratch);
        sl.scratch = s;
        sl.scratch_cap = cap;
    }
    if (!g_m3_up) {
        D3D12_RANGE none{ 0, 0 };
        if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_UPLOAD, kM3UpBytes, D3D12_RESOURCE_STATE_GENERIC_READ, &g_m3_up)) ||
            FAILED(g_m3_up->Map(0, &none, reinterpret_cast<void**>(&g_m3_up_map))) || !g_m3_up_map) {
            g_m3_up.Reset();
            *why = "could not create the upload buffer";
            return false;
        }
    }
    // the instances: the game's, as its own build read them, then our BLAS addresses over theirs
    M3Marker(cl.Get(), L"wwmi rt-m3: TLAS rebuild (instance copy, build)");
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = sl.inst.Get();
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; // (fresh, or decayed since its last use frames ago)
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &br);
    ++g_m3_slot_next;
    sl.frame = g_frame_id;
    cl->CopyBufferRegion(sl.inst.Get(), 0, t.res, t.va - t.res->GetGPUVirtualAddress(), bytes);
    for (const auto& [idx, as] : patches) {
        if (g_m3_up_next + 8 > kM3UpBytes) g_m3_up_next = 0;
        memcpy(g_m3_up_map + g_m3_up_next, &as, 8);
        cl->CopyBufferRegion(sl.inst.Get(), uint64_t(idx) * 64 + 56, g_m3_up.Get(), g_m3_up_next, 8);
        g_m3_up_next += 8;
    }
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // what the build reads instances in
    cl->ResourceBarrier(1, &br);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = nullptr; // the game's build of this TLAS, our BLAS builds, the scratch's last use
    cl->ResourceBarrier(1, &uav);
    in.InstanceDescs = sl.inst->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.DestAccelerationStructureData = t.dest;
    bd.ScratchAccelerationStructureData = sl.scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    cl->ResourceBarrier(1, &uav); // complete before the rays read it
    M3Marker(cl.Get(), patches.empty() ? L"wwmi rt-m3: game's TLAS rebuilt back; the game's rays follow"
                                       : L"wwmi rt-m3: TLAS with our BLAS; the game's rays follow");
    RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "rt-m3 tlas");
    return true;
}

// ---- the TLAS gate (rt_m3=1) ---------------------------------------------------------------------------------------
// The game writes a TLAS's instances after recording -- even after submitting -- its build; by the time the rays are
// recorded they are final (7f1c9af..859fc4b tests), and rebuilding the TLAS in place hangs the GPU even unchanged
// (rt_m3=3 test). So the list holding the TLAS build is made to wait, on the GPU, for a fence of ours (a queue Wait put
// in front of its ExecuteCommandLists); at the rays (CPU) the instances naming a character's BLAS are pointed at ours
// and their records at our buffers, and then the fence is signaled: the game's own build, where and when it always
// runs, reads them. Released at the latest at the next present or the next gated TLAS (a frame without rays). g_m3_m.
ComPtr<ID3D12Fence> g_gate;
struct AsCheck {
    ID3D12Resource* res;
    uint64_t        va, as;
};
std::vector<AsCheck> g_m3_check; // instance addresses we wrote this frame (checked at present: did the game overwrite?)
uint64_t             g_m3_overwritten = 0;
uint64_t            g_gate_next = 0, g_gate_done = 0; // last value handed out / signaled
uint64_t            g_gate_count = 0, g_gate_early = 0;
uint64_t            g_m3_unsubmitted = 0; // TLAS written at the rays before its list was even submitted (no gate needed)
uint64_t            g_m3_rays = 0;        // DispatchRays seen with M3 live

void M3ReleaseGate(uint64_t value) // g_m3_m held: every gate up to `value` opens
{
    if (!g_gate || value <= g_gate_done) return;
    g_gate_done = value;
    g_gate->Signal(value);
}
bool M3GateClosed(const TlasRec& t)
{
    return g_gate && t.gate && t.gate > g_gate_done;
}

// ExecuteCommandLists of a list holding a TLAS build of this frame: put the gate in front of it.
void M3GateSubmit(command_queue* queue, TlasRec& t)
{
    if (M3Mode() != 1 || !queue || t.gate || t.decided) return;
    bool any = false;
    for (const auto& [game, mb] : g_m3) any = any || (mb.ok && int32_t(g_frame_id - mb.built) <= int32_t(kM3Window));
    if (!any) return;
    if (!g_gate && (!g_d3d12_device || FAILED(g_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gate))))) return;
    // an older gate still closed (its rays never came): open it first, it cannot be waited for any longer
    if (g_gate_next > g_gate_done) {
        ++g_gate_early;
        M3ReleaseGate(g_gate_next);
    }
    auto* q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    if (!q) return;
    const uint64_t v = ++g_gate_next;
    if (FAILED(q->Wait(g_gate.Get(), v))) {
        --g_gate_next;
        return;
    }
    t.gate = v;
    ++g_gate_count;
}

// M4 (observe): what a character's material records point at -- the local SRV table (+64: the vertex factory's buffers,
// then the material's textures, in t-register order) and the local CBVs (+80 on: cb0.., one is LocalVF) -- once per
// character, geometry and ray type. g_m3_m held.
void M4Observe(device* dev, uint64_t game, const ModBlas& mb, const RecTx& tx, uint64_t stride)
{
    static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> s_seen;
    if (!dev || s_seen.size() >= 96) return;
    for (uint32_t i = 0; i < mb.geoms * 3 && (i + 1) * stride <= tx.orig.size(); ++i) {
        const uint8_t* r = tx.orig.data() + size_t(i) * stride;
        uint64_t       tab = 0, smp = 0;
        memcpy(&tab, r + 64, 8);
        memcpy(&smp, r + 72, 8);
        if (!tab || !s_seen.insert({ game, i / 3, i % 3 }).second) continue;
        const TableStart ts = TableStartOf(dev, descriptor_table{ tab });
        std::string      slots;
        for (uint32_t k = 0; ts && k < 16 && slots.size() < 700; ++k) {
            ResInfo ri{};
            if (!g_heap_to_res.find({ ts.heap, ts.base + k }, ri) || !ri.view_handle) {
                slots += " [" + std::to_string(k) + ": ?]";
                continue;
            }
            const resource_view_desc vd = dev->get_resource_view_desc(resource_view{ ri.view_handle });
            char b[128];
            snprintf(b, sizeof b, " [%u: %s fmt %u va %llx n %llu%s]", k, vd.type == resource_view_type::buffer ? "buf" : "tex", uint32_t(vd.format),
                (unsigned long long)ri.cached_va, (unsigned long long)(vd.type == resource_view_type::buffer ? vd.buffer.size : 0),
                rtskin::RingKey(ri.cached_va) ? " RING" : "");
            slots += b;
        }
        std::string cbs;
        for (uint64_t o = 80; o + 8 <= stride && o < 128; o += 8) {
            uint64_t va = 0;
            memcpy(&va, r + o, 8);
            if (!va) break;
            ID3D12Resource* res = nullptr;
            uint64_t        rva = 0;
            uint32_t        w[6] = {};
            const bool      rd = ResOfVa(resource{ 0 }, va, &res, &rva) && GuardedPrivateRead(res, va, reinterpret_cast<uint8_t*>(w), 24) == 24;
            char            b[160];
            if (rd)
                snprintf(b, sizeof b, " cb%u=%llx {%x %x %x %x %x %x}", uint32_t((o - 80) / 8), (unsigned long long)va, w[0], w[1], w[2], w[3], w[4], w[5]);
            else
                snprintf(b, sizeof b, " cb%u=%llx", uint32_t((o - 80) / 8), (unsigned long long)va);
            cbs += b;
        }
        g_log.Line(true, "RTM4 table game=%llx geometry %u ray type %u: heap=%llx base=%u%s", (unsigned long long)game, i / 3, i % 3,
            (unsigned long long)ts.heap, ts.base, slots.c_str());
        g_log.Line(true, "RTM4 cbv   game=%llx geometry %u ray type %u: sampler table %llx%s", (unsigned long long)game, i / 3, i % 3,
            (unsigned long long)smp, cbs.c_str());
    }
}

// M4: the vertex factory slots of a character's material records -- the leading buffer views of each record's local
// SRV table (UVs R16G16_FLOAT / R32G32_FLOAT, tangents R16G16B16A16_SNORM, colors R8G8B8A8) -- rewritten in place, in the
// game's (this frame's) descriptor heap, to the mod's UVs / posed tangents / colors, so a hit on our BLAS reads the
// mod's attributes at the mod's vertex indices. Only once the records and instances are ours. rt_m4 = 0: off. g_m3_m.
uint64_t g_m4_tables = 0, g_m4_slots = 0;
uint64_t g_m4_tex_mod = 0, g_m4_tex_swapped = 0, g_m4_tex_norule = 0, g_m4_tex_nosrv = 0; // texture slots seen
uint64_t g_m4_tex_lod = 0;  // ...of them, through the target a streamed-out lower mip stands for
uint64_t g_m4_tex_rsw = 0;  // ...given the mod texture the core placed in the raster's slot of the same game texture
uint64_t g_m4_tex_slot = 0; // ...given the mod's texture its raster draw stamps over the same game texture
// Slot-style mods (`ps-tN = ref ResourceTexture_X` in their component sections, no hash rule): what each raster draw of
// a mod component stamped over which game texture (section_ps_stamp.cpp), so the material table of the same section
// gets the same texture. (mod, comp, game resource) -> the mod's view. Own mutex: noted from the draw threads.
struct SlotTex {
    D3D12_CPU_DESCRIPTOR_HANDLE src{};
    ID3D12Resource*             res = nullptr;
    uint32_t                    frame = 0;
    unsigned                    gen = 0;
};
std::mutex                                                g_slot_tex_m;
std::map<std::tuple<int, int, uint64_t>, SlotTex>         g_slot_tex;
uint64_t                                                  g_slot_tex_notes = 0;
constexpr uint32_t                                        kSlotTexAge = 600; // frames a stamp stays usable
// Game textures a raster draw of the mod reads as the game bound them (no stamp): (mod, game resource) -> frame.
std::map<std::pair<int, uint64_t>, uint32_t> g_raster_kept;
// ...and those whose slot the core gave a mod texture: (mod, game resource) -> (mod resource, frame)
std::map<std::pair<int, uint64_t>, std::pair<ID3D12Resource*, uint32_t>> g_raster_swap;
bool RasterSwapFor(int mod, ID3D12Resource* game, D3D12_CPU_DESCRIPTOR_HANDLE* out)
{
    ID3D12Resource* mr = nullptr;
    {
        std::lock_guard l(g_slot_tex_m);
        const auto      it = g_raster_swap.find({ mod, reinterpret_cast<uint64_t>(game) });
        if (it == g_raster_swap.end() || g_frame_id - it->second.second > kSlotTexAge) return false;
        mr = it->second.first;
    }
    ModLoader::TexEntry e;
    if (!ModLoader::TexEntryAt(ModLoader::TexIndexOf(mr), &e) || !e.srv.ptr || e.res != mr) return false;
    *out = e.srv;
    return true;
}
bool RasterKept(int mod, ID3D12Resource* game)
{
    std::lock_guard l(g_slot_tex_m);
    const auto      it = g_raster_kept.find({ mod, reinterpret_cast<uint64_t>(game) });
    return it != g_raster_kept.end() && g_frame_id - it->second <= kSlotTexAge;
}
std::string TexDesc(ID3D12Resource* r)
{
    if (!r) return "null";
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    char                      b[64];
    snprintf(b, sizeof b, "f%u %llux%u m%u", unsigned(d.Format), (unsigned long long)d.Width, d.Height, unsigned(d.MipLevels));
    return b;
}
bool SlotTexFor(int mod, int comp, ID3D12Resource* game, D3D12_CPU_DESCRIPTOR_HANDLE* out)
{
    std::lock_guard l(g_slot_tex_m);
    const unsigned  gen = PortApi::LoadGeneration();
    const SlotTex*  best = nullptr;
    const auto      it = g_slot_tex.find({ mod, comp, reinterpret_cast<uint64_t>(game) });
    if (it != g_slot_tex.end()) best = &it->second;
    else // another component of the mod stamping the same game texture
        for (const auto& [k, v] : g_slot_tex)
            if (std::get<0>(k) == mod && std::get<2>(k) == reinterpret_cast<uint64_t>(game) && (!best || v.frame > best->frame)) best = &v;
    if (!best || best->gen != gen || g_frame_id - best->frame > kSlotTexAge || !best->src.ptr) return false;
    *out = best->src;
    return true;
}
int      M4Mode()
{
    static int s = -1;
    if (s < 0 && !g_addon_ini_path.empty()) s = GetPrivateProfileIntW(L"wwmi-dx12", L"rt_m4", 1, g_addon_ini_path.c_str()) != 0 ? 1 : 0;
    return s < 0 ? 1 : s;
}
void M4Apply(device* dev, const ModBlas& mb, const RecTx& tx, uint64_t stride, uint64_t game)
{
    if (!dev || !M4Mode() || !g_d3d12_device) return;
    static uint32_t                                s_frame = 0;
    static std::set<std::pair<uint64_t, uint32_t>> s_done; // tables rewritten this frame (shared by ray types)
    if (s_frame != g_frame_id) {
        s_frame = g_frame_id;
        s_done.clear();
    }
    const UINT inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t i = 0; i < mb.geoms * 3 && (i + 1) * stride <= tx.mine.size(); ++i) {
        const uint32_t g = i / 3;
        if (g >= mb.pose_mod.size() || mb.pose_mod[g] < 0) continue; // a section the mod does not draw
        uint64_t tab = 0;
        memcpy(&tab, tx.mine.data() + size_t(i) * stride + 64, 8);
        if (!tab) continue;
        const TableStart ts = TableStartOf(dev, descriptor_table{ tab });
        if (!ts || !s_done.insert({ ts.heap, ts.base }).second) continue;
        auto*          heap = ts.Heap();
        const SIZE_T   cpu0 = heap->GetCPUDescriptorHandleForHeapStart().ptr;
        rtskin::VfViews v{};
        uint32_t        vfmt = 0;
        std::string     what;
        uint32_t k = 0;
        for (; k < 8; ++k) { // the vertex factory's buffers lead the table; the first texture ends them
            ResInfo ri{};
            if (!g_heap_to_res.find({ ts.heap, ts.base + k }, ri) || !ri.view_handle) break;
            const resource_view_desc vd = dev->get_resource_view_desc(resource_view{ ri.view_handle });
            if (vd.type != resource_view_type::buffer) break;
            const uint32_t fmt = uint32_t(vd.format);
            const bool     uv = fmt == uint32_t(DXGI_FORMAT_R16G16_FLOAT) || fmt == uint32_t(DXGI_FORMAT_R32G32_FLOAT);
            if (uv && vfmt != fmt) {
                rtskin::MakeVfViews(mb.pose_mod[g], mb.pose_slot[g], fmt, &v);
                vfmt = fmt;
            } else if (!vfmt) {
                rtskin::MakeVfViews(mb.pose_mod[g], mb.pose_slot[g], 0, &v);
                vfmt = 1;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE src{};
            const char*                 tag = nullptr;
            if (uv) src = v.uv, tag = "uv";
            else if (fmt == uint32_t(DXGI_FORMAT_R16G16B16A16_SNORM)) src = v.tan, tag = "tan";
            else if (fmt == uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM) || fmt == uint32_t(DXGI_FORMAT_B8G8R8A8_UNORM)) src = v.col, tag = "col";
            if (!src.ptr) continue;
            g_d3d12_device->CopyDescriptorsSimple(1, { cpu0 + SIZE_T(ts.base + k) * inc }, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ++g_m4_slots;
            what += " ";
            what += std::to_string(k);
            what += "=";
            what += tag;
        }
        // then the material's textures, up to the next table's first buffer: the game's, unless the core swapped the
        // descriptor at placement; one a mod replaces (by hash rule) gets the mod's texture, as the raster draws it
        uint32_t tex_mod = 0, tex_sw = 0, tex_none = 0, tex_nosrv = 0, tex_slot = 0, tex_lod = 0, tex_rsw = 0;
        std::string detail; // per texture slot: what it holds and what became of it (logged once per table layout)
        for (uint32_t end = k + 24; what.size() && k < end; ++k) {
            ResInfo ri{};
            if (!g_heap_to_res.find({ ts.heap, ts.base + k }, ri) || !ri.view_handle) break;
            const resource_view_desc vd = dev->get_resource_view_desc(resource_view{ ri.view_handle });
            if (vd.type == resource_view_type::buffer || vd.type == resource_view_type::unknown) break;
            // a slot the core already gave a mod texture (placement swap in this table): as it is
            if (ri.swapped && ri.swapped != ri.res) {
                ++tex_mod;
                if (detail.size() < 1400) {
                    char b[64];
                    snprintf(b, sizeof b, " [%u %p ALREADY-SWAPPED]", k, (void*)ri.swapped);
                    detail += b;
                }
                continue;
            }
            ID3D12Resource* res = ri.res; // the game's texture
            if (!res) continue;
            bool target; // a texture some mod replaces (g_modtex_res: GAME resources the mods target, not mod textures)
            {
                std::shared_lock l(g_tex_mutex);
                target = g_modtex_res.count(reinterpret_cast<uint64_t>(res)) != 0;
            }
            const uint32_t hash = TexHashOf(reinterpret_cast<uint64_t>(res));
            if (detail.size() < 1400) {
                char b[64];
                snprintf(b, sizeof b, " [%u %p h%08x ", k, (void*)res, hash);
                detail += b;
                detail += TexDesc(res);
                detail += target ? " target" : "";
            }
            const auto put = [&](D3D12_CPU_DESCRIPTOR_HANDLE src, uint32_t& counter, const char* how) {
                g_d3d12_device->CopyDescriptorsSimple(1, { cpu0 + SIZE_T(ts.base + k) * inc }, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                ++counter;
                if (detail.size() < 1400) detail += how;
            };
            // 1. what the raster's draw of this section binds over this game texture at draw time (section ps-tN stamps,
            //    RabbitFX maps): it wins over placement in the raster too
            {
                D3D12_CPU_DESCRIPTOR_HANDLE st{};
                if (SlotTexFor(mb.pose_mod[g], int(g), res, &st)) {
                    put(st, tex_slot, " SLOT]");
                    continue;
                }
            }
            // 2. the raster's placement resolution (texture_module.cpp GlobalTexSwap / draw_texswap.cpp): the hash selected
            //    in the mod's namespace, the global selection, or the target it is a lower mip of
            if (hash) {
                const int      dm = mb.pose_mod[g];
                const wchar_t* own_ns = dm >= 0 && size_t(dm) < g_geomods.size() && !g_geomods[size_t(dm)].wcl_ns.empty()
                    ? g_geomods[size_t(dm)].wcl_ns.c_str() : nullptr;
                ModLoader::TexEntry e;
                if (ModLoader::TexEntryAt(TexSelectIdxNs(hash, own_ns), &e) && e.srv.ptr) {
                    put(e.srv, tex_sw, " RULE]");
                    continue;
                }
                if (const uint32_t rule = TexRuleHashFor(hash)) {
                    const D3D12_CPU_DESCRIPTOR_HANDLE src = TexReplacement(rule);
                    if (src.ptr) {
                        put(src, tex_sw, " RULE-GLOBAL]");
                        continue;
                    }
                }
                if (const uint32_t tgt = TexLodTargetOf(hash); tgt && ModLoader::TexEntryAt(TexSelectIdxNs(tgt, own_ns), &e) && e.srv.ptr) {
                    ++tex_lod;
                    put(e.srv, tex_sw, " RULE-LOWER-MIP]");
                    continue;
                }
            }
            // 3. the mod texture the core placed in the raster's slot of the same game texture
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rs{};
                if (RasterSwapFor(mb.pose_mod[g], res, &rs)) {
                    put(rs, tex_rsw, " RASTER-SWAP]");
                    continue;
                }
            }
            if (target) {
                ++tex_nosrv;
                if (detail.size() < 1400) detail += " target-unresolved]";
                continue;
            }
            ++tex_none;
            if (detail.size() < 1400) detail += RasterKept(mb.pose_mod[g], res) ? " kept-by-raster]" : " NOT-IN-RASTER]";
        }
        g_m4_tex_mod += tex_mod;
        g_m4_tex_swapped += tex_sw;
        g_m4_tex_norule += tex_none;
        g_m4_tex_nosrv += tex_nosrv;
        g_m4_tex_slot += tex_slot;
        g_m4_tex_lod += tex_lod;
        g_m4_tex_rsw += tex_rsw;
        if (!what.empty()) {
            char t[128];
            snprintf(t, sizeof t, " | textures: %u already swapped, %u by rule, %u from slot stamps, %u game's, %u target unresolved", tex_mod, tex_sw,
                tex_slot, tex_none, tex_nosrv);
            what += t;
            ++g_m4_tables;
            {
                static std::set<std::tuple<int, uint32_t, uint32_t, uint32_t>> s_seen; // (mod, geometry, ray type, texture count)
                if (s_seen.size() < 256 && s_seen.insert({ mb.pose_mod[g], g, i % 3, tex_mod + tex_sw + tex_slot + tex_none + tex_nosrv }).second)
                    g_log.Line(true, "RTM4 tex mod=%d geometry %u ray type %u table %llx+%u:%s", mb.pose_mod[g], g, i % 3, (unsigned long long)ts.heap, ts.base,
                        detail.c_str());
            }
            static std::atomic<uint32_t> s_log{ 0 };
            if (s_log.fetch_add(1) < 24)
                g_log.Line(true, "RTM4 vf game=%llx geometry %u ray type %u: table %llx+%u ->%s (mod %d slot %d, %u uv channel(s))", (unsigned long long)game, g,
                    i % 3, (unsigned long long)ts.heap, ts.base, what.c_str(), mb.pose_mod[g], mb.pose_slot[g], v.uv_channels);
        }
    }
}

// DispatchRays (rt_m3=1). The TLAS these rays trace (the newest built) is still gated: every instance naming a
// character's BLAS whose records are the character's (and of a kind whose records were) is pointed at our BLAS in the
// game's own instance memory, its records at our buffers -- all or none -- and the gate opens. A TLAS whose gate is
// already open only gets the records of the instances that name our BLAS. g_m3_m held by the caller? no: taken here.
void M3Redirect(command_list* cmd_list, resource hit_res, uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride)
{
    if (!M3On() || !g_m3_any.load(std::memory_order_relaxed) || hit_stride < 64 || !cmd_list) return;
    const uint32_t  frame = g_frame_id;
    std::lock_guard l(g_m3_m);
    if (M3Mode() == 2) return; // diagnostic: our BLAS built, nothing redirected
    const TlasRec* tp = nullptr; // the TLAS these rays trace: the newest built
    for (const TlasRec& r : g_m3_tlas)
        if (r.res && r.dest && (!tp || r.seq > tp->seq)) tp = &r;
    if (!tp || int32_t(frame - tp->frame) > 1) return;
    TlasRec& t = const_cast<TlasRec&>(*tp);
    if (M3Mode() == 3) {
        // diagnostic: the TLAS rebuilt in place from the game's instances as they are, once per TLAS build
        if (g_m3_rebuilt_seq == t.seq) return;
        const char* w = nullptr;
        if (M3RebuildTlas(cmd_list, t, {}, &w)) {
            g_m3_rebuilt_seq = t.seq;
            ++g_m3_rebuilds;
        } else
            M3Refuse(w);
        return;
    }
    ID3D12Resource* rd = nullptr;
    uint64_t        rd_va = 0;
    const char*     why = nullptr;
    CopyRec         copy{};
    const bool      src = M3HitSource(cmd_list, hit_res, hit_off, hit_size, frame, &rd, &rd_va, &why, &copy);
    const bool      closed = M3GateClosed(t);
    // the GPU has not read this TLAS's instances yet: its build waits at our gate, or its list is not even submitted
    const bool      writable = closed || (!t.executed && !t.gate && !t.decided);
    ++g_m3_rays;
    struct Cover {
        uint32_t found = 0, good = 0;
        bool     live = false;
    };
    std::map<ModBlas*, Cover> cover;
    struct Cand {
        ModBlas* mb;
        uint32_t idx;
        uint64_t game;
        int      sig;
    };
    std::vector<Cand>  cands;    // to point at our BLAS (gate closed)
    std::vector<RecTx> txs;      // records to make ours: the candidates', then those of instances naming ours already
    std::vector<std::pair<ModBlas*, uint64_t>> tx_owner; // each RecTx's character (M4)
    std::vector<Cand>  stale;    // instances naming ours whose records cannot follow
    std::vector<Cand>  mine;     // instances naming ours whose records can (their RecTx are in txs too)
    for (auto& [game, mb] : g_m3) {
        if (!mb.blas || !mb.ib) continue;
        // (not within 3 frames of a topology change: a rays list recorded meanwhile could pair the new records with the
        // old BLAS)
        const bool     live = mb.ok && int32_t(frame - mb.built) <= int32_t(kM3Window) && int32_t(frame - mb.topo_frame) >= 3;
        const uint64_t ours = mb.blas->GetGPUVirtualAddress();
        const auto     found = FindInstances(t, game, ours);
        Cover&         cv = cover[&mb];
        cv.live = live;
        cv.found = uint32_t(found.size());
        if (live && found.empty()) M3Refuse("rays: a live character's BLAS is named by no instance of the TLAS");
        if (!live && !found.empty())
            M3Refuse(!mb.ok                                     ? "rays: instance kept the game's (our BLAS not built for it this time)"
                     : int32_t(frame - mb.built) > int32_t(kM3Window) ? "rays: instance kept the game's (our BLAS not built in the last frames)"
                                                                      : "rays: instance kept the game's (topology changed under 3 frames ago)");
        for (uint32_t idx : found) {
            uint8_t inst[64];
            if (GuardedPrivateRead(t.res, t.va + uint64_t(idx) * 64, inst, 64) != 64) continue;
            uint64_t as;
            memcpy(&as, inst + 56, 8);
            const uint32_t contrib = inst[52] | inst[53] << 8 | inst[54] << 16;
            const int      sig = inst[51] << 8 | inst[55];
            mb.contrib = contrib;
            RecTx      tx;
            const bool valid = src && M3Records(mb, rd, rd_va, hit_size, hit_stride, contrib, &tx);
            if (valid && as != ours) M4Observe(cmd_list->get_device(), game, mb, tx, hit_stride);
            if (as == ours) {
                // already ours (this frame's first dispatch, or left from an earlier frame): its records must follow
                if (valid) {
                    txs.push_back(std::move(tx));
                    tx_owner.push_back({ &mb, game });
                    mine.push_back({ &mb, idx, game, sig });
                } else
                    stale.push_back({ &mb, idx, game, sig });
                continue;
            }
            if (!live) continue;
            if (!writable) {
                M3Refuse("rays: instance kept the game's (its TLAS was already released to the GPU)");
                continue;
            }
            if (!valid) {
                M3Refuse(src ? "an instance's records are not the character's (it keeps the game's BLAS)" : why);
                continue;
            }
            cands.push_back({ &mb, idx, game, sig });
            txs.push_back(std::move(tx));
            tx_owner.push_back({ &mb, game });
        }
    }
    auto write_as = [&](const Cand& c, uint64_t as) -> bool {
        uint8_t* p = MapUpload(t.res, t.va + uint64_t(c.idx) * 64 + 56, 8);
        if (!p) return false;
        const bool ok = GuardedCopy(p, &as, 8);
        UnmapUpload(t.res);
        return ok;
    };
    // every instance naming our BLAS back to the game's (the TLAS has not read them yet): nothing of ours goes in
    auto all_back = [&]() {
        for (const Cand& c : stale) write_as(c, c.game);
        for (const Cand& c : mine) write_as(c, c.game);
        cands.clear();
        txs.clear();
        tx_owner.clear();
        mine.clear();
        ++g_m3_restores;
    };
    bool backed = false;
    if (!stale.empty()) {
        if (writable) {
            all_back();
            backed = true;
        }
        else {
            static std::atomic<uint32_t> s_warn{ 0 };
            if (s_warn.fetch_add(1) < 16)
                g_log.Line(true, "RTM3 WARNING f=%u: %zu instance(s) of an already built TLAS name our BLAS and their records cannot follow", frame,
                    stale.size());
        }
    }
    bool redirected = false, committed = false;
    if (!txs.empty() && M3CommitRecs(txs)) {
        committed = true;
        size_t done = 0;
        for (; done < cands.size(); ++done)
            if (!write_as(cands[done], cands[done].mb->blas->GetGPUVirtualAddress())) break;
        if (done < cands.size()) {
            // an instance write failed: the written ones back, and the records back
            for (size_t i = 0; i < done; ++i) write_as(cands[i], cands[i].game);
            for (const RecTx& tx : txs) M3WriteRecs(tx, false);
            committed = false;
            M3Refuse("an instance write failed (nothing redirected)");
        } else
            redirected = !cands.empty();
    } else if (!txs.empty()) {
        // the records could not all be written (the written ones put back)
        if (writable) {
            all_back();
            backed = true;
        }
        else {
            static std::atomic<uint32_t> s_warn{ 0 };
            if (s_warn.fetch_add(1) < 16)
                g_log.Line(true, "RTM3 WARNING f=%u: records of an already built TLAS's instances naming our BLAS could not be written", frame);
        }
        cands.clear();
    }
    if (redirected) {
        ++g_m3_rebuilds; // (TLAS builds that read our BLAS)
        for (const Cand& c : cands) {
            ModBlas& mb = *c.mb;
            if (g_m3_check.size() < 64) g_m3_check.push_back({ t.res, t.va + uint64_t(c.idx) * 64 + 56, mb.blas->GetGPUVirtualAddress() });
            if (mb.redirected != frame) ++g_m3_redirects;
            mb.redirected = frame;
            mb.patched = frame;
            mb.redirected_seq = t.seq;
        }
        static std::atomic<uint32_t> s_first{ 0 };
        if (s_first.fetch_add(1) < 4)
            g_log.Line(true, "RTM3 redirect f=%u: TLAS %llx (%u instances, gate %llu) reads our BLAS for %zu instance(s); records patched", frame,
                (unsigned long long)t.dest, t.count, (unsigned long long)t.gate, cands.size());
    }
    // M4: the committed records' vertex factory slots (txs: one RecTx per candidate / instance already ours, each now
    // ours; tx_owner: its character)
    if (!backed && committed) {
        size_t ti = 0;
        for (const RecTx& tx : txs) {
            const ModBlas* owner = ti < tx_owner.size() ? tx_owner[ti].first : nullptr;
            if (owner) M4Apply(cmd_list->get_device(), *owner, tx, hit_stride, tx_owner[ti].second);
            ++ti;
        }
    }
    // coverage: which characters' instances all trace our BLAS with our records after this
    if (!backed && committed) {
        for (const Cand& c : mine) ++cover[c.mb].good;
        for (const Cand& c : cands) ++cover[c.mb].good;
    }
    for (auto& [mb, cv] : cover) {
        if (!cv.live) continue;
        ++mb->rays_live;
        if (cv.found) ++mb->rays_found;
        if (cv.found && cv.good == cv.found) ++mb->rays_covered;
    }
    if (closed)
        M3ReleaseGate(t.gate); // the game's TLAS build may run now
    else if (writable) {
        t.decided = true; // written before its list was submitted: no gate when it is
        if (redirected) ++g_m3_unsubmitted;
    }
}

// RTM3SUM / RTM3CHAR / RTM3WHY lines (every summary).
void M3Summary()
{
    if (!M3On()) {
        g_log.Line(true, "RTM3SUM off (rt_m3=0)");
        return;
    }
    std::lock_guard l(g_m3_m);
    if (M3Mode() != 1) {
        g_log.Line(true, "RTM3SUM DIAGNOSTIC rt_m3=%d%s builds=%llu updates=%llu tlas_rebuilds=%llu", M3Mode(),
            M3Mode() == 2 ? " (our BLAS only)" : " (game TLAS rebuilt in place, unchanged)", (unsigned long long)g_m3_builds,
            (unsigned long long)g_m3_updates, (unsigned long long)g_m3_rebuilds);
        return;
    }
    size_t   ok = 0;
    uint64_t live = 0, found = 0, covered = 0;
    for (const auto& [g, mb] : g_m3) {
        ok += mb.ok ? 1 : 0;
        live += mb.rays_live;
        found += mb.rays_found;
        covered += mb.rays_covered;
    }
    g_log.Line(true,
        "RTM3SUM chars=%zu/%zu builds=%llu updates=%llu rays=%llu coverage=%llu/%llu (instance found %llu) redirects=%llu tlas_redirected=%llu "
        "(before submit %llu) restores=%llu gates=%llu (opened without rays %llu) overwritten=%llu grave=%zu scans=%llu (avg %llu us, max %llu us, slow path %llu)",
        ok, g_m3.size(), (unsigned long long)g_m3_builds, (unsigned long long)g_m3_updates, (unsigned long long)g_m3_rays,
        (unsigned long long)covered, (unsigned long long)live, (unsigned long long)found, (unsigned long long)g_m3_redirects,
        (unsigned long long)g_m3_rebuilds, (unsigned long long)g_m3_unsubmitted, (unsigned long long)g_m3_restores,
        (unsigned long long)g_gate_count, (unsigned long long)g_gate_early, (unsigned long long)g_m3_overwritten, g_m3_grave.size(),
        (unsigned long long)g_scan_n, (unsigned long long)(g_scan_n ? g_scan_us / g_scan_n : 0), (unsigned long long)g_scan_max_us,
        (unsigned long long)g_scan_slow);
    for (const auto& [g, mb] : g_m3)
        if (mb.ok || mb.rays_live)
            g_log.Line(true, "RTM3CHAR game=%llx ok=%d contrib=%d geometries=%u coverage=%llu/%llu found=%llu last_redirect=%d frame(s) ago builds=%u",
                (unsigned long long)g, mb.ok ? 1 : 0, mb.contrib == UINT32_MAX ? -1 : int(mb.contrib), mb.geoms,
                (unsigned long long)mb.rays_covered, (unsigned long long)mb.rays_live, (unsigned long long)mb.rays_found,
                mb.redirected ? int(g_frame_id - mb.redirected) : -1, mb.builds);
    for (const auto& [why, n] : g_m3_why) g_log.Line(true, "RTM3WHY %llu: %s", (unsigned long long)n, why.c_str());
    size_t slot_maps = 0;
    uint64_t slot_notes = 0;
    {
        std::lock_guard l(g_slot_tex_m);
        slot_maps = g_slot_tex.size();
        slot_notes = g_slot_tex_notes;
    }
    g_log.Line(true, "RTM4SUM rt_m4=%d tables rewritten=%llu slots=%llu textures: already swapped %llu, by rule %llu (lower mip %llu), as the raster's slots %llu, from slot stamps %llu, game's %llu, "
        "target unresolved %llu | slot stamps noted %llu (%zu game->mod pairs)",
        M4Mode(), (unsigned long long)g_m4_tables, (unsigned long long)g_m4_slots, (unsigned long long)g_m4_tex_mod, (unsigned long long)g_m4_tex_swapped,
        (unsigned long long)g_m4_tex_lod, (unsigned long long)g_m4_tex_rsw, (unsigned long long)g_m4_tex_slot, (unsigned long long)g_m4_tex_norule, (unsigned long long)g_m4_tex_nosrv, (unsigned long long)slot_notes, slot_maps);
    std::string fw;
    for (const auto& [why, n] : g_m3_full_why) fw += " | " + std::to_string(n) + " " + why;
    if (!fw.empty()) g_log.Line(true, "RTM3FULL full BLAS builds by reason:%s", fw.c_str());
}

} // namespace

// ==== rt_hooks.h =================================================================================
bool RtModulePresent()
{
    return Mode() != 0;
}

bool RtHoldVanilla()
{
    return RtActive() && Mode() == 1; // M2 holds per draw (RtClaimNote), never the whole frame
}

void RtNoteCopy(command_list* cmd_list, resource src, uint64_t src_off, resource dst, uint64_t dst_off, uint64_t size)
{
    if (Mode() != 2 || !(g_have_char_inst.load(std::memory_order_relaxed) || g_m3_any.load(std::memory_order_relaxed)) || size < 256 ||
        size > (8ull << 20) || !src.handle)
        return;
    CopyRec r;
    r.cl = cmd_list;
    r.gen = CmdGen(cmd_list);
    r.seq = g_rec_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    r.dst_va = BufferVa(dst, dst_off);
    r.size = size;
    r.src = reinterpret_cast<ID3D12Resource*>(src.handle);
    r.src_va = BufferVa(src, src_off);
    r.frame = g_frame_id;
    std::lock_guard l(g_copy_m);
    g_copies[g_copy_next++ & 511] = r;
}

void RtOnDeviceRemoved()
{
    // the observe log is written at present, which no longer comes: the essentials straight into ReShade.log
    std::string m3;
    {
        std::unique_lock l(g_m3_m, std::try_to_lock);
        if (l.owns_lock()) {
            char b[256];
            snprintf(b, sizeof b, "rt_m3=%d builds=%llu updates=%llu redirected_frames=%llu tlas_rebuilds=%llu restores=%llu last_rebuilt_tlas_seq=%u",
                M3Mode(), (unsigned long long)g_m3_builds, (unsigned long long)g_m3_updates, (unsigned long long)g_m3_redirects,
                (unsigned long long)g_m3_rebuilds, (unsigned long long)g_m3_restores, g_m3_rebuilt_seq);
            m3 = b;
        } else
            m3 = "(M3 lock held)";
    }
    ReshadeLog(reshade::log::level::error, "wwmi-dx12 RT module at device removal, frame %u: %s", g_frame_id, m3.c_str());
    {
        std::lock_guard l(g_m3_m);
        M3ReleaseGate(g_gate_next);
    }
}

void RtNoteExecute(command_queue* queue, command_list* cmd_list)
{
    if (!cmd_list || Mode() != 2 || !g_m3_any.load(std::memory_order_relaxed)) return;
    const uint64_t gen = CmdGen(cmd_list);
    // g_m3_m held throughout: a redirect checking and writing on another thread finishes before this list goes
    std::lock_guard l(g_m3_m);
    for (TlasRec& r : g_m3_tlas)
        if (r.cl == cmd_list && r.gen == gen && !r.executed) {
            M3GateSubmit(queue, r); // its build waits for the rays' decision
            r.executed = true;
        }
    std::lock_guard l2(g_copy_m);
    for (CopyRec& r : g_copies)
        if (r.cl == cmd_list && r.gen == gen) r.executed = true;
}

void RtNoteRays(command_list* cmd_list, resource rg_res, uint64_t rg_off, uint64_t rg_size, resource miss_res, uint64_t miss_off,
    uint64_t miss_size, uint64_t miss_stride, resource hit_res, uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride, uint32_t w,
    uint32_t h, uint32_t d)
{
    if (Mode() == 0) return;
    NoteRtFrame();
    g_c.rays.fetch_add(1, std::memory_order_relaxed);
    ObserveRays(rg_res, rg_off, rg_size, miss_res, miss_off, miss_size, miss_stride, hit_res, hit_off, hit_size, hit_stride, w, h, d);
    if (Mode() == 2) M3Redirect(cmd_list, hit_res, hit_off, hit_size, hit_stride);
}

// Present: a gate still closed for an earlier frame's TLAS (no rays came for it) opens.
void M3PresentTick()
{
    std::lock_guard l(g_m3_m);
    for (const TlasRec& r : g_m3_tlas)
        if (M3GateClosed(r)) { // the frame's rays were recorded by now
            ++g_gate_early;
            M3ReleaseGate(r.gate);
        }
    // did the game write an instance again after we pointed it at our BLAS? (its records are ours then: a mix)
    for (const AsCheck& c : g_m3_check) {
        uint64_t as = 0;
        if (GuardedPrivateRead(c.res, c.va, reinterpret_cast<uint8_t*>(&as), 8) == 8 && as != c.as) {
            if (g_m3_overwritten++ < 8) g_log.Line(true, "RTM3 WARNING: an instance we pointed at our BLAS was rewritten by the game before present (now %llx)",
                (unsigned long long)as);
        }
    }
    g_m3_check.clear();
}

void RtPresentTick()
{
    if (Mode() == 2) M3PresentTick();
    static bool     s_held = false;
    static bool     s_started = false;
    static uint32_t s_last_summary = 0;
    if (!s_started) {
        s_started = true;
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module M2 is in this build; rt_mode=%d (%s)", Mode(), ModeText(Mode()));
        ReshadeLog(reshade::log::level::info, "wwmi-dx12 RT module M3: rt_m3=%d (%s)", M3Mode(),
            M3Mode() == 0   ? "off"
            : M3Mode() == 1 ? "mod geometry in the ray traced scene"
            : M3Mode() == 2 ? "DIAGNOSTIC: our BLAS built only"
                            : "DIAGNOSTIC: the game's TLAS rebuilt in place, unchanged");
        g_log.Line(true, "RTSTART module M2, rt_mode=%d (%s); waiting for ray tracing (BLAS builds / DispatchRays)", Mode(), ModeText(Mode()));
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
                for (const WwmiMod::Resource& r : mod.resources) {
                    g_log.Line(true, "    res %s stride=%u size=%u va=%llx file=%s", Utf8(r.name).c_str(), r.stride, r.size,
                        (unsigned long long)r.gpu_va, Utf8(r.filename).c_str());
                    // the first bytes of each mesh buffer (to confirm formats), when it lives in an UPLOAD heap
                    if (r.res && r.gpu_va && r.size && r.stride) {
                        D3D12_HEAP_PROPERTIES hp = {};
                        D3D12_HEAP_FLAGS      hf = {};
                        const bool            ok = SUCCEEDED(r.res->GetHeapProperties(&hp, &hf));
                        uint32_t              w[12] = {};
                        const uint32_t        n = ok && hp.Type == D3D12_HEAP_TYPE_UPLOAD ? GuardedPrivateRead(r.res, r.gpu_va, reinterpret_cast<uint8_t*>(w), sizeof w) : 0;
                        g_log.Line(true, "      heap=%d read=%u head=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", ok ? int(hp.Type) : -1, n, w[0],
                            w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
                    }
                    if (!r.filename.empty() && r.stride) { // the same from the file the engine loaded
                        uint32_t fw[12] = {};
                        size_t   fn = 0;
                        if (FILE* f = _wfsopen(r.filename.c_str(), L"rb", _SH_DENYNO)) {
                            fn = fread(fw, 1, sizeof fw, f);
                            fclose(f);
                        }
                        g_log.Line(true, "      file read=%zu head=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", fn, fw[0], fw[1], fw[2], fw[3],
                            fw[4], fw[5], fw[6], fw[7], fw[8], fw[9], fw[10], fw[11]);
                    }
                }
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
    {
        static bool s_drawing = false; // M2: ray tracing on and the module drawing
        const bool  drawing = RtActive() && Mode() == 2;
        if (drawing != s_drawing) {
            g_log.Line(true, drawing ? "RTDRAW ON: the game ray traces -- mod sections drawn with the skin cache palettes"
                                     : "RTDRAW OFF: no ray tracing -- the core draws the mods as always");
            ReshadeLog(reshade::log::level::info, drawing ? "wwmi-dx12 RT module: the game ray traces -- drawing mods with its skin cache (M2)"
                                                          : "wwmi-dx12 RT module: ray tracing stopped");
        }
        s_drawing = drawing;
        // rt_preload = 0: a mod's mesh is loaded only when its character is first drawn (the original shows meanwhile)
        static const bool s_preload = g_addon_ini_path.empty() || GetPrivateProfileIntW(L"wwmi-dx12", L"rt_preload", 1, g_addon_ini_path.c_str()) != 0;
        static size_t     s_preloaded = 0;
        if (drawing && s_preload && g_geomods.size() != s_preloaded) {
            s_preloaded = g_geomods.size();
            rtskin::PreloadAll();
        }
    }
    if (Mode() == 2) rtskin::PresentTick();
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
    // The VS's registers through the layout's register map: t0..t3 (UE's manual vertex fetch buffers) with the view
    // the game put there, and b0..b4 (b2 = LocalVF: VertexFetch_Parameters) with b2's bytes.
    {
        const uint64_t layh = CmdGfxLayout(in.cmd_list);
        RegBind        t_vs[4], b_vs[5];
        bool           have = false;
        {
            std::lock_guard l(g_layout_regmap_mtx);
            auto            it = g_layout_regmap.find(layh);
            if (it != g_layout_regmap.end() && it->second.built) {
                memcpy(t_vs, it->second.t_vs, sizeof t_vs);
                memcpy(b_vs, it->second.b_vs, sizeof b_vs);
                have = true;
            }
        }
        if (!have)
            g_log.Line(true, "  vs registers: no register map for layout %llx", (unsigned long long)layh);
        else {
            device* dv = in.cmd_list->get_device();
            for (unsigned r = 0; r < 4; ++r) {
                const RegBind& b = t_vs[r];
                if (b.param == 0xFF || b.param >= 64) continue;
                if (!b.is_table) {
                    g_log.Line(true, "  vs t%u: root p%u va=%llx", r, b.param, (unsigned long long)st.root[b.param].va);
                    continue;
                }
                const TableStart ts = TableStartOf(dv, st.root[b.param].table);
                ResInfo          ri{};
                if (!ts || !g_heap_to_res.find({ ts.heap, ts.base + b.offset }, ri)) {
                    g_log.Line(true, "  vs t%u: table p%u+%u (slot not tracked)", r, b.param, b.offset);
                    continue;
                }
                const resource_view_desc vd = ri.view_handle ? dv->get_resource_view_desc(resource_view{ ri.view_handle }) : resource_view_desc{};
                bool ring = false;
                {
                    std::lock_guard l(g_m);
                    ring = ri.cached_va && g_reg.IsSkinOut(ri.cached_va, 1);
                }
                g_log.Line(true, "  vs t%u: table p%u+%u res=%llx bytes=%u buffer=%d view type=%u fmt=%u raw_off=%llu raw_size=%llu%s", r, b.param,
                    b.offset, (unsigned long long)ri.cached_va, ri.size, ri.is_buffer ? 1 : 0, uint32_t(vd.type), uint32_t(vd.format),
                    (unsigned long long)vd.buffer.offset, (unsigned long long)vd.buffer.size, ring ? " (ring)" : "");
            }
            for (unsigned r = 0; r < 5; ++r) {
                const RegBind& b = b_vs[r];
                if (b.param == 0xFF || b.param >= 64) continue;
                if (b.is_table) {
                    g_log.Line(true, "  vs b%u: table p%u+%u", r, b.param, b.offset);
                    continue;
                }
                const RootCbvBind& rc = cb.binds[b.param];
                uint32_t           w[8] = {};
                uint32_t           n = 0;
                if (rc.res && rc.va && BufferLive(rc.res)) n = GuardedSnapshot(rc.res, rc.va, reinterpret_cast<uint8_t*>(w), sizeof w);
                g_log.Line(true, "  vs b%u: root p%u va=%llx size=%u read=%u words=%08x %08x %08x %08x %08x %08x %08x %08x", r, b.param,
                    (unsigned long long)rc.va, rc.size, n, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
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
    rtskin::SetClaim({}); // this claim is the thread's current one, whatever it was before
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
    if (Mode() == 2 && in.identified && in.cmd_list && width) {
        // M2: a draw reading a known ring is ours to draw when the mesh and the section's palette are at hand
        const uint64_t vb0 = uint64_t(int64_t(in.vb0) + int64_t(in.vertex_offset) * int64_t(in.vb0_stride));
        const VertexBind vb4 = CmdGfx(in.cmd_list).vb[4];
        const uint64_t   prev = vb4.va && vb4.stride == 12 ? vb4.va : 0; // ATTRIBUTE14: the previous frame's ring
        uint64_t         reg_ring = 0;
        {
            std::lock_guard l(g_m);
            if (const rtm::Ring* r = g_reg.RingAt(vb0, 1)) reg_ring = r->va;
        }
        // the ring: VB0 itself when the skin cache wrote it (no need to wait for the registry to learn it), else the
        // registry's ring around it
        uint64_t       ring = 0;
        const char*    why = nullptr;
        const uint64_t key = rtskin::RingKey(vb0) ? rtskin::RingKey(vb0) : (reg_ring ? rtskin::RingKey(reg_ring) : 0);
        rtskin::RequestMesh(in.mod);
        if (rtskin::MeshFailed(in.mod)) why = "the module cannot draw this mod (see RTM2 mesh FAILED)";
        else if (!rtskin::MeshReady(in.mod)) why = "the mod's mesh is not loaded (yet)";
        else if (key && rtskin::SectionKnown(key, in.comp, in.mod)) ring = key;
        else if (rtskin::PoseCached(in.mod, in.comp)) ring = key ? key : vb0; // our last pose of it
        else if (!key && !reg_ring) why = "VB0 is not a skin cache ring the module saw posed, and no earlier pose";
        else why = "no complete skin cache layout / palette for this section this frame, and no earlier pose";
        if (why) {
            NoteHeld(in.mod, in.comp, why);
            // the first holds in detail (what the module knew of the ring at that moment)
            static std::atomic<uint32_t> s_detail{ 0 };
            if (!strstr(why, "not loaded") && s_detail.fetch_add(1, std::memory_order_relaxed) < 400)
                g_log.Line(true, "RTM2 held detail mod=%d comp=%d vb0=%llx reg_ring=%llx: %s -- %s", in.mod, in.comp, (unsigned long long)vb0,
                    (unsigned long long)reg_ring, why, rtskin::Explain(vb0, in.comp, in.mod).c_str());
        }
        if (!why) {
            {
                rtskin::ClaimCtx c;
                c.cmd_list = in.cmd_list;
                c.gen = CmdGen(in.cmd_list);
                c.ring = ring;
                c.prev_ring = prev;
                c.mod = in.mod;
                c.comp = in.comp;
                c.first_index = in.first_index;
                c.index_count = in.index_count;
                c.vs_hash = in.vs_hash;
                c.frame = g_frame_id;
                rtskin::SetClaim(c);
                g_c.claims_drawn.fetch_add(1, std::memory_order_relaxed);
                return false; // the core draws the mod; RtSkinDraw poses it
            }
        }
    }
    // M1 never draws the mod while the game ray traces: the core would hand the mod's bind-pose vertices to the
    // game's pass-through VS (with RT on, the skin cache has already posed vb0).
    if (in.identified) g_c.claims_held.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RtOnBlasBuild(command_list* cmd_list, acceleration_structure_type type, acceleration_structure_build_flags build_flags,
    uint32_t input_count, const acceleration_structure_build_input* inputs, resource_view dest,
    acceleration_structure_build_mode mode, unsigned long long)
{
    if (Mode() == 0) return false;
    NoteRtFrame();
    g_c.blas_builds.fetch_add(1, std::memory_order_relaxed);
    if (type == acceleration_structure_type::top_level && inputs && input_count &&
        inputs[0].type == acceleration_structure_build_input_type::instances)
    {
        ObserveTlas(build_flags, inputs[0], dest, mode);
        if (Mode() == 2) M3NoteTlas(cmd_list, inputs[0], uint32_t(build_flags), dest.handle);
    }
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
    // M3 observe: a BLAS built from a skin cache ring the module poses a mod for
    if (Mode() == 2 && !geoms.empty()) {
        const uint64_t ring = rtskin::RingKey(geoms[0].vb);
        if (ring) {
            CharBlas cb;
            cb.ring = ring;
            cb.frame = g_frame_id;
            cb.build_flags = uint32_t(build_flags);
            cb.mode = uint32_t(mode);
            for (uint32_t i = 0; i < input_count && i < 256; ++i) {
                if (inputs[i].type != acceleration_structure_build_input_type::triangles) continue;
                const auto& t = inputs[i].triangles;
                cb.vbs.push_back(BufferVa(t.vertex_buffer, t.vertex_offset));
                cb.ibs.push_back((t.index_buffer.handle || t.index_offset) ? BufferVa(t.index_buffer, t.index_offset) : 0);
                cb.tris.push_back(t.index_count / 3);
                cb.verts.push_back(t.vertex_count);
                cb.flags.push_back(uint32_t(inputs[i].flags));
                cb.index_width.push_back(IndexWidth(t.index_format));
            }
            bool fresh = false;
            {
                std::lock_guard l(g_as_m);
                if (g_char_blas.size() > 64) g_char_blas.clear();
                fresh = !g_char_blas.count(dest.handle);
                g_char_blas[dest.handle] = cb;
            }
            if (fresh) {
                std::string geo;
                for (size_t i = 0; i < cb.ibs.size(); ++i) {
                    char b[96];
                    snprintf(b, sizeof b, " [%zu: %u tris, %u verts, ib %llx w%u, flags %x]", i, cb.tris[i], cb.verts[i],
                        (unsigned long long)cb.ibs[i], cb.index_width[i], cb.flags[i]);
                    geo += b;
                }
                g_log.Line(true, "RTBLAS char dest=%llx ring=%llx build_flags=%x mode=%u geometries=%zu%s", (unsigned long long)dest.handle,
                    (unsigned long long)ring, cb.build_flags, cb.mode, cb.ibs.size(), geo.c_str());
            }
            M3OnCharBlas(cmd_list, dest.handle, ring, inputs, input_count, mode); // M3: ours next to it
        }
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

// M4 (observe): a ray tracing pipeline's shaders. Each shader's library is dumped once (rtlib_<hash>.dxil); RTLIB names
// the entry, its kind and, for exports that have one (raygen / miss / callable), its shader identifier's first bytes --
// to tell which hit shader the characters' records (identifier 0xaf...) run, and what it reads.
void RtNotePipeline(uint32_t n, const pipeline_subobject* so, pipeline pl)
{
    if (!so || !n || Mode() == 0) return;
    using T = pipeline_subobject_type;
    auto kind_of = [](T t) -> const char* {
        switch (t) {
        case T::raygen_shader: return "raygen";
        case T::any_hit_shader: return "anyhit";
        case T::closest_hit_shader: return "closesthit";
        case T::miss_shader: return "miss";
        case T::intersection_shader: return "intersection";
        case T::callable_shader: return "callable";
        default: return nullptr;
        }
    };
    bool rt = false;
    for (uint32_t i = 0; i < n; ++i) rt = rt || kind_of(so[i].type) || so[i].type == T::shader_groups || so[i].type == T::libraries;
    if (!rt) return;
    static std::atomic<uint32_t> s_pipes{ 0 };
    const uint32_t               pidx = s_pipes.fetch_add(1);
    if (pidx >= 256) return;
    ComPtr<ID3D12StateObjectProperties> props;
    if (pl.handle) reinterpret_cast<IUnknown*>(pl.handle)->QueryInterface(IID_PPV_ARGS(&props));
    static std::mutex         s_m;
    static std::set<uint64_t> s_dumped;
    std::string               groups;
    uint32_t                  shaders = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const char* kind = kind_of(so[i].type);
        if (kind && so[i].data) {
            const auto* d = static_cast<const shader_desc*>(so[i].data);
            for (uint32_t k = 0; k < so[i].count; ++k, ++shaders) {
                const shader_desc& s = d[k];
                if (!s.code || !s.code_size) continue;
                const uint64_t h = Fnv1a64(s.code, s.code_size);
                std::string    file = "(kept)";
                {
                    std::lock_guard l(s_m);
                    if (s_dumped.insert(h).second) {
                        char name[64];
                        snprintf(name, sizeof name, "rtlib_%016llx.dxil", (unsigned long long)h);
                        file = g_log.Bin(name, static_cast<const uint8_t*>(s.code), s.code_size, true);
                        if (file.empty()) file = "(not written: budget)";
                    }
                }
                char id[40] = "-";
                if (props && s.entry_point && *s.entry_point) {
                    wchar_t w[256];
                    const int wn = MultiByteToWideChar(CP_UTF8, 0, s.entry_point, -1, w, 256);
                    const auto* p = wn > 0 ? static_cast<const uint8_t*>(props->GetShaderIdentifier(w)) : nullptr;
                    if (p) snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x%02x%02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                }
                g_log.Line(true, "RTLIB pipe=%u kind=%s[%u] entry=%s lib=%016llx bytes=%zu id=%s %s", pidx, kind, k,
                    s.entry_point ? s.entry_point : "(none)", (unsigned long long)h, s.code_size, id, file.c_str());
            }
        } else if (so[i].type == T::shader_groups && so[i].data) {
            const auto* g = static_cast<const shader_group*>(so[i].data);
            for (uint32_t k = 0; k < so[i].count; ++k) {
                char b[64];
                if (g[k].type == shader_group_type::hit_group_triangles || g[k].type == shader_group_type::hit_group_aabbs)
                    snprintf(b, sizeof b, " %u:hit(ch%d,ah%d,is%d)", k, int(g[k].hit_group.closest_hit_shader_index),
                        int(g[k].hit_group.any_hit_shader_index), int(g[k].hit_group.intersection_shader_index));
                else
                    snprintf(b, sizeof b, " %u:%s(%d)", k, g[k].type == shader_group_type::raygen ? "raygen" : g[k].type == shader_group_type::miss ? "miss" : "call",
                        int(g[k].raygen.shader_index));
                groups += b;
                if (groups.size() > 800) {
                    g_log.Line(true, "RTPIPE pipe=%u groups:%s", pidx, groups.c_str());
                    groups.clear();
                }
            }
        } else if (so[i].type == T::libraries)
            g_log.Line(true, "RTPIPE pipe=%u links %u existing librar%s", pidx, so[i].count, so[i].count == 1 ? "y" : "ies");
    }
    if (!groups.empty()) g_log.Line(true, "RTPIPE pipe=%u groups:%s", pidx, groups.c_str());
    g_log.Line(true, "RTPIPE pipe=%u handle=%llx subobjects=%u shaders=%u properties=%s", pidx, (unsigned long long)pl.handle, n, shaders,
        props ? "yes" : "no");
}

// RTPAL (observe): a dispatch binding a ring; what else it binds, and what the UPLOAD ones hold.
void RtPalRecord(command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x, uint32_t groups_y,
    uint32_t groups_z)
{
    if (!cmd_list || Mode() == 0) return;
    // M2: every frame, ray tracing seen yet or not (the first frames after RT resumes pose before its BLAS builds)
    if (Mode() == 2) NoteSkinSection(cmd_list, state, cs_hash);
    if (!g_ring_count.load(std::memory_order_relaxed)) return;
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

// M2: a mod section of a claim RtClaimNote took (a pass-through draw reading a ring) is posed and bound by
// rt_skin.cpp; every other draw -- RT off, M1, a claim left to the game -- is left alone.
VfRestore RtSkinDraw(command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm, const WwmiMod::Component* wc, uint32_t c,
    uint32_t s2, uint32_t, uint32_t, int32_t, uint64_t, uint64_t, int, bool)
{
    if (!wc || !d3d12_cl || Mode() != 2 || !rtskin::ClaimIsPassThrough(cmd_list)) return {};
    return rtskin::Draw(cmd_list, d3d12_cl, dm, int(wc->index), c, s2);
}
void RtNoteDrawOutcome(int dm, int comp, int outcome)
{
    static std::mutex                         s_m;
    static std::set<std::tuple<int, int, int>> s_seen;
    static const char*                        kWhat[] = { "drawn", "declined by the module (see RTM2 skip)",
                                               "NOT drawn: no shadow table bind could be made for the fetch slots",
                                               "NOT drawn: a fetch slot's table is not among the shadow tables the core binds",
                                               "dropped by the core (custom blend sub-draws off)" };
    if (outcome == 0) g_c.m2_drawn.fetch_add(1, std::memory_order_relaxed);
    else g_c.m2_not_drawn.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard l(s_m);
        if (s_seen.size() >= 512 || !s_seen.insert({ dm, comp, outcome }).second) return;
    }
    g_log.Line(true, "RTM2 section mod=%d comp=%d: %s", dm, comp, outcome >= 0 && outcome <= 4 ? kWhat[outcome] : "?");
}

// The pass-through VS's b3 / b4 are material constant buffers, not skeletons: nothing of the mod's goes there.
RtPalResult RtPalPublish(uint64_t, int, const WwmiMod::Component*, uint64_t)
{
    RtPalResult r;
    if (Mode() != 2 || !rtskin::ClaimActive()) return r;
    r.tried = r.published = r.no_merge = r.no_cb3 = true;
    return r;
}

void RtDrawOverlay()
{
    const int m = Mode();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "RT module -- rt_mode=%d (%s)", m, ModeText(m));
    if (m == 2) {
        ImGui::TextDisabled("  %s", rtskin::Status().c_str());
        ImGui::TextDisabled("  claims taken: %llu  sections drawn: %llu (not drawn %llu)  skin cache sections recorded: %llu (misses %llu)",
            (unsigned long long)g_c.claims_drawn.load(), (unsigned long long)g_c.m2_drawn.load(), (unsigned long long)g_c.m2_not_drawn.load(),
            (unsigned long long)g_c.sections.load(), (unsigned long long)g_c.section_misses.load());
    }
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

// rt_skin.cpp's lines go to the same log (buffered, written at present).
namespace rtskin {
void Log(bool essential, _Printf_format_string_ const char* fmt, ...)
{
    char    buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    g_log.Line(essential, "%s", buf);
}
} // namespace rtskin

// A raster draw of mod component (mod, comp) stamped the mod's texture `src` (its resource mod_res) over the game's
// texture `game` in its pixel shader's table (section_ps_stamp.cpp): the RT material table of that section gets the
// same (M4Apply).
void RtNoteSlotTexture(int mod, int comp, ID3D12Resource* game, D3D12_CPU_DESCRIPTOR_HANDLE src, ID3D12Resource* mod_res)
{
    if (mod < 0 || comp < 0 || !game || !src.ptr || game == mod_res) return; // mod_res may be unknown (RabbitFX maps)
    std::lock_guard l(g_slot_tex_m);
    ++g_slot_tex_notes;
    if (g_slot_tex.size() > 4096) g_slot_tex.clear();
    SlotTex& e = g_slot_tex[{ mod, comp, reinterpret_cast<uint64_t>(game) }];
    const bool fresh = e.src.ptr != src.ptr;
    e.src = src;
    e.res = mod_res;
    e.frame = g_frame_id;
    e.gen = PortApi::LoadGeneration();
    if (fresh) {
        static std::atomic<uint32_t> s_log{ 0 };
        if (s_log.fetch_add(1) < 64)
            g_log.Line(true, "RTM4 slot stamp mod=%d comp=%d: game texture %p (%s) -> mod texture %p (%s)", mod, comp, (void*)game,
                TexDesc(game).c_str(), (void*)mod_res, TexDesc(mod_res).c_str());
    }
}

// A raster draw of mod component (mod, comp) reads the game's texture `game` as bound (section_ps_stamp.cpp).
void RtNoteKeptTexture(int mod, int comp, ID3D12Resource* game, ID3D12Resource* swapped)
{
    if (mod < 0 || !game) return;
    std::lock_guard l(g_slot_tex_m);
    if (swapped && swapped != game) { // the slot holds a mod texture the core placed (placement swap / in-place stamp)
        if (g_raster_swap.size() > 4096) g_raster_swap.clear();
        auto& e = g_raster_swap[{ mod, reinterpret_cast<uint64_t>(game) }];
        const bool fresh = e.first != swapped;
        e = { swapped, g_frame_id };
        if (fresh) {
            static std::atomic<uint32_t> s_log{ 0 };
            if (s_log.fetch_add(1) < 96)
                g_log.Line(true, "RTM4 raster swap mod=%d comp=%d: game texture %p (%s h%08x) -> mod texture %p (%s)", mod, comp, (void*)game,
                    TexDesc(game).c_str(), TexHashOf(reinterpret_cast<uint64_t>(game)), (void*)swapped, TexDesc(swapped).c_str());
        }
        return;
    }
    if (g_raster_kept.size() > 4096) g_raster_kept.clear();
    uint32_t& f = g_raster_kept[{ mod, reinterpret_cast<uint64_t>(game) }];
    const bool fresh = !f;
    f = g_frame_id;
    if (fresh) {
        static std::atomic<uint32_t> s_log{ 0 };
        if (s_log.fetch_add(1) < 64) g_log.Line(true, "RTM4 raster keeps mod=%d comp=%d: game texture %p (%s)", mod, comp, (void*)game, TexDesc(game).c_str());
    }
}
