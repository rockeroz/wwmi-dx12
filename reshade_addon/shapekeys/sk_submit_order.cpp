// Submission order: which command list the GPU sees when. See sk_internal.h.
#include "sk_internal.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/hot_path.h"
#include "../rt/rt_hooks.h"
#include "../present/present_parts.h" // FrameEndStepGuarded -- the game's frame end, when presents come from another thread

struct SubmitLog {
    uint32_t                 frame = 0;
    std::vector<const void*> order;
};
std::mutex                      g_sub_mtx;
SubmitLog                       g_sub_cur;
SubmitLog                       g_sub_prev;
std::atomic<unsigned long long> g_sub_seen{ 0 };

int SubmitIndexOf(uint32_t frame, const void* list)
{
    std::lock_guard<std::mutex> l(g_sub_mtx);
    const SubmitLog*            src = nullptr;
    if (g_sub_cur.frame == frame)
        src = &g_sub_cur;
    else if (g_sub_prev.frame == frame)
        src = &g_sub_prev;
    if (!src) return -1;
    for (size_t i = 0; i < src->order.size(); ++i)
        if (src->order[i] == list) return (int)i;
    return -1;
}

void on_execute_command_list(reshade::api::command_queue* queue, reshade::api::command_list* cl)
{
    PROF(PROF_CmdExecute);
    if (!cl) return;
    RtNoteExecute(queue, cl); // RT M3: what this list reads from UPLOAD is out of reach from now on
    const void* native = (const void*)cl->get_native();
    if (CmdDrewThisRecording(cl)) {
        g_game_frame_submitted.store(true, std::memory_order_relaxed);
        g_game_submit_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (queue) g_game_submit_queue.store(queue->get_native(), std::memory_order_relaxed);
    }
#ifdef WWMI_DEBUG
    if (g_present_log_left.load(std::memory_order_relaxed) > 0) {
        g_present_log_execs.fetch_add(1, std::memory_order_relaxed);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const uint32_t        tid = GetCurrentThreadId();
        const uint64_t        q = queue ? (uint64_t)queue->get_native() : 0;
        std::lock_guard       l(g_present_log_mtx);
        PresentLogSubmitter*  s = nullptr;
        for (PresentLogSubmitter& e : g_present_log_subs)
            if (e.tid == tid && e.queue == q) s = &e;
        if (!s) {
            g_present_log_subs.push_back({});
            s = &g_present_log_subs.back();
            s->tid = tid;
            s->queue = q;
        }
        ++s->lists;
        if (native == g_claim_last_list.load(std::memory_order_relaxed)) ++s->claimed;
        s->last_qpc = now.QuadPart;
        const CmdBlock* fe = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cl));
        LARGE_INTEGER   freq;
        QueryPerformanceFrequency(&freq);
        const int64_t last = g_present_log_qpc.load(std::memory_order_relaxed);
        const double  since = last ? (double)(now.QuadPart - last) * 1000.0 / (double)freq.QuadPart : -1.0;
        if (fe && fe->bb_notes_gen == fe->gen && !fe->bb_notes.empty()) {
            std::string notes;
            for (const BbNote& n : fe->bb_notes) {
                char e[96];
                snprintf(e, sizeof(e), " %llx%s:%x>%x", (unsigned long long)n.res,
                    std::find(g_present_log_bbs.begin(), g_present_log_bbs.end(), n.res) != g_present_log_bbs.end() ? "(sc)" : "",
                    n.old_s, n.new_s);
                notes += e;
            }
            logger::report("BBCOMMON tid=%u queue=%llx list=%p since_present=%.2f ms:%s", tid, (unsigned long long)q, native, since,
                notes.c_str());
        }
    }
#endif
    HotPath::OnExecute(queue, cl);
    TexReadbackOnExecute(queue, cl);
    if (queue && native == g_claim_last_list.load(std::memory_order_relaxed)) {
        // FUNCTIONAL: [Present] reads this under g_present_on_game_queue. Always recorded.
        g_claim_exec_queue.store((const void*)queue->get_native(), std::memory_order_relaxed);
        if (g_debug) g_claim_exec_seen.fetch_add(1, std::memory_order_relaxed); // heartbeat only
    }
    if (queue && g_frame_end_armed.load(std::memory_order_relaxed) && CmdEndsGameFrame(cl) &&
        GetCurrentThreadId() == g_game_submit_tid.load(std::memory_order_relaxed) &&
        queue->get_native() == g_game_submit_queue.load(std::memory_order_relaxed) &&
        GetCurrentThreadId() != g_presenter_tid.load(std::memory_order_relaxed))
        FrameEndStepGuarded(queue);
    if (!g_debug) return;
    if (queue) {
        auto* nl = reinterpret_cast<ID3D12CommandList*>(cl->get_native());
        auto* nq = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
        RemovalPin::OnExecute(g_d3d12_device, native, nl ? (int)nl->GetType() : -1,
            nq ? (int)nq->GetDesc().Type : -1, "ExecuteCommandLists");
    }
    g_sub_seen.fetch_add(1); // read only by the SHAPEKEY-ORDER log line
    if (queue && MorphDump::Armed()) {
        static std::atomic<unsigned> s_seq{ 0 };
        logger::write("X26 EXEC frame=%u seq=%u queue=%p type=%d list=%p",
            g_frame_id, s_seq.fetch_add(1, std::memory_order_relaxed),
            (const void*)queue->get_native(), (int)queue->get_type(), native);
    }
    std::lock_guard<std::mutex> l(g_sub_mtx);
    if (g_sub_cur.frame != g_frame_id) {
        g_sub_prev = std::move(g_sub_cur);
        g_sub_cur.frame = g_frame_id;
        g_sub_cur.order.clear();
    }
    if (g_sub_cur.order.size() < 512) g_sub_cur.order.push_back(native);
}
