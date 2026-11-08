#pragma once
// The RT module's own entry points, behind the core-facing classes of rt_rings.h, rt_pal_record.h,
// rt_diag_dispatch.h, rt_blas.h and rt_draw.h (rt_glue.cpp forwards each class method here). Module-internal: the
// core never calls these by name, so the stub (rt_stub.cpp) does not define them.
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <reshade.hpp>
#include "../hooks/command_list_registry.h" // CmdState
#include "../mods/wwmi_mod.h" // WwmiMod::Component
#include "rt_rings.h" // RtDrawVb
#include "rt_draw.h" // VfRestore, RtPalResult
#include "rt_hooks.h"

// The skin-cache output rings mod characters' BLAS are built from. `mod`: the mod set's index, -1 unknown.
void RtNoteSkinOut(uint64_t va, uint64_t bytes, int mod = -1);
bool RtIsSkinOut(uint64_t va, uint64_t bytes); // does [va, va+bytes) overlap a recorded range?
// The mod whose ring is EXACTLY a view's window [va, va+bytes), or -1; `overlap_mod` gets one it merely overlaps.
int RtSkinOutMod(uint64_t va, uint64_t bytes, int* overlap_mod = nullptr);
size_t RtSkinOutCount();
// A destroyed buffer's rings go with it.
void RtForgetSkinOutRange(uint64_t va, uint64_t bytes);
// Bumped whenever RtNoteSkinOut records a ring it did not have: a shader judged before then is judged again.
uint32_t RtSkinOutGen();
// RTDBG: the vb0 range a claimed draw of (mod, comp) last read.
void RtNoteDrawVb(int mod_id, int comp_idx, const RtDrawVb& v);
bool RtDrawVbOf(int mod_id, int comp_idx, RtDrawVb* out);

void RtPalRecord(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x,
    uint32_t groups_y, uint32_t groups_z);
// RTBONES / RTSKINREAD (g_debug): what her skin-cache dispatches bind, read once.
bool RtBonesWindowOpen();
void RtBonesLogLine(reshade::api::command_list* cmd_list, const CmdState& state, uint32_t groups_x, uint64_t cs_hash, bool learned);
void RtSkinReadDrain();
void RtSkinReadRecord(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x);

bool RtOnBlasBuild(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
    reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
    const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode, unsigned long long n);

// PTSKIN + RTFETCH + H27 on a pass-through draw of component `wc`; any other draw is left alone.
VfRestore RtSkinDraw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
    const WwmiMod::Component* wc, uint32_t c, uint32_t s2, uint32_t first_index, uint32_t index_count,
    int32_t vertex_offset, uint64_t vs_hash, uint64_t game_cb13_va, int game_cb13_root, bool rtpal_no_merge);
RtPalResult RtPalPublish(uint64_t draw_pso, int dm, const WwmiMod::Component* wc, uint64_t port_cb4_va);
