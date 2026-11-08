#pragma once
// The RT module's view of the core, in the names it was written against (before 2026.11's refactor): thin inline
// forwards to the core's classes, so the module's code reads as before. Module-internal: include it last.
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <d3d12.h>
#include <reshade.hpp>
#include "../addon/addon.h" // g_addon
#include "../addon/settings.h" // g_settings.ini_path
#include "../core/frame_clock.h" // g_frame_id
#include "../core/log.h"
#include "../core/ns_hash.h" // NsHash
#include "../instrumentation/diag_gate.h" // g_debug, DiagOnce
#include "../hooks/command_list_registry.h"
#include "../hooks/descriptor_shadow.h" // g_descriptors
#include "../hooks/resource_tracker.h" // g_resources
#include "../hooks/pipeline_registry.h" // g_pipelines
#include "../mods/mod_set.h" // Mods(), Comps(), CurrentSet()
#include "../mods/mod_registry.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h" // g_game_cbs
#include "../textures/mod_texture_store.h" // g_mod_textures
#include "../textures/tex_place.h" // g_tex_place
#include "../textures/tex_select.h" // g_tex_select
#include "../textures/tex_identity.h" // g_tex_identity

// ---- the device and paths ----
#define g_d3d12_device (g_addon.d3d12_device)
#define g_cso_dir (g_addon.cso_dir)
#define g_data_dir (g_addon.data_dir)
#define g_addon_ini_path (g_settings.ini_path)

// ---- command lists (hooks/command_list_registry.h) ----
inline CmdState& CmdGfx(const void* cmd_list)
{
    return g_command_lists.Gfx(cmd_list);
}
inline CmdCbvBinds& CmdCbv(const void* cmd_list)
{
    return g_command_lists.Cbv(cmd_list);
}
inline uint64_t CmdGen(const void* cmd_list)
{
    return g_command_lists.Gen(cmd_list);
}
inline uint64_t& CmdGfxLayout(const void* cmd_list)
{
    return g_command_lists.GfxLayout(cmd_list);
}
inline uint64_t& CmdCmpLayout(const void* cmd_list)
{
    return g_command_lists.CmpLayout(cmd_list);
}

// ---- descriptors and buffers ----
struct RtHeapToRes { // the core's descriptor slot record, as the old g_heap_to_res.find
    bool find(const HeapKey& key, ResInfo& out) const { return g_descriptors.Find(key, out); }
};
inline RtHeapToRes g_heap_to_res;

// ---- the mods (mods/mod_set.h) ----
// A mod set's ids are its own: a reload (F10) builds a new set whose mods are numbered afresh. The module keys its
// state (meshes, poses, rings, material tables) by an id of its own that a mod keeps across sets while its namespace,
// files and components stay the same; a changed mod gets a new one (its mesh is loaded again). rt_module.cpp.
int RtStableMod(int set_mod); // the module's id of the scoped set's mod `set_mod`; -1 for none
int RtSetMod(int stable); // the scoped set's id of the module's mod `stable`; -1 when that set has no such mod
inline GeoMod* RtUnit(int stable) // the scoped set's unit of the module's mod, or nullptr
{
    const int m = RtSetMod(stable);
    return m >= 0 ? &Mods().At((size_t)m) : nullptr;
}
inline const wchar_t* RtUnitNs(int stable) // its namespace, or nullptr
{
    const GeoMod* gm = RtUnit(stable);
    return gm && !gm->wcl_ns.empty() ? gm->wcl_ns.c_str() : nullptr;
}

// ---- textures ----
inline uint32_t SnapshotGameCB(ID3D12Resource* res, uint64_t va, void* dst, uint32_t max_bytes)
{
    return g_game_cbs.Snapshot(res, va, static_cast<uint8_t*>(dst), max_bytes);
}
inline void TexHoldForRetry(uint64_t res)
{
    g_tex_place.HoldForRetry(res);
}
inline void TexRetryMissed(const char* why, bool quiet_if_none = false)
{
    g_tex_place.RetryMissed(why, quiet_if_none);
}
inline int TexSelectIdxNs(uint32_t hash, const wchar_t* ns, unsigned mip)
{
    return g_tex_select.SelectIdxNs(hash, ns, ns ? NsHash(ns) : 0, mip); // the namespace keys its selection cache
}
inline uint32_t TexLodTargetOf(uint32_t hash)
{
    return g_tex_select.LodTargetOf(hash);
}
inline unsigned TexDrawnMip(uint32_t hash, uint32_t target)
{
    return g_tex_select.DrawnMip(hash, target);
}
namespace ModLoader {
using TexEntry = ModTextureStore::Entry;
inline bool TexEntryAt(int index, TexEntry* out)
{
    return g_mod_textures.EntryAt(index, out);
}
inline int TexIndexOf(ID3D12Resource* res)
{
    return g_mod_textures.IndexOf(res);
}
inline void TexTouchRes(ID3D12Resource* res)
{
    g_mod_textures.TouchRes(res);
}
} // namespace ModLoader
