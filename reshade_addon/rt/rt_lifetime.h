#pragma once
#include "rt_completion.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>

namespace rtm {
// A command list may execute more than once, on more than one queue. Retain its
// dependencies until Reset/destruction ends that recording, then fence every
// observed queue. Signalling in the pre-Execute hook would be too early.
class RecordingLifetimes {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Recording {
        CompletionPtr completion = std::make_shared<Completion>();
        std::vector<Ptr<IUnknown>> objects;
        std::vector<std::shared_ptr<void>> leases;
        std::vector<Ptr<ID3D12CommandQueue>> queues;
    };
    struct Queue {
        Ptr<ID3D12CommandQueue> queue;
        Ptr<ID3D12Fence> fence;
        uint64_t next = 0;
    };
    struct Ticket { Ptr<ID3D12Fence> fence; uint64_t value; };
    struct Retired {
        Recording recording;
        std::vector<Ticket> tickets;
        bool failed = false;
    };
    std::mutex mutex_;
    std::map<const void*, Recording> recordings_;
    std::map<ID3D12CommandQueue*, Queue> queues_;
    std::vector<Retired> retired_;

public:
    CompletionPtr Token(const void* list)
    {
        std::lock_guard lock(mutex_);
        return recordings_[list].completion;
    }
    void Keep(const void* list, IUnknown* object)
    {
        if (!object) return;
        std::lock_guard lock(mutex_);
        recordings_[list].objects.emplace_back(object);
    }
    void KeepLease(const void* list, std::shared_ptr<void> lease)
    {
        if (!lease) return;
        std::lock_guard lock(mutex_);
        recordings_[list].leases.push_back(std::move(lease));
    }
    void Execute(const void* list, ID3D12CommandQueue* queue)
    {
        std::lock_guard lock(mutex_);
        auto it = recordings_.find(list);
        if (it == recordings_.end()) return;
        if (!queue) {
            it->second.completion->state.store(Completion::failed, std::memory_order_release);
            return;
        }
        auto& qs = it->second.queues;
        if (std::none_of(qs.begin(), qs.end(), [queue](const auto& q) { return q.Get() == queue; })) qs.emplace_back(queue);
    }
    void Reset(const void* list)
    {
        std::lock_guard lock(mutex_);
        auto it = recordings_.find(list);
        if (it == recordings_.end()) return;
        Retired retired;
        retired.recording = std::move(it->second);
        recordings_.erase(it);
        retired.failed = retired.recording.completion->state.load(std::memory_order_acquire) == Completion::failed;
        for (const auto& q : retired.recording.queues) {
            auto& state = queues_[q.Get()];
            if (!state.fence) {
                Ptr<ID3D12Device> device;
                state.queue = q;
                if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))) ||
                    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)))) {
                    retired.failed = true;
                    continue;
                }
            }
            const uint64_t value = ++state.next;
            if (FAILED(q->Signal(state.fence.Get(), value))) retired.failed = true;
            else retired.tickets.push_back({ state.fence, value });
        }
        if (retired.recording.queues.empty() && !retired.failed) {
            retired.recording.completion->state.store(Completion::abandoned, std::memory_order_release);
            return; // discarded recording: it was never read by a queue
        }
        if (retired.failed) retired.recording.completion->state.store(Completion::failed, std::memory_order_release);
        retired_.push_back(std::move(retired));
    }
    void Poll()
    {
        std::lock_guard lock(mutex_);
        for (auto it = retired_.begin(); it != retired_.end();) {
            bool done = !it->failed;
            for (const auto& ticket : it->tickets) {
                const uint64_t value = ticket.fence->GetCompletedValue();
                // UINT64_MAX means device removal, never successful completion.
                done = done && value != UINT64_MAX && value >= ticket.value;
            }
            if (!done) { ++it; continue; }
            it->recording.completion->state.store(Completion::complete, std::memory_order_release);
            it = retired_.erase(it);
        }
    }
};
} // namespace rtm
