#define NOMINMAX
#include "../reshade_addon/rt/rt_m5_critical.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>

// CPU-only COM doubles. No GPU or D3D device is created. This test exercises
// the actual x64 GetDesc ABI, post-Execute fence ordering and publication locks.
static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct StubDevice { void** table; };
static StubDevice* device_stub;
static ULONG STDMETHODCALLTYPE Ref(void*) { return 2; }
static HRESULT STDMETHODCALLTYPE DeviceOf(void*, REFIID, void** out) { *out = device_stub; return S_OK; }
struct Fence final : ID3D12Fence {
    std::atomic<ULONG> references{ 1 };
    std::atomic<uint64_t> done{ 0 };
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return DeviceOf(this, iid, out); }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return done.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override { done.store(value); return S_OK; }
};
struct Heap final : ID3D12DescriptorHeap {
    ULONG references = 1;
    D3D12_DESCRIPTOR_HEAP_DESC actual;
    std::vector<uint8_t> private_bytes;
    explicit Heap(const D3D12_DESCRIPTOR_HEAP_DESC& d) : actual(d) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void* out) override {
        if (private_bytes.empty() || *size < private_bytes.size()) return E_FAIL;
        *size = UINT(private_bytes.size()); memcpy(out, private_bytes.data(), private_bytes.size()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT size, const void* data) override {
        const auto* p = static_cast<const uint8_t*>(data); private_bytes.assign(p, p + size); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return DeviceOf(this, iid, out); }
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return actual; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override { return { 0x100000 }; }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override { return { 0x200000 }; }
};
static HRESULT STDMETHODCALLTYPE CreateHeap(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID, void** out)
{
    *out = new Heap(*d); return S_OK;
}
static UINT STDMETHODCALLTYPE Increment(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE) { return 32; }
static HRESULT STDMETHODCALLTYPE CreateFence(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void** out)
{
    *out = new Fence; return S_OK;
}
struct QueueStub {
    void** table;
    std::vector<int> order;
    Fence* fence = nullptr;
    uint64_t signaled = 0;
};
static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT, ID3D12CommandList* const*)
{
    reinterpret_cast<QueueStub*>(q)->order.push_back(1);
}
static HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value)
{
    auto* s = reinterpret_cast<QueueStub*>(q); s->order.push_back(2);
    s->fence = static_cast<Fence*>(f); s->signaled = value; return S_OK;
}
static HRESULT STDMETHODCALLTYPE Wait(ID3D12CommandQueue* q, ID3D12Fence*, UINT64)
{
    reinterpret_cast<QueueStub*>(q)->order.push_back(0); return S_OK;
}
__declspec(noinline) static D3D12_DESCRIPTOR_HEAP_DESC PublicDesc(ID3D12DescriptorHeap* heap) { return heap->GetDesc(); }

int main()
{
    using namespace rtcritical;
    static std::array<void*, 44> device_table{};
    device_table[1] = reinterpret_cast<void*>(&Ref); device_table[2] = reinterpret_cast<void*>(&Ref);
    device_table[14] = reinterpret_cast<void*>(&CreateHeap); device_table[15] = reinterpret_cast<void*>(&Increment);
    device_table[36] = reinterpret_cast<void*>(&CreateFence);
    static StubDevice device{ device_table.data() }; device_stub = &device;
    auto* native_device = reinterpret_cast<ID3D12Device*>(&device);
    Check(InitDevice(native_device), "install device hook");
    D3D12_DESCRIPTOR_HEAP_DESC desc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 250080, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    ID3D12DescriptorHeap* heap = nullptr;
    Check(SUCCEEDED(native_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))), "create expanded heap");
    Check(static_cast<Heap*>(heap)->actual.NumDescriptors == 315616, "native allocation includes the reserved tail");
    Check(PublicDesc(heap).NumDescriptors == 250080, "C++ GetDesc preserves application-visible count (x64 ABI)");
    auto lease = Allocate(heap, 40);
    Check(lease && lease->first == 250080 && lease->gpu.ptr == 0x200000 + 250080ull * 32, "only exclusive capacity is used");

    static std::array<void*, 19> queue_table{};
    queue_table[1] = reinterpret_cast<void*>(&Ref); queue_table[2] = reinterpret_cast<void*>(&Ref);
    queue_table[7] = reinterpret_cast<void*>(&DeviceOf); queue_table[10] = reinterpret_cast<void*>(&Execute);
    queue_table[14] = reinterpret_cast<void*>(&Signal); queue_table[15] = reinterpret_cast<void*>(&Wait);
    static QueueStub q{ queue_table.data(), {}, nullptr, 0 };
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(&q);
    auto resources = std::make_shared<Resources>(); resources->descriptors.push_back(lease);
    const void* list = reinterpret_cast<void*>(0x1234);
    Record(list, 1, resources);
    Check(BeforeSubmit(queue, list, 1), "install submission hook");
    ID3D12CommandList* lists[] = { reinterpret_cast<ID3D12CommandList*>(const_cast<void*>(list)) };
    queue->ExecuteCommandLists(1, lists);
    Check(q.order == std::vector<int>({ 1, 2 }), "fence is signaled AFTER the actual ExecuteCommandLists");
    resources.reset(); lease.reset(); Reset(list); Poll();
    auto next = Allocate(heap, 40);
    Check(next->first == 250120, "GPU-in-flight descriptors cannot be reused after CPU reset");
    q.fence->done.store(UINT64_MAX); Poll();
    Check(Statistics().completed == 0, "device removal is not successful completion");
    q.fence->done.store(q.signaled); Poll();
    auto recycled = Allocate(heap, 40);
    Check(recycled->first == 250080, "range reused only after the real queue fence completes");

    std::array<void*, 15> resource_table{};
    resource_table[1] = reinterpret_cast<void*>(&Ref); resource_table[2] = reinterpret_cast<void*>(&Ref);
    resource_table[7] = reinterpret_cast<void*>(&DeviceOf);
    StubDevice upload{ resource_table.data() };
    std::vector<uint8_t> bytes(128, 0), block(128, 0x4f), changed(128, 0x3b);
    auto p = Pending(reinterpret_cast<ID3D12Resource*>(&upload), bytes.data(), bytes.size());
    Check(p && Finalize(p, block, { recycled }), "publish complete snapshot");
    Check(!Finalize(p, changed, {}) && bytes == block, "published bytes are immutable");
    std::vector<uint8_t> fallback(128, 0);
    auto timed = Pending(reinterpret_cast<ID3D12Resource*>(&upload), fallback.data(), fallback.size());
    for (int i = 0; i < 100 && !IsPublished(timed); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(IsPublished(timed) && !Finalize(timed, block, {}), "deadline opens independently and rejects late refill");
    Check(std::all_of(fallback.begin(), fallback.end(), [](uint8_t b) { return b == 0; }), "fallback stays zero, no racing write");

    Shutdown();
    Check(PublicDesc(heap).NumDescriptors == 315616, "native vtable restored on shutdown");
    std::cout << "rt_m5_native_test: PASS (CPU COM doubles, no GPU validation)\n";
    heap->Release();
}
