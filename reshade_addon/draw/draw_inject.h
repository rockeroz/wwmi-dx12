#pragma once
#include "draw_ctx.h"
#include "../mods/wwmi_mod.h"
#include "../mods/port_section.h" // PortHooks

struct RelocXition {
    ID3D12Resource*       res;
    D3D12_RESOURCE_STATES back;
};

struct RfxGlowState {
    bool                        active = false;
    float                       hue = 0;
    float                       sat = 0;
    float                       val = 0;
    float                       brightness = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE map{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE fxmap{ 0 };
};
struct RfxTexLatch {
    D3D12_CPU_DESCRIPTOR_HANDLE d{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE n{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE l{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE m{ 0 };
}; // m: Materialmap

// What RabbitFX's calls leave behind for the draws that follow them: one per section run, or one for every
// section on a claimed draw, as DX11's bindings outlive the section that made them.
struct RfxDrawState {
    RfxGlowState glow;
    RfxTexLatch  tex;
    std::wstring diffuse_fq;
    std::wstring normal_fq;
    std::wstring light_fq;
    std::wstring material_fq;
    std::wstring cutout_fq;
    std::wstring special_fq;
};

struct InjectFrame {
    const DrawArgs&          da;
    DrawClaim&               dc;
    D3D12_VERTEX_BUFFER_VIEW save_vb[8] = {};
    bool                     save_vb_set[8] = {};
    unsigned                 reloc_vb_mask = 0;
    RelocXition              reloc_xit[9] = {};
    unsigned                 reloc_xit_n = 0;
    D3D12_INDEX_BUFFER_VIEW  save_ib = {};
    bool                     save_ib_set = false;
    bool                     cb_ok = false; // we bound the bone CB at b4/b3 ourselves (the frozen identity pose)
    bool                     x32_mid = false;
    unsigned                 x32_mid_restores = 0;
    unsigned                 x32_mid_skipped = 0;
    bool                     mod_switched_off = false; // its section drew nothing with $mod_enabled = 0: the game's draw
    RfxDrawState*            rfx_shared = nullptr;     // set: every section on this draw reads and writes this one
};

void RestoreGameState(InjectFrame& fr); // draw_restore.cpp
// One mod component's section run in place of the draw; true = it drew (draw_comp.cpp).
bool DrawComponent(InjectFrame& fr, int dm, const WwmiMod::Component* wc);
// The mod components cast into a recognised character depth pass; true = suppress the game's (draw_shadow.cpp).
bool CastModShadows(InjectFrame& fr);

// What a component section's `drawindexed` hook reads and writes, owned by DrawComponent for the run.
struct SectionDraw {
    InjectFrame&              fr;
    int                       dm;
    const WwmiMod::Component* wc;
    const PortHooks&          hk;
    uint32_t&                 did; // draws the section issued
    RfxGlowState&             rfx_glow;
    RfxTexLatch&              rfx_tex;
    // X24: what slot 6 of the draw reads after RELOCBIND (draw_comp.cpp sets it, the dump publish reads it)
    ID3D12Resource*& x24_vb6_res;
    uint64_t&        x24_vb6_va;
    uint32_t&        x24_vb6_bytes;
    uint8_t&         x24_vb6_verdict;
    bool&            rtpal_no_merge;
};
void SectionDrawIndexed(SectionDraw& sd, uint32_t c, uint32_t s2, uint32_t b2); // draw_section_draw.cpp
