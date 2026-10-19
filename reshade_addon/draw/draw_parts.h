#pragma once
// The parts of the draw path (draw_hook.cpp on_draw_indexed_body), in the order it calls them.
#include "draw_ctx.h"
#include <optional>
#include <reshade.hpp>
#include <cstdint>
#include <vector>

struct ResInfo;

// [ShaderOverride*] by VS / PS / GS hash, and the post lists; true = the ini's handling=skip (draw_claim.cpp).
bool RunDrawShaderOverrides(const DrawArgs& da);
// Which mod component this draw is; a value = return it from the hook (a mesh-hash skip) (draw_claim.cpp).
std::optional<bool> ClaimDraw(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc);
void                DrawProbesClaim(const DrawArgs& da, DrawClaim& dc);                    // draw_probes.cpp
void                LodPinNote(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc);       // draw_claim.cpp
void                DrawProbesPasses(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc); // draw_probes.cpp
// A character depth pass the mod components are to be cast into (draw_shadow.cpp).
void RecogniseShadowPass(const DrawArgs& da, BoneRoots& roots, DrawClaim& dc);
void DrawProbeVb6Reader(const DrawArgs& da, DrawClaim& dc);   // draw_probes.cpp
void DrawProbeOffLeak(const DrawArgs& da, DrawClaim& dc);     // draw_probes.cpp
void DrawProbeUnstamp(const ResInfo& ri, bool restored);      // draw_probes.cpp
void StampClaimedTextures(const DrawArgs& da, DrawClaim& dc); // draw_texswap.cpp
bool ModTexturesReady(int mod); // queues once; never waits for file I/O
bool ClaimedTexturesReady(const MergedComp&); // includes every merged sibling
// Where a game character draw binds each texture: recorded while crawling, linked from a seed (draw_comp.cpp).
void NoteMeshPlaces(const DrawArgs& da);
// Which textures a character draw read at ps-t0..t8, kept per list as DX11 keeps its slots (draw_comp.cpp).
void CarryPsReads(const DrawArgs& da);
// The game's own descriptors back in every stamped slot of this draw's tables; how many (draw_texswap.cpp).
unsigned UnstampClaimedTextures(const DrawArgs& da);
// The claimed draw replaced by the mod's geometry; the hook's return value (draw_inject.cpp).
bool InjectClaimedDraw(const DrawArgs& da, DrawClaim& dc);

struct SectionPsSlot {
    unsigned                    reg = 0;
    uint8_t                     param = 0;
    uint32_t                    offset = 0;
    uint64_t                    heap = 0;
    uint32_t                    slot = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE src{ 0 };
    ID3D12Resource*             res = nullptr;
};
// The section's own ps-tN for one injected draw, each resolved to the slot of the game's table it lands in
// (section_ps_stamp.cpp).
struct SectionPsStamps {
    std::vector<SectionPsSlot> slots;
    bool                       shared = false; // an earlier draw of this frame stamped one of these slots with another view
    int                        dm = -1;
    int                        comp = -1;
    void                       StampInPlace() const;
    void                       QueueForShadow() const; // the draw's next private-table bind writes them
};
SectionPsStamps PlanSectionPsSlots(reshade::api::command_list* cmd_list, uint32_t gen0, const std::vector<uint32_t>& this_gen,
    int dm, int comp);
