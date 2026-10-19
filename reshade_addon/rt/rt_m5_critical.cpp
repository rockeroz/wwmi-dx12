#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "rt_m5_critical.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <immintrin.h>

namespace rtcritical {
using Microsoft::WRL::ComPtr;
namespace {
// x64 COM C ABI: struct-returning GetDesc takes This, then the return buffer.
using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using HeapDescFn = D3D12_DESCRIPTOR_HEAP_DESC*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*, D3D12_DESCRIPTOR_HEAP_DESC*);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
struct Patch { void** slot; void* original; void* replacement; };
std::mutex patch_mutex;
std::vector<Patch> patches;
constexpr GUID reservation_id{ 0xa9cafb81, 0x5cc4, 0x4947, { 0x90, 0xa0, 0x39, 0x9e, 0xc5, 0x8b, 0x1a, 0x7d } };
struct Reservation {
    uint32_t requested, count;
    uint64_t native, cpu, gpu;
};
std::atomic<ID3D12Device*> target_device{ nullptr };
std::atomic<uint64_t> reserved_heaps{ 0 }, allocations{ 0 }, submissions{ 0 }, completed{ 0 };
std::atomic<uint64_t> published{ 0 }, fallbacks{ 0 }, late_refills{ 0 };

bool PatchSlot(void* object, size_t index, void* replacement)
{
    if (!object) return false;
    void** slot = *static_cast<void***>(object) + index;
    std::lock_guard lock(patch_mutex);
    for (const auto& p : patches) if (p.slot == slot) return *slot == replacement;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    void* original = *slot;
    patches.push_back({ slot, original, replacement });
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), replacement);
    DWORD ignored;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    return true;
}
template<class F> F Original(void* object, size_t index)
{
    void** slot = *static_cast<void***>(object) + index;
    std::lock_guard lock(patch_mutex);
    for (const auto& p : patches) if (p.slot == slot) return reinterpret_cast<F>(p.original);
    return reinterpret_cast<F>(*slot);
}
bool ReadReservation(ID3D12DescriptorHeap* heap, Reservation* r)
{
    UINT size = sizeof *r;
    return heap && SUCCEEDED(heap->GetPrivateData(reservation_id, &size, r)) && size == sizeof *r && r->count;
}
D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE HeapDesc(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_DESC* result)
{
    auto original = Original<HeapDescFn>(heap, 8);
    original(heap, result);
    Reservation r{};
    if (ReadReservation(heap, &r)) result->NumDescriptors = r.requested;
    return result;
}
HRESULT STDMETHODCALLTYPE CreateHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out)
{
    const auto original = Original<CreateHeapFn>(device, 14);
    const uint32_t extra = device == target_device.load(std::memory_order_acquire) && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? ReservationCapacity(desc->NumDescriptors) : 0;
    if (!extra || !out || iid != __uuidof(ID3D12DescriptorHeap)) return original(device, desc, iid, out);
    auto expanded = *desc;
    expanded.NumDescriptors += extra;
    HRESULT hr = original(device, &expanded, iid, out);
    if (FAILED(hr)) return original(device, desc, iid, out); // no reservation on devices that reject the larger heap
    auto* heap = static_cast<ID3D12DescriptorHeap*>(*out);
    const Reservation r{ desc->NumDescriptors, extra, reinterpret_cast<uint64_t>(heap),
        heap->GetCPUDescriptorHandleForHeapStart().ptr, heap->GetGPUDescriptorHandleForHeapStart().ptr };
    if (!PatchSlot(heap, 8, reinterpret_cast<void*>(&HeapDesc)) || FAILED(heap->SetPrivateData(reservation_id, sizeof r, &r))) {
        heap->Release();
        *out = nullptr;
        return original(device, desc, iid, out);
    }
    ++reserved_heaps;
    return hr;
}

struct Pool {
    ComPtr<ID3D12DescriptorHeap> heap;
    Reservation reservation;
    uint32_t increment;
    FreeRanges ranges;
    std::mutex mutex;
    Pool(ID3D12DescriptorHeap* h, const Reservation& r, uint32_t inc)
        : heap(h), reservation(r), increment(inc), ranges(r.requested, r.count) {}
};
std::mutex pools_mutex;
std::map<uint64_t, std::weak_ptr<Pool>> pools;

struct Recording { uint64_t generation = 0; std::vector<std::shared_ptr<Resources>> resources; };
struct QueueState { ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Fence> fence; uint64_t next = 0; };
struct Flight { std::shared_ptr<QueueState> queue; uint64_t value; std::vector<std::shared_ptr<Resources>> resources; };
std::mutex jobs_mutex;
std::map<const void*, Recording> recordings;
std::map<ID3D12CommandQueue*, std::shared_ptr<QueueState>> queues;
std::vector<Flight> flights;
std::vector<std::shared_ptr<Resources>> quarantine; // failed submission/fence: never pretend the GPU completed

void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    const auto original = Original<ExecuteFn>(queue, 10);
    std::vector<std::shared_ptr<Resources>> refs;
    {
        std::lock_guard lock(jobs_mutex);
        for (UINT i = 0; i < count; ++i)
            if (auto it = recordings.find(lists[i]); it != recordings.end())
                refs.insert(refs.end(), it->second.resources.begin(), it->second.resources.end());
    }
    original(queue, count, lists);
    if (refs.empty()) return;
    // This signal follows the real ExecuteCommandLists, not ReShade's pre-submit callback.
    std::lock_guard lock(jobs_mutex);
    auto& state = queues[queue];
    if (!state) {
        state = std::make_shared<QueueState>();
        state->queue = queue;
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))))
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence));
    }
    const uint64_t value = ++state->next;
    if (!state->fence || FAILED(queue->Signal(state->fence.Get(), value))) {
        quarantine.insert(quarantine.end(), refs.begin(), refs.end());
        return;
    }
    flights.push_back({ state, value, std::move(refs) });
    ++submissions;
}
} // namespace

struct Publication {
    std::mutex mutex;
    PublicationState state;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Fence> gate;
    uint8_t* mapped = nullptr;
    size_t bytes = 0;
    std::vector<DescriptorPtr> descriptors;
    std::chrono::steady_clock::time_point deadline;
};
namespace {
std::mutex publications_mutex;
std::condition_variable publication_cv;
std::vector<std::weak_ptr<Publication>> publications;
std::thread publication_worker;
bool stopping = false;

void FreezeFallback(const PublicationPtr& p)
{
    std::lock_guard lock(p->mutex);
    if (!p->state.Commit(false)) return;
    // The upload was zeroed before any command referenced it. Do not write after freezing.
    _mm_sfence();
    p->gate->Signal(1);
    ++fallbacks;
}
void Worker()
{
    std::unique_lock lock(publications_mutex);
    while (!stopping) {
        publication_cv.wait_for(lock, std::chrono::milliseconds(2));
        auto pending = publications;
        publications.erase(std::remove_if(publications.begin(), publications.end(), [](const auto& p) { return p.expired(); }), publications.end());
        lock.unlock();
        const auto now = std::chrono::steady_clock::now();
        for (const auto& weak : pending)
            if (auto p = weak.lock(); p && now >= p->deadline) FreezeFallback(p);
        lock.lock();
    }
}
} // namespace

bool InitDevice(ID3D12Device* device)
{
    if (!device) return false;
    target_device.store(device, std::memory_order_release);
    return PatchSlot(device, 14, reinterpret_cast<void*>(&CreateHeap));
}
DescriptorLease::~DescriptorLease()
{
    if (auto p = std::static_pointer_cast<Pool>(pool)) {
        std::lock_guard lock(p->mutex);
        p->ranges.Put(first, count);
    }
}
DescriptorPtr Allocate(ID3D12DescriptorHeap* heap, uint32_t count)
{
    Reservation r{};
    if (!ReadReservation(heap, &r)) return {}; // existing/unhooked heaps are never treated as free space
    std::shared_ptr<Pool> pool;
    {
        std::lock_guard lock(pools_mutex);
        pool = pools[r.native].lock();
        if (!pool) {
            ComPtr<ID3D12Device> device;
            if (FAILED(heap->GetDevice(IID_PPV_ARGS(&device)))) return {};
            pool = std::make_shared<Pool>(heap, r, device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            pools[r.native] = pool;
        }
    }
    uint32_t first;
    std::lock_guard lock(pool->mutex);
    if (!pool->ranges.Take(count, &first)) return {};
    auto lease = std::make_shared<DescriptorLease>();
    lease->pool = pool;
    lease->first = first; lease->count = count;
    lease->cpu.ptr = SIZE_T(r.cpu + uint64_t(first) * pool->increment);
    lease->gpu.ptr = r.gpu + uint64_t(first) * pool->increment;
    ++allocations;
    return lease;
}
PublicationPtr Pending(ID3D12Resource* upload, uint8_t* mapped, size_t bytes)
{
    auto p = std::make_shared<Publication>();
    p->upload = upload; p->mapped = mapped; p->bytes = bytes;
    ComPtr<ID3D12Device> device;
    if (!upload || !mapped || !bytes || FAILED(upload->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->gate)))) return {};
    p->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    std::lock_guard lock(publications_mutex);
    if (stopping) return {};
    publications.push_back(p);
    if (!publication_worker.joinable()) publication_worker = std::thread(Worker);
    publication_cv.notify_one();
    return p;
}
bool Finalize(const PublicationPtr& p, const std::vector<uint8_t>& block, const std::vector<DescriptorPtr>& descriptors)
{
    if (!p) return false;
    std::lock_guard lock(p->mutex);
    if (p->state.Get() != PublicationState::pending || block.size() != p->bytes) { ++late_refills; return false; }
    memcpy(p->mapped, block.data(), block.size());
    p->descriptors = descriptors;
    p->state.Commit(true);
    _mm_sfence();
    p->gate->Signal(1);
    ++published;
    return true;
}
bool IsPublished(const PublicationPtr& p)
{
    if (!p) return true;
    std::lock_guard lock(p->mutex);
    return p->state.Get() != PublicationState::pending;
}
void Record(const void* list, uint64_t generation, const std::shared_ptr<Resources>& resources)
{
    std::lock_guard lock(jobs_mutex);
    auto& r = recordings[list];
    if (r.generation != generation) r = { generation, {} };
    if (std::find(r.resources.begin(), r.resources.end(), resources) == r.resources.end()) r.resources.push_back(resources);
}
bool BeforeSubmit(ID3D12CommandQueue* queue, const void* list, uint64_t generation)
{
    std::vector<std::shared_ptr<Resources>> refs;
    {
        std::lock_guard lock(jobs_mutex);
        auto it = recordings.find(list);
        if (it == recordings.end() || it->second.generation != generation) return true;
        refs = it->second.resources;
    }
    const bool hooked = queue && PatchSlot(queue, 10, reinterpret_cast<void*>(&Execute));
    for (const auto& r : refs) {
        const auto& p = r->publication;
        if (!p) continue;
        std::lock_guard lock(p->mutex);
        if (p->state.Get() == PublicationState::pending &&
            (!hooked || FAILED(queue->Wait(p->gate.Get(), 1)))) {
            p->state.Commit(false);
            _mm_sfence(); p->gate->Signal(1); ++fallbacks;
        }
    }
    if (!hooked) {
        std::lock_guard lock(jobs_mutex);
        quarantine.insert(quarantine.end(), refs.begin(), refs.end());
    }
    return hooked;
}
void Reset(const void* list) { std::lock_guard lock(jobs_mutex); recordings.erase(list); }
void Poll()
{
    std::lock_guard lock(jobs_mutex);
    for (auto it = flights.begin(); it != flights.end();) {
        const uint64_t value = it->queue->fence->GetCompletedValue();
        if (value == UINT64_MAX || value < it->value) { ++it; continue; }
        it = flights.erase(it);
        ++completed;
    }
}
Stats Statistics() { return { reserved_heaps.load(), allocations.load(), submissions.load(), completed.load(), published.load(), fallbacks.load(), late_refills.load() }; }
void Shutdown()
{
    std::vector<PublicationPtr> pending;
    {
        std::lock_guard lock(publications_mutex);
        stopping = true;
        for (const auto& weak : publications) if (auto p = weak.lock()) pending.push_back(p);
    }
    for (const auto& p : pending) FreezeFallback(p);
    publication_cv.notify_one();
    if (publication_worker.joinable()) publication_worker.join();
    std::lock_guard lock(patch_mutex);
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        DWORD protection;
        if (!VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &protection)) continue;
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(it->slot), it->original, it->replacement);
        DWORD ignored; VirtualProtect(it->slot, sizeof(void*), protection, &ignored);
    }
    // Callbacks are uninstalled at addon shutdown; GPU references stay pinned in
    // the module until process teardown rather than being discarded on a timeout.
}
} // namespace rtcritical
