#pragma once
// ===========================================================================
//  Addon-wide shared state with no narrower home: the device, the private-data keys the hooks stash
//  shader hashes under, the namespace key, and the per-frame sets the hooks publish to each other.
// ===========================================================================
#include <reshade.hpp>
#include <d3d12.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <string>
#include "mods/wwmi_mod.h"        // WwmiMod::Mod -- GeoMod below
#include "instrumentation/diag.h" // FrameCounter -- GeoMod below
#include "hooks/sharded_map.h"
#include "hooks/state_table.h"
#include "hooks/descriptor_shadow.h"
#include "hooks/cmd_state.h"
#include "mods/layout_registry.h"

/* THE OWNERSHIP KEY, LOWERCASED IN EXACTLY ONE PLACE.

   A mod's namespace is the key for every resource the shapekey chain remembers between the dispatch
   that produced it and the point that consumes it. A second, divergent lowercasing is a mod silently
   reading a neighbour's buffer, which is the class of bug the chain's store exists to prevent. */
inline std::wstring NsKey(const std::wstring& ns)
{
    std::wstring k = ns;
    for (wchar_t& c : k) c = (wchar_t)towlower(c);
    return k;
}

inline ID3D12Device*         g_d3d12_device = nullptr;
inline bool                  g_capture_mode = false;     // Numpad 0 (present_engine.cpp): the capture hotkeys are live only while set
inline reshade::api::device* g_reshade_device = nullptr; // the same device as ReShade hands it out
inline uint32_t              g_reshade_api = 0;          // the add-on API ReShade accepted: RESHADE_API_VERSION, or an older one as a fallback
// Set once in on_init_device; every hook that assumes a D3D12 device gates on it. Defined in
// wwmi_addon.cpp, where the device arrives.
extern bool g_is_dx12;

// Private-data keys used to stash the vs/cs/ps shader hash on each command list.
inline const uint8_t s_iid_vs_hash[16] = { 0x5a, 0x18, 0xa5, 0x8f, 0x19, 0x2b, 0x4f, 0x02, 0x8a, 0x44, 0x86, 0x1a, 0x22, 0xdc, 0x8f, 0x7a };
inline const uint8_t s_iid_cs_hash[16] = { 0x5a, 0x18, 0xa5, 0x8f, 0x19, 0x2b, 0x4f, 0x02, 0x8a, 0x44, 0x86, 0x1a, 0x22, 0xdc, 0x8f, 0x7b };
inline const uint8_t s_iid_ps_hash[16] = { 0x5a, 0x18, 0xa5, 0x8f, 0x19, 0x2b, 0x4f, 0x02, 0x8a, 0x44, 0x86, 0x1a, 0x22, 0xdc, 0x8f, 0x7c };
// GS: their BeforeDraw dispatches [ShaderOverride] for five graphics stages (HackerContext.cpp:847-881);
// of the three left, GS is the one with a live consumer -- [CustomShaderRenderText] needs one.
inline const uint8_t s_iid_gs_hash[16] = { 0x5a, 0x18, 0xa5, 0x8f, 0x19, 0x2b, 0x4f, 0x02, 0x8a, 0x44, 0x86, 0x1a, 0x22, 0xdc, 0x8f, 0x7d };

// ONE LOADED CHARACTER MOD, shared by the hook modules.
struct GeoMod {
    WwmiMod::Mod mod;
    std::wstring fwdmap;
    std::wstring wcl_ns;              // THE mod key: their engine's namespace, from PortApi::NamespaceForIni
    uint32_t     last_char_frame = 0; // g_frame_id of the last claimed draw of this unit's character; 0 = never
    float        det_at_keys = 0.f;   // $object_detected sampled at key-poll time, before [Present]'s post pass clears it
    bool         geom_enabled = true; // $mod_enabled sampled after [Present]'s post pass; what ModGeomEnabled answers
    // this unit's share of the heartbeat's sect= / skip= / replaced=
    FrameCounter sect_runs;
    FrameCounter skip_orig;
    FrameCounter replaced;
};

// Resource Identity Tracking: maps a vanilla resource pointer to its mod SRV.
// Stable within a session if the engine reuses the resource object for streaming.
inline std::unordered_map<uint64_t, SIZE_T> g_res_to_mod_srv;
inline std::mutex                           g_res_to_mod_mtx;

// ---- BUFFER RESIZE ("vertex limit raise") -------------------------------------------------------
// d3dx.ini `allow_buffer_resize = 1` lets a [TextureOverride] grow the buffer the GAME allocated, to
// override_vertex_count * override_byte_stride. The game sizes its morph buffer for the VANILLA mesh and
// a WWMI mod has more vertices. Verified against a DX11 .dsc: 191286 x 24 = 4,590,864 and
// 191286 x 4 = 765,144, both EXACT, while the vanilla buffer -- which is what the ini's hash was computed
// against, and what we bind -- is 1,858,944 B, only 77,456 vertices at stride 24. Everything past that
// index is unaddressable, which is verbatim the WWMI 1.0.0 changelog: "shapekeys application failed after
// exceeding the original maximum vertex ID".
// Their parser resolves both keys into the override's byte width; PortProbeTick reads it per hash through
// PortApi::ForEachBufferResize (stride 1, count = the byte width), keyed by the hash their [TextureOverride] matches.
struct BufResize {
    uint32_t stride = 0;
    uint32_t vertex_count = 0;
    uint64_t bytes() const { return (uint64_t)stride * (uint64_t)vertex_count; }
};
inline std::unordered_map<uint32_t, BufResize> g_buf_resize; // desc-hash -> declared size
inline std::mutex                              g_buf_resize_mtx;

inline std::mutex g_addon_mutex;

// Force-LOD0 Stage 1: GENERIC skin-cache lineage. A compute dispatch that READS our character mesh and
// WRITES a large buffer is the GPU skin-cache skinning our char; we record its output buffer range so any
// later DRAW whose VB0 falls in that range is the character at ANY LOD (recognized by data-flow, not a
// hardcoded cs/VS hash). Classified once per cs_hash to bound cost.
inline std::unordered_set<uint64_t> g_char_skin_cs; // cs_hashes confirmed to skin OUR char
// Char BONE-CB VAs (the b4/b3 skeleton CBV the char VS reads -- ini-marked [TextureOverrideMarkBoneDataCB],
// root-bound and char-SPECIFIC, unlike the shared bindless mesh heap). The skin cache reads these to skin
// our char, so a compute binding one of these is its skinning dispatch. Recorded from claimed char draws.
inline std::unordered_set<uint64_t> g_char_bone_va;
struct SkinOut {
    uint64_t va;
    uint32_t size;
};
inline std::vector<SkinOut> g_skincache_out; // skinned-output buffer ranges (char at all LODs)
inline std::mutex           g_skincache_mtx;

// Real current resource state, tracked from reshade's barrier event. reshade::api::resource_usage is
// bit-identical to D3D12_RESOURCE_STATES, so the stored value IS the exact D3D12 state -- lets a dump
// transition a texture to COPY_SOURCE with the correct StateBefore (no guessing -> no device-removed).
inline ShardedStateTable g_res_state;
// The UNORDERED_ACCESS transitions per list live in CmdBlock::uav_recent (cmd_state.h): descriptor-free
// identity for the shapekey u0 (SkFindU0ByGeometry, sk_identity.cpp) when two live buffers of the
// declared size exist, the one THIS list just prepared for its clear pass is the one its Loader
// dispatches on.
