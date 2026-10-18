// device_watch.cpp -- a device removal written into our log (DRED, the debug layer), from a poller
// thread that does not depend on the render loop. See device_watch.h.
#include "../addon_common.h"
#include "removal_pin.h"
#include "../addon_core.h"
#include "device_watch.h"
#include "../rt/rt_hooks.h" // RtOnDeviceRemoved
#include <thread>

using namespace reshade::api;

static void DumpDeviceRemoval(ID3D12Device* d3d, HRESULT rr)
{
    const char* what = (rr == DXGI_ERROR_DEVICE_HUNG) ? "DEVICE_HUNG (GPU stopped responding -- TDR)"
        : (rr == DXGI_ERROR_DEVICE_REMOVED)           ? "DEVICE_REMOVED"
        : (rr == DXGI_ERROR_DEVICE_RESET)             ? "DEVICE_RESET"
        : (rr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)    ? "DRIVER_INTERNAL_ERROR"
        : (rr == DXGI_ERROR_INVALID_CALL)             ? "INVALID_CALL (our command stream)"
                                                      : "unknown";
    // a removal is always written (without debug=1 the log is otherwise off), and the gist into ReShade.log too
    if (logger::g_level < 1) logger::set_level(1);
    reshade::log::message(reshade::log::level::error, "wwmi-dx12: device removed -- DRED details in wwmi_dx12_addon.log");
    RtOnDeviceRemoved();
    logger::report("*** DEVICE REMOVED: hr=%08x %s  frame=%llu", (unsigned)rr, what, (unsigned long long)g_frame_id);
    IbHash::LogInflight("device removed");
    DrainDebugLayer("at removal");
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dred))) && dred) {
        ID3D12DeviceRemovedExtendedData1*   dred1 = nullptr;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1{};
        const bool                          have1 = SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1 &&
            SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1));
        if (have1) {
            int n = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = bc1.pHeadAutoBreadcrumbNode; node && n < 256; node = node->pNext, ++n) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                char       ours[240];
                RemovalPin::Describe(node->pCommandList, ours, sizeof ours);
                logger::report("  DRED node %d: cmdlist='%S' %p queue='%S' %p executed %u of %u ops, %u marker(s) | %s",
                    n, node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                    (void*)node->pCommandList,
                    node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                    (void*)node->pCommandQueue, done, node->BreadcrumbCount, node->BreadcrumbContextsCount, ours);
                if (node->pCommandHistory && done < node->BreadcrumbCount) {
                    logger::report("      first unfinished op %u = D3D12_AUTO_BREADCRUMB_OP %d (where the GPU stopped, not proof of the culprit)", done, (int)node->pCommandHistory[done]);
                    if (done) { // the ops around it (5 EXECUTEINDIRECT, 6 DISPATCH, 7 COPYBUFFERREGION, 15 BARRIER, 31 AS BUILD, 34 DISPATCHRAYS)
                        char       ops[200] = "";
                        size_t     o = 0;
                        const UINT from = done > 16 ? done - 16 : 0, to = (std::min)(node->BreadcrumbCount, done + 4);
                        for (UINT k = from; k < to && o + 8 < sizeof ops; ++k)
                            o += snprintf(ops + o, sizeof ops - o, k == done ? " [%d]" : " %d", (int)node->pCommandHistory[k]);
                        logger::report("      ops %u..%u:%s", from, to ? to - 1 : 0, ops);
                    }
                    const D3D12_DRED_BREADCRUMB_CONTEXT* c = node->pBreadcrumbContexts;
                    const UINT                           cn = c ? node->BreadcrumbContextsCount : 0;
                    UINT                                 first_after = cn;
                    for (UINT k = 0; k < cn; ++k)
                        if (c[k].BreadcrumbIndex > done) {
                            first_after = k;
                            break;
                        }
                    const UINT from = first_after >= 4 ? first_after - 4 : 0;
                    for (UINT k = from; k < cn && k <= first_after; ++k)
                        logger::report("      marker op %u%s: '%S'", c[k].BreadcrumbIndex,
                            c[k].BreadcrumbIndex > done ? " (after the fault)" : "",
                            c[k].pContextString ? c[k].pContextString : L"");
                    if (!cn) logger::report("      no markers on this list (nothing of ours, no claimed draw)");
                }
            }
            if (!n) logger::report("  DRED: no breadcrumb nodes");
        }
        if (dred1) dred1->Release();
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
        if (!have1 && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
            int n = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode; node && n < 256; node = node->pNext, ++n) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                // The list pointer is the pin's key: an unnamed game list still says what WE recorded on it.
                char ours[240];
                RemovalPin::Describe(node->pCommandList, ours, sizeof ours);
                logger::report("  DRED node %d: cmdlist='%S' %p queue='%S' %p executed %u of %u ops | %s",
                    n, node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                    (void*)node->pCommandList,
                    node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                    (void*)node->pCommandQueue, done, node->BreadcrumbCount, ours);
                // The op still running when the GPU died is the one after the last completed.
                if (node->pCommandHistory && done < node->BreadcrumbCount)
                    logger::report("      first unfinished op %u = D3D12_AUTO_BREADCRUMB_OP %d (where the GPU stopped, not proof of the culprit)",
                        done, (int)node->pCommandHistory[done]);
            }
            if (!n) logger::report("  DRED: no breadcrumb nodes");
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        const HRESULT                pfhr = dred->GetPageFaultAllocationOutput(&pf);
        if (FAILED(pfhr) || !pf.PageFaultVA)
            logger::report("  DRED page fault: none (hr=%08x VA=0) -- a HUNG without a fault is a wait that never completes "
                           "or a shader that never ends",
                (unsigned)pfhr);
        if (SUCCEEDED(pfhr) && pf.PageFaultVA) {
            logger::report("  DRED page fault VA=%llx", (unsigned long long)pf.PageFaultVA);
            auto walk = [](const char* label, const D3D12_DRED_ALLOCATION_NODE* head) {
                int i = 0;
                for (const D3D12_DRED_ALLOCATION_NODE* a = head; a && i < 32; a = a->pNext, ++i) {
                    char name[256] = "(unnamed)";
                    if (a->ObjectNameW)
                        WideCharToMultiByte(CP_UTF8, 0, a->ObjectNameW, -1, name, sizeof(name), nullptr, nullptr);
                    else if (a->ObjectNameA)
                        strncpy_s(name, a->ObjectNameA, _TRUNCATE);
                    logger::report("    %s[%d] type=%d '%s'", label, i, (int)a->AllocationType, name);
                }
                if (!i) logger::report("    %s: none", label);
            };
            walk("freed  ", pf.pHeadRecentFreedAllocationNode);
            walk("existing", pf.pHeadExistingAllocationNode);
        }
        dred->Release();
    } else {
        logger::report("  DRED: interface unavailable -- reason code only");
    }
}

// The DRED poller's stop flag (the poller lives below; wwmi_addon's destroy_device raises it).
static std::atomic<bool> g_dred_watch_stop{ false };
void                     DredWatchdogStop()
{
    g_dred_watch_stop.store(true, std::memory_order_release);
}

void StartRemovalWatchdog(swapchain* sc)
{
    {
        static std::atomic<bool> s_watchdog_started{ false };
        bool                     expected = false;
        if (sc && sc->get_device() && s_watchdog_started.compare_exchange_strong(expected, true)) {
            ID3D12Device* dev_for_watch = reinterpret_cast<ID3D12Device*>(sc->get_device()->get_native());
            if (dev_for_watch) {
                dev_for_watch->AddRef(); // outlives the render loop; released when the poll ends
                ID3D12Fence* fence = nullptr;
                HANDLE       ev = nullptr;
                if (SUCCEEDED(dev_for_watch->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (!ev || FAILED(fence->SetEventOnCompletion(UINT64_MAX, ev))) {
                        if (ev) CloseHandle(ev);
                        ev = nullptr;
                        fence->Release();
                        fence = nullptr;
                    }
                }
                std::thread([dev_for_watch, fence, ev] {
                    while (!g_dred_watch_stop.load(std::memory_order_acquire)) {
                        if (ev)
                            WaitForSingleObject(ev, 200);
                        else
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        HRESULT rr = dev_for_watch->GetDeviceRemovedReason();
                        if (FAILED(rr)) {
                            RemovalPin::g_device_removed.store(true, std::memory_order_release);
                            DumpDeviceRemoval(dev_for_watch, rr);
                            return;
                        }
                    }
                    if (fence) fence->Release();
                    if (ev) CloseHandle(ev);
                    dev_for_watch->Release();
                }).detach();
                logger::report("DRED: removal watchdog started (%s)", ev ? "fence event + 200ms poll" : "200ms poll");
                reshade::log::message(reshade::log::level::info, ev ? "wwmi-dx12: device removal watchdog started (fence event)"
                                                                    : "wwmi-dx12: device removal watchdog started (200 ms poll)");
            }
        }
    }
}
