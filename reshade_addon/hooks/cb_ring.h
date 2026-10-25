#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include <utility>
#include "../../src/d3d12_buffer.h"
#include "../../src/gpu_progress.h"

struct CbRing {
    static constexpr uint32_t kSlot = 256;
    static constexpr uint32_t kBlock = 64; // slots per stamp
    struct Buf {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        uint8_t*                               ptr = nullptr;
        uint32_t                               slots = 0;
        std::vector<uint64_t>                  tag;
    };
    Buf                                                                      cur;
    uint32_t                                                                 next = 0;
    std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, uint64_t>> retired;
    ID3D12Device*                                                            dev = nullptr;
    std::mutex                                                               mtx;
    std::atomic<bool>                                                        made{ false };

    bool Make(Buf& b, uint32_t bytes)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> r;
        if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, &r))) return false;
        uint8_t*    p = nullptr;
        D3D12_RANGE rr = { 0, 0 };
        if (FAILED(r->Map(0, &rr, (void**)&p))) return false;
        b.res = r;
        b.ptr = p;
        b.slots = bytes / kSlot;
        b.tag.assign((b.slots + kBlock - 1) / kBlock, 0);
        return true;
    }
    bool Ensure(ID3D12Device* d, uint32_t total_bytes)
    {
        if (made.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> l(mtx);
        if (cur.res) return true;
        if (!d || total_bytes < kSlot) return false;
        dev = d;
        if (!Make(cur, total_bytes)) return false;
        made.store(true, std::memory_order_release);
        return true;
    }
    // `count` contiguous slots (count * kSlot bytes, at most one block apart); 1 = the single slot of every older caller.
    uint8_t* Take(uint64_t* out_va, ID3D12Resource** out_res = nullptr, uint32_t count = 1)
    {
        std::lock_guard<std::mutex> l(mtx);
        const uint64_t              now = GpuStamp(); // 0 = never written
        for (size_t i = 0; i < retired.size();)
            if (GpuPassed(retired[i].second)) {
                retired[i] = std::move(retired.back());
                retired.pop_back();
            } else
                ++i;
        if (!cur.res || count == 0 || count > kBlock || count > cur.slots) return nullptr;
        if (next + count > cur.slots) next = 0;
        // Every block the span enters at its first slot must be one the GPU is done with (count 1: as before).
        bool busy = false;
        for (uint32_t s = next; s < next + count; ++s)
            if (s % kBlock == 0 && cur.tag[s / kBlock] && !GpuPassed(cur.tag[s / kBlock])) busy = true;
        if (busy) {
            Buf g;
            if (!Make(g, cur.slots * kSlot * 2)) return nullptr;
            retired.push_back({ cur.res, now });
            cur = std::move(g);
            next = 0;
        }
        for (uint32_t s = next; s < next + count; ++s) cur.tag[s / kBlock] = now;
        const uint32_t off = next * kSlot;
        next += count;
        *out_va = cur.res->GetGPUVirtualAddress() + off;
        if (out_res) *out_res = cur.res.Get();
        return cur.ptr + off;
    }
};
