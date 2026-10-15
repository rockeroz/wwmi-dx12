// The draw path: every indexed draw the game issues, and the mod geometry we put in its place.
// See draw_hook.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/claim_state.h"
#include "../mods/skeleton.h"
#include "../mods/layout_registry.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "../instrumentation/hot_path.h"
#include "../rt/rt_hooks.h" // RtHoldVanilla
#include <optional>

thread_local uint8_t  t_x23_claimed = 0;
thread_local uint8_t  t_x23_shadow = 0;
thread_local uint8_t  t_x23_cast = 0;
thread_local uint8_t  t_x23_hidden = 0;
thread_local uint32_t t_x23_injected = 0;
thread_local uint8_t  t_x23_merges = 0;

bool on_draw_indexed_body(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    PROF(PROF_DrawIndexed);
    if (g_addon_disabled || g_faulted || !g_is_dx12) return false;
    MigotoPort::FrameAnalysisDrawScope fa_scope;
    uint64_t                           vs_hash = 0;
    cmd_list->get_private_data(s_iid_vs_hash, &vs_hash);
    if (!vs_hash) return false;
    if (g_hide_vs_hash && vs_hash == g_hide_vs_hash) {
        t_x23_hidden = 1;
        return true;
    }

    auto* d3d12_cl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    if (!d3d12_cl) return false;
    const CmdRT    draw_rt = CmdRt(cmd_list); // stable for this draw: only the game's bind hook writes it
    BoneRoots      roots{ cmd_list };
    const DrawArgs args{ cmd_list, d3d12_cl, index_count, instance_count, first_index, vertex_offset,
        first_instance, vs_hash, draw_rt };

    if (g_debug) ShaderDiffNote(cmd_list, vs_hash, index_count);
    NoteMeshPlaces(args);
    CarryPsReads(args);

    if (index_count >= 256 && logger::enabled(3)) { // feeds only the SEEDHB trace line
        uint64_t hbvb0 = 0;
        {
            hbvb0 = CmdGfx(cmd_list).vb[0].va;
        }
        if (hbvb0) {
            std::lock_guard l(g_hb_mtx);
            ++g_hb_large;
            if (hbvb0 < g_hb_vbmin) g_hb_vbmin = hbvb0;
            if (hbvb0 > g_hb_vbmax) g_hb_vbmax = hbvb0;
            if (InSkinBand(hbvb0)) ++g_hb_pool;
        }
    }

    if (g_merged_loaded) {
        auto hit = g_first_to_comp.find(FirstCountKey(first_index, index_count));
        if (hit != g_first_to_comp.end() && g_merged_comps[hit->second].hide && !RtHoldVanilla()) { // RTHOLD
            t_x23_hidden = 1;
            return true;
        }
    }

    struct PostAfterDraw {
        ~PostAfterDraw()
        {
            PortApi::RunPendingPost();
        }
    } post_after_draw;
    if (!RtHoldVanilla() && RunDrawShaderOverrides(args)) return true; // RTHOLD: no ini edits of game draws

    DrawClaim claim;
    if (const std::optional<bool> r = ClaimDraw(args, roots, claim)) return *r;
    MergedComp*& mc = claim.mc;

    DrawProbesClaim(args, claim);

    PROF(PROF_DI_Inject);
    ProfScope _di_prologue(PROF_DI_Prologue);

    LodPinNote(args, roots, claim);

    DrawProbesPasses(args, roots, claim);

    RecogniseShadowPass(args, roots, claim);
    if (g_skip_no_bone13 && mc && (BoneRootMask(cmd_list) & 0x2000u) == 0) mc = nullptr;

    if (mc) {
        HotPath::NoteClaimed();
        t_x23_claimed = 1; // X23: mc survived every gate above
        g_claim_last_list.store((const void*)cmd_list->get_native(), std::memory_order_relaxed);
        if (g_debug) g_claim_thread.store((uint32_t)GetCurrentThreadId(), std::memory_order_relaxed);
    }

    DrawProbeVb6Reader(args, claim);
    DrawProbeOffLeak(args, claim);

    if (g_hide_unclaimed_char && !mc && index_count >= 2000) {
        uint64_t vb0 = 0;
        {
            vb0 = CmdGfx(cmd_list).vb[0].va;
        }
        if (InSkinBand(vb0)) return true;
    }

    if (g_debug) {
        const int fd_b4 = roots.Get().b4;
        const int fd_b3 = roots.Get().b3;
        DiagOnDrawIndexed(cmd_list, d3d12_cl, vs_hash, first_index, index_count, mc, vertex_offset, fd_b4, fd_b3,
            instance_count, first_instance);
    }

    // Prologue ends HERE: everything above ran for all 1035 draws, everything below is claimed-draw work.
    _di_prologue.End();
    StampClaimedTextures(args, claim);

    return InjectClaimedDraw(args, claim);
}
