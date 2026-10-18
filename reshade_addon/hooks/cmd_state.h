#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstdint>
#include "../instrumentation/profiler.h" // ProfScope / PROF_CmdLock -- CmdLock below measures the acquire

// Guards g_cmd_state AND g_cmd_cbvs below.
inline std::mutex g_cmd_mutex;

inline std::unique_lock<std::mutex> AcquireCmdLock()
{
    ProfScope s(PROF_CmdLock);
    return std::unique_lock<std::mutex>(g_cmd_mutex);
}

struct CmdLock {
    std::unique_lock<std::mutex> lk = AcquireCmdLock();
};

struct ComputeBindNote {
    uint64_t layout = 0; // the signature this binding belongs to
    uint32_t frame = 0;
    uint32_t seq = 0;
};
struct RootParam {
    reshade::api::descriptor_table table = {};
    reshade::api::descriptor_table ctable = {};
    ComputeBindNote                ctable_note;
    uint64_t                       va = 0;
    uint64_t                       view_va = 0;   // a root SRV/UAV's view handle as pushed (an acceleration structure's: its address)
    ID3D12Resource*                res = nullptr; // owning resource of a root SRV/UAV (morph matcher: offset-proof identity)
    uint8_t                        desc_type = 0;

    // The table a DISPATCH sees: the compute-bound one, else the shared one.
    reshade::api::descriptor_table compute_table() const { return ctable.handle ? ctable : table; }
};
struct VertexBind {
    uint64_t        va = 0;
    uint32_t        size = 0;
    uint32_t        stride = 0; // for restoring the game's VBVs after our override
    ID3D12Resource* res = nullptr;
    uint64_t        res_width = 0; // the resource's own width, captured in on_bind_vertex_buffers
};
struct IndexBind {
    uint64_t        va = 0;
    uint32_t        size = 0;
    uint32_t        index_bytes = 0; // game's IB (to restore)
    ID3D12Resource* res = nullptr;   // IB resource (frame-dump capture needs the object, not just the VA)
};

constexpr unsigned kSectionTexRegs = 9; // ps-t0..t8: the game's textures a component section sees

// Per list: where the last character draw that read each of those registers found its texture (draw_comp.cpp
// CarryPsReads), and the root signature, of which recording, the registers' places were taken from.
struct PsCarry {
    struct Slot {
        reshade::api::descriptor_table table = {};
        uint32_t                       offset = 0;
        uint64_t                       gen = 0; // the recording it was noted in (CmdGen)
    };
    Slot     slot[kSectionTexRegs];
    uint64_t layout = 0;
    uint64_t layout_gen = 0;
    uint8_t  param[kSectionTexRegs] = {};
    uint32_t offset[kSectionTexRegs] = {};
};

struct CmdState {
    RootParam  root[64];
    uint32_t   ctable_seq_next = 0;
    VertexBind vb[16];
    IndexBind  ib;
    uint64_t   gfx_pso = 0; // currently-bound graphics PSO (reshade pipeline handle == ID3D12PipelineState*)
    uint64_t   gfx_set = 0; // what is actually set in its place (the UID-cut or decensor variant); 0 = gfx_pso itself
    uint64_t   gfx_restore() const { return gfx_set ? gfx_set : gfx_pso; }
    uint64_t   cmp_pso = 0;

    D3D12_VIEWPORT vp[16] = {};
    uint32_t       vp_count = 0;
    D3D12_RECT     scissor[16] = {};
    uint32_t       scissor_count = 0;
    uint32_t       topology = 0; // 0 = never seen
    float          blend_factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool           blend_factor_set = false;
    uint32_t       stencil_ref = 0;
    bool           stencil_ref_set = false;
    PsCarry        ps_carry;
};
inline std::unordered_map<uintptr_t, CmdState> g_cmd_state; // a null list's state (see CmdGfx below)

struct RootCbvBind {
    ID3D12Resource* res = nullptr;
    uint64_t        va = 0;
    uint32_t        size = 0;
};
struct CmdCbvBinds {
    RootCbvBind binds[64];
};
inline std::unordered_map<uintptr_t, CmdCbvBinds> g_cmd_cbvs; // a null list's binds (see CmdCbv below)

struct CmdRT {
    uint32_t rtv_count = 0;
    bool     has_dsv = false;
    uint64_t dsv = 0;
    uint32_t pass_gen = 0;
    uint64_t rtv[8] = {}; // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
    bool     valid = false;
};
inline std::atomic<uint32_t> g_rt_pass_gen{ 0 };

// A buffer the game transitioned to UNORDERED_ACCESS on this list, this frame or the previous one.
struct UavNote {
    uint64_t res;
    uint32_t frame;
};
struct HeapBind {
    ID3D12DescriptorHeap* heap = nullptr;
    uint64_t              lo = 0;
    uint64_t              hi = 0;
    bool                  fresh = false;
};
using CmdHeaps = std::array<HeapBind, 2>; // by heap type: CBV_SRV_UAV, sampler
#ifdef WWMI_DEBUG
struct BbNote {
    uint64_t res = 0;
    uint32_t old_s = 0;
    uint32_t new_s = 0;
};
#endif
struct CmdBlock {
    CmdState             gfx;
    CmdCbvBinds          cbv;
    CmdRT                rt;
    CmdHeaps             heaps;
    uint64_t             gfx_layout = 0; // active graphics root signature for this list
    uint64_t             cmp_layout = 0; // ... and the compute one
    uint64_t             gen = 1;
    uint64_t             drew_gen = 0; // the recording (gen) a game draw was recorded in
    uint64_t             frame_end_gen = 0; // the recording a back-buffer-sized texture went to COMMON in: the game's frame end
    uint64_t             rt_pso_gen = 0;    // the recording whose bound pipeline is a ray tracing state object (SetPipelineState1)
    std::vector<UavNote> uav_recent; // most recent last; written by on_barrier, read at dispatch, both on the recording thread
#ifdef WWMI_DEBUG
    std::vector<BbNote> bb_notes; // B23: back-buffer-sized textures moved to or from COMMON in recording bb_notes_gen (BBCOMMON)
    uint64_t            bb_notes_gen = 0;
#endif
};
struct CmdBlockMap {
    struct Entry {
        std::atomic<uintptr_t> key;
        std::atomic<CmdBlock*> val;
    };
    struct Tab {
        uint32_t cap;
        Entry*   e;
    };
    std::atomic<Tab*> tab{ new Tab{ 1u << 13, new Entry[1u << 13]() } };
    std::mutex        add;
    uint32_t          used = 0; // under add
    static uint32_t   hash(uintptr_t k, uint32_t cap)
    {
        uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ull;
        return (uint32_t)(x >> 32) & (cap - 1);
    }
    static CmdBlock* find_in(const Tab* t, uintptr_t key)
    {
        for (uint32_t i = hash(key, t->cap);; i = (i + 1) & (t->cap - 1)) {
            const uintptr_t k = t->e[i].key.load(std::memory_order_acquire);
            if (k == key) return t->e[i].val.load(std::memory_order_acquire);
            if (k == 0) return nullptr;
        }
    }
    static void put_in(Tab* t, uintptr_t key, CmdBlock* b)
    { // under add; val before key, so a found key has its block
        uint32_t i = hash(key, t->cap);
        while (t->e[i].key.load(std::memory_order_relaxed)) i = (i + 1) & (t->cap - 1);
        t->e[i].val.store(b, std::memory_order_relaxed);
        t->e[i].key.store(key, std::memory_order_release);
    }
    // Find-or-create. nullptr only for a null list.
    CmdBlock* get(uintptr_t key)
    {
        if (!key) return nullptr;
        thread_local uintptr_t t_key = 0;
        thread_local CmdBlock* t_blk = nullptr;
        if (key == t_key) return t_blk;
        if (CmdBlock* b = find_in(tab.load(std::memory_order_acquire), key)) {
            t_key = key;
            t_blk = b;
            return b;
        }
        std::lock_guard l(add);
        Tab*            t = tab.load(std::memory_order_relaxed);
        if (CmdBlock* b = find_in(t, key)) return b;
        if ((used + 1) * 2 > t->cap) {
            Tab* g = new Tab{ t->cap * 2, new Entry[t->cap * 2]() };
            for (uint32_t i = 0; i < t->cap; ++i)
                if (const uintptr_t k = t->e[i].key.load(std::memory_order_relaxed)) put_in(g, k, t->e[i].val.load(std::memory_order_relaxed));
            tab.store(g, std::memory_order_release);
            t = g;
        }
        CmdBlock* b = new CmdBlock(); // once per command list, never freed
        put_in(t, key, b);
        ++used;
        return b;
    }
};
inline CmdBlockMap g_cmd_blocks;
inline CmdState&   CmdGfx(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->gfx;
    std::lock_guard l(g_cmd_mutex);
    return g_cmd_state[reinterpret_cast<uintptr_t>(cmd_list)]; // a null list
}
inline CmdCbvBinds& CmdCbv(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->cbv;
    std::lock_guard l(g_cmd_mutex);
    return g_cmd_cbvs[reinterpret_cast<uintptr_t>(cmd_list)];
}
inline CmdRT& CmdRt(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->rt;
    static CmdRT overflow_rt;
    overflow_rt = CmdRT{};
    return overflow_rt;
}
inline CmdHeaps& CmdHeap(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->heaps;
    static CmdHeaps overflow_heaps;
    overflow_heaps = CmdHeaps{};
    return overflow_heaps;
}
inline uint64_t& CmdGfxLayout(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->gfx_layout;
    static uint64_t overflow_gfx;
    overflow_gfx = 0;
    return overflow_gfx;
}
inline uint64_t& CmdCmpLayout(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    if (b) return b->cmp_layout;
    static uint64_t overflow_cmp;
    overflow_cmp = 0;
    return overflow_cmp;
}
// Lists this thread began recording. A submission this thread made before its last Reset has reached the queue:
// ReShade raises every list's execute event first and submits the batch after them.
inline thread_local uint64_t t_list_resets = 0;
// 0 for a null list: the port then keeps its old carry-over instead of guessing a recording.
inline uint64_t CmdGen(const void* cmd_list)
{
    CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    return b ? b->gen : 0;
}
// A game draw in the list's current recording: its submission is game work a step can follow (present_hook.cpp).
inline void CmdNoteDraw(const void* cmd_list)
{
    if (CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list))) b->drew_gen = b->gen;
}
inline bool CmdDrewThisRecording(const void* cmd_list)
{
    const CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    return b && b->drew_gen == b->gen;
}
// The list's bound pipeline (SetPipelineState1: a ray tracing state object; SetPipelineState: a PSO), this recording: an
// ExecuteIndirect with a ray tracing state object bound is an indirect DispatchRays (RT M5).
inline void CmdNoteRtPipeline(const void* cmd_list, bool rt)
{
    if (CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list))) {
        if (rt) b->rt_pso_gen = b->gen;
        else if (b->rt_pso_gen) b->rt_pso_gen = 0;
    }
}
inline bool CmdRtPipelineBound(const void* cmd_list)
{
    const CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    return b && b->rt_pso_gen && b->rt_pso_gen == b->gen;
}
inline bool CmdEndsGameFrame(const void* cmd_list)
{
    const CmdBlock* b = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    return b && b->frame_end_gen == b->gen;
}
template <class F>
inline void CmdBlocksForEach(F&& fn)
{
    const CmdBlockMap::Tab* t = g_cmd_blocks.tab.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < t->cap; ++i) {
        if (t->e[i].key.load(std::memory_order_acquire) == 0) continue;
        if (CmdBlock* b = t->e[i].val.load(std::memory_order_acquire)) fn(*b);
    }
}
