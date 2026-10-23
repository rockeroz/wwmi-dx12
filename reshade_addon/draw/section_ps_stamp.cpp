// section_ps_stamp.cpp -- a section's own `ps-tN = ResourceX` / `ps-tN = null` stamped into the injected draw's
// table. See draw_parts.h.
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_hooks.h" // PipelineSrvRegs -- what the bound shaders read (PSV0)
#include "draw_parts.h"
#include "../textures/tex_defer.h" // a mod's textures still loading: its null is "not yet", not "unbind"
#include "../rabbitfx/rabbitfx_bind.h"
#include "../rt/rt_hooks.h" // RtNoteSlotTexture
#include <algorithm>
#include <shared_mutex>
#include <unordered_map>

struct StampedSlot {
    uint64_t src = 0;
    uint32_t frame = 0;
};
static constexpr uint64_t                                    kReadAsBound = 1; // StampedSlot::src: a draw read the slot unstamped
static std::mutex                                            g_stamped_mtx;
static std::unordered_map<HeapKey, StampedSlot, HeapKeyHash> g_stamped;
static uint32_t                                              g_stamped_frame = 0;

static void StampedForThisFrame()
{
    if (g_stamped_frame == g_frame_id) return;
    g_stamped.clear();
    g_stamped_frame = g_frame_id;
}

struct GameSide {
    uint64_t side = 0; // the largest the game's own texture there had
    unsigned gen = 0;  // ... in this ini load
};
static std::shared_mutex                      g_game_side_mtx;
static std::unordered_map<uint64_t, GameSide> g_game_side; // (mod, component, ps register)

// A20: how many mips below the largest it has had there the game draws the texture it bound at `at`.
static unsigned GameMip(int dm, int comp, unsigned reg, const HeapKey& at)
{
    ResInfo ri{};
    if (!g_heap_to_res.find(at, ri) || !ri.res) return ModLoader::kTexMipUnknown;
    const D3D12_RESOURCE_DESC d = ri.res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return ModLoader::kTexMipUnknown;
    const uint64_t side = (std::max)(d.Width, (UINT64)d.Height);
    const uint64_t key = (uint64_t)(uint32_t)CompKey(dm, comp) * MigotoPort::kMaxSRV + reg;
    const unsigned gen = PortApi::LoadGeneration();
    uint64_t       most = 0;
    {
        std::shared_lock l(g_game_side_mtx);
        const auto       it = g_game_side.find(key);
        if (it != g_game_side.end() && it->second.gen == gen) most = it->second.side;
    }
    if (side > most) {
        std::unique_lock l(g_game_side_mtx);
        GameSide&        m = g_game_side[key];
        if (m.gen != gen) m = GameSide{ 0, gen };
        m.side = (std::max)(m.side, side);
        return 0;
    }
    unsigned k = 0;
    while ((side << (k + 1)) <= most) ++k;
    return k;
}

static void PlanStamps(SectionPsStamps& out, device* dev, const RootParam* root, const RegBind* t_ps, uint64_t pso,
    uint64_t layh, uint32_t gen0, const std::vector<uint32_t>& this_gen)
{
    const int                    dm = out.dm;
    const int                    comp = out.comp;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int                    si = MigotoPort::StageIndex('p');
    const DxilSrvRegs            ps_regs = PipelineSrvRegs(pso, 'p');
    for (unsigned r = 0; r < MigotoPort::kMaxSRV; ++r) {
        if ((int32_t)(st.srv_gen[si][r] - gen0) <= 0) continue;
        if (!ps_regs.reads(r)) {
            if (g_debug && DiagOnce("PS-SLOT-UNREAD", CompKey(dm, comp) * 128 + (int)r))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u: the pixel shader does not read it -- not stamped",
                    dm, comp, r);
            continue;
        }
        if (r < this_gen.size() && this_gen[r] && st.srv_gen[si][r] == this_gen[r]) {
            if (g_debug && DiagOnce("PS-SLOT-THIS", CompKey(dm, comp) * 128 + (int)r))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u: bound by a texture section's `this =` -- not "
                              "stamped, the texture module places texture swaps",
                    dm, comp, r);
            continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE src{ 0 };
        ID3D12Resource*             res = nullptr;
        bool                        stored = false;
        int                         index = -1;
        if (ID3D11ShaderResourceView* v = st.srv[si][r]) {
            MigotoPort::WrapView* wv = MigotoPort::AsWrapView(v);
            if (!wv || wv->kind != 't' || !wv->cpu.ptr) continue; // a dump publication, or not a view of ours
            src = wv->cpu;
            res = wv->wres ? wv->wres->dx12 : nullptr;
            index = ModLoader::TexIndexOf(res);
            ModLoader::TexTouch(index);
            stored = index >= 0;
        } else {
            if (!g_ps_slot_stamp_nulls || g_tex_defer.Pending(dm) || PortApi::BoundLoading()) continue;
            src = NullSrvDescriptor(g_d3d12_device, ps_regs.parsed ? ps_regs.kind[r] : 0);
            if (!src.ptr) continue;
        }
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) {
            if (g_debug && DiagOnce("PS-SLOT-UNDECLARED", (int)((layh ^ (layh >> 32)) * 131u + r)))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u: layout %llx declares no table slot for it (param=%u) -- not applied",
                    dm, comp, r, (unsigned long long)layh, (unsigned)b.param);
            continue;
        }
        const TableStart ts = TableStartOf(dev, root[b.param].table);
        if (!ts) continue;
        const uint32_t slot = ts.base + b.offset;
        if (slot >= ts.Heap()->GetDesc().NumDescriptors) continue;
        if (stored && g_tex_distance) ModLoader::TexTouchAt(index, GameMip(dm, comp, r, { ts.heap, slot }));
        if (res) { // RT M4: the game texture this stamp replaces, for the section's ray tracing material table
            ResInfo gri{};
            if (g_heap_to_res.find({ ts.heap, slot }, gri) && gri.res && gri.res != res) RtNoteSlotTexture(dm, comp, gri.res, src, res);
        }
        SectionPsSlot s;
        s.reg = r;
        s.param = b.param;
        s.offset = b.offset;
        s.heap = ts.heap;
        s.slot = slot;
        s.src = src;
        s.res = res;
        s.stored = stored;
        out.slots.push_back(s);
    }
}

SectionPsStamps PlanSectionPsSlots(command_list* cmd_list, uint32_t gen0, const std::vector<uint32_t>& this_gen,
    int dm, int comp)
{
    SectionPsStamps out;
    out.dm = dm;
    out.comp = comp;
    if (!g_ps_slot_stamp || g_disable_tex || !g_d3d12_device || !cmd_list) return out;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int                    si = MigotoPort::StageIndex('p');
    bool                         any = false;
    for (unsigned r = 0; r < MigotoPort::kMaxSRV && !any; ++r) any = (int32_t)(st.srv_gen[si][r] - gen0) > 0;
    device*        dev = cmd_list->get_device();
    const uint64_t layh = CmdGfxLayout(cmd_list);
    static_assert(MigotoPort::kMaxSRV == RegisterFileWidth(TargetClass::ShaderResource), "PipeState and the register map disagree on the SRV file width");
    RegBind t_ps[MigotoPort::kMaxSRV];
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(layh);
        if (it == g_layout_regmap.end() || !it->second.built) {
            if (any && g_debug && DiagOnce("PS-SLOT-NOMAP", (int)(layh ^ (layh >> 32))))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d: no register map for layout %llx -- the section's ps-tN not applied",
                    dm, comp, (unsigned long long)layh);
            return out;
        }
        memcpy(t_ps, it->second.t_ps, sizeof t_ps);
    }
    const CmdState& gs = CmdGfx(cmd_list);
    if (any) PlanStamps(out, dev, gs.root, t_ps, gs.gfx_pso, layh, gen0, this_gen);
    std::vector<HeapKey> read_as_bound;
    const uint64_t       reads = PipelinePsReadMask(gs.gfx_pso);
    for (unsigned r = 0; r < 64; ++r) {
        if (!((reads >> r) & 1u)) continue;
        if (std::any_of(out.slots.begin(), out.slots.end(), [r](const SectionPsSlot& s) { return s.reg == r; })) continue;
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
        const TableStart ts = TableStartOf(dev, gs.root[b.param].table);
        if (ts) {
            read_as_bound.push_back(HeapKey{ ts.heap, ts.base + b.offset });
            ResInfo kri{};
            if (g_heap_to_res.find({ ts.heap, ts.base + b.offset }, kri) && kri.res) RtNoteKeptTexture(dm, comp, kri.res, kri.swapped);
        }
    }
    {
        std::lock_guard l(g_stamped_mtx);
        StampedForThisFrame();
        for (const SectionPsSlot& s : out.slots) {
            auto it = g_stamped.find({ s.heap, s.slot });
            if (it != g_stamped.end() && it->second.src != s.src.ptr) out.shared = true;
        }
        for (const HeapKey& k : read_as_bound) g_stamped.emplace(k, StampedSlot{ kReadAsBound, g_frame_id });
    }
    if (out.shared && g_debug && DiagOnce("PS-SLOT-SHARED", CompKey(dm, comp)))
        logger::write("PS-SLOT-STAMP mod=%d comp=%d f=%u: a slot of this draw's table was stamped, or read unstamped, earlier "
                      "this frame -- this draw binds through a private table",
            dm, comp, g_frame_id);
    return out;
}

void SectionPsStamps::StampInPlace() const
{
    if (slots.empty()) return;
    const UINT                  inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::lock_guard<std::mutex> l(g_stamped_mtx);
    StampedForThisFrame();
    for (const SectionPsSlot& s : slots) {
        ID3D12DescriptorHeap*             heap = reinterpret_cast<ID3D12DescriptorHeap*>(s.heap);
        const D3D12_CPU_DESCRIPTOR_HANDLE dst{ heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)s.slot * inc };
        g_d3d12_device->CopyDescriptorsSimple(1, dst, s.src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (s.res) g_heap_to_res.set_swapped({ s.heap, s.slot }, s.res); // the frame dump publishes what the GPU reads
        if (s.stored) StampedStoreTexture(s.heap, s.slot, s.res);
        g_stamped[{ s.heap, s.slot }] = StampedSlot{ s.src.ptr, g_frame_id };
        g_ps_slot_stamp_count.fetch_add(1, std::memory_order_relaxed);
        if (g_debug && DiagChanged("PS-SLOT-STAMP", CompKey(dm, comp) * 128 + (int)s.reg, (unsigned long long)s.src.ptr))
            logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u <- %s (res=%p) at heap %p slot %u (p%u+%u) f=%u",
                dm, comp, s.reg, s.res ? "section resource" : "null", (void*)s.res, (void*)heap, s.slot,
                (unsigned)s.param, s.offset, g_frame_id);
    }
}

void SectionPsStamps::QueueForShadow() const
{
    std::vector<ShadowSlotOvr> extra;
    extra.reserve(slots.size());
    for (const SectionPsSlot& s : slots) extra.push_back(ShadowSlotOvr{ s.param, s.offset, s.src });
    AddShadowExtraSlots(extra.data(), (unsigned)extra.size());
}
