// rt/rt_stub.cpp -- the RT module left out (WWMI_RT=0): every hook in rt_hooks.h does no
// RT work. The core then behaves as it does with ray tracing off: no ring is ever recorded, so every RT branch in
// it is untaken.
#include <imgui.h>
#include "rt/rt_hooks.h"

void RtNoteDrawVb(int, int, const RtDrawVb&) {}
bool RtIsSkinOut(uint64_t, uint64_t)
{
    return false;
}
size_t RtSkinOutCount()
{
    return 0;
}
void RtForgetSkinOutRange(uint64_t, uint64_t) {}
void RtPalRecord(reshade::api::command_list*, const CmdState&, uint64_t, uint32_t, uint32_t, uint32_t) {}
void RtNoteShader(uint64_t, const void*, size_t) {}
void RtNotePipeline(uint32_t, const reshade::api::pipeline_subobject*, reshade::api::pipeline) {}
void RtNoteVertexShader(uint64_t, const void*, size_t) {}
bool RtBonesWindowOpen()
{
    return false;
}
void RtBonesLog(reshade::api::command_list*, const CmdState&, uint32_t, uint64_t, bool) {}
void RtSkinReadDrain() {}
void RtSkinReadRecord(reshade::api::command_list*, const CmdState&, uint64_t, uint32_t) {}
bool RtOnBlasBuild(reshade::api::command_list*, reshade::api::acceleration_structure_type,
    reshade::api::acceleration_structure_build_flags, uint32_t,
    const reshade::api::acceleration_structure_build_input*, reshade::api::resource_view,
    reshade::api::acceleration_structure_build_mode, unsigned long long)
{
    return false;
}
VfRestore RtSkinDraw(reshade::api::command_list*, ID3D12GraphicsCommandList*, int, const WwmiMod::Component*, uint32_t,
    uint32_t, uint32_t, uint32_t, int32_t, uint64_t, uint64_t, int, bool)
{
    return {};
}
RtPalResult RtPalPublish(uint64_t, int, const WwmiMod::Component*, uint64_t)
{
    return {};
}
bool RtClaimNote(const RtClaimInfo&)
{
    return false;
}
bool RtHoldVanilla()
{
    return false;
}
void RtNoteExecute(reshade::api::command_queue*, reshade::api::command_list*) {}
void RtOnDeviceRemoved() {}
void RtNoteCopy(reshade::api::command_list*, reshade::api::resource, uint64_t, reshade::api::resource, uint64_t, uint64_t) {}
void RtNoteRays(reshade::api::command_list*, reshade::api::resource, uint64_t, uint64_t, reshade::api::resource, uint64_t, uint64_t,
    uint64_t, reshade::api::resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t)
{
}
void RtObserveDraw(reshade::api::command_list*, uint32_t, uint32_t, int32_t, uint64_t) {}
void RtPresentTick() {}
void RtNoteSlotTexture(int, int, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*) {}
void RtNoteKeptTexture(int, int, ID3D12Resource*, ID3D12Resource*) {}
void RtSetNoteDraw(uint32_t, uint32_t) {}
void RtNoteDrawOutcome(int, int, int) {}
void RtDrawOverlay()
{
    ImGui::TextDisabled("  ray tracing: this build has no RT module (WWMI_RT=0)");
}
bool RtModulePresent()
{
    return false;
}
