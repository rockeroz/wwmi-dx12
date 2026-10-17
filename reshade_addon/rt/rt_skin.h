// rt_skin.h -- the RT module's M2: a mod component posed with the native skin cache's palette and drawn through the
// game's pass-through vertex factory while the game ray traces. See rt_skin.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <reshade.hpp>
#include <d3d12.h>
#include "rt_hooks.h"

namespace rtskin {

// A skin cache dispatch (any thread): the ring it writes (u1's buffer), the section's start vertex in it ($Globals
// SkinCacheStart), its bone palette view and tangent output; `expected` = sections the ring's BLAS has (0 unknown).
void NoteSection(uint64_t ring_va, uint64_t ring_bytes, uint32_t start, uint64_t palette_va, uint32_t bones, uint64_t tangent_ring_va,
    uint32_t expected, uint32_t frame, ID3D12Resource* palette_res, uint64_t palette_off);
bool MeshFailed(int mod); // the module cannot draw this mod (RTM2 mesh FAILED says why)

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
bool SectionKnown(uint64_t ring, int comp, int mod); // mod: whose remembered layout may place the section
uint64_t RingKey(uint64_t va); // the skin cache ring containing va (seen recently), or 0
bool RingSeen(uint64_t ring); // has a skin cache dispatch written this buffer recently?
bool PoseCached(int mod, int comp); // do we have a recent pose of ours of this component (stands in for a missing palette)?

// The mod's meshes: asked for from any thread, loaded on the present thread (RtPresentTick).
bool MeshReady(int mod);
void RequestMesh(int mod);
void PresentTick(); // hands requested meshes to the background loader, retires bindings
void PreloadAll();  // every loaded mod's mesh, in the background (ray tracing just started)

// The draw (draw_section_draw.cpp -> RtSkinDraw): pose the sub-draw [s2, s2 + c) of component `comp` of mod `mod`,
// bind it, redirect the VS's fetch buffers and LocalVF. Returns what the core must restore / whether to skip.
VfRestore Draw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, int mod, int comp, uint32_t c, uint32_t s2);

// M3: what the raster draws of section `comp` of the character whose skin cache ring is `ring` (either of the two the
// pool alternates) last posed -- the mod that draws it, our posed positions (FLOAT3, `verts` of them) and the index
// ranges of the mod's index buffer (R32) posed there. The positions buffer is marked in use this frame.
struct GeomPose {
    int                                        mod = -1;
    uint64_t                                   pos_va = 0;
    uint32_t                                   verts = 0;
    ID3D12Resource*                            idx = nullptr;
    uint32_t                                   indices = 0; // the mod's index buffer size (in indices)
    std::vector<std::pair<uint32_t, uint32_t>> ranges;      // (first index, count), sorted
    uint32_t                                   frame = 0;   // when it was posed
};
bool PoseFor(uint64_t ring, int comp, uint32_t frame, GeomPose* out);

std::string Status(); // one line for RTSUM / the overlay
std::vector<std::string> PoseLines(); // RTM2POSE: per mod, how fresh the raster's poses are
std::string Explain(uint64_t va, int comp, int mod); // why a section of this ring is (not) usable: for the held lines

} // namespace rtskin
