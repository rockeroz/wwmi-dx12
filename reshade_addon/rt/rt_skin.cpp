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
    uint64_t pal = 0;
    uint32_t bones = 0;
    uint64_t tan_ring = 0;
};
struct RingFrames {
    uint32_t         frame = 0, last_frame = 0;
    std::vector<Sec> cur, last;
};
std::unordered_map<uint64_t, RingFrames> g_rings;

struct Slot {
    ComPtr<ID3D12Resource>      pos, tan;
    D3D12_CPU_DESCRIPTOR_HANDLE pos_view{}, tan_view{};
    uint32_t                    tan_fmt = 0;
    uint64_t                    stamp = 0;          // GPU progress when last written or read
    uint32_t                    used = UINT32_MAX;  // frame last written or read (kept two frames: the next frame's history)
};
struct Pose {
    int      slot = -1;
    uint32_t frame = 0;
};
struct Mesh {
    int                                             mod = -1;
    bool                                            ready = false, failed = false;
    std::string                                     why;
    ComPtr<ID3D12Resource>                          idx, pos, blend, vec, uv, color;
    uint32_t                                        verts = 0, indices = 0, influences = 0, blend_stride = 0, uv_stride = 0;
    bool                                            has_color = false;
    std::vector<Slot>                               slots;
    std::map<uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE> uv_views, color_views;
    std::map<int, std::pair<Pose, Pose>>            poses; // component -> (latest pose, the one before): velocity history
};
std::map<int, Mesh> g_meshes;
std::set<int>       g_requested;

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
};
std::map<BindKey, Bind> g_binds;

ComPtr<ID3D12RootSignature>  g_rs;
ComPtr<ID3D12PipelineState>  g_pso;
ComPtr<ID3D12DescriptorHeap> g_heap;
uint32_t                     g_heap_next = 0, g_heap_inc = 0;
bool                         g_pipeline_tried = false;
std::string                  g_pipeline_why;
CbRing                       g_cb;
uint64_t                     g_draws = 0, g_skips = 0, g_dispatches = 0;
std::string                  g_last_skip;
std::set<std::string>        g_logged; // one line per distinct message

thread_local ClaimCtx t_claim;

void LogOnce(const std::string& key, const char* line) // g_m held
{
    if (g_logged.size() < 512 && g_logged.insert(key).second) Log(true, "%s", line);
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
    };
    for (Src& s : src) {
        const WwmiMod::Resource* r = WwmiMod::Find(mod, s.name);
        if (!r || r->filename.empty()) {
            if (s.required) {
                m.why = "no file for a required buffer";
                return false;
            }
            continue;
        }
        s.bytes = WwmiReadFile(r->filename.c_str());
        s.stride = r->stride;
        if (s.bytes.empty() && s.required) {
            m.why = "could not read a mesh file";
            return false;
        }
    }
    Src &idx = src[0], &pos = src[1], &blend = src[2], &vec = src[3], &uv = src[4], &col = src[5];
    m.verts = uint32_t(pos.bytes.size() / 12);
    m.indices = uint32_t(idx.bytes.size() / 4);
    m.blend_stride = blend.stride;
    m.influences = blend.stride / 2;
    m.uv_stride = uv.stride;
    m.has_color = !col.bytes.empty() && col.stride == 4 && col.bytes.size() >= size_t(m.verts) * 4;
    if (!m.has_color) col.bytes.clear();
    if (!m.verts || !m.indices || (m.blend_stride != 8 && m.blend_stride != 16) || blend.bytes.size() < size_t(m.verts) * m.blend_stride ||
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

bool LookupSection(uint64_t ring, int comp, uint32_t frame, Sec* out) // g_m held
{
    auto it = g_rings.find(ring);
    if (it == g_rings.end() || comp < 0) return false;
    const RingFrames& r = it->second;
    // the most recent pass over this ring that has the section, if it is recent enough
    if (size_t(comp) < r.cur.size() && frame - r.frame <= kSectionAge) {
        if (out) *out = r.cur[size_t(comp)];
        return true;
    }
    if (size_t(comp) < r.last.size() && frame - r.last_frame <= kSectionAge) {
        if (out) *out = r.last[size_t(comp)];
        return true;
    }
    return false;
}

bool Overlaps(const ResInfo& ri, uint64_t va)
{
    return va && ri.cached_va && va >= ri.cached_va && va < ri.cached_va + (ri.size ? ri.size : 1);
}

} // namespace

void NoteSection(uint64_t ring_va, uint64_t palette_va, uint32_t bones, uint64_t tangent_ring_va, uint32_t frame)
{
    std::lock_guard l(g_m);
    if (g_rings.size() >= 64 && !g_rings.count(ring_va))
        for (auto it = g_rings.begin(); it != g_rings.end();)
            it = frame - it->second.frame > 8 ? g_rings.erase(it) : std::next(it);
    RingFrames& r = g_rings[ring_va];
    if (r.frame != frame) {
        if (!r.cur.empty()) {
            // how many sections the skin cache posed in one pass (section k = the k-th dispatch: a skipped section
            // would shift every later one -- this line shows it)
            char line[160], key[64];
            snprintf(line, sizeof line, "RTM2 ring %llx: %zu skin cache section(s) per pass, bones per section:", (unsigned long long)ring_va,
                r.cur.size());
            std::string full = line;
            for (const Sec& x : r.cur) full += " " + std::to_string(x.bones);
            snprintf(key, sizeof key, "secs %llx %zu", (unsigned long long)ring_va, r.cur.size());
            LogOnce(key, full.c_str());
            r.last.swap(r.cur);
            r.last_frame = r.frame;
        }
        r.cur.clear();
        r.frame = frame;
    }
    if (r.cur.size() < 32) r.cur.push_back({ palette_va, bones, tangent_ring_va });
}

bool SectionKnown(uint64_t ring, int comp)
{
    std::lock_guard l(g_m);
    return LookupSection(ring, comp, g_frame_id, nullptr);
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

void PresentTick()
{
    std::set<int> todo;
    {
        std::lock_guard l(g_m);
        todo.swap(g_requested);
        // a mod asked for again while it was loading is not loaded twice: a live mesh is never replaced (its
        // output buffers may still be read by the GPU)
        for (auto it = todo.begin(); it != todo.end();)
            it = g_meshes.count(*it) ? todo.erase(it) : std::next(it);
        if (!todo.empty()) EnsurePipeline();
        // bindings of recordings from earlier frames are done with (their lists were submitted long ago)
        for (auto it = g_binds.begin(); it != g_binds.end();)
            it = g_frame_id - it->second.frame > 3 ? g_binds.erase(it) : std::next(it);
    }
    for (int mod : todo) {
        Mesh m;
        m.mod = mod;
        const bool ok = LoadMesh(m);
        Log(true, "RTM2 mesh mod=%d %s: verts=%u indices=%u influences=%u uv_stride=%u color=%d%s%s", mod, ok ? "loaded" : "FAILED", m.verts,
            m.indices, m.influences, m.uv_stride, m.has_color ? 1 : 0, ok ? "" : " -- ", ok ? "" : m.why.c_str());
        std::lock_guard l(g_m);
        m.ready = ok;
        m.failed = !ok;
        g_meshes.emplace(mod, std::move(m)); // never over an existing one
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
        Sec sec;
        if (!LookupSection(ctx.ring, comp, frame, &sec)) {
            SkipLocked("no skin cache palette for this ring and section this frame", mod, comp);
            return out;
        }
        Bind* bind = nullptr;
        if (!AcquireSlot(m, cmd_list, gen, frame, bind)) {
            SkipLocked("no free output buffer", mod, comp);
            return out;
        }
        Slot& sl = m.slots[size_t(bind->slot)];
        // last frame's pose of this component (the VS's previous positions, for motion vectors)
        Slot* prev = nullptr;
        {
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
        // pose: COMMON | read -> UAV, dispatch, -> read
        const D3D12_RESOURCE_STATES from = bind->state == 2 ? kRead : D3D12_RESOURCE_STATE_COMMON;
        if (bind->state != 1) Transition(cl, sl.pos.Get(), sl.tan.Get(), from, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t consts[12] = { s2, c, m.verts, sec.bones, m.influences, m.blend_stride, 0u, 0u, 0u, 0u, 0u, 0u };
        cl->SetComputeRootSignature(g_rs.Get());
        cl->SetPipelineState(g_pso.Get());
        cl->SetComputeRoot32BitConstants(0, 12, consts, 0);
        cl->SetComputeRootShaderResourceView(1, m.idx->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(2, m.pos->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(3, m.blend->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(4, m.vec->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(5, sec.pal);
        cl->SetComputeRootUnorderedAccessView(6, sl.pos->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(7, sl.tan->GetGPUVirtualAddress());
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
        vbv = { sl.pos->GetGPUVirtualAddress(), m.verts * 12, 12 };
        vbv_prev = prev ? D3D12_VERTEX_BUFFER_VIEW{ prev->pos->GetGPUVirtualAddress(), m.verts * 12, 12 } : vbv;
        ++g_dispatches;
        char first[200];
        snprintf(first, sizeof first, "RTM2 first draw mod=%d comp=%d: %u indices from %u, %u bones, %zu fetch slot(s) replaced, LocalVF %s %08x %08x %08x %08x%s",
            mod, comp, c, s2, sec.bones, slots.size(), have_vf ? "read" : "NOT read", vf_words[0], vf_words[1], vf_words[2], vf_words[3],
            vf_patch ? " (patched)" : "");
        char key[48];
        snprintf(key, sizeof key, "first %d %d", mod, comp);
        LogOnce(key, first);
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

std::string Status()
{
    std::lock_guard l(g_m);
    char            b[384];
    size_t          ready = 0, slots = 0;
    for (const auto& [k, m] : g_meshes) {
        ready += m.ready ? 1 : 0;
        slots += m.slots.size();
    }
    snprintf(b, sizeof b, "m2 pipeline=%s meshes=%zu/%zu slots=%zu draws=%llu dispatches=%llu skips=%llu last_skip=%s rings=%zu views=%u",
        g_pso ? "ok" : (g_pipeline_why.empty() ? "not yet" : g_pipeline_why.c_str()), ready, g_meshes.size(), slots,
        (unsigned long long)g_draws, (unsigned long long)g_dispatches, (unsigned long long)g_skips,
        g_last_skip.empty() ? "-" : g_last_skip.c_str(), g_rings.size(), g_heap_next);
    return b;
}

} // namespace rtskin
