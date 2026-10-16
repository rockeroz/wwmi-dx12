// rt_skin.h -- the RT module's M2: a mod component posed with the native skin cache's palette and drawn through the
// game's pass-through vertex factory while the game ray traces. See rt_skin.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <reshade.hpp>
#include <d3d12.h>
#include "rt_hooks.h"

namespace rtskin {

// A skin cache dispatch (any thread): the ring it writes (u1's buffer), the section's start vertex in it ($Globals
// SkinCacheStart), its bone palette view and tangent output; `expected` = sections the ring's BLAS has (0 unknown).
void NoteSection(uint64_t ring_va, uint32_t start, uint64_t palette_va, uint32_t bones, uint64_t tangent_ring_va, uint32_t expected,
    uint32_t frame);

// What the module saw of the game's draw it is about to replace (draw_claim.cpp -> RtClaimNote, same thread).
struct ClaimCtx {
    reshade::api::command_list* cmd_list = nullptr;
    uint64_t                    gen = 0;
    uint64_t                    ring = 0;      // the draw's VB0 when it is a skin-cache ring, else 0
    uint64_t                    prev_ring = 0; // its VB4 (previous positions, velocity) when that is a ring too
    int                         mod = -1, comp = -1;
    uint32_t                    first_index = 0, index_count = 0;
    uint64_t                    vs_hash = 0;
    uint32_t                    frame = 0;
};
void SetClaim(const ClaimCtx& c);
bool ClaimIsPassThrough(reshade::api::command_list* cmd_list); // the current claim on this thread reads a ring
bool ClaimActive();                                            // ...on whatever list (RtPalPublish has none)
// Has the skin cache posed section `comp` of this ring recently enough for its palette to be used?
bool SectionKnown(uint64_t ring, int comp);
bool RingSeen(uint64_t ring); // has a skin cache dispatch written this buffer recently?

// The mod's meshes: asked for from any thread, loaded on the present thread (RtPresentTick).
bool MeshReady(int mod);
void RequestMesh(int mod);
void PresentTick(); // loads requested meshes, retires bindings

// The draw (draw_section_draw.cpp -> RtSkinDraw): pose the sub-draw [s2, s2 + c) of component `comp` of mod `mod`,
// bind it, redirect the VS's fetch buffers and LocalVF. Returns what the core must restore / whether to skip.
VfRestore Draw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, int mod, int comp, uint32_t c, uint32_t s2);

std::string Status(); // one line for RTSUM / the overlay

} // namespace rtskin
