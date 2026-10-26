// rt_skin.cpp -- the RT module's M2: mod components posed with the native skin cache's palettes and drawn through
// the game's pass-through vertex factory while the game ray traces (rt_mode=2).
//
// Facts this rests on (observed in game, rt_observe.log):
//   - UE's GPU skin cache poses a character with one SkinCacheUpdateBatchCS dispatch per mesh section, in section
//     order; each binds its bone palette (t0, a typed float4 view in the frame's upload buffer: 3 rows per bone),
//     writes posed positions to the "ring" (u1) and posed tangents (u0, R16G16B16A16_SNORM, 2 per vertex). The
//     rings alternate between two buffers frame to frame (VB0 = this frame's, VB4 = the previous one);
//   - the mod's components replace those sections one for one, and its bone indices are local to the section;
//   - the pass-through VS reads ATTRIBUTE0 (the ring) from IA slot 0 and the rest by SV_VertexID from typed
//     buffers (t0 Buffer<float2>, t1 Buffer<float4>, t2 Buffer<float> in a descriptor table), with LocalVF (b2,
//     24 bytes) carrying NumTexcoords in word 1.
// So for a mod sub-draw: pose its indices' vertices with the section's palette into a buffer of ours (one per
// command list recording), bind it at IA slot 0 (and 4), point the VS's fetch slots at the mod's own buffers
// (through the core's shadow table, ShadowSlotOvr) and patch LocalVF's NumTexcoords.
//
// The mod's mesh buffers are read from the files the engine loaded and kept in buffers of ours (shape keys are
// not applied in this version). Root descriptors only on the game's list; its graphics PSO is put back after the
// dispatch; our output buffers move COMMON -> UAV -> read within a recording (buffers decay between submissions).
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cb_ring.h"
#include "../hooks/pipeline_hooks.h" // PipelineSrvRegs
#include "../instrumentation/removal_pin.h"
#include "../mods/claim_state.h" // g_geomods
#include "../mods/layout_registry.h"
#include "../mods/skeleton.h"     // SnapshotGameCB
#include "../mods/wwmi_runtime.h" // WwmiReadFile
#include "../rabbitfx/rabbitfx_bind.h"
#include "../../src/d3d12_buffer.h"
#include "../../src/gpu_progress.h"
#include "rt_skin.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

namespace rtskin {
void Log(bool essential, _Printf_format_string_ const char* fmt, ...); // rt_module.cpp: the observe log

namespace {

constexpr uint32_t kMaxSlots = 48;    // output buffers per mesh (one per command list recording in flight, two frames)
constexpr uint32_t kHeapSlots = 2048; // CPU-only SRV descriptors (never freed: views per slot / mesh / format)
constexpr uint32_t kSectionAge = 1;   // frames a recorded palette stays usable (it lives in a per-frame upload buffer)

std::mutex g_m; // everything below

struct Sec {
    uint64_t        pal = 0;
    uint32_t        bones = 0;
    uint64_t        tan_ring = 0;
    ID3D12Resource* pal_res = nullptr; // the palette's buffer and offset in it (merged skeletons copy from it)
    uint64_t        pal_off = 0;
    uint32_t        frame = 0;         // the frame the skin cache posed it with this palette
};
// A ring's sections by their start vertex in it ($Globals SkinCacheStart): section k = the k-th start. Dispatch order
// is not section order (two shader variants pose different sections, and a pass can straddle a frame id).
struct SecRec {
    Sec      s;
    uint32_t frame = 0;
};
struct RingSecs {
    std::map<uint32_t, SecRec> by_start;
    uint32_t                   expected = 0; // sections the ring's BLAS has (0 = not known)
    uint32_t                   frame = 0;    // last dispatch seen
    uint64_t                   bytes = 0;    // the ring buffer's size (a VB0 inside it finds it)
    std::string                layout;       // the starts last logged
};
std::unordered_map<uint64_t, RingSecs> g_rings;
// A mod's section layout (start vertex per component) once seen complete: the same mesh lays its sections out the same
// way in every ring the pool gives it, so a new ring is usable from its first dispatches.
std::map<int, std::vector<uint32_t>> g_mod_layout;

uint64_t FindRingKey(uint64_t va) // g_m held; 0 = no ring written by the skin cache contains va
{
    if (!va) return 0;
    if (g_rings.count(va)) return va;
    for (const auto& [key, r] : g_rings)
        if (va >= key && va < key + (r.bytes ? r.bytes : 1)) return key;
    return 0;
}
constexpr uint32_t                     kStartForget = 1800; // frames a start not posed again stays in the layout
// (the skin cache can leave a section unposed for long stretches; a ring the pool gives to another mesh is told apart
// by its section count, see LookupSection)

struct Slot {
    ComPtr<ID3D12Resource>      pos, tan;
    D3D12_CPU_DESCRIPTOR_HANDLE pos_view{}, tan_view{};
    uint32_t                    tan_fmt = 0;
    uint64_t                    stamp = 0;          // GPU progress when last written or read
    uint32_t                    used = UINT32_MAX;  // frame last written or read (kept two frames: the next frame's history)
    ComPtr<ID3D12Resource>      merged;             // merged-skeleton mods: every section's palette at vg_offset
};
struct Pose {
    int      slot = -1;
    uint32_t frame = 0;
};
struct Mesh {
    int                                             mod = -1;
    bool                                            ready = false, failed = false;
    std::string                                     why;
    ComPtr<ID3D12Resource>                          idx, pos, blend, vec, uv, color, vg; // vg: 16-bit bone ids (blend remap)
    uint32_t                                        vg_stride = 0;
    uint32_t                                        verts = 0, indices = 0, influences = 0, blend_stride = 0, uv_stride = 0;
    bool                                            has_color = false;
    std::vector<Slot>                               slots;
    uint32_t                                        ncomps = 0; // the mod's components (= the game mesh's sections)
    std::map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> uv_views, color_views;
    std::map<int, std::pair<Pose, Pose>>            poses; // component -> (latest pose, the one before): velocity history
    // WWMI merged skeleton ($\WWMIv1\vg_offset / vg_count): the mod's bone indices address every section's palette
    // laid end to end, section k's first vg_count bones at vg_offset
    // A run of bones copied from section `index`'s palette (bones src..src+count-1) to the merged palette at offset.
    // WWMI merged skeletons: one run per section (src 0). Mods with their own instance skeleton (stride-32 blends,
    // e.g. Hsin "Moonover Fox"): the canonical runs (section k's bones at its import offset) then the mod's per-
    // component draw bone maps (local bone -> destination), as runs of consecutive pairs.
    struct MergedComp {
        int      index;
        uint32_t offset, count;
        uint32_t src = 0;
    };
    std::vector<MergedComp> merged;
    std::vector<int>        merged_idx; // the distinct sections the runs read
    uint32_t                merged_bones = 0;
    bool                    blend16 = false; // Blend.buf: R16 ids + R16_UNORM weights (BlendMode 1)
    uint32_t                custom_pairs = 0; // instance skeleton: draw bone map pairs read
    Pose                    merged_cur, merged_prev; // the last merged palettes built (velocity-free history)
    // M3: per component, the slots its sub-draws were posed into, this frame and the one before (the BLAS takes one)
    struct RtPose {
        int                                        slot = -1;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        uint64_t                                   sum = 0;
    };
    struct RtPoseGen {
        uint32_t            frame = 0;
        std::vector<RtPose> recs;
    };
    std::map<int, std::pair<RtPoseGen, RtPoseGen>> rt_poses; // comp -> (latest frame's, the one before)
};
// M3: which mod drew a section of a ring (both of the character's rings), and when
struct RingDraw {
    int      mod = -1;
    uint32_t frame = 0;
};
std::unordered_map<uint64_t, std::map<int, RingDraw>> g_ring_draws;

void NoteRtPose(Mesh& m, int comp, int slot, uint32_t frame, uint32_t s2, uint32_t c) // g_m held
{
    auto& g = m.rt_poses[comp];
    if (g.first.frame != frame) {
        g.second = std::move(g.first);
        g.first = Mesh::RtPoseGen{};
        g.first.frame = frame;
    }
    Mesh::RtPose* rec = nullptr;
    for (Mesh::RtPose& r : g.first.recs)
        if (r.slot == slot) rec = &r;
    if (!rec) {
        if (g.first.recs.size() >= 16) return;
        g.first.recs.push_back({});
        rec = &g.first.recs.back();
        rec->slot = slot;
    }
    const std::pair<uint32_t, uint32_t> r{ s2, c };
    if (rec->ranges.size() >= 64 || std::find(rec->ranges.begin(), rec->ranges.end(), r) != rec->ranges.end()) return;
    rec->ranges.insert(std::upper_bound(rec->ranges.begin(), rec->ranges.end(), r), r);
    rec->sum += c;
}
std::map<int, Mesh> g_meshes;
std::set<int>       g_requested;
// Meshes are loaded on a background thread (a big mod's files and upload took long enough on the frame thread to
// freeze the game when its character first appeared). g_m.
std::set<int>       g_loading;
std::deque<int>     g_load_queue;
bool                g_loader_running = false;

struct BindKey {
    uintptr_t cl;
    uint64_t  gen;
    int       mod;
    bool      operator<(const BindKey& o) const { return std::tie(cl, gen, mod) < std::tie(o.cl, o.gen, o.mod); }
};
struct Bind {
    int      slot = -1;
    int      state = 0; // 0 COMMON, 1 UAV, 2 read
    uint32_t frame = 0;
    bool     merged_built = false;
};
std::map<BindKey, Bind> g_binds;

ComPtr<ID3D12RootSignature>  g_rs;
ComPtr<ID3D12PipelineState>  g_pso;
ComPtr<ID3D12DescriptorHeap> g_heap;
uint32_t                     g_heap_next = 0, g_heap_inc = 0;
bool                         g_pipeline_tried = false;
std::string                  g_pipeline_why;
CbRing                       g_cb;
uint64_t                     g_draws = 0, g_skips = 0, g_dispatches = 0, g_cached = 0;
// Per mod: how fresh the poses the raster draws are (a pose of an earlier frame shows as a jump in motion). g_m.
struct PoseStats {
    uint64_t draws = 0;      // posed this draw
    uint64_t age1 = 0;       // ...with last frame's palette (this frame's skin cache dispatch not recorded yet)
    uint64_t merged_old = 0; // merged skeleton: some section kept an earlier frame's palette
    uint64_t no_prev = 0;    // no pose of the frame before (no skinning motion vectors)
    uint64_t cached = 0, cached_age = 0, cached_max = 0; // drawn with an earlier pose of ours (how many frames old)
};
std::map<int, PoseStats> g_pose_stats;
std::string                  g_last_skip;
std::set<std::string>        g_logged; // one line per distinct message

thread_local ClaimCtx t_claim;

void LogOnce(const std::string& key, const char* line) // g_m held
{
    if (g_logged.size() < 4096 && g_logged.insert(key).second) Log(true, "%s", line);
}

void SkipLocked(const char* why, int mod, int comp) // g_m held
{
    ++g_skips;
    g_last_skip = why;
    char b[256];
    snprintf(b, sizeof b, "RTM2 skip mod=%d comp=%d: %s", mod, comp, why);
    LogOnce(b, b);
}

bool EnsurePipeline() // g_m held
{
    if (g_pso) return true;
    if (g_pipeline_tried) return false;
    ID3D12Device* dev = g_d3d12_device;
    if (!dev) {
        g_pipeline_why = "no device yet"; // not tried: again later
        return false;
    }
    g_pipeline_tried = true;
    const std::vector<uint8_t> cso = WwmiReadFile((g_cso_dir + L"\\RtSkinMod.cso").c_str());
    if (cso.empty()) {
        g_pipeline_why = "RtSkinMod.cso missing";
    } else if (FAILED(dev->CreateRootSignature(0, cso.data(), cso.size(), IID_PPV_ARGS(&g_rs)))) {
        g_pipeline_why = "root signature";
    } else {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = g_rs.Get();
        d.CS = { cso.data(), cso.size() };
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kHeapSlots;
        if (FAILED(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&g_pso))))
            g_pipeline_why = "compute PSO";
        else if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap))))
            g_pipeline_why = "descriptor heap";
        else if (!g_cb.Ensure(dev, 256 * 1024))
            g_pipeline_why = "LocalVF ring";
        else {
            g_heap_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Log(true, "RTM2 pipeline ready (RtSkinMod.cso %zu bytes)", cso.size());
            return true;
        }
    }
    g_pso.Reset();
    g_rs.Reset();
    g_heap.Reset();
    Log(true, "RTM2 pipeline FAILED: %s -- the mod is not drawn while the game ray traces", g_pipeline_why.c_str());
    return false;
}

D3D12_CPU_DESCRIPTOR_HANDLE MakeView(ID3D12Resource* res, DXGI_FORMAT fmt, uint64_t first, uint32_t count) // g_m held
{
    if (!g_heap || g_heap_next >= kHeapSlots || !res || !count) return { 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE h{ g_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(g_heap_next++) * g_heap_inc };
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement = first;
    d.Buffer.NumElements = count;
    g_d3d12_device->CreateShaderResourceView(res, &d, h);
    return h;
}

// ---- mesh loading (present thread) --------------------------------------------------------------------------
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Mods that build their own instance skeleton (WWMI-Tools 1.7 "instance skeleton" exports, e.g. Hsin "Moonover Fox"):
// Blend.buf has 8 R16_UINT ids + 8 R16_UNORM weights per vertex addressing a skeleton their compute shader
// (InstanceSkeleton.hlsl) fills from each component's native bone CB:
//   - canonical import, [CommandListImportInstance_TextureOverrideComponentN] x0 = bones, y0 = offset, w0 = 0:
//     destination y0 + i <- local bone i (as a WWMI merged skeleton);
//   - the draw bone map, ResourceDrawBoneMapComponentN (R32G32_UINT pairs, local bone -> destination): before the
//     component's draw, its own bones overwrite the destinations they map to.
// Under RT the draws carry no bone CB, so their shader cannot run: the same skeleton is built from the sections' skin
// cache palettes (same local bone order as the bone CB). Every mapping of a destination names the same skeleton bone,
// so one palette per frame serves every component.
struct IniNums {
    float x0 = -1, y0 = -1, w0 = -1;
};
void OnImportLine(const wchar_t* line, void* user)
{
    IniNums* n = static_cast<IniNums*>(user);
    while (*line == L' ' || *line == L'\t') ++line;
    float* dst = nullptr;
    if (!_wcsnicmp(line, L"x0", 2)) dst = &n->x0;
    else if (!_wcsnicmp(line, L"y0", 2)) dst = &n->y0;
    else if (!_wcsnicmp(line, L"w0", 2)) dst = &n->w0;
    if (!dst || *dst >= 0) return; // the first numeric assignment only (later ones restore saved values)
    const wchar_t* p = line + 2;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p != L'=') return;
    ++p;
    while (*p == L' ' || *p == L'\t') ++p;
    wchar_t* end = nullptr;
    const double v = wcstod(p, &end);
    if (end == p) return;
    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') ++end;
    if (*end) return; // an expression, not a number
    *dst = float(v);
}
struct ImportCtx {
    std::map<int, std::wstring> fq; // component -> its import section
};
void OnNsSection(const wchar_t* fq, void* user)
{
    static const wchar_t kTail[] = L"importinstance_textureoverridecomponent";
    const wchar_t*       t = wcsrchr(fq, L'\\');
    t = t ? t + 1 : fq;
    const size_t n = wcslen(t), k = wcslen(kTail);
    for (size_t i = 0; i + k <= n; ++i)
        if (!_wcsnicmp(t + i, kTail, k)) {
            const wchar_t* d = t + i + k;
            if (*d < L'0' || *d > L'9') return;
            wchar_t*   end = nullptr;
            const long c = wcstol(d, &end, 10);
            if (end && !*end && c >= 0 && c < 64) static_cast<ImportCtx*>(user)->fq[int(c)] = fq;
            return;
        }
}

bool LoadInstanceSkeleton(const WwmiMod::Mod& mod, Mesh& m) // g_m NOT held
{
    constexpr uint32_t kMaxBones = 4096; // InstanceSkeleton.hlsl's Skeleton is array/3 rows; 553 seen
    // the namespace, from a resource of the mod
    const WwmiMod::Resource* br = WwmiMod::Find(mod, L"ResourceBlendBuffer");
    std::wstring             ns;
    if (br && !br->fq.empty()) {
        const size_t a = br->fq.find(L'\\'), b = br->fq.find_last_of(L'\\');
        if (a != std::wstring::npos && b != std::wstring::npos && b > a) ns = br->fq.substr(a + 1, b - a - 1);
    }
    ImportCtx ic;
    if (!ns.empty()) PortApi::ForEachSectionInNs(ns.c_str(), &OnNsSection, &ic);
    struct Pair {
        uint32_t src, dst;
    };
    std::map<int, std::vector<Pair>> maps;
    uint32_t                         next = 0; // canonical offsets when the import sections cannot be read
    std::vector<Mesh::MergedComp>    canon, over;
    for (const WwmiMod::Component& wc : mod.comps) {
        const int                idx = wc.index;
        const std::wstring       name = L"ResourceDrawBoneMapComponent" + std::to_wstring(idx);
        const WwmiMod::Resource* r = WwmiMod::Find(mod, name.c_str());
        if (!r || r->fq.empty()) {
            m.why = "instance skeleton (stride-32 blend): no " + Narrow(name);
            return false;
        }
        std::vector<uint32_t> words(16384);
        const size_t          got = PortApi::BufferInitialData(r->fq.c_str(), words.data(), words.size() * 4);
        words.resize(got / 4);
        if (words.size() < 2) {
            m.why = "instance skeleton (stride-32 blend): " + Narrow(name) + " has no data";
            return false;
        }
        std::vector<Pair>& pv = maps[idx];
        for (size_t i = 0; i + 1 < words.size(); i += 2) pv.push_back({ words[i], words[i + 1] });
        m.custom_pairs += uint32_t(pv.size());
        // canonical import: x0 bones at y0 (mode 0); without the section, the bone map's count at the running offset
        uint32_t count = uint32_t(pv.size()), offset = next;
        const auto f = ic.fq.find(idx);
        if (f != ic.fq.end()) {
            IniNums nums;
            PortApi::ForEachSectionLine(f->second.c_str(), &OnImportLine, &nums);
            if (nums.w0 > 0.5f && (uint32_t(nums.w0) & 7) != 0) {
                m.why = "instance skeleton: component " + std::to_string(idx) + " imports through a bone map (mode " +
                        std::to_string(uint32_t(nums.w0) & 7) + "), not supported";
                return false;
            }
            if (nums.x0 >= 1) count = uint32_t(nums.x0);
            if (nums.y0 >= 0) offset = uint32_t(nums.y0);
        }
        next = offset + count;
        if (offset + count > kMaxBones) {
            m.why = "instance skeleton past " + std::to_string(kMaxBones) + " bones";
            return false;
        }
        canon.push_back({ idx, offset, count, 0u });
        m.merged_bones = (std::max)(m.merged_bones, offset + count);
        // the draw bone map as runs, minus pairs the canonical import already does
        for (size_t i = 0; i < pv.size();) {
            const Pair p = pv[i];
            size_t     j = i + 1;
            while (j < pv.size() && pv[j].src == p.src + (j - i) && pv[j].dst == p.dst + (j - i)) ++j;
            const uint32_t n = uint32_t(j - i);
            i = j;
            if (p.src >= 512 || p.dst + n > kMaxBones) continue; // their shader skips sources past 512
            if (p.dst == offset + p.src && p.src + n <= count) continue;
            over.push_back({ idx, p.dst, n, p.src });
            m.merged_bones = (std::max)(m.merged_bones, p.dst + n);
        }
    }
    if (canon.empty()) {
        m.why = "instance skeleton: no components";
        return false;
    }
    m.merged = std::move(canon);
    m.merged.insert(m.merged.end(), over.begin(), over.end());
    Log(true, "RTM2 instance skeleton mod=%d: %u bones, %zu components, %u bone map pairs -> %zu runs (%zu import sections read)",
        m.mod, m.merged_bones, mod.comps.size(), m.custom_pairs, m.merged.size(), ic.fq.size());
    return true;
}

bool LoadMesh(Mesh& m) // g_m NOT held (file and GPU work); the caller publishes
{
    if (m.mod < 0 || size_t(m.mod) >= g_geomods.size()) {
        m.why = "no such mod";
        return false;
    }
    ID3D12Device* dev = g_d3d12_device;
    if (!dev) {
        m.why = "no device";
        return false;
    }
    const WwmiMod::Mod& mod = g_geomods[m.mod].mod;
    struct Src {
        const wchar_t*          name;
        ComPtr<ID3D12Resource>* dst;
        bool                    required;
        std::vector<uint8_t>    bytes;
        uint32_t                stride = 0;
    } src[] = {
        { L"ResourceIndexBuffer", &m.idx, true, {} },       { L"ResourcePositionBuffer", &m.pos, true, {} },
        { L"ResourceBlendBuffer", &m.blend, true, {} },     { L"ResourceVectorBuffer", &m.vec, true, {} },
        { L"ResourceTexcoordBuffer", &m.uv, true, {} },     { L"ResourceColorBuffer", &m.color, false, {} },
        { L"ResourceBlendRemapVertexVGBuffer", &m.vg, false, {} },
    };
    for (Src& s : src) {
        const WwmiMod::Resource* r = WwmiMod::Find(mod, s.name);
        if (!r || r->filename.empty()) {
            if (s.required) {
                m.why = "no file for " + Narrow(s.name);
                return false;
            }
            continue;
        }
        s.bytes = WwmiReadFile(r->filename.c_str());
        s.stride = r->stride;
        if (s.bytes.empty() && s.required) {
            m.why = "could not read " + Narrow(r->filename);
            return false;
        }
    }
    Src &idx = src[0], &pos = src[1], &blend = src[2], &vec = src[3], &uv = src[4], &col = src[5], &vg = src[6];
    m.verts = uint32_t(pos.bytes.size() / 12);
    m.indices = uint32_t(idx.bytes.size() / 4);
    m.blend_stride = blend.stride;
    m.influences = blend.stride / 2;
    m.uv_stride = uv.stride;
    m.ncomps = uint32_t(mod.comps.size());
    // WWMI blend remap: Blend.buf's byte ids cannot reach merged-skeleton bones past 255; BlendRemapVertexVG.buf has
    // every influence's 16-bit id in the merged skeleton, which our palette is (no per-component remapped skeleton)
    m.vg_stride = blend.stride ? blend.stride : 0;
    if (vg.bytes.empty() || !m.blend_stride || vg.bytes.size() < size_t(m.verts) * m.blend_stride) {
        vg.bytes.clear();
        m.vg_stride = 0;
    }
    bool any_remap = false;
    for (const WwmiMod::Component& wc : mod.comps) {
        any_remap = any_remap || wc.remapped;
        if (wc.remapped && !m.vg_stride) {
            m.why = "remapped skeleton (BlendRemap) without a usable BlendRemapVertexVG buffer";
            return false;
        }
        if (wc.vg_count) {
            m.merged.push_back({ int(wc.index), wc.vg_offset, wc.vg_count });
            m.merged_bones = (std::max)(m.merged_bones, wc.vg_offset + wc.vg_count);
        }
    }
    m.has_color = !col.bytes.empty() && col.stride == 4 && col.bytes.size() >= size_t(m.verts) * 4;
    if (!m.has_color) col.bytes.clear();
    // merged skeleton with the 16-bit ids shipped: use them even without a remap section (older WWMI-Tools exports, e.g.
    // Cartethyia "A Knight's Sweet Dream": 296 bones, Blend.buf bytes, BlendRemapVertexVG.buf present but unused by
    // its ini); a non-merged mod keeps Blend.buf's bytes
    if (!any_remap && m.merged.empty()) {
        vg.bytes.clear();
        m.vg_stride = 0;
    }
    // Blend stride 32 is not 16 byte influences: seen as 8 R16_UINT bone ids + 8 R16_UNORM weights addressing a skeleton the
    // mod builds itself (its own compute shader and bone maps, e.g. Hsin "Moonover Fox"): our palettes cannot pose it
    if (m.blend_stride == 32) {
        if (!LoadInstanceSkeleton(mod, m)) return false;
        vg.bytes.clear();
        m.vg_stride = 0;
        m.influences = 8;
        m.blend16 = true;
    }
    {
        std::set<int> seen;
        for (const Mesh::MergedComp& mc : m.merged)
            if (seen.insert(mc.index).second) m.merged_idx.push_back(mc.index);
    }
    if (!m.verts || !m.indices || (m.blend_stride != 8 && m.blend_stride != 16 && m.blend_stride != 32) || blend.bytes.size() < size_t(m.verts) * m.blend_stride ||
        vec.stride != 8 || vec.bytes.size() < size_t(m.verts) * 8 || !m.uv_stride || (m.uv_stride & 3) ||
        uv.bytes.size() < size_t(m.verts) * m.uv_stride) {
        m.why = "unexpected mesh layout";
        return false;
    }
    uint64_t total = 0;
    for (Src& s : src)
        if (!s.bytes.empty()) total += (s.bytes.size() + 255) & ~uint64_t(255);
    ComPtr<ID3D12Resource> staging;
    if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ, &staging))) {
        m.why = "staging buffer";
        return false;
    }
    uint8_t*    mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(staging->Map(0, &none, reinterpret_cast<void**>(&mapped))) || !mapped) {
        m.why = "staging map";
        return false;
    }
    ComPtr<ID3D12CommandQueue>        q;
    ComPtr<ID3D12CommandAllocator>    a;
    ComPtr<ID3D12GraphicsCommandList> l;
    ComPtr<ID3D12Fence>               f;
    D3D12_COMMAND_QUEUE_DESC          qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
        staging->Unmap(0, nullptr);
        m.why = "upload queue";
        return false;
    }
    uint64_t off = 0;
    for (Src& s : src) {
        if (s.bytes.empty()) continue;
        const uint64_t bytes = (s.bytes.size() + 3) & ~uint64_t(3);
        if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, s.dst->ReleaseAndGetAddressOf()))) {
            staging->Unmap(0, nullptr);
            m.why = "mesh buffer";
            return false;
        }
        memcpy(mapped + off, s.bytes.data(), s.bytes.size());
        l->CopyBufferRegion(s.dst->Get(), 0, staging.Get(), off, s.bytes.size());
        off += (s.bytes.size() + 255) & ~uint64_t(255);
    }
    staging->Unmap(0, nullptr);
    if (FAILED(l->Close())) {
        m.why = "upload list";
        return false;
    }
    ID3D12CommandList* lists[] = { l.Get() };
    q->ExecuteCommandLists(1, lists);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool   done = false;
    if (ev && SUCCEEDED(q->Signal(f.Get(), 1)) && SUCCEEDED(f->SetEventOnCompletion(1, ev))) done = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    if (ev) CloseHandle(ev);
    if (!done) {
        static std::vector<ComPtr<IUnknown>> s_keep; // the GPU may still be copying: nothing of it is released
        s_keep.push_back(staging);
        s_keep.push_back(q);
        s_keep.push_back(a);
        s_keep.push_back(l);
        s_keep.push_back(f);
        for (Src& s : src)
            if (*s.dst) s_keep.push_back(*s.dst);
        m.why = "upload did not complete";
        return false;
    }
    // The buffers decayed to COMMON when the copy completed: read later through implicit promotion.
    return true;
}

bool AcquireSlot(Mesh& m, reshade::api::command_list* cmd_list, uint64_t gen, uint32_t frame, Bind*& bind) // g_m held
{
    const BindKey k{ reinterpret_cast<uintptr_t>(cmd_list), gen, m.mod };
    auto          it = g_binds.find(k);
    if (it != g_binds.end() && it->second.slot >= 0) {
        bind = &it->second;
        bind->frame = frame;
        return true;
    }
    std::vector<bool> named(m.slots.size(), false); // still named by a live recording
    for (const auto& [bk, b] : g_binds)
        if (bk.mod == m.mod && b.slot >= 0 && size_t(b.slot) < named.size()) named[size_t(b.slot)] = true;
    int pick = -1;
    for (int i = 0; i < int(m.slots.size()); ++i) {
        const Slot& s = m.slots[size_t(i)];
        if (!named[size_t(i)] && (s.used == UINT32_MAX || frame - s.used >= 2) && (!s.stamp || GpuPassed(s.stamp))) {
            pick = i;
            break;
        }
    }
    if (pick < 0 && m.slots.size() < kMaxSlots) {
        Slot s;
        if (FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(m.verts) * 12, D3D12_RESOURCE_STATE_COMMON, &s.pos,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
            FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, uint64_t(m.verts) * 16, D3D12_RESOURCE_STATE_COMMON, &s.tan,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            return false;
        s.pos_view = MakeView(s.pos.Get(), DXGI_FORMAT_R32_FLOAT, 0, m.verts * 3);
        m.slots.push_back(std::move(s));
        pick = int(m.slots.size()) - 1;
    }
    if (pick < 0) return false;
    Bind b;
    b.slot = pick;
    b.state = 0; // a slot the GPU is done with has decayed to COMMON
    b.frame = frame;
    bind = &(g_binds[k] = b);
    return true;
}

void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* a, ID3D12Resource* b, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER br[2]{};
    ID3D12Resource*        r[2] = { a, b };
    for (int i = 0; i < 2; ++i) {
        br[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br[i].Transition.pResource = r[i];
        br[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br[i].Transition.StateBefore = from;
        br[i].Transition.StateAfter = to;
    }
    cl->ResourceBarrier(2, br);
}

constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

bool LookupSection(uint64_t ring_va, int comp, uint32_t frame, Sec* out, int mod) // g_m held
{
    const uint64_t ring = FindRingKey(ring_va);
    auto           it = g_rings.find(ring);
    if (!ring || it == g_rings.end() || comp < 0) return false;
    const RingSecs& r = it->second;
    // the mod's remembered layout: its start for this component, when the ring has posed it
    if (mod >= 0) {
        const auto lm = g_mod_layout.find(mod);
        if (lm != g_mod_layout.end() && size_t(comp) < lm->second.size()) {
            const auto rs = r.by_start.find(lm->second[size_t(comp)]);
            if (rs != r.by_start.end() && int32_t(frame - rs->second.frame) <= int32_t(kSectionAge)) {
                if (out) *out = rs->second.s;
                return true;
            }
        }
    }
    uint32_t ncomps = 0;
    if (mod >= 0) {
        const auto mi = g_meshes.find(mod);
        if (mi != g_meshes.end()) ncomps = mi->second.ncomps;
    }
    std::vector<std::pair<uint32_t, const SecRec*>> live; // (start, record), by start
    for (const auto& [start, rec] : r.by_start)
        if (frame - rec.frame <= kStartForget) live.push_back({ start, &rec });
    // the layout must be complete: a missing start would shift every later section
    if (r.expected && live.size() < r.expected) return false;
    if (r.expected && live.size() > r.expected) {
        // more starts than sections: the pool gave the ring to another mesh; the most recently posed ones are its own
        std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.second->frame > b.second->frame; });
        live.resize(r.expected);
        std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    // without the BLAS's count, at least as many sections as the mod has components (a partial layout would shift them)
    if (!r.expected && ncomps && live.size() < ncomps) return false;
    if (size_t(comp) >= live.size()) return false;
    // complete: remember it for this mod
    if (mod >= 0 && (r.expected ? live.size() == r.expected : ncomps && live.size() == ncomps)) {
        std::vector<uint32_t>& lay = g_mod_layout[mod];
        if (lay.size() != live.size()) {
            lay.clear();
            for (const auto& [start, rec] : live) lay.push_back(start);
        }
    }
    const SecRec& rec = *live[size_t(comp)].second;
    if (int32_t(frame - rec.frame) > int32_t(kSectionAge)) return false; // posed too long ago: its palette is gone
    if (out) *out = rec.s;
    return true;
}

// Merged skeleton for this list's slot: last frame's merged palettes as the base, then every section posed this frame
// copied over its range. false = nothing to build from (no base, not every section posed).
bool BuildMerged(Mesh& m, Slot& sl, Bind& bind, int self_slot, uint64_t ring, uint32_t frame, ID3D12GraphicsCommandList* cl) // g_m held
{
    if (bind.merged_built) return true;
    const uint64_t bytes = uint64_t(m.merged_bones) * 48;
    if (!sl.merged && FAILED(CreateD3D12Buffer(g_d3d12_device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, &sl.merged)))
        return false;
    Slot*       base = nullptr;
    const Pose& bp = m.merged_cur.frame < frame ? m.merged_cur : m.merged_prev;
    if (bp.slot >= 0 && bp.slot != self_slot && size_t(bp.slot) < m.slots.size() && bp.frame < frame && m.slots[size_t(bp.slot)].merged)
        base = &m.slots[size_t(bp.slot)];
    std::map<int, Sec> posed; // section -> this frame's palette
    for (const int idx : m.merged_idx) {
        Sec x;
        if (LookupSection(ring, idx, frame, &x, m.mod) && x.pal_res) posed[idx] = x;
    }
    if (!base && posed.size() != m.merged_idx.size()) return false;
    if (posed.empty() && !base) return false;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = sl.merged.Get();
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &br);
    if (base) {
        cl->CopyBufferRegion(sl.merged.Get(), 0, base->merged.Get(), 0, bytes);
        base->used = frame;
        base->stamp = GpuStamp();
    }
    for (const Mesh::MergedComp& mc : m.merged) {
        const auto px = posed.find(mc.index);
        if (px == posed.end() || mc.src >= px->second.bones) continue;
        const Sec&     x = px->second;
        const uint32_t n = (std::min)(mc.count, x.bones - mc.src);
        if (n && uint64_t(mc.offset + n) * 48 <= bytes)
            cl->CopyBufferRegion(sl.merged.Get(), uint64_t(mc.offset) * 48, x.pal_res, x.pal_off + uint64_t(mc.src) * 48, uint64_t(n) * 48);
    }
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &br);
    bind.merged_built = true;
    if (m.merged_cur.frame != frame) m.merged_prev = m.merged_cur;
    m.merged_cur = { self_slot, frame };
    return true;
}

constexpr uint32_t kCachedPose = 3600; // frames our last pose may stand in for a section the skin cache left as it was

Slot* CachedPose(Mesh& m, int comp, uint32_t frame) // g_m held
{
    const auto it = m.poses.find(comp);
    if (it == m.poses.end()) return nullptr;
    const Pose& p = it->second.first;
    // a pose from this very frame (another list's) will do as well: same palette
    if (p.slot < 0 || size_t(p.slot) >= m.slots.size() || p.frame > frame || frame - p.frame > kCachedPose) return nullptr;
    return &m.slots[size_t(p.slot)];
}

bool Overlaps(const ResInfo& ri, uint64_t va)
{
    return va && ri.cached_va && va >= ri.cached_va && va < ri.cached_va + (ri.size ? ri.size : 1);
}

} // namespace

void NoteSection(uint64_t ring_va, uint64_t ring_bytes, uint32_t start, uint64_t palette_va, uint32_t bones, uint64_t tangent_ring_va,
    uint32_t expected, uint32_t frame, ID3D12Resource* palette_res, uint64_t palette_off)
{
    std::lock_guard l(g_m);
    if (g_rings.size() >= 64 && !g_rings.count(ring_va))
        for (auto it = g_rings.begin(); it != g_rings.end();)
            it = frame - it->second.frame > kStartForget ? g_rings.erase(it) : std::next(it);
    RingSecs& r = g_rings[ring_va];
    if (expected) r.expected = expected;
    if (ring_bytes) r.bytes = ring_bytes;
    r.frame = frame;
    // the pool gave this ring to another mesh: starts not posed for a while leave the layout
    for (auto it = r.by_start.begin(); it != r.by_start.end();)
        it = frame - it->second.frame > kStartForget ? r.by_start.erase(it) : std::next(it);
    const bool fresh = !r.by_start.count(start);
    if (fresh && r.by_start.size() >= 32) return;
    SecRec& rec = r.by_start[start];
    rec.s = { palette_va, bones, tangent_ring_va, palette_res, palette_off, frame };
    rec.frame = frame;
    if (fresh) { // the layout as it grows (one line per distinct layout)
        std::string lay;
        for (const auto& [st, x] : r.by_start) lay += " " + std::to_string(st) + "/" + std::to_string(x.s.bones);
        if (lay != r.layout) {
            r.layout = lay;
            char head[160];
            snprintf(head, sizeof head, "RTM2 ring %llx: %zu section(s) (expected %u), start/bones:", (unsigned long long)ring_va,
                r.by_start.size(), r.expected);
            const std::string line = head + lay;
            LogOnce("lay " + std::to_string(ring_va) + lay, line.c_str());
        }
    }
}

bool SectionKnown(uint64_t ring, int comp, int mod)
{
    std::lock_guard l(g_m);
    return LookupSection(ring, comp, g_frame_id, nullptr, mod);
}

uint64_t RingKey(uint64_t va)
{
    std::lock_guard l(g_m);
    const uint64_t  key = FindRingKey(va);
    return key && g_frame_id - g_rings[key].frame <= kStartForget ? key : 0;
}

std::string Explain(uint64_t va, int comp, int mod)
{
    std::lock_guard l(g_m);
    const uint32_t  frame = g_frame_id;
    char            b[512];
    const uint64_t  key = FindRingKey(va);
    if (!key) {
        snprintf(b, sizeof b, "va=%llx: in no recorded ring (%zu rings)", (unsigned long long)va, g_rings.size());
        return b;
    }
    const RingSecs& r = g_rings[key];
    std::string     starts;
    for (const auto& [st, rec] : r.by_start) starts += " " + std::to_string(st) + "@-" + std::to_string(frame - rec.frame);
    const auto  lm = g_mod_layout.find(mod);
    std::string lay = lm == g_mod_layout.end() ? std::string(" none") : std::string();
    if (lm != g_mod_layout.end())
        for (uint32_t st : lm->second) lay += " " + std::to_string(st);
    std::string pose = "none";
    const auto  mi = g_meshes.find(mod);
    if (mi != g_meshes.end()) {
        const auto pi = mi->second.poses.find(comp);
        if (pi != mi->second.poses.end()) pose = "slot " + std::to_string(pi->second.first.slot) + " @-" + std::to_string(int64_t(frame) - pi->second.first.frame);
    }
    snprintf(b, sizeof b, "ring %llx (va %llx) last dispatch @-%u expected=%u starts(age):%s | mod layout:%s | last pose: %s",
        (unsigned long long)key, (unsigned long long)va, frame - r.frame, r.expected, starts.c_str(), lay.c_str(), pose.c_str());
    return b;
}

bool MeshFailed(int mod)
{
    std::lock_guard l(g_m);
    auto            it = g_meshes.find(mod);
    return it != g_meshes.end() && it->second.failed;
}

bool PoseCached(int mod, int comp)
{
    // this mod's pose of the component, or that of another mod of the same object (a toggle mod -- Hsin's face -- can
    // draw components its body mod's claims never draw themselves)
    std::lock_guard l(g_m);
    const int       guid = mod >= 0 && size_t(mod) < g_geomods.size() ? g_geomods[size_t(mod)].mod.object_guid : 0;
    for (auto& [id, m] : g_meshes) {
        if (!m.ready || (id != mod && (!guid || size_t(id) >= g_geomods.size() || g_geomods[size_t(id)].mod.object_guid != guid))) continue;
        if (CachedPose(m, comp, g_frame_id)) return true;
    }
    return false;
}

bool RingSeen(uint64_t ring)
{
    return RingKey(ring) != 0;
}

void SetClaim(const ClaimCtx& c)
{
    t_claim = c;
}
bool ClaimIsPassThrough(reshade::api::command_list* cmd_list)
{
    return t_claim.cmd_list == cmd_list && t_claim.ring != 0 && t_claim.frame == g_frame_id;
}
bool ClaimActive()
{
    return t_claim.cmd_list && t_claim.ring != 0 && t_claim.frame == g_frame_id;
}

bool MeshReady(int mod)
{
    std::lock_guard l(g_m);
    auto            it = g_meshes.find(mod);
    return it != g_meshes.end() && it->second.ready;
}
void RequestMesh(int mod)
{
    std::lock_guard l(g_m);
    auto            it = g_meshes.find(mod);
    if (it != g_meshes.end() && (it->second.ready || it->second.failed)) return;
    g_requested.insert(mod);
}

void LoaderThread()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        int mod;
        {
            std::lock_guard l(g_m);
            if (g_load_queue.empty()) {
                g_loader_running = false;
                return;
            }
            mod = g_load_queue.front();
            g_load_queue.pop_front();
        }
        Mesh          m;
        m.mod = mod;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const bool ok = LoadMesh(m);
        QueryPerformanceCounter(&t1);
        Log(true, "RTM2 mesh mod=%d %s in %.0f ms (background): verts=%u indices=%u influences=%u%s uv_stride=%u color=%d merged_skeleton=%u bones (%zu sections, %zu runs)%s%s",
            mod, ok ? "loaded" : "FAILED", double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart ? f.QuadPart : 1), m.verts, m.indices,
            m.influences, m.blend16 ? " (R16 ids, instance skeleton)" : "", m.uv_stride, m.has_color ? 1 : 0, m.merged_bones, m.merged_idx.size(),
            m.merged.size(), ok ? "" : " -- ", ok ? "" : m.why.c_str());
        std::lock_guard l(g_m);
        m.ready = ok;
        m.failed = !ok;
        g_meshes.emplace(mod, std::move(m)); // never over an existing one
        g_loading.erase(mod);
    }
}

void PresentTick()
{
    std::lock_guard l(g_m);
    std::set<int>   todo;
    todo.swap(g_requested);
    // a mod asked for again while it was loading is not loaded twice: a live mesh is never replaced (its output
    // buffers may still be read by the GPU)
    for (int mod : todo)
        if (!g_meshes.count(mod) && g_loading.insert(mod).second) g_load_queue.push_back(mod);
    if (!g_load_queue.empty()) {
        EnsurePipeline();
        if (!g_loader_running) {
            g_loader_running = true;
            std::thread(LoaderThread).detach();
        }
    }
    // bindings of recordings from earlier frames are done with (their lists were submitted long ago)
    for (auto it = g_binds.begin(); it != g_binds.end();)
        it = g_frame_id - it->second.frame > 3 ? g_binds.erase(it) : std::next(it);
}

void PreloadAll()
{
    std::lock_guard l(g_m);
    for (size_t i = 0; i < g_geomods.size(); ++i)
        if (!g_meshes.count(int(i)) && !g_geomods[i].dead) g_requested.insert(int(i));
}

bool LoaderIdle(unsigned timeout_ms)
{
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    for (;;) {
        {
            std::lock_guard l(g_m);
            if (!g_loader_running) return true;
        }
        if (GetTickCount64() >= until) return false;
        Sleep(1);
    }
}

VfRestore Draw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, int mod, int comp, uint32_t c, uint32_t s2)
{
    VfRestore      out;
    const ClaimCtx ctx = t_claim;
    if (ctx.cmd_list != cmd_list || !ctx.ring || ctx.frame != g_frame_id) return out; // not a pass-through draw: core path
    out.skip = true;                                                                  // from here, no draw unless it is right
    const uint32_t frame = g_frame_id;
    const uint64_t gen = CmdGen(cmd_list);
    // the layout's VS registers: the fetch slots (t0..t3) and LocalVF (b2)
    RegBind t_vs[4], b_vs[3];
    {
        std::unique_lock l(g_layout_regmap_mtx);
        auto             it = g_layout_regmap.find(CmdGfxLayout(cmd_list));
        if (it == g_layout_regmap.end() || !it->second.built) {
            l.unlock();
            std::lock_guard l2(g_m);
            SkipLocked("no register map for the draw's root signature", mod, comp);
            return out;
        }
        memcpy(t_vs, it->second.t_vs, sizeof t_vs);
        memcpy(b_vs, it->second.b_vs, sizeof b_vs);
    }
    const CmdState& st = CmdGfx(cmd_list);
    device*         dv = cmd_list->get_device();
    // LocalVF as the game bound it (read before the lock: SnapshotGameCB may map the buffer)
    const RegBind& lb = b_vs[2];
    uint32_t       vf_words[8] = {};
    bool           have_vf = false;
    uint64_t       vf_game_va = 0;
    if (lb.param != 0xFF && lb.param < 64 && !lb.is_table) {
        const RootCbvBind rc = CmdCbv(cmd_list).binds[lb.param];
        if (rc.res && rc.va && BufferLive(rc.res) && SnapshotGameCB(rc.res, rc.va, reinterpret_cast<uint8_t*>(vf_words), sizeof vf_words, true) >= 24) {
            have_vf = true;
            vf_game_va = rc.va;
        }
    }
    std::vector<ShadowSlotOvr> slots;
    uint32_t                   uv_per_vertex = 0;
    bool                       vf_patch = false;
    D3D12_VERTEX_BUFFER_VIEW   vbv{}, vbv_prev{};
    {
        std::lock_guard l(g_m);
        if (!EnsurePipeline()) {
            SkipLocked(g_pipeline_why.c_str(), mod, comp);
            return out;
        }
        auto mit = g_meshes.find(mod);
        if (mit == g_meshes.end() || !mit->second.ready) {
            if (mit == g_meshes.end() || !mit->second.failed) g_requested.insert(mod);
            SkipLocked(mit != g_meshes.end() && mit->second.failed ? "the mod's mesh could not be loaded" : "mesh not loaded yet", mod, comp);
            return out;
        }
        Mesh& m = mit->second;
        if (uint64_t(s2) + c > m.indices) {
            SkipLocked("sub-draw past the mod's index buffer", mod, comp);
            return out;
        }
        Sec  sec;
        bool fresh = LookupSection(ctx.ring, comp, frame, &sec, mod);
        if (!m.merged.empty()) { // merged skeleton: any section posed this frame, or last frame's palettes, will do
            bool any = false;
            for (const int idx : m.merged_idx) {
                Sec x;
                any = any || LookupSection(ctx.ring, idx, frame, &x, mod);
            }
            fresh = any;
        }
        // no palette this frame (the skin cache did not pose the section): our last pose of it, drawn as it was
        Slot* cached = fresh ? nullptr : CachedPose(m, comp, frame);
        if (!fresh && !cached) {
            SkipLocked("no skin cache palette for this ring and section this frame, and no earlier pose of ours", mod, comp);
            return out;
        }
        Bind* bind = nullptr;
        if (fresh && !AcquireSlot(m, cmd_list, gen, frame, bind)) {
            SkipLocked("no free output buffer", mod, comp);
            return out;
        }
        Slot& sl = fresh ? m.slots[size_t(bind->slot)] : *cached;
        uint64_t palette_va = sec.pal;
        uint32_t palette_bones = sec.bones;
        if (fresh && !m.merged.empty()) {
            if (!BuildMerged(m, sl, *bind, bind->slot, ctx.ring, frame, cl)) {
                SkipLocked("merged skeleton: not every section posed and no earlier merged palettes", mod, comp);
                return out;
            }
            palette_va = sl.merged->GetGPUVirtualAddress();
            palette_bones = m.merged_bones;
            bool old = false;
            for (const int idx : m.merged_idx) {
                Sec x;
                old = old || !LookupSection(ctx.ring, idx, frame, &x, mod) || x.frame != frame;
            }
            if (old) ++g_pose_stats[mod].merged_old;
        }
        // last frame's pose of this component (the VS's previous positions, for motion vectors)
        Slot* prev = nullptr;
        if (fresh) {
            const auto pit = m.poses.find(comp);
            if (pit != m.poses.end()) {
                const Pose& p = pit->second.first.frame == frame ? pit->second.second : pit->second.first;
                if (p.slot >= 0 && p.slot != bind->slot && size_t(p.slot) < m.slots.size() && p.frame + 1 == frame) prev = &m.slots[size_t(p.slot)];
            }
        }
        // the VS's fetch slots: what the game put there decides what of ours replaces it
        const DxilSrvRegs vs_regs = PipelineSrvRegs(st.gfx_pso, 'v');
        bool              new_uv = false, new_color = false, any = false;
        for (unsigned r = 0; r < 4; ++r) {
            const RegBind& b = t_vs[r];
            if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
            if (vs_regs.parsed && !vs_regs.reads(r)) continue; // a slot the VS does not declare
            const TableStart ts = TableStartOf(dv, st.root[b.param].table);
            ResInfo          ri{};
            if (!ts || !g_heap_to_res.find({ ts.heap, ts.base + b.offset }, ri) || !ri.view_handle) continue;
            const resource_view_desc    vd = dv->get_resource_view_desc(resource_view{ ri.view_handle });
            const format                fmt = vd.format;
            D3D12_CPU_DESCRIPTOR_HANDLE h{ 0 };
            const char*                 what = "kept (the game's)";
            const bool                  ring = Overlaps(ri, ctx.ring) || Overlaps(ri, ctx.prev_ring);
            // R32_FLOAT in the pass-through VS is a skin cache ring: this frame's (VB0's buffer) or the previous one
            if (fmt == format::r32_float && Overlaps(ri, ctx.ring)) {
                h = sl.pos_view;
                what = "posed positions";
            } else if (fmt == format::r32_float && prev) {
                h = prev->pos_view;
                prev->used = frame;
                prev->stamp = GpuStamp();
                what = "previous posed positions";
            } else if (fmt == format::r32_float) {
                h = sl.pos_view;
                what = "posed positions (no previous pose yet: no motion)";
            } else if (fmt == format::r16g16b16a16_snorm || (sec.tan_ring && Overlaps(ri, sec.tan_ring))) {
                if (!sl.tan_view.ptr) sl.tan_view = MakeView(sl.tan.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, 0, m.verts * 2);
                h = sl.tan_view;
                what = fmt == format::r16g16b16a16_snorm ? "posed tangents" : "posed tangents (game format differs!)";
            } else if ((fmt == format::r16g16_float && m.uv_stride % 4 == 0) || (fmt == format::r32g32_float && m.uv_stride % 8 == 0)) {
                const uint32_t fb = fmt == format::r16g16_float ? 4u : 8u;
                auto&          v = m.uv_views[uint32_t(fmt)];
                if (!v.ptr) v = MakeView(m.uv.Get(), DXGI_FORMAT(fmt), 0, m.verts * (m.uv_stride / fb));
                h = v;
                uv_per_vertex = m.uv_stride / fb;
                new_uv = true;
                what = "mod UVs";
            } else if (m.has_color && (fmt == format::r8g8b8a8_unorm || fmt == format::b8g8r8a8_unorm)) {
                auto& v = m.color_views[uint32_t(format::r8g8b8a8_unorm)]; // the mod's colors are RGBA in memory
                if (!v.ptr) v = MakeView(m.color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, m.verts);
                h = v;
                new_color = true;
                what = "mod colors";
            }
            char line[320];
            snprintf(line, sizeof line, "RTM2 fetch vs=%016llx t%u (p%u+%u, game fmt %u first %llu count %llu, res %llx%s) -> %s",
                (unsigned long long)ctx.vs_hash, r, b.param, b.offset, uint32_t(fmt), (unsigned long long)vd.buffer.offset,
                (unsigned long long)vd.buffer.size, (unsigned long long)ri.cached_va, ring ? " ring" : "", what);
            char key[96];
            snprintf(key, sizeof key, "fetch %016llx %u %s", (unsigned long long)ctx.vs_hash, r, what);
            LogOnce(key, line);
            if (h.ptr) {
                slots.push_back({ b.param, b.offset, h });
                out.shadow_params |= 1ull << b.param;
                any = true;
            }
        }
        // LocalVF for our buffers: NumTexCoords = the mod's UV channels, ColorIndexMask = every vertex has its color,
        // vertex fetch base = 0 (the VS reads [base + SV_VertexID]; our views start at the mod's vertex 0)
        if (have_vf && any) {
            uint32_t want[8];
            memcpy(want, vf_words, sizeof want);
            if (new_uv) want[1] = uv_per_vertex;
            if (new_color) want[0] = 0xFFFFFFFFu;
            want[3] = 0;
            if (memcmp(want, vf_words, 16) != 0) {
                vf_patch = true;
                memcpy(vf_words, want, sizeof want);
            }
        }
        // M3: this character's section `comp` is drawn by this mod (under both of its rings)
        for (const uint64_t rv : { ctx.ring, ctx.prev_ring })
            if (const uint64_t rk = FindRingKey(rv)) {
                auto& rd = g_ring_draws[rk];
                rd[comp] = { mod, frame };
            }
        if (g_ring_draws.size() > 128)
            for (auto it = g_ring_draws.begin(); it != g_ring_draws.end();) {
                bool live = false;
                for (const auto& [cc, d] : it->second) live = live || frame - d.frame < 600;
                it = live ? std::next(it) : g_ring_draws.erase(it);
            }
        if (!fresh) {
            sl.used = frame;
            sl.stamp = GpuStamp();
            vbv = { sl.pos->GetGPUVirtualAddress(), m.verts * 12, 12 };
            vbv_prev = vbv;
            ++g_cached;
            {
                PoseStats& ps = g_pose_stats[mod];
                const auto pit = m.poses.find(comp);
                const uint64_t age = pit != m.poses.end() ? uint64_t(frame - pit->second.first.frame) : 0;
                ++ps.cached;
                ps.cached_age += age;
                ps.cached_max = (std::max)(ps.cached_max, age);
            }
            char key[48];
            snprintf(key, sizeof key, "cached %d %d", mod, comp);
            char line[160];
            snprintf(line, sizeof line, "RTM2 mod=%d comp=%d drawn with its last pose (the skin cache did not pose the section this frame)", mod, comp);
            LogOnce(key, line);
        } else {
        // pose: COMMON | read -> UAV, dispatch, -> read
        const D3D12_RESOURCE_STATES from = bind->state == 2 ? kRead : D3D12_RESOURCE_STATE_COMMON;
        if (bind->state != 1) Transition(cl, sl.pos.Get(), sl.tan.Get(), from, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t consts[12] = { s2, c, m.verts, palette_bones, m.influences, m.blend_stride, 0u, 0u, 0u, m.vg ? m.vg_stride : 0u,
            m.blend16 ? 1u : 0u, 0u };
        cl->SetComputeRootSignature(g_rs.Get());
        cl->SetPipelineState(g_pso.Get());
        cl->SetComputeRoot32BitConstants(0, 12, consts, 0);
        cl->SetComputeRootShaderResourceView(1, m.idx->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(2, m.pos->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(3, m.blend->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(4, m.vec->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(5, palette_va);
        cl->SetComputeRootShaderResourceView(6, (m.vg ? m.vg : m.blend)->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(7, sl.pos->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(8, sl.tan->GetGPUVirtualAddress());
        cl->Dispatch((c + 63) / 64, 1, 1);
        RemovalPin::Ours(cl, "rt-m2 skin");
        Transition(cl, sl.pos.Get(), sl.tan.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        bind->state = 2;
        sl.used = frame;
        sl.stamp = GpuStamp();
        {
            auto& pp = m.poses[comp];
            if (pp.first.frame != frame) pp.second = pp.first;
            pp.first = { bind->slot, frame };
        }
        NoteRtPose(m, comp, bind->slot, frame, s2, c);
        vbv = { sl.pos->GetGPUVirtualAddress(), m.verts * 12, 12 };
        vbv_prev = prev ? D3D12_VERTEX_BUFFER_VIEW{ prev->pos->GetGPUVirtualAddress(), m.verts * 12, 12 } : vbv;
        ++g_dispatches;
        {
            PoseStats& ps = g_pose_stats[mod];
            ++ps.draws;
            if (m.merged.empty() && sec.frame != frame) ++ps.age1;
            if (!prev) ++ps.no_prev;
        }
        char first[200];
        snprintf(first, sizeof first, "RTM2 first draw mod=%d comp=%d: %u indices from %u, %u bones, %zu fetch slot(s) replaced, LocalVF %s %08x %08x %08x %08x%s",
            mod, comp, c, s2, palette_bones, slots.size(), have_vf ? "read" : "NOT read", vf_words[0], vf_words[1], vf_words[2], vf_words[3],
            vf_patch ? " (patched)" : "");
        char key[48];
        snprintf(key, sizeof key, "first %d %d", mod, comp);
        LogOnce(key, first);
        }
    }
    // the game's graphics PSO back (the dispatch replaced it); the core's variants are applied after this
    cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(st.gfx_restore()));
    cl->IASetVertexBuffers(0, 1, &vbv);
    if (ctx.prev_ring) cl->IASetVertexBuffers(4, 1, &vbv_prev); // ATTRIBUTE14: previous positions
    if (vf_patch) {
        uint64_t va = 0;
        if (uint8_t* p = g_cb.Take(&va)) {
            memcpy(p, vf_words, sizeof vf_words);
            cl->SetGraphicsRootConstantBufferView(lb.param, va);
            out.param = lb.param;
            out.va = vf_game_va;
        }
    }
    if (!slots.empty()) {
        SetShadowExtraSlots(slots.data(), unsigned(slots.size()));
        out.shadow_bind = true;
    }
    out.skip = false;
    std::lock_guard l(g_m);
    ++g_draws;
    return out;
}

bool PoseFor(uint64_t ring, int comp, uint32_t frame, GeomPose* out)
{
    std::lock_guard l(g_m);
    const uint64_t  key = FindRingKey(ring);
    const auto      rd = g_ring_draws.find(key);
    if (!key || rd == g_ring_draws.end()) return false;
    const auto di = rd->second.find(comp);
    // drawn recently (a character the raster no longer draws -- off screen -- keeps the game's own geometry)
    if (di == rd->second.end() || int32_t(frame - di->second.frame) > 8) return false;
    auto mi = g_meshes.find(di->second.mod);
    if (mi == g_meshes.end() || !mi->second.ready) return false;
    Mesh&      m = mi->second;
    const auto pi = m.rt_poses.find(comp);
    if (pi == m.rt_poses.end()) return false;
    // the newest complete frame: this frame's poses may still be coming (the raster draws after the BLAS update)
    const Mesh::RtPoseGen& g = pi->second.first.frame < frame || pi->second.second.recs.empty() ? pi->second.first : pi->second.second;
    const Mesh::RtPose*    best = nullptr;
    for (const Mesh::RtPose& r : g.recs)
        if (r.slot >= 0 && size_t(r.slot) < m.slots.size() && (!best || r.sum > best->sum)) best = &r;
    if (!best || best->ranges.empty() || int32_t(frame - g.frame) > int32_t(kCachedPose)) return false;
    Slot& sl = m.slots[size_t(best->slot)];
    sl.used = frame; // read by this frame's BLAS and rays: not handed to another recording for two frames
    sl.stamp = GpuStamp();
    out->mod = m.mod;
    out->pos_va = sl.pos->GetGPUVirtualAddress();
    out->verts = m.verts;
    out->idx = m.idx.Get();
    out->indices = m.indices;
    out->ranges = best->ranges;
    out->frame = g.frame;
    out->slot = best->slot;
    return true;
}

bool MakeVfViews(int mod, int slot, uint32_t uv_fmt, VfViews* out)
{
    std::lock_guard l(g_m);
    auto            mi = g_meshes.find(mod);
    if (mi == g_meshes.end() || !mi->second.ready || !out) return false;
    Mesh& m = mi->second;
    *out = VfViews{};
    const uint32_t fb = uv_fmt == uint32_t(DXGI_FORMAT_R16G16_FLOAT) ? 4u : uv_fmt == uint32_t(DXGI_FORMAT_R32G32_FLOAT) ? 8u : 0u;
    if (fb && m.uv && m.uv_stride % fb == 0) {
        auto& v = m.uv_views[uv_fmt];
        if (!v.ptr) v = MakeView(m.uv.Get(), DXGI_FORMAT(uv_fmt), 0, m.verts * (m.uv_stride / fb));
        out->uv = v;
        out->uv_channels = m.uv_stride / fb;
    }
    if (slot >= 0 && size_t(slot) < m.slots.size() && m.slots[size_t(slot)].tan) {
        Slot& sl = m.slots[size_t(slot)];
        if (!sl.tan_view.ptr) sl.tan_view = MakeView(sl.tan.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, 0, m.verts * 2);
        out->tan = sl.tan_view;
    }
    if (m.has_color && m.color) {
        auto& v = m.color_views[uint32_t(format::r8g8b8a8_unorm)];
        if (!v.ptr) v = MakeView(m.color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, m.verts);
        out->col = v;
    }
    return out->uv.ptr || out->tan.ptr || out->col.ptr;
}

std::string Status()
{
    std::lock_guard l(g_m);
    char            b[384];
    size_t          ready = 0, slots = 0;
    for (const auto& [k, m] : g_meshes) {
        ready += m.ready ? 1 : 0;
        slots += m.slots.size();
    }
    snprintf(b, sizeof b, "m2 pipeline=%s meshes=%zu/%zu slots=%zu draws=%llu dispatches=%llu cached=%llu skips=%llu last_skip=%s rings=%zu views=%u",
        g_pso ? "ok" : (g_pipeline_why.empty() ? "not yet" : g_pipeline_why.c_str()), ready, g_meshes.size(), slots,
        (unsigned long long)g_draws, (unsigned long long)g_dispatches, (unsigned long long)g_cached, (unsigned long long)g_skips,
        g_last_skip.empty() ? "-" : g_last_skip.c_str(), g_rings.size(), g_heap_next);
    return b;
}

std::vector<std::string> PoseLines()
{
    std::lock_guard          l(g_m);
    std::vector<std::string> out;
    for (const auto& [mod, ps] : g_pose_stats) {
        char b[320];
        snprintf(b, sizeof b,
            "RTM2POSE mod=%d posed=%llu last_frame_palette=%llu merged_with_old_sections=%llu no_previous_pose=%llu cached=%llu (avg %llu, max %llu frame(s) old)",
            mod, (unsigned long long)ps.draws, (unsigned long long)ps.age1, (unsigned long long)ps.merged_old, (unsigned long long)ps.no_prev,
            (unsigned long long)ps.cached, (unsigned long long)(ps.cached ? ps.cached_age / ps.cached : 0), (unsigned long long)ps.cached_max);
        out.push_back(b);
    }
    return out;
}

} // namespace rtskin
