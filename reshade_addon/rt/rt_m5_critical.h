#pragma once
#include "rt_m5_policy.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <vector>

namespace rtcritical {
struct DescriptorLease;
using DescriptorPtr = std::shared_ptr<DescriptorLease>;
struct DescriptorLease {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t first = 0, count = 0;
    std::shared_ptr<void> pool;
    ~DescriptorLease();
};
struct Publication;
using PublicationPtr = std::shared_ptr<Publication>;
struct Resources {
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> buffers;
    std::vector<DescriptorPtr> descriptors;
    PublicationPtr publication;
};

bool InitDevice(ID3D12Device*);
DescriptorPtr Allocate(ID3D12DescriptorHeap*, uint32_t count);
PublicationPtr Pending(ID3D12Resource* upload, uint8_t* mapped, size_t bytes);
bool Finalize(const PublicationPtr&, const std::vector<uint8_t>&, const std::vector<DescriptorPtr>&);
// All direct dispatches for this frame have been recorded; no later refill can
// serve this payload. Freeze its existing fallback without waiting for a timer.
bool FinishFrame(const PublicationPtr&);
bool IsPublished(const PublicationPtr&);
void Record(const void* native_list, uint64_t generation, const std::shared_ptr<Resources>&);
bool BeforeSubmit(ID3D12CommandQueue*, const void* native_list, uint64_t generation);
void Reset(const void* native_list);
void Poll();
void Shutdown();
struct Stats {
    uint64_t reserved_heaps, allocations, submissions, completed, published, fallback, late_refills;
    uint64_t frame_fallbacks, deadline_fallbacks, queue_waits, wait_signal_us, max_wait_signal_us;
};
Stats Statistics();
} // namespace rtcritical
