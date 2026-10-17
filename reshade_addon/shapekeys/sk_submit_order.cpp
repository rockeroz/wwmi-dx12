// Submission order: which command list the GPU sees when. See sk_internal.h.
#include "sk_internal.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/hot_path.h"
#include "../rt/rt_hooks.h"

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
    HotPath::OnExecute(queue, cl);
    RtNoteExecute(queue, cl); // RT M3: what this list reads from UPLOAD is out of reach from now on
    const void* native = (const void*)cl->get_native();
    if (queue && native == g_claim_last_list.load(std::memory_order_relaxed)) {
        // FUNCTIONAL: [Present] reads this under g_present_on_game_queue. Always recorded.
        g_claim_exec_queue.store((const void*)queue->get_native(), std::memory_order_relaxed);
        if (g_debug) g_claim_exec_seen.fetch_add(1, std::memory_order_relaxed); // heartbeat only
    }
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
