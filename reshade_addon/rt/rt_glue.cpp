// rt/rt_glue.cpp -- the RT module in this build (WWMI_RT=observe): the core-facing classes of rt_rings.h,
// rt_pal_record.h, rt_diag_dispatch.h, rt_blas.h and rt_draw.h, each method forwarded to the module's own entry
// point (rt_module_api.h, rt_module.cpp). rt_stub.cpp stands in for this file when the module is left out.
#include "rt_module_api.h"
#include "rt_pal_record.h"
#include "rt_diag_dispatch.h"
#include "rt_blas.h"

RtDrawVbs g_rt_draw_vbs;
RtSkinRings g_rt_rings;
RtPalRecorder g_rt_pal_recorder;
RtSkinRead g_rt_skin_read;
RtBonesLog g_rt_bones;
RtBlasHook g_rt_blas;
RtDrawPass g_rt_draw;

void RtDrawVbs::Note(int mod_id, int comp_idx, const RtDrawVb& v)
{
    RtNoteDrawVb(mod_id, comp_idx, v);
}
bool RtDrawVbs::Of(int mod_id, int comp_idx, RtDrawVb* out) const
{
    return RtDrawVbOf(mod_id, comp_idx, out);
}

void RtSkinRings::Note(uint64_t va, uint64_t bytes, int mod)
{
    RtNoteSkinOut(va, bytes, mod);
}
bool RtSkinRings::Overlaps(uint64_t va, uint64_t bytes) const
{
    return RtIsSkinOut(va, bytes);
}
int RtSkinRings::ModOf(uint64_t va, uint64_t bytes, int* overlap_mod) const
{
    return RtSkinOutMod(va, bytes, overlap_mod);
}
size_t RtSkinRings::Count() const
{
    return RtSkinOutCount();
}
void RtSkinRings::ForgetRange(uint64_t va, uint64_t bytes)
{
    RtForgetSkinOutRange(va, bytes);
}
uint32_t RtSkinRings::Gen() const
{
    return RtSkinOutGen();
}

void RtPalRecorder::Record(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x,
    uint32_t groups_y, uint32_t groups_z)
{
    RtPalRecord(cmd_list, state, cs_hash, groups_x, groups_y, groups_z);
}

bool RtBonesLog::WindowOpen() const
{
    return RtBonesWindowOpen();
}
void RtBonesLog::Log(
    reshade::api::command_list* cmd_list, const CmdState& state, uint32_t groups_x, uint64_t cs_hash, bool learned)
{
    RtBonesLogLine(cmd_list, state, groups_x, cs_hash, learned);
}
void RtSkinRead::Drain()
{
    RtSkinReadDrain();
}
void RtSkinRead::Record(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x)
{
    RtSkinReadRecord(cmd_list, state, cs_hash, groups_x);
}

bool RtBlasHook::OnBuild(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
    reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
    const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode, unsigned long long n)
{
    return RtOnBlasBuild(cmd_list, type, build_flags, input_count, inputs, dest, mode, n);
}

VfRestore RtDrawPass::SkinDraw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
    const WwmiMod::Component* wc, uint32_t c, uint32_t s2, uint32_t first_index, uint32_t index_count, int32_t vertex_offset,
    uint64_t vs_hash, uint64_t b4_va, int b4_root, bool rtpal_no_merge)
{
    return RtSkinDraw(
        cmd_list, d3d12_cl, dm, wc, c, s2, first_index, index_count, vertex_offset, vs_hash, b4_va, b4_root, rtpal_no_merge);
}
RtPalResult RtDrawPass::PalPublish(uint64_t draw_pso, int dm, const WwmiMod::Component* wc, uint64_t b4_va)
{
    return RtPalPublish(draw_pso, dm, wc, b4_va);
}
