// The present path: the frame's own bookkeeping, the [Present] command list, and everything that
// only makes sense once per frame. See present_parts.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../addon_core.h"
#include "../mods/claim_state.h"
#include "../mods/mods_gate.h" // ReloadMods
#include "../instrumentation/occlusion_probe.h"
#include "../hooks/resource_tracker.h"
#include "present_parts.h"
#include "../instrumentation/device_watch.h"
#include "../config.h"     // g_skip_engine_tick (A9)
#include "../ui/overlay.h" // DrawStoppedNotice
#include "../instrumentation/hot_path.h"
#include "../instrumentation/perf_monitor.h"
#include "../hud/uid_hide.h"
#include "../rt/rt_hooks.h" // RtPresentTick, RtMeshLoaderIdle
#include "../textures/tex_replace.h"
#include "../textures/tex_budget.h"
#include "../textures/tex_encode.h"

static bool GameKeyDown(int vk)
{
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    const bool mods = ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
    return pid == GetCurrentProcessId() && !mods && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static std::atomic<swapchain*> g_step_sc{ nullptr };

static void FrameStep(command_queue* queue, swapchain* sc, bool at_present);

void on_present_body(command_queue* queue, swapchain* sc)
{
    HotPath::FrameEnd();
    g_perf.FrameEnd();
#ifdef WWMI_DEBUG
    {
        static LARGE_INTEGER s_freq{};
        static LARGE_INTEGER s_last{};
        LARGE_INTEGER        now;
        QueryPerformanceCounter(&now);
        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
        const double ms = s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)s_freq.QuadPart : 0.0;
        if (ms >= 100.0) logger::write("FRAME-SLOW f=%u: %.0f ms since the previous present", (unsigned)g_frame_id, ms);
        s_last = now;
    }
#endif
    PROF(PROF_Present);
    const uint32_t me = GetCurrentThreadId();
    g_presenter_tid.store(me, std::memory_order_relaxed);
    g_present_queue.store(reinterpret_cast<void*>(queue->get_native()), std::memory_order_relaxed);
    g_step_sc.store(sc, std::memory_order_release);
    {
        const resource_desc bbd = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        g_bb_width.store(bbd.texture.width, std::memory_order_relaxed);
        g_bb_height.store(bbd.texture.height, std::memory_order_relaxed);
    }
    // Presents from another thread than the game's (frame generation): the game's thread steps at its own frame end, and
    // a present steps only when no frame end came for a while, and then only after the game drew.
    const uint32_t game_tid = g_game_submit_tid.load(std::memory_order_relaxed);
    const bool     other_thread = g_frame_end_step && game_tid && game_tid != me;
    g_frame_end_armed.store(other_thread, std::memory_order_relaxed);
    {
        static bool s_other = false;
        if (other_thread != s_other) {
            s_other = other_thread;
            logger::write("FRAMESTEP: presents come from %s thread -- the per-frame step runs at %s", other_thread ? "another" : "the game's",
                other_thread ? "the game's frame end" : "the present");
        }
    }
    // A frame the step already ran for at its end (presents just moved to the game's thread) gets only the redraw.
    const bool stepped_at_end = g_frame_end_stepped.exchange(false, std::memory_order_relaxed);
    const bool step = other_thread
        ? GetTickCount64() - g_frame_end_step_ms.load(std::memory_order_relaxed) >= 250 &&
            g_game_frame_submitted.exchange(false, std::memory_order_relaxed)
        : !stepped_at_end;
#ifdef WWMI_DEBUG
    if (g_present_log_left.load(std::memory_order_relaxed) > 0) {
        static LARGE_INTEGER s_last{};
        LARGE_INTEGER        now;
        LARGE_INTEGER        freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double dt = s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
        s_last = now;
        g_present_log_qpc.store(now.QuadPart, std::memory_order_relaxed);
        std::string subs;
        {
            std::lock_guard l(g_present_log_mtx);
            g_present_log_bbs.clear();
            for (uint32_t i = 0; i < sc->get_back_buffer_count(); ++i) g_present_log_bbs.push_back(sc->get_back_buffer(i).handle);
            for (const PresentLogSubmitter& s : g_present_log_subs) {
                char e[96];
                snprintf(e, sizeof(e), " %u/%llx:%u%s@%.2f", s.tid, (unsigned long long)s.queue, s.lists, s.claimed ? "c" : "",
                    (double)(now.QuadPart - s.last_qpc) * 1000.0 / (double)freq.QuadPart);
                subs += e;
            }
            g_present_log_subs.clear();
        }
        logger::report("PRESENTLOG f=%u tid=%lu dt=%.2f ms step=%d draws=%u execs=%u claimed=%u bb=%u queue=%p swapchain=%p bbres=%llx subs=%s",
            g_frame_id, GetCurrentThreadId(), dt, (int)step, g_present_log_draws.exchange(0), g_present_log_execs.exchange(0),
            g_draw_replace_count.n.load(), sc->get_current_back_buffer_index(), (void*)queue->get_native(),
            (void*)sc->get_native(), (unsigned long long)sc->get_current_back_buffer().handle, subs.c_str());
        if (g_present_log_left.fetch_sub(1) == 1) logger::report("PRESENTLOG: done");
    }
#endif
    if (g_rt_blocked || g_faulted) {
        static ULONGLONG s_notice_until = 0;
        if (!g_addon_disabled) SetAddonDisabled(true);
        if (!s_notice_until) {
            reshade::register_overlay("OSD", DrawStoppedNotice);
            s_notice_until = GetTickCount64() + 60000;
        } else if (s_notice_until != ~0ull && GetTickCount64() > s_notice_until) {
            reshade::unregister_overlay("OSD", DrawStoppedNotice);
            s_notice_until = ~0ull;
        }
    }
    { // F10: reload the mods, as on DX11 (their engine re-parses every ini; meshes added, changed or removed)
        static bool s_f10 = false;
        const bool  f10 = GameKeyDown(VK_F10);
        if (f10 && !s_f10 && !g_addon_disabled && !g_rt_blocked && !g_faulted && !g_reload_requested.exchange(true))
            logger::write("RELOAD: F10 -- at the next frame step");
        s_f10 = f10;
    }
    { // F6 (no modifiers) toggles A1. Polled before the bypass return below, so it also turns the addon back on.
        static bool s_f6 = false;
        const bool  f6 = GameKeyDown(VK_F6);
        if (f6 && !s_f6 && !g_rt_blocked && !g_faulted) SetAddonDisabled(!g_addon_disabled.load());
        s_f6 = f6;
    }
    VerboseTick();
    if (g_addon_disabled) return;
#ifdef WWMI_DEBUG
    { // Numpad 7: the per-frame step at the game's frame end (frame generation) or at every present, as 2026.10.2
        static bool s_np7 = false;
        const bool  np7 = GameKeyDown(VK_NUMPAD7);
        if (np7 && !s_np7) {
            g_frame_end_step = !g_frame_end_step;
            logger::write("HOTKEY: per-frame step at the game's frame end = %d", (int)g_frame_end_step);
        }
        s_np7 = np7;
    }
#endif
    if (g_hide_uid) g_uid_hide.NoteBackBuffer(sc);
    RemovalPin::Check(g_d3d12_device, "Present");

    PresentQueueDiag(queue);
    if (!step) {
        if (!g_skip_engine_tick) EngineRedrawTick(queue, sc);
        return;
    }
    FrameStep(queue, sc, true);
}

static void SetReloadStatus(std::string s)
{
    std::lock_guard l(g_reload_status_m);
    g_reload_status = std::move(s);
}

// F10 / Reload mods: their engine re-parses every ini (as their F10), the mod textures follow the new parse, and the
// geometry units are made again where a mod changed. The mod lists change only while no game callback runs (the mod
// gate), so this runs before the frame step takes its lock: a thread waiting on that lock is inside a callback.
static void ReloadMods()
{
    const ULONGLONG t0 = GetTickCount64();
    unsigned        waited = 0;
    if (!ModsPause(2000, &waited)) {
        logger::write("RELOAD: not done -- the game's threads did not leave the add-on's callbacks within %u ms", waited);
        SetReloadStatus("Reload not done: the game was busy. Press F10 again.");
        return;
    }
    struct Resume {
        ~Resume() { ModsResume(); }
    } resume;
    if (!RtMeshLoaderIdle(5000)) {
        logger::write("RELOAD: not done -- the RT mesh loader is still loading a mesh");
        SetReloadStatus("Reload not done: a mesh was still loading. Press F10 again.");
        return;
    }
    std::lock_guard frame_lock(EngineFrameMutex()); // free: every thread that takes it is inside a callback
    if (!PortApi::ReloadConfigNow()) {
        logger::write("RELOAD: not done -- their engine has not loaded the mods yet");
        SetReloadStatus("Reload not done: the mods are not loaded yet.");
        return;
    }
    ModLoader::Reload(); // LOD and look-alike caches read again, mod SRV slots handed out afresh (a new epoch)
    ModLoader::Tick();
    g_texovr_ready = false; // the texture rules of the new parse
    ModTexturesTick();
    const ModsReloadResult r = ReloadGeometryMods();
    const ULONGLONG        ms = GetTickCount64() - t0;
    logger::write("RELOAD: done in %llu ms (game threads paused after %u ms) -- meshes: %d kept, %d changed, %d added, %d removed",
        ms, waited, r.kept, r.changed, r.added, r.removed);
    char text[160];
    snprintf(text, sizeof text, "Reloaded in %llu ms: %d mod mesh(es) changed, %d added, %d removed, %d unchanged.", ms,
        r.changed, r.added, r.removed, r.kept);
    SetReloadStatus(text);
}

// The per-frame step, once per game frame: at a present on the game's own thread, or at the game's frame end on its
// thread (at_present = false), where [Present]'s draws to the back buffer are left to the presents' redraw.
static void FrameStep(command_queue* queue, swapchain* sc, bool at_present)
{
    if (g_reload_requested.exchange(false)) ReloadMods();
    std::lock_guard frame_lock(EngineFrameMutex());
    RtPresentTick(); // RT module, once per game frame: hold transitions, M3 gates left closed, the observe log
    PortProbeTick(queue, sc);
    ConfigWatchTick();
    UpdateSkinBand(); // keep the skin-pool VA band tracking the learned char VB0s (auto re-derive across patches)
    SeedHeartbeat();
    ModLoader::Tick();
    if (!g_skip_engine_tick && !RtHoldVanilla()) EngineFrameTick(queue, sc, at_present); // RTHOLD: no [Present] lists while held (as A9)
    StartRemovalWatchdog(sc);
    g_buffer_reloc.Sweep(sc->get_device());
    ModTexturesTick();
    g_tex_replace.Tick((ID3D12CommandQueue*)queue->get_native());
    g_tex_budget.Tick();
    g_tex_encode.Tick((ID3D12CommandQueue*)queue->get_native());
    g_tex_encoder.Tick();
    if (g_wwmi_geom) {
        static int s_gl = 0;
        uint32_t   gc = g_wwmi_geom_count.load();
        if (s_gl < 5) {
            s_gl++;
            logger::write("WwmiGeom: M4 ini-geometry drew %u direct-comp sub-draws this frame", gc);
        }
    }
    g_draw_replace_count.Roll();
    g_port_sect_runs.Roll();
    g_port_sect_nocb.Roll();
    g_port_obj_borrowed.Roll();
    g_port_fellback_count.Roll();
    for (GeoMod& gm : g_geomods) {
        gm.replaced.Roll();
        gm.skip_orig.Roll();
        gm.sect_runs.Roll();
    }
    const uint32_t texinj_last = g_tex_inject_count.exchange(0);
    g_global_tex_count.store(0);
    g_tex_lineage_count.store(0);
    g_wwmi_geom_count.store(0);
    g_custom_blend_count.store(0);
    g_custom_blend_bound_count.store(0);
    {
        std::lock_guard l(g_char_skel_mtx);
        g_frame_skel.clear();
    } // per-frame skeleton scope (LOD-pin)
    {
        std::lock_guard l(g_skincache_mtx);
        g_char_bone_va.clear();
    } // per-frame bone-arena scope (POOLDRAW boneNear)
    {
        PROF(PROF_DrainTexHash);
        DrainTexReadback();
    } // hash streamed char textures (source-mip0 copies appended on the game's copy list)
    ++g_frame_id; // new frame -> allow re-capturing each component's native bones once
    g_sections_this_frame.store(0, std::memory_order_relaxed);

    Heartbeat(texinj_last);
    // ARMED-DIAGNOSTIC COUNTDOWN, centrally: "run for N frames then stop" budgets set by overlay buttons.
    g_occl.PresentTick();
    {
        int t = g_vbdiag_frames.load(std::memory_order_relaxed);
        if (t > 0) {
            g_vbdiag_frames.store(t - 1, std::memory_order_relaxed);
            if (t == 1) logger::write("VBDIAG: capture window closed");
        }
    }
    if (g_infoq) DrainDebugLayer("frame");
    HealthHeartbeat();
    {
        int r = g_shadow_recog_watch.load(std::memory_order_relaxed);
        if (r > 0) g_shadow_recog_watch.store(r - 1, std::memory_order_relaxed);
    }
    {
        extern std::atomic<int> g_tex_flicker_watch; // B4, same rule: a budget that actually expires
        int                     f = g_tex_flicker_watch.load(std::memory_order_relaxed);
        if (f > 0) g_tex_flicker_watch.store(f - 1, std::memory_order_relaxed);
    }
    LoadGeometryMods();
    PROF(PROF_PresTail); // on_present's tail: shapekey / ibhash drain / diag
    SkRefreshModHashes();
    MigotoPort::SkCapDrain();
    {
        PROF(PROF_IbHashDrain);
        IbHash::DrainQueue();
    } // GPU IB-hash anchor: hash queued char IBs (bounded, cached) -- needed in prod too
    if (g_debug) {
        PROF(PROF_DiagPresent);
        DiagOnPresent(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()));
    }
    // Ticks the armed window and writes out once the GPU has run the copies. Never waits.
    MigotoPort::FrameAnalysisFrameEnd();
    if (g_debug && g_wwmi_parse.exchange(false)) {
        // Self-check: log every discovered geometry mod's parsed layout.
        for (auto& gm : g_geomods) WwmiMod::LogSelfCheck(gm.mod);
    }
}

static void FrameEndStep(command_queue* queue)
{
    swapchain* const sc = g_step_sc.load(std::memory_order_acquire);
    if (!sc || g_addon_disabled || g_rt_blocked || g_faulted) return;
    if (reinterpret_cast<ID3D12CommandQueue*>(queue->get_native())->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    if (!g_game_frame_submitted.exchange(false, std::memory_order_relaxed)) return;
    g_frame_end_step_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_frame_end_stepped.store(true, std::memory_order_relaxed);
#ifdef WWMI_DEBUG
    if (g_present_log_left.load(std::memory_order_relaxed) > 0)
        logger::report("FRAMEEND step f=%u tid=%lu queue=%p draws=%u", g_frame_id, GetCurrentThreadId(),
            (void*)queue->get_native(), g_frame_end_draws.exchange(0));
#endif
    FrameStep(queue, sc, false);
}

void FrameEndStepGuarded(command_queue* queue)
{
    WWMI_GUARD_BEGIN("frame end step")
    FrameEndStep(queue);
    WWMI_GUARD_END_VOID()
}

void FrameEndForgetSwapchain()
{
    g_step_sc.store(nullptr, std::memory_order_release);
}
