#pragma once
#include <d3d12.h>
#include <memory>
#include <set>
#include <tuple>
#include <vector>
#include <cstring>
#include "snapshot_plan.h"

namespace rt_capture {
template<class T> using Com = std::shared_ptr<T>;
template<class T> Com<T> own(T* p) { return Com<T>(p, [](T* value) { if (value) value->Release(); }); }
template<class T> Com<T> retain(T* p) { if (p) p->AddRef(); return own(p); }
struct UploadRequest {
    Com<ID3D12Resource> source;
    uint64_t resource_id = 0, offset = 0, requested = 16384, source_va = 0;
    uint32_t root = 0, first = 0, count = 0;
    bool compute = false;
    std::string context; // internally generated list/generation fields
};
struct UploadSample {
    uint64_t id = 0, resource_id = 0, offset = 0, source_va = 0;
    uint32_t root = 0, first = 0, count = 0;
    std::string context;
    std::vector<char> bytes;
};
class UploadReadbacks {
    struct Item { UploadRequest request; uint64_t id, dest, bytes; };
    struct Job {
        Com<ID3D12CommandQueue> queue;
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        Com<ID3D12Resource> readback;
        Com<ID3D12Fence> fence;
        std::vector<Item> items;
        uint64_t bytes = 0;
        bool signaled = false;
    };
    std::vector<std::unique_ptr<Job>> jobs;
    std::set<std::tuple<uint64_t, uint64_t>> copied;
    uint64_t used_bytes = 0, next_id = 0, submitted_jobs = 0, compute_samples = 0;
public:
    static constexpr uint64_t max_bytes = 2 * 1024 * 1024, max_samples = 128, max_jobs = 16;
    std::vector<UploadSample> samples;
    uint64_t skipped_heap = 0, skipped_range = 0, skipped_budget = 0, failures = 0;
    bool pending() const { return !jobs.empty(); }
    bool begin() {
        if (pending()) return false;
        copied.clear(); samples.clear(); used_bytes = next_id = submitted_jobs = compute_samples = 0;
        skipped_heap = skipped_range = skipped_budget = failures = 0;
        return true;
    }
    // Called BEFORE the application's submission, on that submission's thread.
    // Only UPLOAD sources are accepted: GENERIC_READ is permanent, GPU cannot
    // write them, and no guessed state transition or cross-queue wait is needed.
    // This samples upload memory at submission, not the game's GPU skin output.
    void submit(ID3D12Device* device, ID3D12CommandQueue* queue, const std::vector<UploadRequest>& requests)
    {
        if (requests.empty()) return;
        if (submitted_jobs >= max_jobs || (submitted_jobs >= 8 && std::all_of(requests.begin(), requests.end(), [](const auto& r) { return r.compute; }))) { skipped_budget += requests.size(); return; }
        const auto type = queue->GetDesc().Type;
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) { ++failures; return; }
        auto job = std::make_unique<Job>();
        job->queue = retain(queue);
        std::set<std::tuple<uint64_t, uint64_t>> admitted;
        uint64_t batch_compute = 0;
        for (const auto& r : requests) {
            const auto key = std::make_tuple(uint64_t(reinterpret_cast<uintptr_t>(r.source.get())), r.offset);
            if (!r.source || copied.count(key) || admitted.count(key)) continue;
            D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
            if (FAILED(r.source->GetHeapProperties(&heap, &flags)) || heap.Type != D3D12_HEAP_TYPE_UPLOAD) { ++skipped_heap; continue; }
            const auto desc = r.source->GetDesc();
            const auto n = snapshot_bytes(desc.Width, r.offset, r.requested);
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !n) { ++skipped_range; continue; }
            if ((r.compute && compute_samples + batch_compute >= 32) || next_id + job->items.size() >= max_samples || n > max_bytes - used_bytes - job->bytes) { ++skipped_budget; continue; }
            admitted.insert(key);
            job->items.push_back({ r, next_id + job->items.size() + 1, job->bytes, n });
            job->bytes += n;
            if (r.compute) ++batch_compute;
        }
        if (job->items.empty()) return;
        ID3D12CommandAllocator* allocator = nullptr;
        HRESULT hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
        job->allocator = own(allocator);
        if (FAILED(hr)) { ++failures; return; }
        ID3D12GraphicsCommandList* list = nullptr;
        hr = device->CreateCommandList(0, type, job->allocator.get(), nullptr, IID_PPV_ARGS(&list));
        job->list = own(list);
        if (FAILED(hr)) { ++failures; return; }
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = job->bytes;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* readback = nullptr;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        job->readback = own(readback);
        if (FAILED(hr)) { ++failures; return; }
        ID3D12Fence* fence = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        job->fence = own(fence);
        if (FAILED(hr)) { ++failures; return; }
        for (const auto& item : job->items)
            job->list->CopyBufferRegion(job->readback.get(), item.dest, item.request.source.get(), item.request.offset, item.bytes);
        if (FAILED(job->list->Close())) { ++failures; return; }
        // Finish every throwing allocation BEFORE GPU submission. An exception
        // after ExecuteCommandLists must not destroy an in-flight allocation.
        copied.insert(admitted.begin(), admitted.end());
        used_bytes += job->bytes; next_id += job->items.size(); compute_samples += batch_compute; ++submitted_jobs;
        jobs.push_back(std::move(job));
        auto& active = *jobs.back();
        ID3D12CommandList* lists[] = { active.list.get() };
        queue->ExecuteCommandLists(1, lists); // native queue, no ReShade re-entry
        active.signaled = SUCCEEDED(queue->Signal(active.fence.get(), 1));
        if (!active.signaled) ++failures; // retain job until device removal/unload
    }
    // Poll only. Map is allowed solely after OUR post-copy queue fence completes.
    void poll()
    {
        for (auto it = jobs.begin(); it != jobs.end();) {
            auto& job = **it;
            const auto completed = job.fence->GetCompletedValue();
            if (completed == UINT64_MAX) { ++failures; it = jobs.erase(it); continue; } // device removed
            if (!job.signaled || completed < 1) { ++it; continue; }
            void* data = nullptr; D3D12_RANGE range{ 0, size_t(job.bytes) };
            if (FAILED(job.readback->Map(0, &range, &data)) || !data) { ++failures; it = jobs.erase(it); continue; }
            struct Unmap {
                ID3D12Resource* resource;
                ~Unmap() { if (resource) { D3D12_RANGE none{ 0, 0 }; resource->Unmap(0, &none); } }
            } unmap{ job.readback.get() };
            // Build the CPU results transactionally: retrying after bad_alloc
            // must not append a partial batch a second time.
            std::vector<UploadSample> batch;
            batch.reserve(job.items.size());
            for (const auto& item : job.items) {
                const auto& r = item.request;
                UploadSample sample{ item.id, r.resource_id, r.offset, r.source_va, r.root, r.first, r.count, r.context, {} };
                const auto* begin = static_cast<const char*>(data) + item.dest;
                sample.bytes.assign(begin, begin + item.bytes);
                batch.push_back(std::move(sample));
            }
            samples.reserve(samples.size() + batch.size());
            for (auto& sample : batch) samples.push_back(std::move(sample));
            // Unmap must run before the resource is released by erasing the job.
            unmap.resource = nullptr;
            D3D12_RANGE none{ 0, 0 }; job.readback->Unmap(0, &none);
            it = jobs.erase(it);
        }
    }
    // Releasing GPU allocations before their fence is unsafe. On a forced unload
    // or device teardown, retain any unfinished bounded jobs until process exit.
    // No worker or callback survives DLL unload. Completed jobs are drained first.
    void abandon_unfinished() noexcept {
        for (auto& job : jobs) (void)job.release();
        jobs.clear();
    }
    ~UploadReadbacks() { abandon_unfinished(); }
};
} // namespace rt_capture
