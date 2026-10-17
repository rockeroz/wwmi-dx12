#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../addon_state.h"          // GeoMod
#include "wwmi_mod.h"                // WwmiMod::Component
#include "../instrumentation/diag.h" // g_frame_id -- ModClaimAge

// ---- the loaded geometry mods (filled once by the mod discovery in on_present) ------------------
inline std::atomic<bool>                     g_ps_slot_queries{ false }; // a loaded mod asks what the game bound in ps-tN
extern std::deque<GeoMod>                    g_geomods;   // deque: a unit never moves (comps are pointed into)
extern std::unordered_map<std::wstring, int> g_ns_to_mod; // their namespace (GeoMod::wcl_ns) -> geomod index
extern bool                                  g_geom_loaded;

inline const WwmiMod::Component* WmodComp(int mod_id, int idx)
{
    if (mod_id < 0 || mod_id >= (int)g_geomods.size()) return nullptr;
    for (auto& c : g_geomods[mod_id].mod.comps)
        if (c.index == idx) return &c;
    return nullptr;
}
inline std::unordered_map<uint32_t, int>                    g_ibhash_slot;
inline std::vector<std::unordered_map<uint32_t, uint32_t>> g_mod_index_end;
inline uint32_t                                             ModIndexEnd(int md, uint32_t ib_hash)
{
    if (md < 0 || md >= (int)g_mod_index_end.size()) return 0u;
    const auto it = g_mod_index_end[md].find(ib_hash);
    return it == g_mod_index_end[md].end() ? 0u : it->second;
}
// WWMI enable-flag compliance: is this mod's geometry meant to be applied right now?
bool            ModGeomEnabled(int md);
inline uint32_t ModClaimAge(int md)
{
    if (md < 0 || md >= (int)g_geomods.size()) return 0xFFFFFFFFu;
    const uint32_t f = g_geomods[md].last_char_frame;
    return f == 0 ? 0xFFFFFFFFu : (uint32_t)(g_frame_id - f);
}
inline bool ModOnScreen(int md)
{
    return ModClaimAge(md) <= 4u;
}

struct CharVbOrigin {
    uint32_t frame = 0;
    int      mod = -1;
};
extern std::unordered_map<uint64_t, CharVbOrigin> g_char_mesh_vb0;
extern std::mutex                                 g_char_mesh_mtx;
void                                              ForgetCharMeshVb0(uint64_t va, uint64_t bytes);
bool                                              IbAnchorNoteAndAsk(uint64_t ib_va, uint64_t ib_bytes, int mod_id, int comp_idx); // ib_bytes: the bound IB's extent
void                                              IbAnchorForget(uint64_t va, uint64_t bytes);
bool                                              IbAnchorResolve(uint64_t index_va, uint32_t index_bytes, int* mod_id, uint64_t* ib_base_va);
bool                                              IbAnchorJustLearned(uint64_t index_va); // this is the first frame its draws are claimed
extern std::unordered_set<uint32_t>               g_frame_skel;
extern std::unordered_set<uint64_t>               g_char_lod_vb0;
extern std::mutex                                 g_char_skel_mtx;
// Hash the first N bone matrices of an UPLOAD bone CB (the DX12 equivalent of WWMI's MarkBoneDataCB hash).
uint32_t HashBoneCB(ID3D12Resource* res, uint64_t va);

// ---- texture-identity ownership -----------------------------------------------------------------
bool DrawBindsModTexture(reshade::api::command_list* cmd_list);
// IB-HASH SUPPRESS: ini `hash=` values of components that HAVE claimed via (first,count) this session.
extern std::mutex                                          g_claimed_ibhash_mtx;
extern std::unordered_set<uint32_t>                        g_claimed_ibhash;
extern std::mutex                                          g_skip_vbhash_mtx;
extern std::shared_ptr<const std::unordered_set<uint32_t>> g_skip_vbhash_snap;
extern std::atomic<bool>                                   g_skip_vbhash_any; // the per-draw gate: no lock unless a mod declares one
extern std::atomic<uint32_t>                               g_skip_vbhash_drops;

// ---- engine skinned-vertex VA pool band --------------------------------------------------------
extern std::atomic<uint64_t> g_skin_band_lo; // diagnostics.cpp filters the frame dump with these
extern std::atomic<uint64_t> g_skin_band_hi;
inline bool                  InSkinBand(uint64_t va)
{
    return va >= g_skin_band_lo.load() && va < g_skin_band_hi.load();
}
void UpdateSkinBand(); // every on_present; anchors the band on the learned VB0s
// SEEDHB heartbeat aggregation: the VB0 range of substantial draws this second (draw tallies, present flushes).
extern std::mutex g_hb_mtx;
extern uint64_t   g_hb_vbmin;
extern uint64_t   g_hb_vbmax;
extern uint32_t   g_hb_large;
extern uint32_t   g_hb_pool;
