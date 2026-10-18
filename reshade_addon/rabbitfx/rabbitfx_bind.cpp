// The RabbitFX texture binder and the private shadow heap it binds through. See rabbitfx_bind.h.
#include "../addon_common.h"
#include "rabbitfx_bind.h"
#include "../instrumentation/removal_pin.h"
#include "../hooks/cb_ring.h"
#include "../../src/gpu_progress.h"
#include "../draw/read_slots.h"
#include "../rt/rt_hooks.h" // RtNoteSlotTexture

static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_shadow_srv_heap;
static uint32_t                                     g_shadow_srv_cap = 0;
static uint32_t                                     g_shadow_srv_cur = 0;
static uint32_t                                     g_shadow_srv_inc = 0;
static std::mutex                                   g_shadow_heap_mtx;

std::atomic<unsigned long long> g_rfx_mirror_misses{ 0 }; // slots mirrored as null for want of a source record

static bool ShadowHeapsEnsure(ID3D12Device* dev)
{
    if (g_shadow_srv_heap) return true;
    g_shadow_srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = 262144;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_shadow_srv_heap)))) return false;
    g_shadow_srv_cap = d.NumDescriptors;
    return true;
}

static std::vector<uint64_t>                                                          g_shadow_tag; // per block: GpuStamp() at its last write
static std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, uint64_t>> g_shadow_retired;
static bool                                                                           ShadowReserve(uint32_t n)
{
    constexpr uint32_t kBlk = 4096;
    constexpr uint32_t kMax = 1000000;
    const uint64_t     now = GpuStamp();
    for (size_t i = 0; i < g_shadow_retired.size();)
        if (GpuPassed(g_shadow_retired[i].second)) {
            g_shadow_retired[i] = std::move(g_shadow_retired.back());
            g_shadow_retired.pop_back();
        } else
            ++i;
    for (;;) {
        const uint32_t cap = g_shadow_srv_cap;
        const uint32_t blocks = (cap + kBlk - 1) / kBlk;
        if (g_shadow_tag.size() != blocks) g_shadow_tag.assign(blocks, 0);
        uint32_t start = g_shadow_srv_cur;
        if (start + n > cap) start = 0;
        const uint32_t first = (start == g_shadow_srv_cur && start % kBlk) ? start / kBlk + 1 : start / kBlk;
        const uint32_t last = (start + n - 1) / kBlk;
        bool           free = start + n <= cap;
        for (uint32_t b = first; free && b <= last; ++b)
            if (g_shadow_tag[b] && !GpuPassed(g_shadow_tag[b])) free = false;
        if (free) {
            for (uint32_t b = start / kBlk; b <= last; ++b) g_shadow_tag[b] = now;
            g_shadow_srv_cur = start;
            return true;
        }
        uint32_t size = cap;
        do size = size >= kMax / 2 ? kMax : size * 2;
        while (size < n && size < kMax);
        if (size <= cap || size < n) return false; // at D3D12's limit and every slot in flight
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = size;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
        if (FAILED(g_d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
        g_shadow_retired.push_back({ g_shadow_srv_heap, now });
        g_shadow_srv_heap = h;
        g_shadow_srv_cap = size;
        g_shadow_srv_cur = 0;
        g_shadow_tag.clear();
        logger::write("RFX-SHADOW: shadow heap grown to %u descriptors (its slots were still in flight)", size);
    }
}

struct RfxDecision {
    int8_t   dec[4] = {};
    bool     set = false;
    int8_t   cand[4] = {};
    uint16_t cand_cnt = 0;
    uint32_t cand_frame = 0; // the last frame the candidate was counted on
    uint8_t  have = 0;
}; // dec / cand = the four register MASKS d/n/l/m; `have` = which mod maps had resolved (d 1, n 2, l 4, m 8)
static constexpr uint16_t kLockFrames = 8;
static constexpr uint16_t kRelockFrames = 32;
static std::mutex         g_rfx_dec_mtx;
struct RfxDecKey {
    uint64_t mc;
    uint64_t ps;
    bool     operator==(const RfxDecKey& o) const { return mc == o.mc && ps == o.ps; }
};
struct RfxDecKeyHash {
    size_t operator()(const RfxDecKey& k) const { return (size_t)(k.mc * 0x9E3779B97F4A7C15ull ^ k.ps); }
};
static std::unordered_map<RfxDecKey, RfxDecision, RfxDecKeyHash> g_rfx_dec; // grown as draws come

struct Param0Window {
    reshade::api::descriptor_table tabs[64] = {};
    std::vector<LayoutTableParam>  ltabs;
    ID3D12DescriptorHeap*          srv_heap = nullptr;
    ID3D12DescriptorHeap*          smp_heap = nullptr;
    uint8_t                        srv_param = 0;
    uint32_t                       srv_base = 0;
    uint32_t                       srv_basereg = 0;
    uint32_t                       srv_count = 0;
    int                            n_srv = 0;
};
static bool ResolveParam0Window(reshade::api::command_list* cmd_list, Param0Window& w)
{
    using namespace reshade::api;
    uint64_t layh = 0;
    {
        auto& s = CmdGfx(cmd_list);
        for (int p = 0; p < 64; ++p) w.tabs[p] = s.root[p].table;
        layh = CmdGfxLayout(cmd_list);
    }
    if (!layh) return false;
    {
        std::lock_guard l(g_layout_tables_mtx);
        auto            it = g_layout_tables.find(layh);
        if (it != g_layout_tables.end()) w.ltabs = it->second;
    }
    // RABBITFX-BAIL (g_debug, once per layout): the window could not be resolved, and from which step.
    if (w.ltabs.empty()) {
        if (g_debug && DiagOnce("RFX-NOTABLES", (int)(layh ^ (layh >> 32))))
            logger::write("RABBITFX-BAIL layout=%llx: no g_layout_tables entry", (unsigned long long)layh);
        return false;
    }
    int unbound = 0;
    for (const auto& t : w.ltabs) {
        const TableStart ts = TableStartOf(cmd_list->get_device(), w.tabs[t.param]);
        if (!ts) {
            if (t.kind == 1) ++unbound;
            continue;
        }
        auto*          hp = ts.Heap();
        const uint32_t b = ts.base;
        if (t.kind == 1) {
            ++w.n_srv;
            if (!w.srv_heap) {
                w.srv_heap = hp;
                w.srv_param = t.param;
                w.srv_base = b;
                w.srv_basereg = t.base_reg;
                w.srv_count = t.count;
            }
        }
        if (t.kind == 0 && !w.smp_heap) w.smp_heap = hp;
    }
    if (!w.srv_heap && g_debug && DiagOnce("RFX-NOSRVTAB", (int)(layh ^ (layh >> 32))))
        logger::write("RABBITFX-BAIL layout=%llx: %zu table params, none a bound CBV_SRV_UAV table (%d srv/uav/cbv params with no bound table)",
            (unsigned long long)layh, w.ltabs.size(), unbound);
    return w.srv_heap != nullptr;
}

static unsigned MirrorTable(uint64_t heap, uint32_t base, uint32_t count, uint32_t cur, SIZE_T sh_cpu0,
    uint64_t heap_lo, uint64_t heap_hi, D3D12_CPU_DESCRIPTOR_HANDLE null_srv,
    const std::bitset<kTexScanMax>* want)
{
    unsigned misses = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (want && i < kTexScanMax && !want->test(i)) continue;
        ResInfo                     ri{};
        D3D12_CPU_DESCRIPTOR_HANDLE src = null_srv;
        const bool                  have = g_heap_to_res.find({ heap, base + i }, ri);
        ModLoader::TexEntry         stamp{};
        if (have && ri.swapped && ModLoader::TexEntryAt(ModLoader::TexIndexOf(ri.swapped), &stamp))
            src = stamp.srv; // the mod SRV the slot was stamped with
        else if (have && ri.view_handle && (ri.view_handle < heap_lo || ri.view_handle >= heap_hi))
            src = { (SIZE_T)ri.view_handle };
        else
            ++misses;
        g_d3d12_device->CopyDescriptorsSimple(1, { sh_cpu0 + (SIZE_T)(cur + i) * g_shadow_srv_inc }, src,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return misses;
}

static thread_local std::vector<RfxDumpBind> t_dump_binds;
static thread_local unsigned                 t_dump_draw = ~0u;
const std::vector<RfxDumpBind>*              RfxBindsForDump(unsigned draw_index)
{
    return (draw_index == t_dump_draw && !t_dump_binds.empty()) ? &t_dump_binds : nullptr;
}

static thread_local std::vector<ShadowSlotOvr> t_extra;
void                                           SetShadowExtraSlots(const ShadowSlotOvr* s, unsigned n)
{
    t_extra.assign(s, s ? s + n : s);
}
void AddShadowExtraSlots(const ShadowSlotOvr* s, unsigned n)
{
    if (s) t_extra.insert(t_extra.end(), s, s + n);
}

static RfxBindState BindThroughShadow(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, const Param0Window& w,
    const std::vector<WclSlotOvr>& binds)
{
    RfxBindState st{};
    if (MigotoPort::FrameAnalysisArmed()) {
        t_dump_draw = MigotoPort::FrameAnalysisDrawIndex();
        t_dump_binds.clear();
        for (const auto& b : binds) t_dump_binds.push_back(RfxDumpBind{ b.reg, b.srv, ModLoader::TexResOfSrv(b.srv) });
    }
    uint32_t maxwi = 0;
    for (const auto& b : binds)
        if (b.reg >= (int)w.srv_basereg) maxwi = std::max<uint32_t>(maxwi, (uint32_t)b.reg - w.srv_basereg);
    for (const ShadowSlotOvr& e : t_extra)
        if (e.param == w.srv_param) maxwi = std::max<uint32_t>(maxwi, e.offset);
    uint32_t win = w.srv_count ? w.srv_count : (maxwi + 1);
    if (win < maxwi + 1) win = maxwi + 1;
    if (win == 0 || win > 128) return st; // not the small param0 table -> bail (avoid bindless mirror)
    struct Mirror {
        uint8_t  param;
        uint64_t heap;
        uint32_t base;
        uint32_t count;
        uint32_t cur;
    };
    Mirror   mir[64];
    unsigned nm = 0;
    uint32_t total = 0;
    for (const auto& t : w.ltabs) {
        if (t.kind != 1 || !w.tabs[t.param].handle) continue;
        const TableStart ts = TableStartOf(g_reshade_device, w.tabs[t.param]);
        if (!ts) continue;
        const uint32_t cnt = (t.param == w.srv_param) ? win : t.count;
        if (cnt == 0 || cnt > 1024 || nm >= 64) return st;
        mir[nm++] = Mirror{ t.param, ts.heap, ts.base, cnt, 0 };
        total += cnt;
    }
    if (!nm) return st;
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv{};
    if (!RfxPatchNullSrvs(&null_srv, nullptr)) return st;
    std::lock_guard lh(g_shadow_heap_mtx);
    if (!ShadowHeapsEnsure(g_d3d12_device) || !g_shadow_srv_inc) return st;
    if (!ShadowReserve(total)) return st;
    const SIZE_T sh_cpu0 = g_shadow_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const UINT64 sh_gpu0 = g_shadow_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    // The game's heap's CPU range: a recorded handle inside it is not a legal source.
    const uint64_t   heap_lo = w.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const uint64_t   heap_hi = heap_lo + (uint64_t)w.srv_heap->GetDesc().NumDescriptors * g_shadow_srv_inc;
    unsigned         misses = 0;
    uint32_t         cur0 = 0;
    const ReadSlots& rs = DrawReadSlots(cmd_list);
    for (unsigned k = 0; k < nm; ++k) {
        mir[k].cur = g_shadow_srv_cur;
        g_shadow_srv_cur += mir[k].count;
        const uint8_t pm = mir[k].param;
        misses += MirrorTable(mir[k].heap, mir[k].base, mir[k].count, mir[k].cur, sh_cpu0, heap_lo, heap_hi, null_srv,
            pm < 64 && rs.known[pm] ? &rs.want[pm] : nullptr);
        if (mir[k].param == w.srv_param) cur0 = mir[k].cur;
    }
    for (const auto& b : binds) {
        if (b.is_null || !b.srv.ptr || b.reg < (int)w.srv_basereg) continue;
        uint32_t wi = (uint32_t)b.reg - w.srv_basereg;
        if (wi >= win) continue;
        D3D12_CPU_DESCRIPTOR_HANDLE dd = { sh_cpu0 + (SIZE_T)(cur0 + wi) * g_shadow_srv_inc };
        g_d3d12_device->CopyDescriptorsSimple(1, dd, b.srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    for (const ShadowSlotOvr& e : t_extra) {
        bool put = false;
        for (unsigned k = 0; k < nm && !put; ++k)
            if (mir[k].param == e.param && e.offset < mir[k].count) {
                g_d3d12_device->CopyDescriptorsSimple(1, { sh_cpu0 + (SIZE_T)(mir[k].cur + e.offset) * g_shadow_srv_inc },
                    e.src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                put = true;
            }
        if (!put && DiagOnce("SHADOW-EXTRA-MISS", e.param * 4096 + (int)e.offset))
            logger::write("SHADOW-EXTRA: p%u+%u is in no mirrored table -- the draw reads the game's descriptor there",
                (unsigned)e.param, e.offset);
    }
    if (misses) {
        g_rfx_mirror_misses.fetch_add(misses, std::memory_order_relaxed);
        if (g_debug && DiagOnce("RFX-MIRROR-MISS", (int)(w.srv_param * 4096u + total)))
            logger::write("RFX-MIRROR: %u of %u mirrored slot(s) had no CPU-only source record -> null SRV (param0 %u, %u table(s))",
                misses, total, (unsigned)w.srv_param, nm);
    }
    ID3D12DescriptorHeap* game_srv = GameHeap(cmd_list, 0);
    ID3D12DescriptorHeap* game_smp = GameHeap(cmd_list, 1);
    if (!game_srv) game_srv = w.srv_heap;
    if (!game_smp) game_smp = w.smp_heap;
    ID3D12DescriptorHeap* heaps[2];
    UINT                  nh = 0;
    heaps[nh++] = g_shadow_srv_heap.Get();
    if (game_smp) heaps[nh++] = game_smp;
    cl->SetDescriptorHeaps(nh, heaps);
    for (unsigned k = 0; k < nm; ++k) // every mirrored table -> its shadow copy
        cl->SetGraphicsRootDescriptorTable(mir[k].param, { sh_gpu0 + (UINT64)mir[k].cur * g_shadow_srv_inc });
    RemovalPin::Ours(cl, "rfx-bind");
    for (const auto& t : w.ltabs)
        if (w.tabs[t.param].handle) st.tables.push_back({ t.param, w.tabs[t.param].handle });
    st.bound = true;
    st.applied = true;
    st.game_srv = game_srv;
    st.game_smp = game_smp;
    return st;
}

RfxBindState ApplyDrawBindings(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    const std::vector<WclSlotOvr>& binds, bool guard_bindless)
{
    RfxBindState st{};
    if ((binds.empty() && t_extra.empty()) || !cmd_list || !cl || !g_d3d12_device) return st;
    Param0Window w;
    if (!ResolveParam0Window(cmd_list, w)) return st;
    if (guard_bindless && w.n_srv > 1) return st; // bindless pass -> BAIL (a shadow swap would unbind it)
    return BindThroughShadow(cmd_list, cl, w, binds);
}

static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_rfxp_cpu_heap;
static constexpr uint32_t                           kRfxpCpuSlots = 4096;
static std::atomic<uint32_t>                        g_rfxp_cpu_next{ 0 };
static std::mutex                                   g_rfxp_cpu_mtx;
static CbRing                                       g_rfxp_params_ring;
static constexpr uint32_t                           kRfxpParamsBytes = 256u * 1024u; // the ring's first buffer: 1024 draws

static bool RfxPatchCpuHeapEnsure()
{
    static std::atomic<bool> s_made{ false };
    if (s_made.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> l(g_rfxp_cpu_mtx);
    if (g_rfxp_cpu_heap) return true;
    if (!g_d3d12_device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = kRfxpCpuSlots; // not shader-visible: a legal copy SOURCE
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
    if (FAILED(g_d3d12_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) return false;
    if (!g_shadow_srv_inc) g_shadow_srv_inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const SIZE_T                    base = h->GetCPUDescriptorHandleForHeapStart().ptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC t = {};
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    t.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    t.Texture2D.MipLevels = 1;
    g_d3d12_device->CreateShaderResourceView(nullptr, &t, { base });
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.NumElements = 1;
    g_d3d12_device->CreateShaderResourceView(nullptr, &b, { base + g_shadow_srv_inc });
    g_rfxp_cpu_heap = h;
    s_made.store(true, std::memory_order_release);
    return true;
}
bool RfxPatchNullSrvs(D3D12_CPU_DESCRIPTOR_HANDLE* null_tex, D3D12_CPU_DESCRIPTOR_HANDLE* null_buf)
{
    if (!RfxPatchCpuHeapEnsure()) return false;
    const SIZE_T base = g_rfxp_cpu_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (null_tex) *null_tex = { base };
    if (null_buf) *null_buf = { base + g_shadow_srv_inc };
    return true;
}
D3D12_CPU_DESCRIPTOR_HANDLE RfxPatchParamsSrv(float hue, float sat, float val, float brightness)
{
    if (!RfxPatchCpuHeapEnsure() || !g_rfxp_params_ring.Ensure(g_d3d12_device, kRfxpParamsBytes)) return { 0 };
    uint64_t        va = 0;
    ID3D12Resource* ring = nullptr;
    float*          p = (float*)g_rfxp_params_ring.Take(&va, &ring);
    if (!p) return { 0 };
    p[0] = hue;
    p[1] = sat;
    p[2] = val;
    p[3] = brightness; // x217..w217
    const uint64_t                  off = va - ring->GetGPUVirtualAddress();
    D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
    b.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    b.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    b.Buffer.FirstElement = off / 16;
    b.Buffer.NumElements = 1;
    const uint32_t                    slot = 2 + g_rfxp_cpu_next.fetch_add(1, std::memory_order_relaxed) % (kRfxpCpuSlots - 2);
    const D3D12_CPU_DESCRIPTOR_HANDLE h = { g_rfxp_cpu_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * g_shadow_srv_inc };
    g_d3d12_device->CreateShaderResourceView(ring, &b, h);
    return h;
}

bool RfxParam0Covers(reshade::api::command_list* cmd_list, unsigned max_reg, unsigned* base_reg, unsigned* count)
{
    Param0Window w;
    if (!cmd_list || !ResolveParam0Window(cmd_list, w)) return false;
    if (base_reg) *base_reg = w.srv_basereg;
    if (count) *count = w.srv_count;
    return w.srv_basereg == 0 && max_reg < w.srv_count;
}

RfxBindState RfxDecideAndBind(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    int mod_idx, int comp_idx,
    D3D12_CPU_DESCRIPTOR_HANDLE dif, D3D12_CPU_DESCRIPTOR_HANDLE nor, D3D12_CPU_DESCRIPTOR_HANDLE lit,
    D3D12_CPU_DESCRIPTOR_HANDLE mat, bool gbuffer_pass, uint8_t ps_reads, const std::vector<WclSlotOvr>* extra)
{
    using namespace reshade::api;
    RfxBindState st{};
    const int    bail_key = (mod_idx << 16) | (comp_idx & 0xFFFF);
    if (!cmd_list || !cl) {
        if (g_debug && DiagOnce("RFX-BAIL-GATE", bail_key))
            logger::write("RABBITFX-BAIL mod=%d comp=%d: cmd_list=%p cl=%p", mod_idx, comp_idx, (void*)cmd_list, (void*)cl);
        return st;
    }
    if (!dif.ptr && !nor.ptr && !lit.ptr && !mat.ptr) { // mod provided no base maps for this comp
        if (g_debug && DiagOnce("RFX-BAIL-NOMAPS", bail_key))
            logger::write("RABBITFX-BAIL mod=%d comp=%d: no diffuse/normal/light SRV resolved", mod_idx, comp_idx);
        return st;
    }
    Param0Window w;
    if (!ResolveParam0Window(cmd_list, w)) {
        if (g_debug && DiagOnce("RFX-BAIL-WINDOW", bail_key))
            logger::write("RABBITFX-BAIL mod=%d comp=%d: param0 window unresolved (see the layout line above)", mod_idx, comp_idx);
        return st;
    }
    uint64_t      heap = reinterpret_cast<uint64_t>(w.srv_heap);
    const uint8_t have = (uint8_t)((dif.ptr ? 1 : 0) | (nor.ptr ? 2 : 0) | (lit.ptr ? 4 : 0) | (mat.ptr ? 8 : 0));
    int           ds = -1;
    int           ns = -1;
    int           ls = -1;
    int           ms = -1;
    char          flags[8] = "";
    int           cls[3] = { 0, 0, 0 };
    TexInfo       td[3] = {};
    for (int s = 0; s < 3; ++s) {
        ID3D12Resource* res = nullptr;
        {
            ResInfo ri;
            if (g_heap_to_res.find(HeapKey{ heap, w.srv_base + (uint32_t)s }, ri)) res = ri.res;
        }
        if (!res) {
            if (g_debug && DiagOnce("RFX-BAIL-UNRECORDED", bail_key * 4 + s))
                logger::write("RABBITFX-BAIL mod=%d comp=%d: the game's t%d has no recorded texture -- not classified, the game's textures stay",
                    mod_idx, comp_idx, s);
            return st;
        }
        TexInfo& d = td[s];
        if (TexDimsOf(reinterpret_cast<uint64_t>(res), &d) && d.width) cls[s] = RfxTexClass(d.format, d.width, d.height, d.mips, d.arr);
    }
    uint8_t dm = 0;
    uint8_t nm = 0;
    uint8_t lm = 0;
    uint8_t mm = 0;
    bool    ok = RfxSetTexturesDecision(cls, dif.ptr != 0, nor.ptr != 0, lit.ptr != 0, mat.ptr != 0, dm, nm, lm, mm, flags);
    ds = dm;
    ns = nm;
    ls = lm;
    ms = mm;
    // Reuse the cached decision once LOCKED; until then DEBOUNCE (see RfxDecision).
    bool     locked = false; // the decision in ds/ns/ls/ms is the cached one, not this frame's classification
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    if (mod_idx >= 0 && comp_idx >= 0) {
        std::lock_guard<std::mutex> l(g_rfx_dec_mtx);
        RfxDecision&                d = g_rfx_dec[RfxDecKey{ ((uint64_t)(uint32_t)mod_idx << 32) | (uint32_t)comp_idx, ps_hash }];
        if (d.set && d.have != have) d = RfxDecision{}; // a map arrived or went: this lock was for another set
        locked = d.set;
        const int8_t fresh[4] = { (int8_t)ds, (int8_t)ns, (int8_t)ls, (int8_t)ms };
        const bool   same_as_lock = d.set && !memcmp(fresh, d.dec, 4);
        if (ok && d.set && same_as_lock)
            d.cand_cnt = 0; // the lock was seen again: no challenger
        else if (ok) {
            const bool same_cand = d.cand_cnt && !memcmp(fresh, d.cand, 4);
            if (!same_cand) {
                memcpy(d.cand, fresh, 4);
                d.cand_cnt = 1;
                d.cand_frame = g_frame_id;
            } else if (d.cand_frame != g_frame_id) {
                ++d.cand_cnt;
                d.cand_frame = g_frame_id;
            }
            if (d.cand_cnt >= (d.set ? kRelockFrames : kLockFrames)) {
                const bool relock = d.set;
                memcpy(d.dec, fresh, 4);
                d.set = true;
                d.have = have;
                d.cand_cnt = 0;
                if (g_debug) logger::write("RABBITFX-%s mod=%d comp=%d ps=%016llx flags=%s masks d=%x n=%x l=%x m=%x cls=[%d %d %d] -- this decision binds from here on",
                    relock ? "RELOCK" : "LOCK", mod_idx, comp_idx, (unsigned long long)ps_hash, flags, ds, ns, ls, ms, cls[0], cls[1], cls[2]);
            }
        }
        if (d.set) {
            ds = d.dec[0];
            ns = d.dec[1];
            ls = d.dec[2];
            ms = d.dec[3];
            ok = true;
        }
    }
    uint32_t                 heap_num = w.srv_heap->GetDesc().NumDescriptors;
    const unsigned long long dec_key = ((unsigned long long)(uint32_t)(ds + 1) << 32) | ((unsigned long long)(uint32_t)(ns + 1) << 24) | ((unsigned long long)(uint32_t)(ls + 1) << 16) | ((unsigned long long)(uint32_t)(ms + 1) << 8) | ((unsigned long long)ok << 1) | (unsigned long long)locked;
    if (g_debug && DiagChanged("RABBITFX-DECIDE", (int)((((uint32_t)mod_idx << 16) | (uint32_t)comp_idx) ^ (uint32_t)(ps_hash ^ (ps_hash >> 32))), dec_key)) {
        logger::write("RABBITFX-DECIDE mod=%d comp=%d ps=%016llx reads t0-t3=%x cls=[%d %d %d] flags=%s masks d=%x n=%x l=%x m=%x matched=%d locked=%d heapNum=%u %s nSrvTbl=%d param0=%u baseReg=%u count=%u (srv d=%llx n=%llx l=%llx m=%llx) "
                      "game t0=%ux%u m%u f%u t1=%ux%u m%u f%u t2=%ux%u m%u f%u",
            mod_idx, comp_idx, (unsigned long long)ps_hash, (unsigned)ps_reads, cls[0], cls[1], cls[2], flags, ds, ns, ls, ms, (int)ok, (int)locked,
            heap_num, heap_num > 65536 ? "BINDLESS" : "small-table", w.n_srv, w.srv_param, w.srv_basereg, w.srv_count,
            (unsigned long long)dif.ptr, (unsigned long long)nor.ptr, (unsigned long long)lit.ptr, (unsigned long long)mat.ptr,
            td[0].width, td[0].height, td[0].mips, td[0].format, td[1].width, td[1].height, td[1].mips, td[1].format,
            td[2].width, td[2].height, td[2].mips, td[2].format);
    }
    bool want_rfx = g_rabbitfx_tex && ok && (!g_rabbitfx_tex_gbuffer || gbuffer_pass) && (!g_rabbitfx_tex_main || (ps_reads & 7) == 7); // RFX semantic slot bind (diffuse / normal / light / material)
    if (!want_rfx) return st;
    if (!g_d3d12_device) return st;
    std::vector<WclSlotOvr> binds;
    auto                    push = [&](int mask, D3D12_CPU_DESCRIPTOR_HANDLE srv) { // masks: one bind per set bit, the map's SRV at that register
        for (int r = 0; r < 4; ++r)
            if (mask & (1 << r)) binds.push_back({ r, srv, false });
    };
    push(ds, dif);
    push(ns, nor);
    push(ls, lit);
    push(ms, mat);
    // RT M4: the game texture each map replaces, for the section's ray tracing material table
    for (const WclSlotOvr& b : binds) {
        ResInfo gri{};
        if (b.reg >= 0 && b.reg < 4 && b.srv.ptr && g_heap_to_res.find(HeapKey{ heap, w.srv_base + (uint32_t)b.reg }, gri) && gri.res)
            RtNoteSlotTexture(mod_idx, comp_idx, gri.res, b.srv, nullptr);
    }
    // One shadow window per draw: a second mirror would be taken from the GAME's table and lose these.
    if (extra) binds.insert(binds.end(), extra->begin(), extra->end());
    st = BindThroughShadow(cmd_list, cl, w, binds);
    if (st.applied) ++g_rabbitfx_tex_count;
    return st;
}
void RfxRestore(ID3D12GraphicsCommandList* cl, const RfxBindState& st)
{
    if (!st.bound || !cl) return;
    ID3D12DescriptorHeap* heaps[2];
    UINT                  nh = 0;
    if (st.game_srv) heaps[nh++] = st.game_srv;
    if (st.game_smp) heaps[nh++] = st.game_smp;
    if (nh) cl->SetDescriptorHeaps(nh, heaps);
    for (const auto& t : st.tables) cl->SetGraphicsRootDescriptorTable(t.first, { t.second }); // restore ALL tables
}
