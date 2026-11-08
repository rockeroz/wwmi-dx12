// The ray-tracing hooks: the RT module's feed, and the RT BLOCK without it. See rt_block_hooks.h.
#include "../addon_common.h"
#include "rt_block_hooks.h"
#include "../addon/addon.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/mod_set.h"
#include "../rt/rt_hooks.h" // RtModulePresent
#include "../rt/rt_blas.h"
#include "command_list_registry.h" // CmdRtPipelineBound
#include <optional>

static void RtBlockWithoutModule()
{
    if (RtModulePresent() || g_addon.rt_blocked.load(std::memory_order_relaxed) || g_addon.rt_blocked.exchange(true))
        return;
    logger::report(
        "RT BLOCK: the game is ray tracing and this build has no RT module -- the add-on is off until the game "
        "restarts (frame %u). Turn ray tracing off in the game's graphics settings.",
        g_frame_id);
}

bool on_build_acceleration_structure(reshade::api::command_list* cmd_list,
    reshade::api::acceleration_structure_type type, reshade::api::acceleration_structure_build_flags build_flags,
    uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource,
    uint64_t, reshade::api::resource_view, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode)
{
    RtBlockWithoutModule();
    static std::atomic<unsigned long long> s_n{ 0 };
    const unsigned long long n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n == 1000 || n == 100000)
        logger::report(
            "RT ACTIVE: BuildRaytracingAccelerationStructure #%llu (type %d, mode %d, %u input(s)) at frame %u -- "
            "the game builds acceleration structures, so ray tracing is on and running",
            n + 1, (int)type, (int)mode, input_count, g_frame_id);
    std::optional<ModSetScope> set_scope;
    if (!g_addon.disabled) set_scope.emplace(cmd_list);
    // RT (rt/rt_blas.cpp): the rings RTPAL finds the mod character's skin cache by, H19 FREEZE, and the RTGEO / RTDBG lines.
    if (g_rt_blas.OnBuild(cmd_list, type, build_flags, input_count, inputs, dest, mode, n)) return true;
    if (cmd_list) RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "game-as-build");
    return false;
}

bool on_dispatch_rays(reshade::api::command_list* cmd_list, reshade::api::resource rg_res, uint64_t rg_off,
    uint64_t rg_size, reshade::api::resource miss_res, uint64_t miss_off, uint64_t miss_size, uint64_t miss_stride,
    reshade::api::resource hit_res, uint64_t hit_off, uint64_t hit_size, uint64_t hit_stride, reshade::api::resource call_res,
    uint64_t call_off, uint64_t call_size, uint64_t call_stride, uint32_t width, uint32_t height, uint32_t depth)
{
    RtBlockWithoutModule();
    static std::atomic<unsigned long long> s_n{ 0 };
    const unsigned long long n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n == 1000 || n == 100000)
        logger::report(
            "RT ACTIVE: DispatchRays #%llu (%u x %u x %u) at frame %u", n + 1, width, height, depth, g_frame_id);
    std::optional<ModSetScope> set_scope;
    if (!g_addon.disabled) set_scope.emplace(cmd_list);
    // RT M5: the module issued it itself with an extended hit group table (or it must not run): the game's call is skipped
    if (RtNoteRays(cmd_list, rg_res, rg_off, rg_size, miss_res, miss_off, miss_size, miss_stride, hit_res, hit_off,
            hit_size, hit_stride, call_res, call_off, call_size, call_stride, width, height, depth))
        return true;
    if (cmd_list) RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "game-dispatch-rays");
    return false;
}

// ExecuteIndirect (ReShade: draw_or_dispatch_indirect, type unknown, the argument buffer and MaxCommandCount). With a ray
// tracing state object bound it is an indirect DispatchRays: the RT module (M5) checks its hit group table.
bool on_draw_or_dispatch_indirect(reshade::api::command_list* cmd_list, reshade::api::indirect_command,
    reshade::api::resource buffer, uint64_t offset, uint32_t draw_count, uint32_t)
{
    if (!cmd_list || !CmdRtPipelineBound(cmd_list)) return false;
    std::optional<ModSetScope> set_scope;
    if (!g_addon.disabled) set_scope.emplace(cmd_list);
    if (RtNoteIndirectRays(cmd_list, buffer, offset, draw_count)) return true;
    RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "game-indirect-rays");
    return false;
}
