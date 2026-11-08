#pragma once
// The RT module's entry points that are not a class: its overlay section, whether the module is in this build, and
// the core's notes to it (draws, pipelines, copies, rays, textures). The rest: rt_rings.h, rt_pal_record.h,
// rt_diag_dispatch.h, rt_blas.h, rt_draw.h -- each stubbed by rt_stub.cpp when WWMI_RT=0.
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <d3d12.h>
#include <reshade.hpp>

// ---- overlay (rt/rt_overlay.cpp) ----
void RtDrawOverlay();

// Is the RT module in this build? false = rt_stub.cpp: mods cannot be drawn while the game ray traces.
bool RtModulePresent();

// A compute pipeline's shader at creation (pipeline_registry.cpp), by the core's hash: the module keeps a copy so
// the shaders it sees binding a ring can be dumped for offline disassembly. Records only.
void RtNoteShader(uint64_t cs_hash, const void* code, size_t code_size);
// A ray tracing pipeline (state object) created: its shaders' libraries dumped, entries / groups / identifiers logged (M4).
void RtNotePipeline(uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline);
// The same for a graphics pipeline's vertex shader (dumped when a held RT draw of a mod component uses it).
void RtNoteVertexShader(uint64_t vs_hash, const void* code, size_t code_size);

// What became of a section the module handled (draw_section_draw.cpp): 0 drawn, 1 the module declined it, 2 no shadow
// table bind could be made, 3 a fetch slot's table was not among the bound shadow tables, 4 dropped by the core.
void RtNoteDrawOutcome(int dm, int comp, int outcome);

// RTHOLD: a draw the core matched to a mod component by first index + count (draw_claim.cpp), with what it
// saw bound. `identified` = the core's own definitive identification (IB hash, texture, anchor, learned VB0).
// The module pairs it with the BLAS geometry reading the same index range to learn the character's ring.
// true = leave this draw to the game: the module cannot draw the mod here.
struct RtClaimInfo {
    reshade::api::command_list* cmd_list = nullptr; // the draw's list (its bindings: CmdGfx / CmdCbv)
    int mod = -1;
    int comp = -1;
    bool identified = false;
    uint64_t vb0 = 0; // bound VB0 VA, vertex_offset NOT applied
    uint32_t vb0_bytes = 0;
    uint32_t vb0_stride = 0;
    uint64_t ib_va = 0; // bound IB VA (first_index NOT applied)
    uint32_t index_bytes = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
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
// Every buffer copy (resource_tracker.cpp): the RT module notes where DEFAULT regions -- shader tables -- come from.
void RtNoteCopy(reshade::api::command_list* cmd_list, reshade::api::resource src, uint64_t src_off, reshade::api::resource dst,
    uint64_t dst_off, uint64_t size);
// The device was removed (watchdogs.cpp): the module's state into ReShade.log.
void RtOnDeviceRemoved();
// A list submitted (ExecuteCommandLists): from now on what it reads from UPLOAD memory may already be on the GPU.
void RtNoteExecute(reshade::api::command_queue* queue, reshade::api::command_list* cmd_list);
// The game issued DispatchRays (rt_block_hooks.cpp). true = the module issued the dispatch itself (M5: with an
// extended hit group table) or it must not run: skip the game's.
bool RtNoteRays(reshade::api::command_list* cmd_list, reshade::api::resource rg_res, uint64_t rg_off, uint64_t rg_size,
    reshade::api::resource miss_res, uint64_t miss_off, uint64_t miss_size, uint64_t miss_stride, reshade::api::resource hit_res,
    uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride, reshade::api::resource call_res, uint64_t call_off, uint64_t call_size,
    uint64_t call_stride, uint32_t w, uint32_t h, uint32_t d);
// An ExecuteIndirect recorded with a ray tracing pipeline bound (rt_block_hooks.cpp): an indirect DispatchRays, its desc
// in the argument buffer. true = skip it (M5: instances point past the game's table and its desc could not be pointed at ours).
bool RtNoteIndirectRays(reshade::api::command_list* cmd_list, reshade::api::resource args, uint64_t args_off, uint32_t max_count);
// A raster draw of mod component (mod, comp) stamped its texture over the game's texture `game` (section_ps_stamp.cpp):
// M4 puts the same texture in that section's ray tracing material table.
void RtNoteSlotTexture(int mod, int comp, ID3D12Resource* game, D3D12_CPU_DESCRIPTOR_HANDLE src, ID3D12Resource* mod_res);
// The texture budget (tex_replace.cpp) is about to let go of these mod textures, or just did: no RT material table may
// be given their views again. A stamp of a mod off screen in raster stays usable kSlotTexAge frames, and the
// character can still be in the rays (reflections) meanwhile. The raster draw notes the new view on its next draw.
void RtForgetModTextures(const std::unordered_set<ID3D12Resource*>& mod_res);
// ...and a game texture it reads unstamped (diagnostic: RTM4 tex tells these from textures the raster never reads).
void RtNoteKeptTexture(int mod, int comp, ID3D12Resource* game, ID3D12Resource* swapped);
// The index count of the sub-draw whose binds are noted next on this thread (a component's material table can hold one
// texture set: the largest sub-draw's wins).
void RtSetNoteDraw(uint32_t first_index, uint32_t index_count);
// Once per frame from the present path, while the add-on is enabled: the module's hold transitions
// (entering the hold gives every stamped texture slot back to the game) and its periodic work.
void RtPresentTick();

// M5 critical resource ownership. Called before application heaps are created,
// and when a command-list recording is discarded/reset.
void RtCriticalInit(reshade::api::device*);
void RtCriticalReset(reshade::api::command_list*);
void RtCriticalShutdown();
