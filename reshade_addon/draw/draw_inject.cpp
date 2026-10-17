// draw_inject.cpp -- a claimed draw (or a recognised depth pass) replaced by the mod's geometry: the game's
// bindings snapshotted, the component's section run in its place, everything put back. See draw_parts.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/claim_state.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/skeleton.h"
#include "../instrumentation/draw_dump_publish.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "draw_inject.h"

bool InjectClaimedDraw(const DrawArgs& da, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = da.d3d12_cl;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    instance_count = da.instance_count;
    const uint32_t                    first_index = da.first_index;
    const int32_t                     vertex_offset = da.vertex_offset;
    const uint32_t                    first_instance = da.first_instance;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    bool&                             mc_cb13 = dc.mc_cb13;
    int&                              game_cb13_root = dc.cb13_root;
    int&                              game_cb14_root = dc.cb14_root;
    uint64_t&                         rep_vs = dc.rep_vs;
    bool&                             is_full_shadow_pass = dc.full_shadow;
    if (mc && mc->vanilla) return false;

    if ((mc || is_full_shadow_pass) && !g_disable_vb) {
        ProfScope   _di_vbprep(PROF_DI_VbPrep);
        GpuBracket  _gt(d3d12_cl, GT_ClaimedDraw);
        InjectFrame fr{ da, dc };
        D3D12_VERTEX_BUFFER_VIEW(&save_vb)
        [8] = fr.save_vb;
        bool (&save_vb_set)[8] = fr.save_vb_set;
        D3D12_INDEX_BUFFER_VIEW& save_ib = fr.save_ib;
        bool&                    save_ib_set = fr.save_ib_set;
        {
            auto& st = CmdGfx(cmd_list);
            for (int s = 0; s < 8; ++s) {
                if (st.vb[s].va && st.vb[s].stride > 0) {
                    save_vb[s] = { st.vb[s].va, st.vb[s].size, st.vb[s].stride };
                    save_vb_set[s] = true;
                }
            }
            if (st.ib.va && st.ib.index_bytes > 0) {
                save_ib = { st.ib.va, st.ib.size,
                    st.ib.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
                save_ib_set = true;
            }
        }
        uint64_t apply_vs = (mc && rep_vs) ? rep_vs : vs_hash;

        bool  frozen = (mc && mc->freeze && g_identity_cb_va && mc_cb13);
        bool& cb_ok = fr.cb_ok;
        if (frozen) {
            if (game_cb13_root >= 0 && game_cb13_root < 64) {
                d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb13_root, g_identity_cb_va);
                if (game_cb14_root >= 0 && game_cb14_root < 64)
                    d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb14_root, g_identity_cb_va);
                cb_ok = true;
            }
        }

        // The end-of-draw restore. draw_restore.cpp.
        auto restore_state = [&]() { RestoreGameState(fr); };

        if (mc && !is_full_shadow_pass) {
            const CmdRT& rtc = draw_rt;
            if (g_debug) { // COMPRTV: one-shot per (comp, rtv_count) -- shows each pass's real RT count at DRAW time
                if (DiagOnce("COMPRTV", (mc->comp_idx << 8) | (rtc.rtv_count & 0xff))) {
                    uint64_t psh = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &psh);
                    logger::write("COMPRTV comp=%d rtv_count=%u has_dsv=%d vs=%016llx ps=%016llx",
                        mc->comp_idx, rtc.rtv_count, (int)rtc.has_dsv, vs_hash, psh);
                }
            }
            bool color_pass = (rtc.rtv_count >= 1);
            bool cloth_hit = (color_pass && g_skip_translucent_cloth && mc->comp_idx == 5 && rtc.rtv_count == 2);
            // slider = comp5(bow)-only so sweeping it pins the bow's pass without dropping the whole character.
            bool slider_hit = (color_pass && g_dbg_comp5_suppress_rtv >= 1 && mc->comp_idx == 5 && (int)rtc.rtv_count == g_dbg_comp5_suppress_rtv);
            if (cloth_hit || slider_hit) {
                restore_state();
                return true;
            } // drop this color pass only
        }
        _di_vbprep.End();

        if (g_wwmi_geom && !g_geomods.empty()) {
            // One component's section run in place of this draw (draw_comp.cpp).
            auto draw_comp = [&](int dm, const WwmiMod::Component* wc) { return DrawComponent(fr, dm, wc); };

            if (is_full_shadow_pass) {
                return CastModShadows(fr); // draw_shadow.cpp
            } else if (mc) {
                int                       dm = (mc->mod_id >= 0 && mc->mod_id < (int)g_geomods.size()) ? mc->mod_id : 0;
                const WwmiMod::Component* wc = WmodComp(dm, mc->comp_idx);
                const int                 mc_slot = (int)(mc - g_merged_comps.data());
                RfxDrawState              rfx_draw;
                bool                      drew = false;
                bool                      mc_drew = false;
                auto                      run_slot = [&](int s) {
                    const MergedComp& co = g_merged_comps[s];
                    const bool        own = s == mc_slot;
                    if (!own && (co.vanilla || co.hide)) return;
                    if (drew) restore_state();
                    const bool d = own ? draw_comp(dm, wc) : draw_comp(co.mod_id, WmodComp(co.mod_id, co.comp_idx));
                    if (own) mc_drew = d;
                    drew = d || drew;
                };
                fr.rfx_shared = &rfx_draw;
                if (mc->run_order.empty()) run_slot(mc_slot);
                for (const int s : mc->run_order) run_slot(s);
                fr.rfx_shared = nullptr;
                if (g_debug) { // SKELDBG: per (comp, skeleton-root) -- so depth pass (b4r=9) logs apart
                    if (DiagOnce("DEPTH-CB13", (mc->comp_idx << 8) | (game_cb13_root & 0xff))) {
                        uint64_t psh = 0;
                        cmd_list->get_private_data(s_iid_ps_hash, &psh);
                        logger::write("SKELDBG comp=%d cb13root=%d ps=%016llx remapped=%d drew=%d",
                            mc->comp_idx, game_cb13_root, psh, (int)(wc && wc->remapped), (int)mc_drew);
                    }
                }
                if (drew) {
                    restore_state();
                    return true;
                }
                if (fr.mod_switched_off) {
                    for (int s = 0; s < 8; ++s) {
                        const D3D12_VERTEX_BUFFER_VIEW none{ 0, 0, 0 };
                        d3d12_cl->IASetVertexBuffers(s, 1, save_vb_set[s] ? &save_vb[s] : &none);
                    }
                    if (save_ib_set) d3d12_cl->IASetIndexBuffer(&save_ib);
                    const unsigned unstamped = UnstampClaimedTextures(da);
                    if (g_debug && DiagChanged("SECTION-DECLINED", (mc->mod_id << 8) | (mc->comp_idx & 0xff), unstamped ? 1u : 0u))
                        logger::write("SECTION-DECLINED mod=%d comp=%d f=%u: the mod switched itself off "
                                      "($mod_enabled = 0) and its section drew nothing -- the game's own draw goes "
                                      "through, as on DX11; %u stamped slot(s) given the game's texture back",
                            mc->mod_id, mc->comp_idx, g_frame_id, unstamped);
                    return false;
                }
            }
        }

        PROF(PROF_DI_Tail); // the legacy tail, to the end of this block
        if (g_debug && g_shadow_via_claim && mc) {
            const CmdRT& rtd = draw_rt;
            if (rtd.rtv_count == 0 && rtd.has_dsv) {
                const uint64_t k = apply_vs ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
                if (ProbeArmed() && DiagOnce("DEPTHINJ", (int)(k ^ (k >> 32)))) logger::write("DEPTHINJ comp=%d first=%u count=%u applyVS=%016llx cb_ok=%d cb13root=%d",
                    mc->comp_idx, first_index, index_count, apply_vs, (int)cb_ok, game_cb13_root);
            }
        }

        auto report_to_dump = [&](uint32_t count, uint32_t first, uint32_t insts) {
            if (!MigotoPort::FrameAnalysisArmed()) return;
            uint64_t fd_pso = 0;
            {
                fd_pso = CmdGfx(cmd_list).gfx_pso;
            }
            MigotoPort::SetCurrentInputLayout(fd_pso);
            PublishGameBoundStateForDump(cmd_list);
            PublishGameTexturesForDump(cmd_list);
            DrawCallInfo fci{};
            fci.type = DrawCall::DrawIndexedInstanced;
            fci.IndexCount = count;
            fci.InstanceCount = insts;
            fci.FirstIndex = first;
            fci.FirstInstance = first_instance;
            uint64_t fa_ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
            PublishGameDrawForDump(cmd_list, /*injected*/ true, nullptr, 0, 0, 0); // X24: no RELOCBIND on this path
            MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
            PublishGameDrawForDumpEnd();
        };
        report_to_dump(index_count, first_index, instance_count ? instance_count : 1);
        d3d12_cl->DrawIndexedInstanced(index_count, instance_count ? instance_count : 1,
            first_index, vertex_offset, first_instance);
        RemovalPin::Ours(d3d12_cl, "mod-draw");
        g_last_replace_hash = vs_hash;
        g_last_replace_ib_count = index_count;
        ++g_draw_replace_count;
        restore_state();
        return true; // we drew it; suppress the original
    }
    return false;
}
