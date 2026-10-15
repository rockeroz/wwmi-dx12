#include "../tools/rt_capture/trace_buffer.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace rt_capture;
    assert(hex(0) == "0x0000000000000000");
    assert(hex(UINT64_MAX) == "0xffffffffffffffff");
    assert(fnv1a64("", 0) == 0xcbf29ce484222325ull);
    assert(fnv1a64("hello", 5) == 0xa430d84680aabd0bull);
    TraceBuffer trace;
    assert(trace.append(std::string(TraceBuffer::max_bytes, 'x')));
    assert(!trace.append("x") && trace.dropped == 1);
    trace.clear();
    assert(trace.events.empty() && trace.bytes == 0 && trace.dropped == 0);
    for (size_t i = 0; i < TraceBuffer::max_events; ++i) assert(trace.append("{}"));
    assert(!trace.append("{}") && trace.dropped == 1);
    std::cout << "rt_capture_test: PASS\n";
}
