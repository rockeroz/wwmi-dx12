#include "../tools/rt_capture/upload_readback.h"
#include <sstream>
#include <iostream>
#include <cassert>

int main()
{
    using namespace rt_capture;
    std::istringstream config("WWMI_RT_TARGET_V1 10 2 0 6 6 3");
    auto target = read_target(config); assert(target);
    std::vector<GeometrySlice> geometry{ { 4096, 8192, 10, 6, 12, 6, 42, false }, { 4096, 8216, 10, 3, 12, 6, 42, false } };
    assert(match_ring(*target, geometry));
    geometry[1].indices += 4; assert(!match_ring(*target, geometry));
    for (const auto* bad : { "wrong 10 1 0 6", "WWMI_RT_TARGET_V1 10 2 0 6 5 3", "WWMI_RT_TARGET_V1 10 1 4294967295 3", "WWMI_RT_TARGET_V1 10 1 0 6 extra" }) {
        std::istringstream input(bad); assert(!read_target(input));
    }
    uint64_t offset = 0, bytes = 0;
    assert(typed_bytes(3, 8, 4, offset, bytes) && offset == 12 && bytes == 32);
    assert(!typed_bytes(UINT64_MAX, 2, 4, offset, bytes));
    assert(snapshot_bytes(100, 90) == 10 && snapshot_bytes(100, 100) == 0);
    auto device = own(new ID3D12Device);
    auto queue = own(new ID3D12CommandQueue);
    auto source = own(new ID3D12Resource(D3D12_HEAP_TYPE_UPLOAD, 64));
    source->data[16] = 42;
    UploadReadbacks capture;
    assert(capture.begin());
    UploadRequest request; request.source = source; request.offset = 16; request.requested = 16; request.root = 13;
    capture.submit(device.get(), queue.get(), { request, request });
    assert(capture.pending() && !capture.begin());
    capture.poll();
    assert(mock_map_calls == 0 && capture.samples.empty()); // fence incomplete: Map is forbidden
    queue->complete(); capture.poll();
    assert(!capture.pending() && mock_map_calls == 1 && capture.samples.size() == 1);
    assert(capture.samples[0].bytes.size() == 16 && capture.samples[0].bytes[0] == 42);
    capture.submit(device.get(), queue.get(), { request }); // dedupe within one capture
    assert(!capture.pending());
    assert(capture.begin());
    request.source = own(new ID3D12Resource(D3D12_HEAP_TYPE_DEFAULT, 64));
    capture.submit(device.get(), queue.get(), { request });
    assert(!capture.pending() && capture.skipped_heap == 1); // DEFAULT never transitions or maps
    request.source = source; request.offset = 64;
    capture.submit(device.get(), queue.get(), { request });
    assert(!capture.pending() && capture.skipped_range == 1);
    request.offset = 0; device->fail_create = true;
    capture.submit(device.get(), queue.get(), { request });
    assert(!capture.pending() && capture.failures == 1 && queue->lists.empty());
    device->fail_create = false;
    {
        UploadReadbacks limited;
        auto large = own(new ID3D12Resource(D3D12_HEAP_TYPE_UPLOAD, size_t(UploadReadbacks::max_bytes + 16384)));
        std::vector<UploadRequest> windows;
        for (uint64_t i = 0; i < 129; ++i) {
            UploadRequest r; r.source = large; r.offset = i * 16384;
            windows.push_back(r);
        }
        limited.submit(device.get(), queue.get(), windows);
        assert(limited.skipped_budget == 1 && limited.pending());
        queue->complete(); limited.poll();
        assert(limited.samples.size() == 128);
    }
    {
        UploadReadbacks reserved;
        auto large = own(new ID3D12Resource(D3D12_HEAP_TYPE_UPLOAD, 40 * 16384));
        std::vector<UploadRequest> windows;
        for (uint64_t i = 0; i < 40; ++i) {
            UploadRequest r; r.source = large; r.offset = i * 16384; r.compute = true;
            windows.push_back(r);
        }
        reserved.submit(device.get(), queue.get(), windows);
        queue->complete(); reserved.poll();
        assert(reserved.samples.size() == 32 && reserved.skipped_budget == 8);
        windows[32].compute = false; // reserved raster capacity remains usable
        reserved.submit(device.get(), queue.get(), { windows[32] });
        queue->complete(); reserved.poll();
        assert(reserved.samples.size() == 33);
    }
    queue->fail_signal = true;
    capture.submit(device.get(), queue.get(), { request });
    capture.poll();
    assert(capture.pending() && capture.samples.empty()); // failed Signal is not completion
    const auto refs = source->refs;
    capture.abandon_unfinished(); // bounded retention on forced unload, never early Release
    assert(!capture.pending() && source->refs == refs);
    queue->complete(); // retained objects are still alive even after cancellation/unload
    std::cout << "rt_upload_readback_test: PASS (API double; GPU execution not validated)\n";
}
