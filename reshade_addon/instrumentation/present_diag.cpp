// present_diag.cpp -- the present path's diagnostics: the PRESENTQ queue check, the config watch and the
// heartbeats (SEEDHB, HEARTBEAT, the health heartbeat). See present_parts.h.
#include "../addon_common.h"
#include "../mods/claim_state.h"
#include "../rabbitfx/rabbitfx_bind.h"
#include "../present/present_parts.h"
#include "../hooks/pipeline_hooks.h" // TakePipelineHookStats

using namespace reshade::api;

std::wstring g_cfg_engine_path;
std::wstring g_cfg_db_path;
static void  MonitorConfig()
{
    if (g_cfg_engine_path.empty()) return;
    static int s_fl = -2;
    static int s_lod = -999;
    static int s_img = -2;
    int        fl = (int)GetPrivateProfileIntW(L"SystemSettings", L"r.Streaming.FullyLoadUsedTextures", -1, g_cfg_engine_path.c_str());
    int        lod = (int)GetPrivateProfileIntW(L"SystemSettings", L"r.MipMapLODBias", -999, g_cfg_engine_path.c_str());
    if (fl != s_fl || lod != s_lod) {
        logger::write("CFGWATCH Engine.ini: FullyLoadUsedTextures=%d MipMapLODBias=%d%s", fl, lod, (s_fl == -2) ? " (initial)" : " <-CHANGED");
        s_fl = fl;
        s_lod = lod;
    }
    int img = -1; // DB is SQLite (no lib in-addon): raw-scan for the "ImageDetail" data row
    {
        FILE* f = _wfsopen(g_cfg_db_path.c_str(), L"rb", _SH_DENYNO);
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < 8 * 1024 * 1024) {
                std::vector<char> b(n);
                if (fread(b.data(), 1, n, f) == (size_t)n)
                    for (long i = 0; i + 12 < n; ++i)
                        if (memcmp(b.data() + i, "ImageDetail", 11) == 0)
                            if (b[i + 11] >= '0' && b[i + 11] <= '9') img = b[i + 11] - '0';
            } // keep last (newest) match
            fclose(f);
        }
    }
    if (img != s_img) {
        logger::write("CFGWATCH LocalStorage.db ImageDetail=%d%s", img, (s_img == -2) ? " (initial)" : " <-CHANGED");
        s_img = img;
    }
}

void PresentQueueDiag(command_queue* queue)
{
    if (g_debug) {
        const void* pq = queue ? (const void*)queue->get_native() : nullptr;
        const void* gq = g_claim_exec_queue.load(std::memory_order_relaxed);
        if (pq && gq) {
            const bool same = (pq == gq);
            (same ? g_presentq_same : g_presentq_diff).fetch_add(1, std::memory_order_relaxed);
            static const void* s_pq = nullptr;
            static const void* s_gq = nullptr;
            static int         s_same = -1;
            if (pq != s_pq || gq != s_gq || (int)same != s_same) {
                s_pq = pq;
                s_gq = gq;
                s_same = (int)same;
                logger::write("PRESENTQ %s: [Present] executes on queue %p (this thread %u); the list "
                              "carrying the last claimed draw executed on queue %p (recorded on thread %u)%s",
                    same ? "SAME" : "DIFFERENT", pq, (unsigned)GetCurrentThreadId(), gq,
                    g_claim_thread.load(std::memory_order_relaxed),
                    same ? "" : " -- the [Present] copy of the merged skeleton is unordered against "
                                "the merges that write it and the draws that read it");
            }
        }
    }
}

void ConfigWatchTick()
{
    if (g_debug) {
        static int s_cw = 0;
        if (++s_cw >= 60) {
            s_cw = 0;
            MonitorConfig();
        }
    } // ~1/sec config watch
}

void SeedHeartbeat()
{
    if (!logger::enabled(3)) return;
    {
        static int s_hb = 0;
        if (++s_hb >= 60) {
            s_hb = 0;
            size_t   mN;
            size_t   sN = 0;
            uint64_t s0 = 0;
            uint64_t s1 = 0;
            uint64_t s2 = 0;
            {
                std::lock_guard l(g_char_mesh_mtx);
                mN = g_char_mesh_vb0.size();
                auto it = g_char_mesh_vb0.begin();
                if (it != g_char_mesh_vb0.end()) s0 = (it++)->first;
                if (it != g_char_mesh_vb0.end()) s1 = (it++)->first;
                if (it != g_char_mesh_vb0.end()) s2 = (it++)->first;
            }
            {
                std::lock_guard l(g_skincache_mtx);
                sN = g_skincache_out.size();
            }
            uint64_t vmin;
            uint64_t vmax;
            uint32_t lg;
            uint32_t pl;
            {
                std::lock_guard l(g_hb_mtx);
                vmin = g_hb_vbmin;
                vmax = g_hb_vbmax;
                lg = g_hb_large;
                pl = g_hb_pool;
                g_hb_vbmin = ~0ull;
                g_hb_vbmax = 0;
                g_hb_large = 0;
                g_hb_pool = 0;
            }
            logger::trace("SEEDHB meshVB0=%zu[%llx %llx %llx] skinOut=%zu largeDraws=%u poolBand=%u vb0range=[%llx..%llx] band=[%llx..%llx]",
                mN, s0, s1, s2, sN, lg, pl, (lg ? vmin : 0ull), vmax, g_skin_band_lo.load(), g_skin_band_hi.load());
        }
    }
}

void Heartbeat(uint32_t texinj_last)
{
    if (logger::enabled(1)) {
        static uint32_t s_next = 0;
        if (g_frame_id >= s_next) {
            s_next = g_frame_id + 300;
            std::string per;
            for (size_t i = 0; i < g_geomods.size(); ++i) {
                const std::wstring& uns = g_geomods[i].wcl_ns;
                char                b[288]; // widened with sect/skip/rep; the longest ns in the library is ~50 chars
                _snprintf_s_l(b, sizeof(b), _TRUNCATE, " [%zu %ls age=%u det=%g en=%g sect=%u skip=%u rep=%u]",
                    logger::utf8_locale(), i,
                    uns.empty() ? L"(no-ns)" : uns.c_str(),
                    (uint32_t)(g_frame_id - g_geomods[i].last_char_frame),
                    g_geomods[i].det_at_keys,
                    PortApi::GetVarNs(uns.c_str(), L"mod_enabled", -1.f),
                    g_geomods[i].sect_runs.last.load(), g_geomods[i].skip_orig.last.load(),
                    g_geomods[i].replaced.last.load());
                per += b;
            }
            size_t learned = 0;
            {
                std::lock_guard l(g_char_mesh_mtx);
                learned = g_char_mesh_vb0.size();
            }
            static size_t s_prev_learned = 0;
            const long    grew = (long)learned - (long)s_prev_learned;
            s_prev_learned = learned;
            static uint32_t s_rfx_prev = 0;
            const uint32_t  rfx_now = g_rabbitfx_tex_count.load();
            const uint32_t  rfx_win = rfx_now - s_rfx_prev;
            s_rfx_prev = rfx_now;
            logger::report("HEARTBEAT f=%u replaced=%u texinj=%u learnedVB=%zu(%+ld) "
                           "portDisp=%llu portDispCall=%llu "
                           "sect=%u nocb=%u (object cb borrowed %u) drewnothing=%u refusedNoCL=%u unmet=%u rfx=%u gmiss=%u%s",
                g_frame_id, g_draw_replace_count.last.load(), texinj_last,
                learned, grew,
                MigotoPort::DispatchesIssued(), MigotoPort::DispatchCalls(),
                g_port_sect_runs.last.load(),
                g_port_sect_nocb.last.load(), g_port_obj_borrowed.last.load(), g_port_fellback_count.last.load(),
                PortApi::TakeRefusedNoCl(),
                MigotoPort::ShimUnmetTakeCount(), rfx_win, g_global_tex_miss.load(), per.c_str());
            {
                const PipelineHookStats ph = TakePipelineHookStats();
                const RfxPatchStats     cs = GetRfxPatchStats();
                MEMORYSTATUSEX          ms{};
                ms.dwLength = sizeof ms;
                GlobalMemoryStatusEx(&ms);
                logger::report("PIPESTAT f=%u since the last: %llu pipeline(s) created, %.1f ms in our hook (slowest %.2f ms); "
                               "%llu destroyed, %.1f ms | patch totals: %u patched, %u not, %u variant(s) | RAM free %.1f GB, "
                               "commit %.1f of %.1f GB",
                    g_frame_id, (unsigned long long)ph.created, ph.created_us / 1000.0, ph.created_max_us / 1000.0,
                    (unsigned long long)ph.destroyed, ph.destroyed_us / 1000.0, cs.ps_patched, cs.ps_failed, cs.variants,
                    ms.ullAvailPhys / 1073741824.0, (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0,
                    ms.ullTotalPageFile / 1073741824.0);
            }
        }
    }
}

void HealthHeartbeat()
{
    if (g_debug && g_health_heartbeat && (g_frame_id % 600) == 0 && g_frame_id) {
        std::string ages;
        for (int md = 0; md < (int)g_geomods.size(); ++md) {
            const uint32_t a = ModClaimAge(md);
            char           b[48];
            snprintf(b, sizeof(b), "%s%d:%s", ages.empty() ? "" : " ", md,
                (a == 0xFFFFFFFFu) ? "never" : std::to_string(a).c_str());
            ages += b;
        }
        logger::write("HEARTBEAT frame=%llu srv_heap=%u/%u geomods=%zu | claim ages: %s",
            (unsigned long long)g_frame_id, ModLoader::SrvHeapSize(), ModLoader::SrvHeapCap(),
            g_geomods.size(), ages.c_str());
        logger::write("BRIDGE foreign-list=%llu", MigotoPort::BridgeForeignListRefusals());
        logger::write("CHARVB claims=%u xmod=%u refused=%u (H17 refuse xmod: %d)",
            g_charvb_claims.load(), g_charvb_xmod.load(), g_charvb_refused.load(), (int)g_charvb_refuse_xmod);
        {
            const PsoCloneStats ps = PsoCloneGetStats();
            logger::write("PSOCLONE held=%zu (%.1f MB) shared code: %zu shader(s), %.1f MB; forgotten=%u variants_released=%u",
                ps.clones, ps.bytes / (1024.0 * 1024.0), ps.code_blobs, ps.code_bytes / (1024.0 * 1024.0), ps.forgotten, ps.variants_released);
        }
        {
            const RfxPatchStats cs = GetRfxPatchStats();
            logger::write("RFXPATCH (E2 on: %d) ps_patched=%u ps_failed=%u variants=%u draws=%u glow_draws=%u nobind=%u mirror_null_slots=%llu", (int)g_rfx_patch,
                cs.ps_patched, cs.ps_failed, cs.variants, g_rfx_patch_draws.load(), g_rfx_glow_count.load(), g_rfx_patch_nobind.load(),
                g_rfx_mirror_misses.load());
        }
        logger::write("VBNULL-UNBIND count=%llu (executed `vbN = null` as a real D3D12 unbind)",
            MigotoPort::VbNullUnbinds());
        {
            static unsigned long long s_c = 0;
            static unsigned long long s_u = 0;
            const unsigned long long  c = g_vb6_readers_claimed.load(std::memory_order_relaxed);
            const unsigned long long  u = g_vb6_readers_unclaimed.load(std::memory_order_relaxed);
            logger::write("VB6-READERS since-last: claimed=%llu unclaimed=%llu (draws whose slot 6 is a buffer "
                          "we widened; unclaimed = the vanilla morph output drawn as-is, named once each above)",
                c - s_c, u - s_u);
            s_c = c;
            s_u = u;
        }
        {
            static unsigned long long s_s = 0;
            static unsigned long long s_q = 0;
            static unsigned long long s_e = 0;
            const unsigned long long  s = g_presentq_same.load(std::memory_order_relaxed);
            const unsigned long long  q = g_presentq_diff.load(std::memory_order_relaxed);
            const unsigned long long  e = g_claim_exec_seen.load(std::memory_order_relaxed);
            logger::write("PRESENTQ since-last: same=%llu different=%llu (presents whose queue is / is not the "
                          "queue that executed a list with a claimed draw; %llu such executes) present-thread=%u "
                          "draw-thread=%u",
                s - s_s, q - s_q, e - s_e, (unsigned)GetCurrentThreadId(),
                g_claim_thread.load(std::memory_order_relaxed));
            s_s = s;
            s_q = q;
            s_e = e;
        }
        {
            static unsigned long long s_f = 0;
            const unsigned long long  f = g_freeze_binds.load(std::memory_order_relaxed);
            logger::write("FREEZE-BIND since-last: %llu (identity bones bound inside the section's draw for a "
                          "component with overlay F ticked; 0 with F ticked = the freeze is not reaching the draw)",
                f - s_f);
            s_f = f;
        }
        {
            unsigned long long c0;
            unsigned long long o0;
            unsigned long long r0;
            unsigned long long c1;
            unsigned long long o1;
            unsigned long long r1;
            SkZeroStats(TargetClass::UnorderedAccessView, 0, &c0, &o0, &r0);
            SkZeroStats(TargetClass::UnorderedAccessView, 1, &c1, &o1, &r1);
            logger::write("UAVCLEAR-RATE u0 cleared=%llu not_ours=%llu not_ready=%llu | u1 cleared=%llu "
                          "not_ours=%llu not_ready=%llu",
                c0, o0, r0, c1, o1, r1);
        }
        { /* Which record answered the u0 table, since the last report. */
            static unsigned q_c = 0;
            static unsigned q_f = 0;
            static unsigned q_n = 0;
            const unsigned  c = g_u0tab_compute.load(std::memory_order_relaxed);
            const unsigned  f = g_u0tab_fallback.load(std::memory_order_relaxed);
            const unsigned  n = g_u0tab_none.load(std::memory_order_relaxed);
            if ((c - q_c) + (f - q_f) + (n - q_n))
                logger::write("U0TABLE since-last: compute=%u fallback=%u none=%u | totals %u/%u/%u "
                              "(fallback can hold another dispatch's or the graphics bind)",
                    c - q_c, f - q_f, n - q_n, c, f, n);
            q_c = c;
            q_f = f;
            q_n = n;
        }
        { /* Totals AND the delta since the last report -- see the counters' declaration. */
            static unsigned p_ok = 0;
            static unsigned p_nd = 0;
            static unsigned p_off = 0;
            static unsigned p_mu = 0;
            const unsigned  ok = g_claim_ok.load(std::memory_order_relaxed);
            const unsigned  nd = g_claim_nodecl.load(std::memory_order_relaxed);
            const unsigned  of = g_claim_offscreen.load(std::memory_order_relaxed);
            const unsigned  mu = g_claim_multi.load(std::memory_order_relaxed);
            const unsigned  d = (ok - p_ok) + (nd - p_nd) + (of - p_off) + (mu - p_mu);
            if (d)
                logger::write("CLAIMGATE since-last: ok=%u no-callback-declares=%u none-on-screen=%u "
                              "multi=%u | totals ok=%u/%u (%.0f%%)",
                    ok - p_ok, nd - p_nd, of - p_off, mu - p_mu,
                    ok, ok + nd + of + mu,
                    100.0 * (double)ok / (double)((ok + nd + of + mu) ? (ok + nd + of + mu) : 1));
            p_ok = ok;
            p_nd = nd;
            p_off = of;
            p_mu = mu;
        }
    }
}
