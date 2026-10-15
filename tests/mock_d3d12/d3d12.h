#pragma once
// Minimal deterministic API double for upload_readback.h; never used in the DLL.
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cassert>
#include <cstring>
using HRESULT = int;
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define IID_PPV_ARGS(p) (p)
enum D3D12_COMMAND_LIST_TYPE { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY };
enum { D3D12_HEAP_TYPE_DEFAULT = 1, D3D12_HEAP_TYPE_UPLOAD = 2, D3D12_HEAP_TYPE_READBACK = 3,
    D3D12_RESOURCE_DIMENSION_BUFFER = 1, D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1, D3D12_RESOURCE_STATE_COPY_DEST = 1,
    D3D12_HEAP_FLAG_NONE = 0, D3D12_FENCE_FLAG_NONE = 0 };
using D3D12_HEAP_FLAGS = int;
struct D3D12_HEAP_PROPERTIES { int Type = 0; unsigned CreationNodeMask = 0, VisibleNodeMask = 0; };
struct D3D12_RANGE { size_t Begin, End; };
struct D3D12_RESOURCE_DESC {
    int Dimension = 0; uint64_t Width = 0; unsigned Height = 0, DepthOrArraySize = 0, MipLevels = 0;
    struct { unsigned Count = 0; } SampleDesc;
    int Layout = 0;
};
struct D3D12_COMMAND_QUEUE_DESC { D3D12_COMMAND_LIST_TYPE Type = D3D12_COMMAND_LIST_TYPE_DIRECT; };
struct MockCom {
    unsigned refs = 1;
    virtual ~MockCom() = default;
    unsigned AddRef() { return ++refs; }
    unsigned Release() { const auto n = --refs; if (!n) delete this; return n; }
};
inline int mock_map_calls = 0;
struct ID3D12Resource : MockCom {
    int heap; std::vector<char> data; bool copied = false;
    ID3D12Resource(int h, size_t bytes) : heap(h), data(bytes) {}
    HRESULT GetHeapProperties(D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS*) { p->Type = heap; return 0; }
    D3D12_RESOURCE_DESC GetDesc() { D3D12_RESOURCE_DESC d; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = data.size(); return d; }
    HRESULT Map(unsigned, D3D12_RANGE* range, void** out) {
        assert(heap == D3D12_HEAP_TYPE_READBACK && copied && range->End <= data.size());
        ++mock_map_calls; *out = data.data(); return 0;
    }
    void Unmap(unsigned, D3D12_RANGE* range) { assert(range->Begin == 0 && range->End == 0); }
};
struct ID3D12Fence : MockCom { uint64_t completed = 0; uint64_t GetCompletedValue() { return completed; } };
struct ID3D12CommandAllocator : MockCom {};
struct ID3D12CommandList : MockCom {};
struct ID3D12GraphicsCommandList : ID3D12CommandList {
    struct Copy { ID3D12Resource* dest; uint64_t offset; ID3D12Resource* source; uint64_t source_offset, bytes; };
    std::vector<Copy> copies;
    void CopyBufferRegion(ID3D12Resource* d, uint64_t o, ID3D12Resource* s, uint64_t so, uint64_t n) { copies.push_back({ d, o, s, so, n }); }
    HRESULT Close() { return 0; }
};
struct ID3D12CommandQueue : MockCom {
    bool fail_signal = false;
    std::vector<ID3D12GraphicsCommandList*> lists;
    std::vector<ID3D12Fence*> fences;
    D3D12_COMMAND_QUEUE_DESC GetDesc() { return {}; }
    void ExecuteCommandLists(unsigned n, ID3D12CommandList** incoming) {
        for (unsigned i = 0; i < n; ++i) lists.push_back(static_cast<ID3D12GraphicsCommandList*>(incoming[i]));
    }
    HRESULT Signal(ID3D12Fence* fence, uint64_t value) { assert(value == 1); if (fail_signal) return -1; fences.push_back(fence); return 0; }
    void complete() {
        for (auto* list : lists) for (const auto& c : list->copies) {
            assert(c.source->heap == D3D12_HEAP_TYPE_UPLOAD);
            assert(c.offset + c.bytes <= c.dest->data.size() && c.source_offset + c.bytes <= c.source->data.size());
            std::memcpy(c.dest->data.data() + c.offset, c.source->data.data() + c.source_offset, size_t(c.bytes));
            c.dest->copied = true;
        }
        for (auto* fence : fences) fence->completed = 1;
        lists.clear(); fences.clear();
    }
};
struct ID3D12Device : MockCom {
    bool fail_create = false;
    HRESULT CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator** out) { if (fail_create) return -1; *out = new ID3D12CommandAllocator; return 0; }
    HRESULT CreateCommandList(unsigned, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, void*, ID3D12GraphicsCommandList** out) { *out = new ID3D12GraphicsCommandList; return 0; }
    HRESULT CreateCommittedResource(D3D12_HEAP_PROPERTIES* h, int, D3D12_RESOURCE_DESC* d, int, void*, ID3D12Resource** out) { *out = new ID3D12Resource(h->Type, size_t(d->Width)); return 0; }
    HRESULT CreateFence(uint64_t, int, ID3D12Fence** out) { *out = new ID3D12Fence; return 0; }
};
