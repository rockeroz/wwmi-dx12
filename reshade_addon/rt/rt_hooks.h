#pragma once
#include <cstddef>
#include <cstdint>
#include <reshade.hpp>
#include "../hooks/cmd_state.h" // CmdState
#include "../mods/wwmi_mod.h"   // WwmiMod::Component

struct ID3D12GraphicsCommandList;

// The skin-cache output rings mod characters' BLAS are built from. `mod`: g_geomods index, -1 unknown.
void RtNoteSkinOut(uint64_t va, uint64_t bytes, int mod = -1);
bool RtIsSkinOut(uint64_t va, uint64_t bytes); // does [va, va+bytes) overlap a recorded range?
// The mod whose ring is EXACTLY a view's window [va, va+bytes), or -1; `overlap_mod` gets one it merely overlaps.
int    RtSkinOutMod(uint64_t va, uint64_t bytes, int* overlap_mod = nullptr);
size_t RtSkinOutCount();
// A destroyed buffer's rings go with it.
void RtForgetSkinOutRange(uint64_t va, uint64_t bytes);
// Bumped whenever RtNoteSkinOut records a ring it did not have: a shader judged before then is judged again.
uint32_t RtSkinOutGen();
// RTDBG: the vb0 range a claimed draw of (mod, comp) last read.
struct RtDrawVb {
    uint32_t frame = 0;
    uint64_t va = 0;
    uint32_t size = 0;
    uint32_t stride = 0;
    uint64_t vs = 0;
};
void RtNoteDrawVb(int mod_id, int comp_idx, const RtDrawVb& v);
bool RtDrawVbOf(int mod_id, int comp_idx, RtDrawVb* out);

void RtPalRecord(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x,
    uint32_t groups_y, uint32_t groups_z);
// A compute pipeline's shader at creation (pipeline_hooks.cpp), by the core's hash: the module keeps a copy so
// the shaders it sees binding a ring can be dumped for offline disassembly. Records only.
void RtNoteShader(uint64_t cs_hash, const void* code, size_t code_size);
// The same for a graphics pipeline's vertex shader (dumped when a held RT draw of a mod component uses it).
void RtNoteVertexShader(uint64_t vs_hash, const void* code, size_t code_size);
// RTBONES / RTSKINREAD (g_debug): what her skin-cache dispatches bind, read once.
bool RtBonesWindowOpen();
void RtBonesLog(reshade::api::command_list* cmd_list, const CmdState& state, uint32_t groups_x, uint64_t cs_hash, bool learned);
void RtSkinReadDrain();
void RtSkinReadRecord(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x);

bool RtOnBlasBuild(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
    reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
    const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode, unsigned long long n);

struct VfRestore {
    uint8_t  param = 0xFF;
    uint64_t va = 0;
    bool     skip = false;        // the module cannot draw this section correctly: no draw, every restore still runs
    bool     shadow_bind = false; // the module set ShadowExtraSlots (the VS's fetch buffers): a shadow table bind is required
    uint64_t shadow_params = 0;   // ...in these root parameters (bit per param): each must be among the bound shadow tables
};
// What became of a section the module handled (draw_section_draw.cpp): 0 drawn, 1 the module declined it, 2 no shadow
// table bind could be made, 3 a fetch slot's table was not among the bound shadow tables, 4 dropped by the core.
void RtNoteDrawOutcome(int dm, int comp, int outcome);
// PTSKIN + RTFETCH + H27 on a pass-through draw of component `wc`; any other draw is left alone.
VfRestore RtSkinDraw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
    const WwmiMod::Component* wc, uint32_t c, uint32_t s2, uint32_t first_index, uint32_t index_count,
    int32_t vertex_offset, uint64_t vs_hash, uint64_t game_cb13_va, int game_cb13_root,
    bool rtpal_no_merge);
struct RtPalResult {
    bool tried = false;
    bool published = false;
    bool no_merge = false;
    bool no_cb3 = false; // the draw's vs-cb3 is not a skeleton either: the mod's vs-cb3 binding goes nowhere
};
RtPalResult RtPalPublish(uint64_t draw_pso, int dm, const WwmiMod::Component* wc, uint64_t port_cb4_va);

// RTHOLD: a draw the core matched to a mod component by first index + count (draw_claim.cpp), with what it
// saw bound. `identified` = the core's own definitive identification (IB hash, texture, anchor, learned VB0).
// The module pairs it with the BLAS geometry reading the same index range to learn the character's ring.
// true = leave this draw to the game: the module cannot draw the mod here (M1 observes only).
struct RtClaimInfo {
    reshade::api::command_list* cmd_list = nullptr; // the draw's list (its bindings: CmdGfx / CmdCbv)
    int      mod = -1;
    int      comp = -1;
    bool     identified = false;
    uint64_t vb0 = 0; // bound VB0 VA, vertex_offset NOT applied
    uint32_t vb0_bytes = 0;
    uint32_t vb0_stride = 0;
    uint64_t ib_va = 0; // bound IB VA (first_index NOT applied)
    uint32_t index_bytes = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t  vertex_offset = 0;
    uint64_t vs_hash = 0;
};
bool RtClaimNote(const RtClaimInfo& info);
// true = every mod edit of the game's frame is held back (component hides, draw / dispatch overrides,
// texture placement swaps, shapekey relocation, the [Present] lists): the module's observe mode while the
// game ray traces. false whenever RT is off.
bool RtHoldVanilla();
// Every indexed draw, before the core looks at it (draw_hook.cpp): the module samples, on observation frames
// only, which index windows the draws reading a BLAS vertex base use. Records only; never changes the draw.
void RtObserveDraw(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t first_index, int32_t vertex_offset,
    uint64_t vs_hash);
// The game issued DispatchRays (removal_pin.cpp): ray tracing is on even in a frame without a BLAS build.
// Every buffer copy (resource_tracker.cpp): the RT module notes where DEFAULT regions -- shader tables -- come from.
void RtNoteCopy(reshade::api::command_list* cmd_list, reshade::api::resource src, uint64_t src_off, reshade::api::resource dst,
    uint64_t dst_off, uint64_t size);
// A list submitted (ExecuteCommandLists): from now on what it reads from UPLOAD memory may already be on the GPU.
void RtNoteExecute(reshade::api::command_list* cmd_list);
void RtNoteRays(reshade::api::command_list* cmd_list, reshade::api::resource rg_res, uint64_t rg_off, uint64_t rg_size,
    reshade::api::resource miss_res, uint64_t miss_off, uint64_t miss_size, uint64_t miss_stride, reshade::api::resource hit_res,
    uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride, uint32_t w, uint32_t h, uint32_t d);
// Once per frame from the present path, while the add-on is enabled: the module's hold transitions
// (entering the hold gives every stamped texture slot back to the game) and its periodic work.
void RtPresentTick();

// ---- overlay (rt/rt_overlay.cpp) ----
void RtDrawOverlay();

// Is the RT module in this build? false = rt_stub.cpp: mods cannot be drawn while the game ray traces.
bool RtModulePresent();
