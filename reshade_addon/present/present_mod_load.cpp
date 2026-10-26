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

namespace {
// One geometry unit: the inis of one mod folder that share a namespace.
struct UnitGroup {
    std::wstring              ns;
    std::vector<std::wstring> inis;
    std::wstring              dir;
};

std::vector<UnitGroup> EnumerateUnitGroups()
{
    std::vector<UnitGroup> out;
    for (auto& dm : ModLoader::EnumerateMods()) {
        std::vector<UnitGroup> groups;
        for (auto& ini : dm.inis) {
            auto git = std::find_if(groups.begin(), groups.end(), [&](const UnitGroup& g) { return g.ns == ini.ns; });
            if (git == groups.end())
                groups.push_back({ ini.ns, { ini.path }, dm.dir });
            else
                git->inis.push_back(ini.path);
        }
        if (groups.empty()) continue;
        auto has_lod = [](const std::wstring& p) {
            auto lo = [](wchar_t x) { return (x >= L'A' && x <= L'Z') ? (wchar_t)(x + 32) : x; };
            for (size_t i = 0; i + 3 <= p.size(); ++i)
                if (lo(p[i]) == L'l' && lo(p[i + 1]) == L'o' && lo(p[i + 2]) == L'd') return true;
            return false;
        };
        std::stable_partition(groups.begin(), groups.end(),
            [&](const UnitGroup& g) { return !(has_lod(g.ns) || has_lod(g.inis.front())); });
        for (auto& g : groups) out.push_back(std::move(g));
    }
    return out;
}

uint64_t FpMix(uint64_t h, uint64_t v)
{
    return (h ^ v) * 1099511628211ull;
}
uint64_t FpFile(uint64_t h, const std::wstring& path)
{
    for (wchar_t c : path) h = FpMix(h, (uint64_t)((c >= L'A' && c <= L'Z') ? c + 32 : c));
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
        h = FpMix(h, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
        h = FpMix(h, ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime);
    } else {
        h = FpMix(h, 0x9e3779b97f4a7c15ull); // gone
    }
    return h;
}
// What a unit is made of on disk: its inis and the files its buffers load (a change to either = another unit).
uint64_t UnitFingerprint(const std::vector<std::wstring>& inis, const WwmiMod::Mod& mod)
{
    uint64_t h = 1469598103934665603ull;
    for (const auto& p : inis) h = FpFile(h, p);
    for (const auto& r : mod.resources)
        if (!r.filename.empty()) h = FpFile(h, r.filename);
    if (!mod.forward_map_file.empty()) h = FpFile(h, mod.forward_map_file);
    return h;
}

bool CompLive(size_t i)
{
    const int md = g_merged_comps[i].mod_id;
    return md >= 0 && md < (int)g_geomods.size() && !g_geomods[md].dead;
}

// A unit for this group, its buffers adopted from their engine and its components appended; its id, or -1 when their
// engine cannot describe it (nothing is added then).
int AddUnit(const UnitGroup& g)
{
    g_geomods.emplace_back(); // popped again below if their engine cannot describe it
    GeoMod&   gm = g_geomods.back();
    const int mod_id = (int)g_geomods.size() - 1;
    gm.wcl_ns = g.ns; // their namespace for this unit's inis
    if (WwmiMod::ReadComponentsFromEngine(gm.wcl_ns, gm.mod) <= 0 || gm.mod.comps.empty()) {
        g_geomods.pop_back();
        return -1;
    }
    if (gm.mod.dir.empty()) gm.mod.dir = g.dir; // only if their SectionFolder had none
    WwmiMod::LoadResources(gm.mod, g_d3d12_device);
    if (gm.mod.queries_ps_slots) g_ps_slot_queries.store(true, std::memory_order_relaxed);
    if (!gm.mod.forward_map_file.empty()) {
        std::wstring rel = gm.mod.forward_map_file;
        for (auto& c : rel)
            if (c == L'/') c = L'\\';
        const bool rooted = (rel.size() > 2 && rel[1] == L':') || (rel.size() > 1 && rel[0] == L'\\');
        gm.fwdmap = rooted ? rel : (gm.mod.dir + L"\\" + rel);
    }
    gm.files_fp = UnitFingerprint(g.inis, gm.mod);
    g_ns_to_mod[gm.wcl_ns] = mod_id;
    for (auto& c : gm.mod.comps) {
        MergedComp mc;
        mc.mod_id = mod_id;
        mc.comp_idx = c.index;
        mc.first_index = c.match_first_index;
        mc.match_count = c.match_index_count;
        mc.remap_size = 0;
        g_merged_comps.push_back(mc);
    }
    logger::write("WwmiGeom: geometry unit #%d ns='%ls' guid=%d '%ls' (%zu comps) fwdmap=%ls", mod_id, gm.wcl_ns.c_str(),
        gm.mod.object_guid, gm.mod.dir.c_str(), gm.mod.comps.size(), gm.fwdmap.c_str());
    return mod_id;
}

// A removed or changed mod's unit: never claimed again. Its namespace no longer names one of their engine's, so every
// lookup by it finds nothing; its components leave the claim maps (RebuildCompIndex); its buffers (their engine's,
// released by the parse) are forgotten. The unit stays: pointers into it may still be held.
void KillUnit(int mod_id, const char* why)
{
    GeoMod& gm = g_geomods[mod_id];
    logger::write("WwmiGeom: geometry unit #%d ns='%ls' %s -- not claimed any more", mod_id, gm.wcl_ns.c_str(), why);
    gm.dead = true;
    gm.wcl_ns = L"\x1removed\x1" + std::to_wstring(mod_id);
    gm.last_char_frame = 0;
    gm.geom_enabled = false;
    for (auto& r : gm.mod.resources) {
        r.res = nullptr;
        r.gpu_va = 0;
        r.size = 0;
    }
}

void AuditUnits()
{
    int reachable = 0;
    int inert = 0;
    int live = 0;
    for (size_t i = 0; i < g_geomods.size(); ++i) {
        const auto& gm = g_geomods[i];
        if (gm.dead || gm.mod.comps.empty()) continue;
        ++live;
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
    if (live)
        logger::write("PORT-NS audit: %d of %d geometry mods can resolve their own component sections%s", reachable, live,
            inert ? "  *** SOME ARE INERT, see above ***" : "");
}

// Everything the claim reads that is built from the live units' components: the (first, count) maps, the IB-hash
// slots, each mod's index end, and which components share a range (`with`, `run_order`).
void RebuildCompIndex()
{
    g_first_to_comp.clear();
    g_first_to_shared.clear();
    for (size_t i = 0; i < g_merged_comps.size(); ++i) {
        if (!CompLive(i)) continue;
        const uint64_t key = FirstCountKey(g_merged_comps[i].first_index, g_merged_comps[i].match_count);
        auto           prev = g_first_to_comp.find(key);
        if (prev != g_first_to_comp.end()) {
            auto& shared = g_first_to_shared[key];
            if (shared.empty()) shared.push_back(prev->second);
            shared.push_back((int)i);
        }
        g_first_to_comp[key] = (int)i;
    }
    g_ns_to_mod.clear();
    for (size_t md = 0; md < g_geomods.size(); ++md)
        if (!g_geomods[md].dead) g_ns_to_mod[g_geomods[md].wcl_ns] = (int)md;
    g_ibhash_slot.clear();
    g_mod_index_end.assign(g_geomods.size(), {});
    for (size_t i = 0; i < g_merged_comps.size(); ++i) {
        if (!CompLive(i)) continue;
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
        logger::write("WwmiGeom: first=%u count=%u -- its sections run lowest match_priority first:%s", mc.first_index,
            mc.match_count, order.c_str());
    }
}
} // namespace

void LoadGeometryMods()
{
    if (g_wwmi_geom && !g_geom_loaded && g_d3d12_device && !g_wwmi_dir.empty()) {
        PortApi::LogModTree(); // which mod folders their engine parsed, and which it did not (excluded / unparsed)
        for (const UnitGroup& g : EnumerateUnitGroups()) AddUnit(g);
        if (!g_geomods.empty()) AuditUnits();
        RebuildCompIndex();
        if (!g_geomods.empty()) FinalizeMergedRemap(); // one-time shared setup + g_merged_loaded
        g_geom_loaded = true;
    }
}

ModsReloadResult ReloadGeometryMods()
{
    ModsReloadResult r;
    if (!g_wwmi_geom || !g_d3d12_device || g_wwmi_dir.empty()) return r;
    if (!g_geom_loaded) { // not loaded yet: the first load, as at startup
        LoadGeometryMods();
        for (const GeoMod& gm : g_geomods) r.added += gm.dead ? 0 : 1;
        r.changed_units = r.added > 0;
        if (r.changed_units) g_mods_epoch.fetch_add(1, std::memory_order_acq_rel);
        return r;
    }
    PortApi::LogModTree();
    std::unordered_map<std::wstring, int> live; // namespace -> its live unit
    for (size_t md = 0; md < g_geomods.size(); ++md)
        if (!g_geomods[md].dead) live[g_geomods[md].wcl_ns] = (int)md;
    std::vector<bool> seen(g_geomods.size(), false);
    for (const UnitGroup& g : EnumerateUnitGroups()) {
        const auto it = live.find(g.ns);
        if (it == live.end()) {
            if (AddUnit(g) >= 0) ++r.added;
            continue;
        }
        const int md = it->second;
        seen[md] = true;
        WwmiMod::Mod probe; // what their engine describes now, against what the unit was made from
        const bool   described = WwmiMod::ReadComponentsFromEngine(g.ns, probe) > 0 && !probe.comps.empty();
        if (described && probe.dir.empty()) probe.dir = g.dir;
        if (described && UnitFingerprint(g.inis, probe) == g_geomods[md].files_fp) {
            // Unchanged: kept, with what it learned. Its buffers are their engine's, made again by the parse.
            WwmiMod::LoadResources(g_geomods[md].mod, g_d3d12_device);
            ++r.kept;
            continue;
        }
        KillUnit(md, described ? "changed on disk" : "no longer described by their engine");
        if (described && AddUnit(g) >= 0)
            ++r.changed;
        else
            ++r.removed;
    }
    for (size_t md = 0; md < seen.size(); ++md)
        if (!seen[md] && !g_geomods[md].dead) {
            KillUnit((int)md, "removed");
            ++r.removed;
        }
    AuditUnits();
    RebuildCompIndex();
    if (!g_geomods.empty()) FinalizeMergedRemap(); // once: a no-op when it ran at startup
    r.changed_units = r.added || r.changed || r.removed;
    if (r.changed_units) g_mods_epoch.fetch_add(1, std::memory_order_acq_rel);
    logger::write("WwmiGeom: reload -- %d kept, %d changed (new unit), %d added, %d removed; %zu units, %zu components",
        r.kept, r.changed, r.added, r.removed, g_geomods.size(), g_merged_comps.size());
    return r;
}
