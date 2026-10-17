// draw_claim.cpp -- which mod component a game draw is: the [ShaderOverride*] dispatch by shader hash, the
// mesh-hash probe and suppress, the claim, and the LOD pin. See draw_parts.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/claim_state.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_hooks.h"
#include "../rt/rt_hooks.h"
#include "../mods/shader_alias.h" // RunShaderOverrides, t_cur_vs_hash
#include "draw_ctx.h"
#include "draw_parts.h"
#include <optional>

using namespace reshade::api;

bool RunDrawShaderOverrides(const DrawArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    instance_count = da.instance_count;
    const uint32_t                    first_index = da.first_index;
    const int32_t                     vertex_offset = da.vertex_offset;
    const uint32_t                    first_instance = da.first_instance;
    const uint64_t                    vs_hash = da.vs_hash;
    if (g_shader_override_dispatch) {
        PortApi::DrawInfo ddi;
        ddi.index_count = index_count;
        ddi.first_index = first_index;
        ddi.first_vertex = (unsigned)vertex_offset;
        ddi.instance_count = instance_count;
        ddi.first_instance = first_instance;
        ddi.indexed = true;
        uint64_t draw_ps_h = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &draw_ps_h);
        t_cur_vs_hash = vs_hash;
        t_cur_ps_hash = draw_ps_h; // for the bare ps/vs filter operand
        uint64_t draw_gs_h = 0;
        cmd_list->get_private_data(s_iid_gs_hash, &draw_gs_h);
        const uint64_t vs_ini = AliasShaderHash(vs_hash);
        const uint64_t ps_ini = AliasShaderHash(draw_ps_h);
        const uint64_t gs_ini = AliasShaderHash(draw_gs_h);
        if (g_publish_shader_state &&
            (PortApi::ShaderOverrideMatches(vs_ini) || PortApi::ShaderOverrideMatches(ps_ini) || PortApi::ShaderOverrideMatches(gs_ini))) {
            PortApi::SetBoundShader('v', vs_ini);
            PortApi::SetBoundShader('p', ps_ini);
            PortApi::SetBoundShader('g', gs_ini);
        }
        bool sk = false;
        if (vs_hash) sk |= RunShaderOverrides(vs_hash, &ddi, cmd_list);
        if (draw_ps_h) sk |= RunShaderOverrides(draw_ps_h, &ddi, cmd_list);
        if (draw_gs_h) sk |= RunShaderOverrides(draw_gs_h, &ddi, cmd_list);
        if (sk) {
            if (g_debug && DiagOnce("SHADEROVR-SKIP", (int)((vs_hash ^ draw_ps_h) ^ ((vs_hash ^ draw_ps_h) >> 32)))) {
                logger::write("SHADEROVR-SKIP vs=%016llx ps=%016llx (ini handling=skip)", vs_hash, draw_ps_h);
            }
            return true; // suppressed exactly as the ini asks
        }
    }
    return false;
}

static int PickSharedRangeSlot(command_list* cmd_list, uint32_t first_index, uint32_t index_count, int dflt)
{
    auto sh = g_first_to_shared.find(FirstCountKey(first_index, index_count));
    if (sh == g_first_to_shared.end()) return dflt;
    const std::vector<int>& cands = sh->second;
    ID3D12Resource*         ibr = nullptr;
    ID3D12Resource*         vbr = nullptr;
    uint64_t                ibva = 0;
    uint64_t                vbva = 0;
    uint32_t                idxb = 2;
    uint32_t                vstride = 0;
    {
        auto& st = CmdGfx(cmd_list);
        ibr = st.ib.res;
        ibva = st.ib.va;
        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
        vbr = st.vb[0].res;
        vbva = st.vb[0].va;
        vstride = st.vb[0].stride;
    }
    if (vbva) {
        std::lock_guard l(g_char_mesh_mtx);
        auto            cv = g_char_mesh_vb0.find(vbva);
        if (cv != g_char_mesh_vb0.end())
            for (int s : cands)
                if (g_merged_comps[s].mod_id == cv->second.mod) return s;
    }
    if (!ibr || !vbr || !vstride) return dflt;
    const uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
    const uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
    for (int s : cands) {
        const int                 md = g_merged_comps[s].mod_id;
        const WwmiMod::Component* wc = WmodComp(md, g_merged_comps[s].comp_idx);
        if (!wc || !wc->ib_hash) continue;
        const uint32_t max_end = ModIndexEnd(md, wc->ib_hash);
        if (max_end && IbHash::Lookup(ibr, iboff, max_end * idxb, idxb, vbr, vboff, vstride) == wc->ib_hash) {
            if (g_debug && DiagChanged("SHAREDRANGE", (int)(first_index ^ index_count), (unsigned long long)s))
                logger::write("SHAREDRANGE first=%u count=%u: %zu mods declare this range; the index buffer names mod %d comp %d",
                    first_index, index_count, cands.size(), md, g_merged_comps[s].comp_idx);
            return s;
        }
    }
    return dflt;
}

std::optional<bool> ClaimDraw(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = da.d3d12_cl;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    MergedComp*&                      mc = dc.mc;
    MergedComp*&                      depth_claim = dc.depth_claim;
    bool&                             mc_cb13 = dc.mc_cb13;
    uint64_t&                         game_cb13_va = dc.cb13_va;
    uint64_t&                         game_cb14_va = dc.cb14_va;
    ID3D12Resource*&                  port_cb4_res = dc.port_cb4_res;
    uint64_t&                         port_cb4_va = dc.port_cb4_va;
    ID3D12Resource*&                  port_cb3_res = dc.port_cb3_res;
    uint64_t&                         port_cb3_va = dc.port_cb3_va;
    int&                              game_cb13_root = dc.cb13_root;
    int&                              game_cb14_root = dc.cb14_root;
    uint64_t&                         rep_vs = dc.rep_vs;
    if (g_merged_loaded && g_merged_enable) {
        auto fi_it = g_first_to_comp.find(FirstCountKey(first_index, index_count));
        int  mc_slot = (fi_it != g_first_to_comp.end()) ? (int)fi_it->second : -1;
        if (mc_slot >= 0) mc_slot = PickSharedRangeSlot(cmd_list, first_index, index_count, mc_slot);
        const bool skip_any = g_skip_vbhash_any.load(std::memory_order_relaxed);
        if ((g_debug || g_ibhash_suppress || skip_any) && mc_slot < 0 && index_count >= 256) {
            uint64_t bvb0 = 0;
            bool     vb6 = false;
            {
                const auto& cg = CmdGfx(cmd_list);
                bvb0 = cg.vb[0].va;
                vb6 = cg.vb[6].res != nullptr;
            }
            const bool skinned = InSkinBand(bvb0) || vb6; // a character mesh: in the skin-cache band, or reading the character's stream 6
            if (g_debug && skip_any && !skinned && DiagOnce("SKIPONLY-BAND", (int)(first_index * 31u + index_count)))
                logger::write("SKIPONLY-BAND vs=%016llx first=%u count=%u vb0=%llx outside the skin band %llx..%llx, no vb6 -- not hashed",
                    (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0,
                    (unsigned long long)g_skin_band_lo.load(), (unsigned long long)g_skin_band_hi.load());
            if (skinned) {
                ID3D12Resource* ibr = nullptr;
                ID3D12Resource* vbr = nullptr;
                uint64_t        ibva = 0;
                uint64_t        vbva = 0;
                uint32_t        idxb = 2;
                uint32_t        vstride = 0;
                {
                    auto& st = CmdGfx(cmd_list);
                    ibr = st.ib.res;
                    ibva = st.ib.va;
                    if (st.ib.index_bytes) idxb = st.ib.index_bytes;
                    vbr = st.vb[0].res;
                    vbva = st.vb[0].va;
                    vstride = st.vb[0].stride;
                }
                if (ibr && vbr && vstride) {
                    uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                    uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                    uint32_t ib_bytes = (first_index + index_count) * idxb; // drawn extent (single-section LOD = full LOD IB)
                    // Lookup is a cached hash-map hit per (vb bind VA) after the first async compute -> per-draw cheap.
                    uint32_t                                            got = IbHash::Lookup(ibr, iboff, ib_bytes, idxb, vbr, vboff, vstride, /*grow*/ true);
                    std::shared_ptr<const std::unordered_set<uint32_t>> skip_vbhash;
                    if (got && skip_any) {
                        std::lock_guard<std::mutex> ls(g_skip_vbhash_mtx);
                        skip_vbhash = g_skip_vbhash_snap;
                    }
                    if (g_debug && skip_any && DiagOnce("SKIPONLY-SEEN", (int)((first_index * 31u + index_count) ^ (got ? 0x40000000u : 0u))))
                        logger::write("SKIPONLY-SEEN vs=%016llx first=%u count=%u vb0=%llx ibBytes=%u stride=%u -> hash %08x%s",
                            (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0, ib_bytes, vstride, got,
                            !got ? " (pending)" : (skip_vbhash && skip_vbhash->count(got) ? " IN the skip set" : ""));
                    if (skip_vbhash && skip_vbhash->count(got) && !RtHoldVanilla()) { // RTHOLD: no mod commands on game draws
                        PortApi::DrawInfo di;
                        di.first_index = first_index;
                        di.index_count = index_count;
                        PortApi::DispatchTextureOverride(got, &di);
                        if (di.skip) {
                            ++g_skip_vbhash_drops;
                            if (g_debug && DiagOnce("SKIPONLY-DROP", (int)((vs_hash ^ got) ^ ((vs_hash ^ got) >> 32))))
                                logger::write("SKIPONLY-DROP vs=%016llx first=%u count=%u hash=%08x -- the game's draw is cancelled",
                                    (unsigned long long)vs_hash, first_index, index_count, got);
                            return true;
                        }
                    }
                    int      match_mod = -1;
                    int      match_comp = -1;
                    uint32_t match_hash = 0;
                    if (got)
                        if (auto hs = g_ibhash_slot.find(got); hs != g_ibhash_slot.end()) {
                            match_mod = g_merged_comps[hs->second].mod_id;
                            match_comp = g_merged_comps[hs->second].comp_idx;
                            match_hash = got;
                        }
                    bool never_claims = false;
                    if (match_mod >= 0) {
                        std::lock_guard l(g_claimed_ibhash_mtx);
                        never_claims = (g_claimed_ibhash.count(got) == 0);
                    }
                    if (g_ibhash_suppress && match_mod >= 0 && never_claims && !RtHoldVanilla()) {
                        ++g_ibhash_suppress_count;
                        if (g_debug && ProbeArmed() && DiagOnce("IBHASH-SUPPRESS", (int)((vs_hash ^ got) ^ ((vs_hash ^ got) >> 32)))) {
                            logger::trace("IBHASH-SUPPRESS vs=%016llx first=%u count=%u hash=%08x mod=%d comp=%d (LOD proxy dropped)",
                                (unsigned long long)vs_hash, first_index, index_count, got, match_mod, match_comp);
                        }
                        return true;
                    }
                    if (g_debug) {
                        static std::atomic<int> npend{ 0 };
                        bool                    fresh;
                        if (got) {
                            const uint64_t k = vs_hash ^ ((uint64_t)got << 32);
                            fresh = ProbeArmed() && DiagOnce("IBHASH-PROBE", (int)(k ^ (k >> 32)));
                        } else
                            fresh = (npend.fetch_add(1, std::memory_order_relaxed) < 20);
                        if (fresh) {
                            const RootCbvBind b4cb = roots.Get().b4cb;
                            uint32_t          skh = HashBoneCB(b4cb.res, b4cb.va);
                            bool              inskel = false;
                            {
                                std::lock_guard l(g_char_skel_mtx);
                                inskel = skh && g_frame_skel.count(skh) != 0;
                            }
                            logger::trace("IBHASH-PROBE vs=%016llx first=%u count=%u vb0=%llx ourVbHash=%08x inSkel=%d -> %s",
                                (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0, got, (int)inskel,
                                got ? (match_mod >= 0 ? (never_claims ? "MATCH never-claims (proxy!)" : "MATCH but hash also claims (main pass)") : "nomatch") : "pending");
                            if (match_mod >= 0) logger::write("   IBHASH-PROBE MATCH mod=%d comp=%d iniHash=%08x", match_mod, match_comp, match_hash);
                        }
                    }
                }
            }
        }
        if (mc_slot >= 0) {
            PROF(PROF_DI_ClaimInject); // per-claimed-draw claim bookkeeping
            int         ci = g_merged_comps[mc_slot].comp_idx;
            int         md = g_merged_comps[mc_slot].mod_id; // which geometry mod owns this draw
            RootCbvBind cur = {};
            RootCbvBind prev = {};
            uint64_t    vb0 = 0;
            int         b4_root = -1;
            int         b3_root = -1;
            uint64_t    all_vb_va[8] = {}; // every slot the GAME has bound at this draw -- see VBSLOTS
            uint32_t    all_vb_sz[8] = {};
            uint32_t    all_vb_str[8] = {};
            uint64_t    all_vb_res_w[8] = {}; // the RESOURCE's own width -- see VBSLOTS
            {
                auto& st = CmdGfx(cmd_list);
                vb0 = st.vb[0].va;
                for (unsigned q = 0; q < 8; ++q) {
                    all_vb_va[q] = st.vb[q].va;
                    all_vb_sz[q] = st.vb[q].size;
                    all_vb_str[q] = st.vb[q].stride;
                    all_vb_res_w[q] = st.vb[q].res_width;
                }
                const BoneRoots& br = roots.Get();
                b4_root = br.b4;
                b3_root = br.b3;
                cur = br.b4cb;
                prev = br.b3cb;
            }
            port_cb4_res = cur.res;
            port_cb4_va = cur.va;
            port_cb3_res = prev.res;
            port_cb3_va = prev.va;
            bool         char_vb = false;
            CharVbOrigin char_vb_org; // who learned this VA, and when (claim_state.h)
            {
                std::lock_guard l(g_char_mesh_mtx);
                if (vb0) {
                    auto cv = g_char_mesh_vb0.find(vb0);
                    if (cv != g_char_mesh_vb0.end()) {
                        char_vb = true;
                        char_vb_org = cv->second;
                    }
                }
            }
            if (char_vb) {
                g_charvb_claims.fetch_add(1, std::memory_order_relaxed);
                if (char_vb_org.mod >= 0 && char_vb_org.mod != md) {
                    g_charvb_xmod.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug && LogBudget("CHARVB-XMOD", L"", 12))
                        logger::write("CHARVB-XMOD: draw names mod %d comp %d (first=%u count=%u vs=%016llx) on vb0=%llx "
                                      "learned from mod %d %u frames ago%s",
                            md, ci, first_index, index_count,
                            vs_hash, (unsigned long long)vb0, char_vb_org.mod, g_frame_id - char_vb_org.frame,
                            g_charvb_refuse_xmod ? " -- REFUSED (H17)" : "");
                    if (g_charvb_refuse_xmod) {
                        char_vb = false;
                        g_charvb_refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (char_vb && logger::enabled(1)) {
                const bool fr = DiagOnce((const void*)&on_draw_indexed_body,
                    md * 4096 + (int)(first_index % 4096));
                if (fr) {
                    char sl[512];
                    int  n = 0;
                    for (unsigned q = 0; q < 8; ++q)
                        n += snprintf(sl + n, sizeof(sl) - (size_t)n, " vb%u=%llx/%u/%u res=%llu", q,
                            (unsigned long long)all_vb_va[q], all_vb_sz[q], all_vb_str[q],
                            (unsigned long long)all_vb_res_w[q]);
                    logger::write("VBSLOTS mod=%d first_index=%u (va/size/stride):%s", md,
                        first_index, sl);
                }
            }
            if (!char_vb) {
                std::lock_guard l(g_char_skel_mtx);
                char_vb = (vb0 && g_char_lod_vb0.count(vb0) != 0);
            }
            bool     char_ib = false;
            bool     has_ib_ref = false;
            uint32_t ib_expect = 0; // RTCLAIM: the ini's hash= and what we computed for it
            uint32_t ib_got = 0;
            if (!char_vb) {
                const WwmiMod::Component* wc2 = WmodComp(md, ci);
                has_ib_ref = (wc2 && wc2->ib_hash);
                if (has_ib_ref) {
                    ib_expect = wc2->ib_hash;
                    const uint32_t  max_end = ModIndexEnd(md, wc2->ib_hash);
                    ID3D12Resource* ibr = nullptr;
                    ID3D12Resource* vbr = nullptr;
                    uint64_t        ibva = 0;
                    uint64_t        vbva = 0;
                    uint32_t        idxb = 2;
                    uint32_t        vstride = 0;
                    {
                        auto& st = CmdGfx(cmd_list);
                        ibr = st.ib.res;
                        ibva = st.ib.va;
                        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
                        vbr = st.vb[0].res;
                        vbva = st.vb[0].va;
                        vstride = st.vb[0].stride;
                    }
                    if (ibr && vbr && max_end && vstride) {
                        uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                        uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                        ib_got = IbHash::Lookup(ibr, iboff, max_end * idxb, idxb, vbr, vboff, vstride);
                        char_ib = (ib_got == wc2->ib_hash);
                    }
                }
            }
            bool char_tex = false;
            if (!char_vb && !char_ib && !has_ib_ref) char_tex = DrawBindsModTexture(cmd_list);
            bool is_mod_char = char_ib || char_tex; // a definitive identification this draw
            bool char_ibset = false;
            if (g_ib_anchor_claim) {
                uint64_t claim_ib_va = 0;
                uint64_t claim_ib_bytes = 0;
                {
                    const auto& gs = CmdGfx(cmd_list);
                    claim_ib_va = gs.ib.va;
                    claim_ib_bytes = gs.ib.size;
                }
                char_ibset = IbAnchorNoteAndAsk(claim_ib_va, claim_ib_bytes, md, ci);
                if (char_ibset && !is_mod_char && !char_vb)
                    g_ib_anchor_claims.fetch_add(1, std::memory_order_relaxed);
            }
            bool rt_hold = false; // RTHOLD (rt_hooks.h): the RT module keeps this draw the game's own
            {
                RtClaimInfo ri;
                ri.cmd_list = cmd_list;
                ri.mod = md;
                ri.comp = ci;
                ri.identified = is_mod_char || char_vb || char_ibset;
                ri.vb0 = all_vb_va[0];
                ri.vb0_bytes = all_vb_sz[0];
                ri.vb0_stride = all_vb_str[0];
                {
                    const auto& gs = CmdGfx(cmd_list);
                    ri.ib_va = gs.ib.va;
                    ri.index_bytes = gs.ib.index_bytes;
                }
                ri.first_index = first_index;
                ri.index_count = index_count;
                ri.vertex_offset = da.vertex_offset;
                ri.vs_hash = vs_hash;
                rt_hold = RtClaimNote(ri);
            }
            if (g_debug && !is_mod_char && !char_vb && !char_ibset && DiagOnce("RTCLAIM", CompKey(md, ci)))
                logger::write("RTCLAIM mod=%d comp=%d first=%u count=%u NOT IDENTIFIED: "
                              "char_vb=0 (vb0=%llx stride=%u inSkinPool=%d -- never learned) | char_ib=0 (ini hash=%08x computed=%08x%s) | "
                              "char_tex %s | vb6=%llx",
                    md, ci, first_index, index_count, (unsigned long long)vb0, all_vb_str[0],
                    (int)InSkinBand(vb0), ib_expect, ib_got,
                    !has_ib_ref ? ", no hash= in the ini" : (ib_got == 0 ? ", NOT COMPUTED YET" : ", MISMATCH"),
                    has_ib_ref ? "skipped (the mod carries hash=)" : "probed and missed",
                    (unsigned long long)all_vb_va[6]);
            if (is_mod_char && !rt_hold) { // RTHOLD: vb0 is the skin cache's posed ring, not a bind-pose mesh
                const uint32_t sh = HashBoneCB(cur.res, cur.va);
                if (vb0) {
                    std::lock_guard l(g_char_mesh_mtx);
                    g_char_mesh_vb0[vb0] = CharVbOrigin{ g_frame_id, md };
                } // learned by THIS mod, now
                if (sh) {
                    std::lock_guard l(g_char_skel_mtx);
                    g_frame_skel.insert(sh);
                }
                if (g_debug && char_tex && DiagOnce("CHARTEX", ci)) {
                    logger::write("CHARTEX comp=%d claimed via texture-identity (vs=%016llx no rule) first=%u count=%u",
                        ci, vs_hash, first_index, index_count);
                }
            }
            if ((is_mod_char || char_vb || char_ibset) && !rt_hold) { // RTHOLD: a held mod is not on screen
                g_geomods[md].last_char_frame = g_frame_id;
                for (const int s : g_merged_comps[mc_slot].with) g_geomods[g_merged_comps[s].mod_id].last_char_frame = g_frame_id;
            }
            if (g_debug && (is_mod_char || char_vb || char_ibset)) { // RTDBG: see RtNoteDrawVb
                RtDrawVb v;
                v.frame = g_frame_id;
                v.va = all_vb_va[0];
                v.size = all_vb_sz[0];
                v.stride = all_vb_str[0];
                v.vs = vs_hash;
                RtNoteDrawVb(md, ci, v);
            }
            if ((is_mod_char || char_vb || char_ibset) && !rt_hold) {
                const WwmiMod::Component* wcc = WmodComp(md, ci);
                if (wcc && wcc->ib_hash) {
                    std::lock_guard l(g_claimed_ibhash_mtx);
                    g_claimed_ibhash.insert(wcc->ib_hash);
                }
            }
            if ((is_mod_char || char_vb || char_ibset) && (g_wcl_draw || ModGeomEnabled(md)) && !rt_hold) {
                mc = &g_merged_comps[mc_slot];
                if (g_decensor) PsoNoteCharacterDraw(CmdGfx(cmd_list).gfx_pso);
                RemovalPin::Mark(d3d12_cl, "game: claimed character draw"); // DRED breadcrumb context
                rep_vs = vs_hash;
                mc_cb13 = (cur.res && cur.va);
                game_cb13_va = cur.va;  // CURRENT skeleton CBV (b4 root)
                game_cb14_va = prev.va; // PREVIOUS skeleton CBV (b3 root)
                if (g_debug) {          // POOLDRAW's boneNear (draw_probes.cpp): the frame's character bone-CB VAs
                    std::lock_guard l(g_skincache_mtx);
                    if (cur.va && g_char_bone_va.size() < 64) g_char_bone_va.insert(cur.va);
                    if (prev.va && g_char_bone_va.size() < 64) g_char_bone_va.insert(prev.va);
                }
                game_cb13_root = b4_root;
                game_cb14_root = b3_root;
                {
                    const CmdRT& rtg = draw_rt;
                    if (g_debug) {
                        bool           is_depth = (rtg.rtv_count == 0 && rtg.has_dsv);
                        bool           in_pool = InSkinBand(vb0);
                        const uint64_t k = ((uint64_t)is_depth << 62) ^ ((uint64_t)index_count << 8) ^ (uint64_t)ci;
                        if (DiagOnce("SKINSRC", (int)(k ^ (k >> 32)))) logger::trace("SKINSRC pass=%s comp=%d idx=%u vb0=%llx inSkinPool=%d b4root=%d boneVA=%llx rtv=%u dsv=%d",
                            is_depth ? "DEPTH" : "GBUF", ci, index_count, (unsigned long long)vb0, (int)in_pool, b4_root,
                            (unsigned long long)cur.va, rtg.rtv_count, (int)rtg.has_dsv);
                    }
                }
            }
            if (mc && !g_shadow_via_claim) {
                const CmdRT& rt = draw_rt;
                if (rt.rtv_count == 0 && rt.has_dsv) {
                    depth_claim = mc;
                    mc = nullptr;
                }
            }
        }
    }
    return std::nullopt;
}

void LodPinNote(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const uint64_t                    vs_hash = da.vs_hash;
    MergedComp*&                      mc = dc.mc;
    if (g_lod_pin && !mc && index_count >= 256) {
        const uint64_t        vb0 = CmdGfx(cmd_list).vb[0].va;
        ID3D12Resource* const b4res = roots.Get().b4cb.res;
        const uint64_t        b4va = roots.Get().b4cb.va;
        uint32_t              sh = HashBoneCB(b4res, b4va);
        bool                  linked_now = false;
        bool                  recognized = false;
        {
            std::lock_guard l(g_char_skel_mtx);
            if (sh && vb0 && g_frame_skel.count(sh)) {
                if (g_char_lod_vb0.insert(vb0).second) linked_now = true;
            }
            recognized = vb0 && g_char_lod_vb0.count(vb0) != 0;
        }
        if (linked_now) logger::write("LODPIN linked char LOD buffer vb0=%llx (vs=%016llx first=%u count=%u)", vb0, vs_hash, first_index, index_count);
        if (recognized) {
            uint64_t key = vs_hash ^ ((uint64_t)first_index << 3) ^ ((uint64_t)index_count << 17);
            if (DiagOnce("LODPIN", (int)(key ^ (key >> 32)))) {
                uint64_t ps_hash = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
                logger::write("LODPIN char lower-LOD: vs=%016llx ps=%016llx first=%u count=%u vb0=%llx",
                    vs_hash, ps_hash, first_index, index_count, vb0);
            }
        }
    }
}
