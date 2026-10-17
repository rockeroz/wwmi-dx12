// present_mod_load.cpp -- mods loaded from their engine's parse: texture overrides and the geometry units.
// See present_parts.h.
#include "../addon_common.h"
#include "../addon_core.h"
#include "../mods/claim_state.h"
#include "../mods/skeleton.h"
#include "../mods/port_section.h"
#include "present_parts.h"

using namespace reshade::api;

static void WwmiBannerTick()
{
    static unsigned s_done_gen = ~0u;
    const unsigned  gen = PortApi::LoadGeneration();
    if (s_done_gen == gen) return;
    s_done_gen = gen;
    const wchar_t* const        fq = L"resource\\wwmiv1\\versionnotification";
    PortApi::BufferResourceInfo bi{};
    if (!PortApi::GetBufferResource(fq, &bi) || !bi.dx12) {
        logger::write("BANNER: WWMI's VersionNotification buffer not found -- its banner stays as shipped");
        return;
    }
    char shipped[64] = {};
    PortApi::BufferInitialData(fq, shipped, (std::min)((size_t)bi.byte_width, sizeof shipped - 1));
    size_t k = 0;
    if (strncmp(shipped, "WWMI ", 5) == 0)
        for (k = 5; shipped[k] && shipped[k] != ' ' && shipped[k] != '\n';) ++k;
    shipped[k] = 0;
    char text[96];
    snprintf(text, sizeof text, "%s\nWWMI-DX12 %s", k ? shipped : "WWMI", WWMI_VERSION);
    const size_t n = strlen(text);
    if (bi.byte_width < n) {
        logger::write("BANNER: VersionNotification is %u B -- '%s' (%zu B) not written", bi.byte_width, text, n);
        return;
    }
    std::vector<char> data(bi.byte_width, 0);
    memcpy(data.data(), text, n);
    if (!PortApi::UploadBufferResource(fq, data.data(), data.size())) {
        logger::write("BANNER: upload failed -- its banner stays as shipped");
        return;
    }
    logger::write("BANNER: WWMI's startup banner now reads '%s' / 'WWMI-DX12 %s'", k ? shipped : "WWMI", WWMI_VERSION);
}

void ModTexturesTick()
{
    if (g_texovr_ready) {
        WwmiBannerTick();
        const size_t described = ModLoader::TexResources().size();
        ModLoader::SubstantiateWclResources();
        if (ModLoader::TexResources().size() > described) TexRetryMissed("new mod textures described");
    }
    if (!g_texovr_ready) {
        PortApi::Load(g_wwmi_dir.c_str());
        ModLoader::LoadTextureOverrides(); // the SRV heap, sized from what their engine declared
        {
            {
                const unsigned h = PortApi::GameConstantBufferHash(kDx11SkeletonCbByteWidth);
                logger::write("PORT-LEAF skeleton CB: ByteWidth=%u -> hash=%08x %s", kDx11SkeletonCbByteWidth,
                    h, h == 0xf02baf77u ? "(matches WWMI MarkBoneDataCB)" : "*** MISMATCH: WWMI expects f02baf77 ***");
            }
            TexRulesPublish();
            TexLodTargetsReady(TexIsTarget, nullptr);
            g_texovr_ready = true;
            {
                static bool s_said = false;
                if (!s_said) {
                    s_said = true;
                    logger::write("PORT-GAP: bare `ps`/`vs` shader-filter operands evaluate to 0 -- their "
                                  "process_shader_filter needs a bound-shader leaf we do not have yet");
                }
            }
        }
    }
}

void LoadGeometryMods()
{
    if (g_wwmi_geom && !g_geom_loaded && g_d3d12_device && !g_wwmi_dir.empty()) {
        for (auto& dm : ModLoader::EnumerateMods()) {
            struct ObjGroup {
                std::wstring              ns;
                std::vector<std::wstring> inis;
            };
            std::vector<ObjGroup> groups;
            for (auto& ini : dm.inis) {
                auto git = std::find_if(groups.begin(), groups.end(),
                    [&](const ObjGroup& g) { return g.ns == ini.ns; });
                if (git == groups.end())
                    groups.push_back({ ini.ns, { ini.path } });
                else
                    git->inis.push_back(ini.path);
            }
            if (groups.empty()) continue;
            {
                auto has_lod = [](const std::wstring& p) {
                    auto lo = [](wchar_t x) { return (x >= L'A' && x <= L'Z') ? (wchar_t)(x + 32) : x; };
                    for (size_t i = 0; i + 3 <= p.size(); ++i)
                        if (lo(p[i]) == L'l' && lo(p[i + 1]) == L'o' && lo(p[i + 2]) == L'd') return true;
                    return false;
                };
                std::stable_partition(groups.begin(), groups.end(),
                    [&](const ObjGroup& g) { return !(has_lod(g.ns) || has_lod(g.inis.front())); });
            }
            for (size_t oi = 0; oi < groups.size(); ++oi) {
                g_geomods.emplace_back(); // popped again below if their engine cannot describe it
                GeoMod&   gm = g_geomods.back();
                const int mod_id = (int)g_geomods.size() - 1;
                gm.wcl_ns = groups[oi].ns; // their namespace for this unit's inis, asked once above
                if (WwmiMod::ReadComponentsFromEngine(gm.wcl_ns, gm.mod) <= 0 || gm.mod.comps.empty()) {
                    g_geomods.pop_back();
                    continue;
                }
                if (gm.mod.dir.empty()) gm.mod.dir = dm.dir; // only if their SectionFolder had none
                WwmiMod::LoadResources(gm.mod, g_d3d12_device);
                if (gm.mod.queries_ps_slots) g_ps_slot_queries.store(true, std::memory_order_relaxed);
                if (!gm.mod.forward_map_file.empty()) {
                    std::wstring rel = gm.mod.forward_map_file;
                    for (auto& c : rel)
                        if (c == L'/') c = L'\\';
                    const bool rooted = (rel.size() > 2 && rel[1] == L':') || (rel.size() > 1 && rel[0] == L'\\');
                    gm.fwdmap = rooted ? rel : (gm.mod.dir + L"\\" + rel);
                }
                g_ns_to_mod[gm.wcl_ns] = mod_id;
                for (auto& c : g_geomods[mod_id].mod.comps) {
                    MergedComp mc;
                    mc.mod_id = mod_id;
                    mc.comp_idx = c.index;
                    mc.first_index = c.match_first_index;
                    mc.match_count = c.match_index_count;
                    mc.remap_size = 0;
                    g_merged_comps.push_back(mc);
                }
                g_first_to_comp.clear();
                g_first_to_shared.clear();
                for (size_t i = 0; i < g_merged_comps.size(); ++i) {
                    const uint64_t key = FirstCountKey(g_merged_comps[i].first_index, g_merged_comps[i].match_count);
                    auto           prev = g_first_to_comp.find(key);
                    if (prev != g_first_to_comp.end()) {
                        auto& shared = g_first_to_shared[key];
                        if (shared.empty()) shared.push_back(prev->second);
                        shared.push_back((int)i);
                    }
                    g_first_to_comp[key] = (int)i;
                }
                logger::write("WwmiGeom: geometry unit #%d ns='%ls' guid=%d '%ls' (%zu comps) fwdmap=%ls",
                    mod_id, g_geomods[mod_id].wcl_ns.c_str(), g_geomods[mod_id].mod.object_guid,
                    g_geomods[mod_id].mod.dir.c_str(), g_geomods[mod_id].mod.comps.size(),
                    g_geomods[mod_id].fwdmap.c_str());
            }
        }
        if (!g_geomods.empty()) {
            int reachable = 0;
            int inert = 0;
            for (size_t i = 0; i < g_geomods.size(); ++i) {
                const auto& gm = g_geomods[i];
                if (gm.mod.comps.empty()) continue;
                const WwmiMod::Component& c0 = gm.mod.comps.front();
                const std::wstring        fq = PortComponentSection((int)i, c0.index, c0.tail, gm.wcl_ns);
                if (!fq.empty()) {
                    ++reachable;
                    continue;
                }
                ++inert;
                logger::write("PORT-NS INERT mod %zu ns='%ls' -- their engine has no [TextureOverride...%ls] "
                              "under that namespace, so this mod will never run a section",
                    i, gm.wcl_ns.c_str(), c0.tail.c_str());
            }
            logger::write("PORT-NS audit: %d of %zu geometry mods can resolve their own component sections%s",
                reachable, g_geomods.size(),
                inert ? "  *** SOME ARE INERT, see above ***" : "");
        }
        g_ibhash_slot.clear();
        g_mod_index_end.assign(g_geomods.size(), {});
        for (size_t i = 0; i < g_merged_comps.size(); ++i) {
            const MergedComp&         mc = g_merged_comps[i];
            const WwmiMod::Component* wc = WmodComp(mc.mod_id, mc.comp_idx);
            if (wc && wc->ib_hash) g_ibhash_slot.emplace(wc->ib_hash, (int)i);
            if (mc.mod_id >= 0 && mc.mod_id < (int)g_mod_index_end.size()) {
                uint32_t& end = g_mod_index_end[mc.mod_id][wc ? wc->ib_hash : 0u];
                end = (std::max)(end, mc.first_index + mc.match_count);
            }
        }
        for (MergedComp& mc : g_merged_comps) mc.with.clear();
        for (const auto& kv : g_first_to_shared) {
            for (const int a : kv.second) {
                const WwmiMod::Component* ca = WmodComp(g_merged_comps[a].mod_id, g_merged_comps[a].comp_idx);
                if (!ca || !ca->ib_hash) continue;
                for (const int b : kv.second) {
                    const WwmiMod::Component* cb = WmodComp(g_merged_comps[b].mod_id, g_merged_comps[b].comp_idx);
                    if (b != a && cb && cb->ib_hash == ca->ib_hash) g_merged_comps[a].with.push_back(b);
                }
                if (a == kv.second.front() && !g_merged_comps[a].with.empty())
                    logger::write("WwmiGeom: hash=%08x first=%u count=%u is declared by %zu mods -- every one's section runs "
                                  "on its draw, as on DX11",
                        ca->ib_hash, g_merged_comps[a].first_index, g_merged_comps[a].match_count,
                        g_merged_comps[a].with.size() + 1);
            }
        }
        {
            auto priority = [](int s) {
                const WwmiMod::Component* c = WmodComp(g_merged_comps[s].mod_id, g_merged_comps[s].comp_idx);
                return c ? c->match_priority : 0;
            };
            auto section = [](int s) {
                const MergedComp&         m = g_merged_comps[s];
                const WwmiMod::Component* c = WmodComp(m.mod_id, m.comp_idx);
                return c ? PortComponentSection(m.mod_id, c->index, c->tail, g_geomods[m.mod_id].wcl_ns) : std::wstring();
            };
            for (size_t a = 0; a < g_merged_comps.size(); ++a) {
                MergedComp& mc = g_merged_comps[a];
                mc.run_order.assign(1, (int)a);
                mc.run_order.insert(mc.run_order.end(), mc.with.begin(), mc.with.end());
                if (mc.run_order.size() < 2) continue;
                std::stable_sort(mc.run_order.begin(), mc.run_order.end(), [&](int x, int y) {
                    const int px = priority(x);
                    const int py = priority(y);
                    return px != py ? px < py : section(x) < section(y);
                });
                if ((int)a != mc.run_order.front()) continue;
                std::string order;
                for (const int s : mc.run_order)
                    order += " mod" + std::to_string(g_merged_comps[s].mod_id) + "(" + std::to_string(priority(s)) + ")";
                logger::write("WwmiGeom: first=%u count=%u -- its sections run lowest match_priority first:%s",
                    mc.first_index, mc.match_count, order.c_str());
            }
        }
        if (!g_geomods.empty()) FinalizeMergedRemap(); // one-time shared setup + g_merged_loaded
        g_geom_loaded = true;
    }
}
