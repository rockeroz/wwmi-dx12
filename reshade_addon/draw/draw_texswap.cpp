// draw_texswap.cpp -- mod textures into the game's descriptor tables, by texture hash on a claimed draw.
// See draw_parts.h.
#include "../addon_common.h"
#include "../mods/claim_state.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_hooks.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "read_slots.h"
#include "../instrumentation/tex_timing.h"
#include "../textures/tex_defer.h" // ModTexturesReady: a mod's textures still loading in the background
#include <bitset>
#include <optional>

using namespace reshade::api;

namespace {
void TableReadSlots(reshade::api::command_list* cmd_list, ReadSlots& rs)
{
    for (bool& k : rs.known) k = false;
    if (!g_tex_read_slots) return;
    const uint64_t pso = CmdGfx(cmd_list).gfx_pso;
    uint64_t       vsh = 0;
    uint64_t       psh = 0;
    uint64_t       gsh = 0;
    PsoBoundHashes(pso, &vsh, &psh, &gsh);
    if (gsh) return;
    const DxilSrvRegs pr = PipelineSrvRegs(pso, 'p');
    const DxilSrvRegs vr = PipelineSrvRegs(pso, 'v');
    if (!pr.parsed || pr.unbounded || !vr.parsed || vr.unbounded) return;
    const uint64_t layh = CmdGfxLayout(cmd_list);
    uint32_t       declared[64] = {};
    uint32_t       covered[64] = {};
    {
        std::lock_guard l(g_layout_tables_mtx);
        auto            it = g_layout_tables.find(layh);
        if (it == g_layout_tables.end()) return;
        for (const auto& t : it->second)
            if (t.param < 64 && t.is_srv && t.count) declared[t.param] = t.count;
    }
    for (int p = 0; p < 64; ++p) {
        rs.len[p] = 0;
        rs.want[p].reset();
    }
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(layh);
        if (it == g_layout_regmap.end() || !it->second.built) return;
        const LayoutRegMap& m = it->second;
        for (unsigned r = 0; r < RegisterFileWidth(TargetClass::ShaderResource); ++r) {
            const RegBind& b = m.t_gfx[r];
            if (b.param >= 64 || !b.is_table) continue;
            ++covered[b.param];
            if (!pr.has(r) && !vr.has(r)) continue;
            if (b.offset >= kTexScanMax) continue;
            rs.want[b.param].set(b.offset);
            if (b.offset + 1 > rs.len[b.param]) rs.len[b.param] = b.offset + 1;
        }
    }
    for (int p = 0; p < 64; ++p) rs.known[p] = declared[p] && covered[p] == declared[p];
}
} // namespace

const ReadSlots& DrawReadSlots(reshade::api::command_list* cmd_list)
{
    struct Memo {
        uint64_t  pso = 0;
        uint64_t  layout = 0;
        uint32_t  frame = 0;
        bool      on = false;
        ReadSlots rs;
    };
    thread_local Memo     t_memo[4];
    thread_local unsigned t_next = 0;
    const uint64_t        pso = CmdGfx(cmd_list).gfx_pso;
    const uint64_t        layout = CmdGfxLayout(cmd_list);
    for (Memo& m : t_memo)
        if (m.frame == g_frame_id && m.pso == pso && m.layout == layout && m.on == g_tex_read_slots) return m.rs;
    Memo& m = t_memo[t_next++ & 3];
    TableReadSlots(cmd_list, m.rs);
    m.pso = pso;
    m.layout = layout;
    m.frame = g_frame_id;
    m.on = g_tex_read_slots;
    return m.rs;
}

// Keep the game's draw while this mod's textures load for the first time in an ini load, so no mod command list
// (texture copies included) runs against textures that are not in yet. Since 2026.10.4 textures load on first draw
// (TexDeferWorker), not as a whole namespace at the first claim: the first run of a mod's commands is what asks for
// them, so the draws of that frame go through and the draws after it wait here until every texture asked is in.
// Once that has happened the mod is warm until the inis reload: a texture that loads later -- one the budget let go
// of and that is asked again, or one a part or toggle first uses now -- keeps the mod drawn, with the game's texture
// in that slot until it is in (section_ps_stamp.cpp), instead of flipping the whole character back to the game's
// model each time (seen as parts of the original flickering under RT, where the budget is short of video memory).
namespace {
constexpr int         kTexWarmCap = 4096;
std::atomic<uint64_t> g_tex_warm[kTexWarmCap]; // (generation << 33) | (warm << 32) | frame of the first ask
} // namespace
bool ModTexturesReady(int mod_id)
{
    if (mod_id < 0 || mod_id >= (int)g_geomods.size() || g_geomods[mod_id].wcl_ns.empty()) return true;
    if (mod_id >= kTexWarmCap) return !g_tex_defer.Pending(mod_id);
    const uint64_t         gen = PortApi::LoadGeneration() & 0x7FFFFFFFu;
    std::atomic<uint64_t>& s = g_tex_warm[mod_id];
    const uint64_t         v = s.load(std::memory_order_acquire);
    if ((v >> 33) != gen) { // the first ask of this ini load: let the run through, so it asks for its textures
        s.store((gen << 33) | g_frame_id, std::memory_order_release);
        return true;
    }
    if (v & (1ull << 32)) return true;                // warm
    if (g_tex_defer.Pending(mod_id)) return false;    // first load still running: the game's draw
    if ((uint32_t)v == g_frame_id) return true;       // the first frame's other draws ask for theirs too
    s.store(v | (1ull << 32), std::memory_order_release);
    return true;
}

bool ClaimedTexturesReady(const MergedComp& mc)
{
    bool ready = ModTexturesReady(mc.mod_id);
    // Queue every dependency even when the first is pending, so merged mods
    // do not warm up in a sequence of first-draw stalls.
    for (const int s : mc.with)
        if (s >= 0 && size_t(s) < g_merged_comps.size()) ready = ModTexturesReady(g_merged_comps[s].mod_id) && ready;
    return ready;
}

static void NoteClaimedMod(int mod_id)
{
    if (mod_id < 0 || mod_id >= (int)g_geomods.size()) return;
    const std::wstring& det_ns = g_geomods[mod_id].wcl_ns;
    if (det_ns.empty()) return;
    PROF(PROF_TH_Detect);
    struct DetSlot {
        unsigned gen = ~0u;
        float*   slot = nullptr;
    };
    thread_local std::vector<DetSlot> t_det;
    if (t_det.size() < g_geomods.size()) t_det.resize(g_geomods.size());
    DetSlot&       ds = t_det[mod_id];
    const unsigned gen = PortApi::LoadGeneration();
    if (ds.gen != gen || (ds.slot && !PortApi::VarSlotHolds(ds.slot, gen, 1.f))) {
        std::optional<PortApi::EngineScope> det_scope;
        {
            PROF(PROF_TH_DetLock);
            det_scope.emplace();
        }
        const unsigned held_gen = PortApi::LoadGeneration();
        if (ds.gen != held_gen) {
            ds.slot = PortApi::VarSlotNs(det_ns.c_str(), L"object_detected");
            ds.gen = held_gen;
        }
        PortApi::SetVarSlot(ds.slot, 1.f);
    }
    TexTiming::NoteClaimed(mod_id, det_ns.c_str());
}

void StampClaimedTextures(const DrawArgs& da, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    MergedComp*&                      mc = dc.mc;
    if (mc && g_texovr_ready && !g_disable_tex) {
        PROF(PROF_DI_TexHash);
        const wchar_t* own_ns = nullptr;
        if (mc->mod_id >= 0 && mc->mod_id < (int)g_geomods.size() && !g_geomods[mc->mod_id].wcl_ns.empty())
            own_ns = g_geomods[mc->mod_id].wcl_ns.c_str();
        NoteClaimedMod(mc->mod_id);
        for (const int s : mc->with) NoteClaimedMod(g_merged_comps[s].mod_id);
        ProfScope   _th_prep(PROF_TH_Prep);
        auto*       dev = reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native());
        uint32_t    inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto& root = CmdGfx(cmd_list).root;
        if (g_debug) {
            uint64_t lkey = vs_hash ^ ((uint64_t)index_count << 1) ^ ((uint64_t)mc->comp_idx << 40);
            if (DiagOnce("DRAW-LKEY", (int)(lkey ^ (lkey >> 32)))) {
                uint64_t psh = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &psh);
                for (int p = 0; p < 64; ++p) {
                    const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
                    if (!ts) continue;
                    D3D12_DESCRIPTOR_HEAP_DESC hd = ts.Heap()->GetDesc();
                    logger::trace("TBLLAYOUT comp=%d vs=%016llx ps=%016llx count=%u first=%u | p=%d heap=%llx type=%d num=%u base=%u",
                        mc->comp_idx, vs_hash, psh, index_count, first_index, p,
                        (unsigned long long)ts.heap, (int)hd.Type, hd.NumDescriptors, ts.base);
                }
            }
        }
        uint32_t scan_len[64];
        LayoutTexScanLens(CmdGfxLayout(cmd_list), scan_len);
        const ReadSlots& rs = DrawReadSlots(cmd_list);
        _th_prep.End();
        int      pr_res = 0;
        int      pr_hash = 0;
        int      pr_match = 0;
        uint32_t pr_h0 = 0; // TEXPROBE
        uint32_t pr_w = 0;
        uint32_t pr_hh = 0;
        uint32_t pr_fmt = 0;
        int      texflip_ord = -1;
        uint64_t texflip_ps = 0; // TEXFLIP: this draw's identity, resolved on its first slot
        for (int p = 0; p < 64; ++p) {
            if (root[p].table.handle == 0) continue;
            if (rs.known[p] && !rs.len[p]) continue;
            ProfScope        _th_walk(PROF_TH_Walk);
            const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
            if (!ts) continue;
            auto*           heap = ts.Heap();
            const uint32_t  base = ts.base;
            uint32_t        num = heap->GetDesc().NumDescriptors;
            ID3D12Resource* local_res[kTexScanMax];
            uint32_t        local_hash[kTexScanMax];
            const uint32_t  local_max_s = ResolveBoundTextures(ts.heap, base, num, local_res, local_hash,
                rs.known[p] ? rs.len[p] : scan_len[p],
                rs.known[p] ? &rs.want[p] : nullptr);
            uint32_t        far_hit = 0;
            _th_walk.End();

            PROF(PROF_TH_Select); // per table, not per slot: a per-slot scope would cost more than the work
            for (uint32_t s = 0; s < local_max_s; ++s) {
                uint32_t        slot = base + s;
                ID3D12Resource* res = local_res[s];
                if (!res) continue;
                if (rs.known[p] && !rs.want[p].test(s)) continue;
                const int sel = local_hash[s] ? TexSelectIdxNs(local_hash[s], own_ns, 0) : -1;
                if (g_texflip_arm.load(std::memory_order_relaxed) > 0) {
                    struct Seen {
                        ID3D12Resource* res;
                        ID3D12Resource* sw;
                        uint32_t        hash;
                        uint32_t        frame;
                        unsigned        logged;
                        int             sel;
                    };
                    static std::unordered_map<uint64_t, Seen>                     s_seen;
                    static std::mutex                                             s_mtx;
                    static unsigned                                               s_flips = 0;
                    static unsigned                                               s_gen = 0;
                    static std::unordered_map<uint64_t, std::pair<uint32_t, int>> s_ord; // (mod, comp, ps) -> (frame, draws so far)
                    ResInfo                                                       fri{};
                    g_heap_to_res.find({ ts.heap, slot }, fri);
                    std::lock_guard lf(s_mtx);
                    if (const unsigned gen = g_texflip_gen.load(); gen != s_gen) {
                        s_gen = gen;
                        s_seen.clear();
                        s_ord.clear();
                        s_flips = 0;
                    }
                    if (texflip_ord < 0) {
                        cmd_list->get_private_data(s_iid_ps_hash, &texflip_ps);
                        const uint64_t dkey = ((uint64_t)(uint32_t)mc->mod_id << 56) | ((uint64_t)(uint32_t)mc->comp_idx << 48) | (texflip_ps & 0xFFFFFFFFFFFFull);
                        auto&          o = s_ord[dkey];
                        if (o.first != g_frame_id) {
                            o.first = g_frame_id;
                            o.second = 0;
                        }
                        texflip_ord = o.second++;
                    }
                    const uint64_t key = ((uint64_t)(uint32_t)mc->mod_id << 56) | ((uint64_t)(uint32_t)mc->comp_idx << 48) |
                        ((texflip_ps ^ (texflip_ps >> 24)) & 0xFFFFFFull) << 24 | ((uint64_t)(texflip_ord & 0xFF) << 16) | ((uint64_t)p << 8) | s;
                    auto     it = s_seen.find(key);
                    unsigned logged = 0;
                    if (it != s_seen.end()) {
                        logged = it->second.logged;
                        const bool hash_flip = it->second.hash && local_hash[s] && it->second.hash != local_hash[s];
                        const bool stamp_flip = (it->second.sw != nullptr) != (fri.swapped != nullptr);
                        const bool sel_flip = it->second.sel != sel;
                        if (g_frame_id > it->second.frame && g_frame_id - it->second.frame <= 2 && (hash_flip || stamp_flip || sel_flip)) {
                            if (++s_flips <= 256 && ++logged <= 8)
                                logger::write("TEXFLIP mod=%d comp=%d ps=%016llx #%d idx=%u+%u p=%d t%u heap=%p slot=%u: f%u res=%p hash=%08x %s%p sel=%d -> f%u res=%p hash=%08x %s%p sel=%d (inplace=%d, flip #%u)",
                                    mc->mod_id, mc->comp_idx, (unsigned long long)texflip_ps, texflip_ord, first_index, index_count, p, s, (void*)ts.Heap(), slot,
                                    it->second.frame, (void*)it->second.res, it->second.hash,
                                    it->second.sw ? "STAMPED modres=" : "game ", (void*)it->second.sw, it->second.sel,
                                    g_frame_id, (void*)res, local_hash[s], fri.swapped ? "STAMPED modres=" : "game ", (void*)fri.swapped, sel,
                                    (int)g_tex_inplace_stamp, s_flips);
                            else if (s_flips == 257)
                                logger::write("TEXFLIP capped at 256");
                        }
                    }
                    s_seen[key] = Seen{ res, fri.swapped, local_hash[s], g_frame_id, logged, sel };
                }
                D3D12_CPU_DESCRIPTOR_HANDLE rep = { 0 };
                ID3D12Resource*             rep_res = nullptr; // the resource behind `rep`, for the slot's stamp record
                uint32_t                    hash = local_hash[s];
                ModLoader::TexEntry         sel_entry;
                const bool                  direct = hash && ModLoader::TexEntryAt(sel, &sel_entry); // `sel` above: one selection per slot
                if (direct) {
                    rep = sel_entry.srv;
                    rep_res = sel_entry.res;
                }
                uint32_t swap_rule = direct ? hash : 0;
                // pr_* feed only the g_debug TEXPROBE line.
                ++pr_res;
                if (hash) {
                    ++pr_hash;
                    if (g_debug && !pr_h0) {
                        pr_h0 = hash;
                        TexInfo di{};
                        if (TexDimsOf(reinterpret_cast<uint64_t>(res), &di)) {
                            pr_w = di.width;
                            pr_hh = di.height;
                            pr_fmt = di.format;
                        }
                    }
                }
                if (rep.ptr) ++pr_match;
                if (g_debug) {
                    uint32_t tw = 0;
                    uint32_t th = 0;
                    uint32_t tf = 0;
                    {
                        TexInfo di{};
                        if (TexDimsOf(reinterpret_cast<uint64_t>(res), &di)) {
                            tw = di.width;
                            th = di.height;
                            tf = di.format;
                        }
                    }
                    bool bc = (tf >= 70 && tf <= 99);
                    if (bc && (uint64_t)tw * th >= 256ull * 256ull) {
                        bool byptr;
                        {
                            std::lock_guard l(g_res_to_mod_mtx);
                            byptr = g_res_to_mod_srv.count(reinterpret_cast<uint64_t>(res)) != 0;
                        }
                        if (ProbeArmed() && DiagOnce("MIDTEX", (int)(reinterpret_cast<uint64_t>(res) ^ (reinterpret_cast<uint64_t>(res) >> 32)))) logger::trace("MIDTEX comp=%d res=%llx %ux%u fmt=%u hashable=%d byptr=%d count=%u",
                            mc->comp_idx, reinterpret_cast<uint64_t>(res), tw, th, tf, (int)(hash != 0), (int)byptr, index_count);
                    }
                }
                if (rep.ptr == 0) {
                    const uint32_t      tgt = TexLodTargetOf(hash);
                    ModLoader::TexEntry alias_entry;
                    if (tgt && ModLoader::TexEntryAt(TexSelectIdxNs(tgt, own_ns, TexDrawnMip(hash, tgt)), &alias_entry)) {
                        rep = alias_entry.srv;
                        rep_res = alias_entry.res;
                        swap_rule = tgt;
                    }
                    if (rep.ptr == 0) continue;
                }
                if (s >= kTexScanMin) far_hit = s;
                if (g_debug) {
                    std::lock_guard ltp(g_res_to_mod_mtx);
                    g_res_to_mod_srv[reinterpret_cast<uint64_t>(res)] = rep.ptr;
                }

                if (g_tex_inplace_stamp) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dst = { heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * inc };
                    dev->CopyDescriptorsSimple(1, dst, rep, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    g_heap_to_res.set_swapped({ ts.heap, slot }, rep_res); // the frame dump publishes what the GPU reads
                    StampedStoreTexture(ts.heap, slot, rep_res);
                    ++g_tex_inject_count;
                    TexTiming::NoteSwapped(rep_res, swap_rule);
                }
            }
            if (g_debug && far_hit && DiagChanged("TEXSCANFAR", CompKey(mc->mod_id, mc->comp_idx) ^ (p << 20), far_hit))
                logger::write("TEXSCANFAR mod=%d comp=%d p=%d: a mod texture matched at t%u, past the old 128-slot window (scan %u)",
                    mc->mod_id, mc->comp_idx, p, far_hit, scan_len[p]);
        }
        // TEXPROBE (one-shot per comp): why textures don't swap -- res-bearing vs hashed vs target-matched.
        if (g_debug && DiagOnce("TEXPROBE", CompKey(mc->mod_id, mc->comp_idx))) {
            logger::write("TEXPROBE mod=%d comp=%d slotsWithRes=%d slotsWithHash=%d targetMatches=%d hSample=%08x (%ux%u fmt=%u) targets=%zu",
                mc->mod_id, mc->comp_idx, pr_res, pr_hash, pr_match, pr_h0, pr_w, pr_hh, pr_fmt, TexRulesNow().targets.size());
        }
    }
}

unsigned UnstampClaimedTextures(const DrawArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const auto&                       root = CmdGfx(cmd_list).root;
    uint32_t                          scan_len[64];
    LayoutTexScanLens(CmdGfxLayout(cmd_list), scan_len);
    unsigned restored = 0;
    for (int p = 0; p < 64; ++p) {
        if (root[p].table.handle == 0) continue;
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        const uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        for (uint32_t s = 0; s < scan_len[p] && ts.base + s < num; ++s) {
            ResInfo ri;
            if (!g_heap_to_res.find({ ts.heap, ts.base + s }, ri) || !ri.res || !ri.swapped) continue;
            const bool ok = UnstampSlot(ts.heap, ts.base + s);
            DrawProbeUnstamp(ri, ok);
            if (!ok) continue;
            ++restored;
            TexHoldForRetry(reinterpret_cast<uint64_t>(ri.res));
        }
    }
    return restored;
}
