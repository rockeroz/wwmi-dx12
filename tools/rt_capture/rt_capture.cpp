// SPDX-License-Identifier: GPL-3.0-only
// Independent, passive ReShade add-on. This does NOT enable mesh replacement with RT.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <reshade.hpp>
#include "trace_buffer.h"
#include "callback_gate.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <map>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_map>

using namespace reshade::api;
namespace fs = std::filesystem;
namespace {
using rt_capture::hex;
std::mutex mutex;
std::atomic<device*> owner{ nullptr };
std::atomic<bool> recording{ false };
std::atomic<bool> monitoring{ false }; // starts dormant; F7 enables metadata AFTER the game is running
std::atomic<bool> trace_full{ false }, trace_rt_full{ false }, probe_failed{ false };
std::atomic<HANDLE> boot_log{ INVALID_HANDLE_VALUE };
std::atomic<uint32_t> boot_hooks_seen{ 0 };
rt_capture::TraceBuffer trace;
rt_capture::TraceBuffer rt_trace; // separate budget: raster bindings cannot consume the RT records' space
uint64_t sequence = 0, frame = 0, next_id = 1, end_frame = 0;
uint64_t metadata_dropped = 0, shader_bytes = 0;
size_t metadata_bytes = 0;
bool last_f7 = false, last_f8 = false, last_f9 = false;
bool present_seen = false;
fs::path output_base;
constexpr size_t max_metadata = 16384;
constexpr size_t max_shader_bytes = 16 * 1024 * 1024;
constexpr size_t max_shaders = 2048;
constexpr size_t max_metadata_bytes = 16 * 1024 * 1024;

struct Metadata { uint64_t id; std::string json; };
std::map<uint64_t, Metadata> resources, pipelines, layouts;
std::map<uint64_t, std::vector<char>> shaders;
struct TableBind { uint64_t handle = 0; };
struct List {
    uint64_t id = 0, generation = 0, compute_layout = 0;
    std::array<TableBind, 64> compute_tables{};
};
struct Descriptor {
    uint64_t resource_id = 0, resource_handle = 0, offset = 0, bytes = 0, written = 0;
    uint32_t type = 0, format = 0, structure_stride = 0;
    bool buffer = false, valid = false;
};
std::map<uint64_t, Descriptor> descriptors; // actual CPU descriptor handle, not a GPU VA
uint64_t descriptor_writes = 0, descriptor_dropped = 0;
constexpr size_t max_descriptors = 131072;
std::unordered_map<command_list*, List> lists;
std::vector<std::string> inventory;

void boot_line(const char* text) noexcept
{
    const auto file = boot_log.load();
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, DWORD(lstrlenA(text)), &written, nullptr);
    FlushFileBuffers(file); // only startup/control/first-hook breadcrumbs, never every draw
}
void boot_hook(uint32_t bit, const char* text) noexcept
{
    if ((boot_hooks_seen.fetch_or(bit) & bit) == 0) boot_line(text);
}
void open_boot_log() noexcept
{
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return;
    DWORD start = length;
    while (start && path[start - 1] != L'\\' && path[start - 1] != L'/') --start;
    constexpr wchar_t name[] = L"wwmi_rt_capture_boot.log";
    if (start + sizeof(name) / sizeof(wchar_t) > 32768) return;
    for (size_t i = 0; i < sizeof(name) / sizeof(wchar_t); ++i) path[start + i] = name[i];
    boot_log.store(CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}
bool ours(device* dev) { return monitoring.load() && dev && !probe_failed.load() && owner.load(std::memory_order_acquire) == dev; }
bool active(command_list* list, bool rt_priority = false)
{
    return recording.load(std::memory_order_relaxed) && !(rt_priority ? trace_rt_full.load() : trace_full.load()) && list && ours(list->get_device());
}
uint64_t id_of(const std::map<uint64_t, Metadata>& entries, uint64_t key)
{
    const auto it = entries.find(key);
    return it == entries.end() ? 0 : it->second.id;
}
void add(std::string row, bool rt_priority = false) // mutex held
{
    if (!recording.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t seq = ++sequence;
    auto& target = rt_priority ? rt_trace : trace;
    if (!target.append("{\"seq\":" + std::to_string(seq) + ",\"frame\":" + std::to_string(frame) +
        ",\"qpc\":" + std::to_string(now.QuadPart) + "," + row + "}", seq))
        (rt_priority ? trace_rt_full : trace_full).store(true);
}
std::string list_fields(command_list* list) // mutex held
{
    auto it = lists.find(list);
    if (it == lists.end()) {
        if (lists.size() >= 2048) { ++metadata_dropped; return "\"list\":0,\"generation\":0,"; }
        List value;
        value.id = next_id++;
        it = lists.emplace(list, value).first;
    }
    return "\"list\":" + std::to_string(it->second.id) +
        ",\"generation\":" + std::to_string(it->second.generation) + ",";
}
void remember(std::map<uint64_t, Metadata>& entries, uint64_t handle, std::string fields)
{
    if (!entries.count(handle) && entries.size() >= max_metadata) { ++metadata_dropped; return; }
    Metadata m{ next_id++, {} };
    m.json = "{\"id\":" + std::to_string(m.id) + ",\"handle\":\"" + hex(handle) + "\"," + fields + "}";
    const auto old = entries.find(handle);
    const size_t old_bytes = old == entries.end() ? 0 : old->second.json.size();
    if (m.json.size() > max_metadata_bytes - (metadata_bytes - old_bytes)) { ++metadata_dropped; return; }
    metadata_bytes = metadata_bytes - old_bytes + m.json.size();
    entries[handle] = m;
    add("\"event\":\"metadata\",\"value\":" + m.json);
}
void ensure_buffer_metadata(resource res) // only callback-supplied resource handles; never raw GPU VAs
{
    if (!res.handle || resources.count(res.handle)) return;
    boot_hook(64, "first lazy metadata query for a live callback resource\r\n");
    auto* native = reinterpret_cast<ID3D12Resource*>(res.handle);
    const auto desc = native->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    std::ostringstream s;
    s << "\"kind\":\"resource\",\"type\":1,\"lazy\":true,\"va\":\"" << hex(native->GetGPUVirtualAddress())
      << "\",\"bytes\":" << desc.Width;
    remember(resources, res.handle, s.str());
}
std::string address(resource res, uint64_t offset) // mutex held; never dereference a raw GPU VA
{
    // Native DX12 AS and root SRV/UAV events use a null resource + an absolute GPU address.
    if (!res.handle) return "{\"resource_id\":0,\"va\":\"" + hex(offset) + "\"}";
    ensure_buffer_metadata(res);
    return "{\"resource_id\":" + std::to_string(id_of(resources, res.handle)) +
        ",\"resource\":\"" + hex(res.handle) + "\",\"offset\":\"" + hex(offset) + "\"}";
}
bool compute_stage(shader_stage stages)
{
    return uint32_t(stages & (shader_stage::all_compute | shader_stage::all_ray_tracing)) != 0;
}
void set_descriptor(uint64_t cpu, Descriptor value) // mutex held
{
    if (!value.valid) { descriptors.erase(cpu); return; }
    if (!descriptors.count(cpu) && descriptors.size() >= max_descriptors) { ++descriptor_dropped; return; }
    value.written = ++descriptor_writes;
    descriptors[cpu] = value;
}
uint64_t descriptor_cpu(device* dev, descriptor_table table, uint32_t binding, uint32_t* available = nullptr)
{
    if (!table.handle) return 0;
    descriptor_heap heap{};
    uint32_t offset = 0;
    dev->get_descriptor_heap_offset(table, binding, 0, &heap, &offset);
    if (!heap.handle) return 0;
    auto* native = reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
    const auto desc = native->GetDesc();
    if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return 0;
    if (offset >= desc.NumDescriptors) return 0;
    if (available) *available = desc.NumDescriptors - offset;
    const uint32_t stride = reinterpret_cast<ID3D12Device*>(dev->get_native())->GetDescriptorHandleIncrementSize(desc.Type);
    return native->GetCPUDescriptorHandleForHeapStart().ptr + uint64_t(offset) * stride;
}
void on_view(device* dev, resource res, resource_usage usage, const resource_view_desc& desc, resource_view view)
{
    if (!ours(dev) || !view.handle || desc.type == resource_view_type::acceleration_structure) return;
    std::lock_guard lock(mutex);
    Descriptor d;
    if (desc.type == resource_view_type::buffer) ensure_buffer_metadata(res);
    d.resource_id = id_of(resources, res.handle);
    d.resource_handle = res.handle;
    d.valid = res.handle != 0;
    d.buffer = desc.type == resource_view_type::buffer;
    d.type = uint32_t((uint32_t(usage & resource_usage::unordered_access) != 0) ? descriptor_type::unordered_access_view : descriptor_type::shader_resource_view);
    d.format = uint32_t(desc.format);
    if (d.buffer) {
        d.offset = desc.buffer.offset;
        if (desc.format == format::unknown) {
            d.structure_stride = desc.buffer.structured.stride;
            d.bytes = uint64_t(desc.buffer.structured.count) * d.structure_stride;
        } else d.bytes = desc.buffer.size;
    }
    set_descriptor(view.handle, d);
}
bool on_update_tables(device* dev, uint32_t count, const descriptor_table_update* updates)
{
    if (!ours(dev)) return false;
    boot_hook(1, "first descriptor update entered\r\n");
    std::lock_guard lock(mutex);
    for (uint32_t u = 0; u < count; ++u) {
        const auto& update = updates[u];
        uint32_t available = 0;
        const auto cpu = descriptor_cpu(dev, update.table, update.binding, &available);
        if (!cpu || !update.descriptors) continue;
        const uint32_t stride = reinterpret_cast<ID3D12Device*>(dev->get_native())->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const uint32_t n = (std::min)({update.count, available, 16384u});
        if (n != update.count) { ++descriptor_dropped; descriptors.clear(); }
        for (uint32_t i = 0; i < n; ++i) {
            Descriptor d;
            if (update.type == descriptor_type::constant_buffer) {
                const auto& b = static_cast<const buffer_range*>(update.descriptors)[i];
                ensure_buffer_metadata(b.buffer);
                d.resource_id = id_of(resources, b.buffer.handle); d.resource_handle = b.buffer.handle;
                d.offset = b.offset; d.bytes = b.size; d.type = uint32_t(update.type);
                d.buffer = true; d.valid = b.buffer.handle != 0;
            } else if (update.type == descriptor_type::shader_resource_view || update.type == descriptor_type::unordered_access_view ||
                       update.type == descriptor_type::buffer_shader_resource_view || update.type == descriptor_type::buffer_unordered_access_view) {
                const auto source = static_cast<const resource_view*>(update.descriptors)[i].handle;
                const auto it = descriptors.find(source);
                if (it != descriptors.end()) d = it->second;
            }
            set_descriptor(cpu + uint64_t(i) * stride, d);
        }
    }
    return false;
}
bool on_copy_tables(device* dev, uint32_t count, const descriptor_table_copy* copies)
{
    if (!ours(dev)) return false;
    boot_hook(2, "first descriptor copy entered\r\n");
    std::lock_guard lock(mutex);
    for (uint32_t c = 0; c < count; ++c) {
        const auto& copy = copies[c];
        uint32_t sa = 0, da = 0;
        const auto src = descriptor_cpu(dev, copy.source_table, copy.source_binding, &sa);
        const auto dst = descriptor_cpu(dev, copy.dest_table, copy.dest_binding, &da);
        if (!dst) continue;
        const uint32_t n = (std::min)({copy.count, da, 16384u});
        if (n != copy.count) ++descriptor_dropped;
        const uint32_t stride = reinterpret_cast<ID3D12Device*>(dev->get_native())->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        std::vector<Descriptor> values(n);
        for (uint32_t i = 0; src && i < n && i < sa; ++i) {
            const auto it = descriptors.find(src + uint64_t(i) * stride);
            if (it != descriptors.end()) values[i] = it->second;
        }
        if (n != copy.count) descriptors.clear(); // missed writes must never leave old destinations looking current
        for (uint32_t i = 0; i < n; ++i) set_descriptor(dst + uint64_t(i) * stride, values[i]);
    }
    return false;
}
std::string sample_compute_tables(command_list* list) // mutex held
{
    const auto it = lists.find(list);
    if (it == lists.end()) return "[]";
    std::ostringstream s;
    s << '[';
    uint32_t emitted = 0;
    const uint32_t stride = reinterpret_cast<ID3D12Device*>(list->get_device()->get_native())->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t p = 0; p < 64 && emitted < 64; ++p) {
        const auto table = it->second.compute_tables[p];
        if (!table.handle) continue;
        uint32_t available = 0;
        const auto cpu = descriptor_cpu(list->get_device(), { table.handle }, 0, &available);
        if (!cpu) continue;
        // A sample, not a declaration that these slots are read/written by this shader.
        for (uint32_t i = 0; i < available && i < 64 && emitted < 64; ++i) {
            const auto d = descriptors.find(cpu + uint64_t(i) * stride);
            if (d == descriptors.end() || !d->second.buffer) continue;
            const auto& v = d->second;
            if (emitted++) s << ',';
            s << "{\"param\":" << p << ",\"slot_offset\":" << i << ",\"type\":" << v.type << ",\"format\":" << v.format
              << ",\"resource_id\":" << v.resource_id << ",\"offset\":\"" << hex(v.offset)
              << "\",\"bytes\":" << v.bytes << ",\"structure_stride\":" << v.structure_stride << ",\"descriptor_write\":" << v.written << '}';
        }
    }
    s << ']'; return s.str();
}

void on_destroy_device(device* dev)
{
    // A temporary D3D11/secondary device must never tear down the primary capture.
    if (owner.load() != dev) return; // no capture mutex during unrelated device creation/destruction
    std::lock_guard lock(mutex);
    if (owner.load() != dev) return;
    monitoring.store(false);
    recording.store(false);
    owner.store(nullptr);
    resources.clear(); pipelines.clear(); layouts.clear(); shaders.clear(); lists.clear(); inventory.clear(); descriptors.clear();
    shader_bytes = metadata_dropped = 0;
    metadata_bytes = 0;
    descriptor_writes = descriptor_dropped = 0;
    trace.clear();
    rt_trace.clear();
}
void on_init_resource(device* dev, const resource_desc& desc, const subresource_data*, resource_usage state, resource res)
{
    if (!ours(dev)) return;
    boot_hook(4, "first resource metadata entered\r\n");
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << "\"kind\":\"resource\",\"type\":" << uint32_t(desc.type) << ",\"heap\":" << uint32_t(desc.heap)
      << ",\"initial_state\":" << uint32_t(state) << ",\"usage\":" << uint32_t(desc.usage);
    if (desc.type == resource_type::buffer) {
        const uint64_t va = reinterpret_cast<ID3D12Resource*>(res.handle)->GetGPUVirtualAddress();
        s << ",\"va\":\"" << hex(va) << "\",\"bytes\":" << desc.buffer.size;
    } else if (desc.type == resource_type::texture_2d) {
        s << ",\"width\":" << desc.texture.width << ",\"height\":" << desc.texture.height
          << ",\"format\":" << uint32_t(desc.texture.format);
    }
    remember(resources, res.handle, s.str());
}
void on_destroy_resource(device* dev, resource res)
{
    if (!ours(dev)) return;
    std::lock_guard lock(mutex);
    add("\"event\":\"destroy_resource\",\"resource_id\":" + std::to_string(id_of(resources, res.handle)));
    const auto old = resources.find(res.handle);
    if (old != resources.end()) metadata_bytes -= old->second.json.size();
    resources.erase(res.handle);
}
void on_init_pipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe)
{
    if (!ours(dev)) return;
    boot_hook(8, "first shader metadata entered\r\n");
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << "\"kind\":\"pipeline\",\"layout_id\":" << id_of(layouts, layout.handle) << ",\"shaders\":[";
    bool comma = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto type = subobjects[i].type;
        if (type != pipeline_subobject_type::vertex_shader && type != pipeline_subobject_type::pixel_shader &&
            type != pipeline_subobject_type::compute_shader && type != pipeline_subobject_type::raygen_shader &&
            type != pipeline_subobject_type::any_hit_shader && type != pipeline_subobject_type::closest_hit_shader &&
            type != pipeline_subobject_type::miss_shader && type != pipeline_subobject_type::intersection_shader &&
            type != pipeline_subobject_type::callable_shader) continue;
        const auto* sd = static_cast<const shader_desc*>(subobjects[i].data);
        if (subobjects[i].count > 256) ++metadata_dropped;
        for (uint32_t j = 0; sd && j < subobjects[i].count && j < 256; ++j) {
            if (!sd[j].code || !sd[j].code_size) continue;
            const uint64_t hash = rt_capture::fnv1a64(sd[j].code, sd[j].code_size);
            bool stored = shaders.count(hash) != 0;
            if (!stored && shaders.size() < max_shaders && sd[j].code_size <= max_shader_bytes - shader_bytes) {
                const auto* data = static_cast<const char*>(sd[j].code);
                shaders.emplace(hash, std::vector<char>(data, data + sd[j].code_size));
                shader_bytes += sd[j].code_size;
                stored = true;
            }
            if (!stored) ++metadata_dropped;
            if (comma) s << ',';
            comma = true;
            const char* stage = type == pipeline_subobject_type::compute_shader ? "cs" :
                type == pipeline_subobject_type::vertex_shader ? "vs" :
                type == pipeline_subobject_type::pixel_shader ? "ps" : "raytracing";
            s << "{\"stage\":" << uint32_t(type) << ",\"stage_name\":\"" << stage << "\",\"fnv1a64\":\"" << hex(hash)
              << "\",\"bytes\":" << sd[j].code_size << ",\"stored\":" << (stored ? "true" : "false") << '}';
        }
    }
    s << ']';
    remember(pipelines, pipe.handle, s.str());
}
void on_destroy_pipeline(device* dev, pipeline pipe)
{
    if (!ours(dev)) return;
    std::lock_guard lock(mutex);
    add("\"event\":\"destroy_pipeline\",\"pipeline_id\":" + std::to_string(id_of(pipelines, pipe.handle)));
    const auto old = pipelines.find(pipe.handle);
    if (old != pipelines.end()) metadata_bytes -= old->second.json.size();
    pipelines.erase(pipe.handle);
}
void on_init_layout(device* dev, uint32_t count, const pipeline_layout_param* params, pipeline_layout layout)
{
    if (!ours(dev)) return;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << "\"kind\":\"layout\",\"params\":[";
    for (uint32_t p = 0; p < count && p < 64; ++p) {
        if (p) s << ',';
        const auto& param = params[p];
        s << "{\"param\":" << p << ",\"type\":" << uint32_t(param.type) << ",\"ranges\":[";
        auto range = [&](const descriptor_range& r, uint32_t i) {
            if (i) s << ',';
            s << "{\"binding\":" << r.binding << ",\"reg\":" << r.dx_register_index << ",\"space\":"
              << r.dx_register_space << ",\"count\":" << r.count << ",\"type\":" << uint32_t(r.type)
              << ",\"stages\":" << uint32_t(r.visibility) << '}';
        };
        switch (param.type) {
        case pipeline_layout_param_type::push_descriptors: range(param.push_descriptors, 0); break;
        case pipeline_layout_param_type::descriptor_table:
        case pipeline_layout_param_type::push_descriptors_with_ranges:
            for (uint32_t i = 0; i < param.descriptor_table.count && i < 128; ++i) range(param.descriptor_table.ranges[i], i);
            if (param.descriptor_table.count > 128) ++metadata_dropped;
            break;
        case pipeline_layout_param_type::descriptor_table_with_flags:
        case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
            for (uint32_t i = 0; i < param.descriptor_table_with_flags.count && i < 128; ++i) range(param.descriptor_table_with_flags.ranges[i], i);
            if (param.descriptor_table_with_flags.count > 128) ++metadata_dropped;
            break;
        default: break;
        }
        s << ']';
        if (param.type == pipeline_layout_param_type::push_constants) {
            const auto& r = param.push_constants;
            s << ",\"constants\":{\"reg\":" << r.dx_register_index << ",\"space\":" << r.dx_register_space
              << ",\"count\":" << r.count << ",\"stages\":" << uint32_t(r.visibility) << '}';
        }
        s << '}';
    }
    if (count > 64) ++metadata_dropped;
    s << ']';
    remember(layouts, layout.handle, s.str());
}
void on_destroy_layout(device* dev, pipeline_layout layout)
{
    if (!ours(dev)) return;
    std::lock_guard lock(mutex);
    add("\"event\":\"destroy_layout\",\"layout_id\":" + std::to_string(id_of(layouts, layout.handle)));
    const auto old = layouts.find(layout.handle);
    if (old != layouts.end()) metadata_bytes -= old->second.json.size();
    layouts.erase(layout.handle);
}
void on_reset(command_list* list)
{
    if (!list || !ours(list->get_device())) return;
    std::lock_guard lock(mutex);
    list_fields(list);
    auto it = lists.find(list);
    if (it != lists.end()) {
        ++it->second.generation;
        it->second.compute_tables = {};
        it->second.compute_layout = 0;
    }
    add(list_fields(list) + "\"event\":\"reset\"");
}
void on_destroy_list(command_list* list)
{
    if (!list || !ours(list->get_device())) return;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"destroy_list\"");
    lists.erase(list);
}
void on_bind_pipeline(command_list* list, pipeline_stage stages, pipeline pipe)
{
    if (!active(list)) return;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"bind_pipeline\",\"stages\":" + std::to_string(uint32_t(stages)) +
        ",\"pipeline_id\":" + std::to_string(id_of(pipelines, pipe.handle)) + ",\"pipeline_handle\":\"" + hex(pipe.handle) + "\"");
}
void on_tables(command_list* list, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table* tables)
{
    if (!active(list)) return;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"tables\",\"stages\":" << uint32_t(stages) << ",\"layout_id\":"
      << id_of(layouts, layout.handle) << ",\"compute\":" << (compute_stage(stages) ? "true" : "false")
      << ",\"first\":" << first << ",\"tables\":[";
    for (uint32_t i = 0; i < count && i < 64; ++i) { if (i) s << ','; s << '"' << hex(tables[i].handle) << '"'; }
    s << ']';
    const auto lit = lists.find(list);
    if (compute_stage(stages) && lit != lists.end()) {
        auto& state = lit->second;
        if (state.compute_layout != layout.handle) state.compute_tables = {};
        state.compute_layout = layout.handle;
        for (uint32_t i = 0; i < count && first < 64 && i < 64 - first; ++i)
            state.compute_tables[first + i].handle = tables[i].handle;
    }
    add(s.str());
}
void on_constants(command_list* list, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count, const void* values)
{
    if (!active(list) || !values) return;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"constants\",\"stages\":" << uint32_t(stages) << ",\"layout_id\":"
      << id_of(layouts, layout.handle) << ",\"param\":" << param << ",\"first\":" << first << ",\"values\":[";
    const auto lit = lists.find(list);
    if (compute_stage(stages) && lit != lists.end()) {
        auto& state = lit->second;
        if (state.compute_layout != layout.handle) state.compute_tables = {};
        state.compute_layout = layout.handle;
        if (param < 64) state.compute_tables[param] = {};
    }
    for (uint32_t i = 0; i < count && i < 64; ++i) { if (i) s << ','; s << static_cast<const uint32_t*>(values)[i]; }
    s << ']';
    add(s.str());
}
void on_push(command_list* list, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& u)
{
    if (!active(list) || !u.descriptors) return;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"root_descriptors\",\"stages\":" << uint32_t(stages) << ",\"layout_id\":"
      << id_of(layouts, layout.handle) << ",\"param\":" << param << ",\"type\":" << uint32_t(u.type)
      << ",\"compute\":" << (compute_stage(stages) ? "true" : "false") << ",\"values\":[";
    const auto lit = lists.find(list);
    if (compute_stage(stages) && lit != lists.end()) {
        auto& state = lit->second;
        if (state.compute_layout != layout.handle) state.compute_tables = {};
        state.compute_layout = layout.handle;
        if (param < 64) state.compute_tables[param] = {};
    }
    for (uint32_t i = 0; i < u.count && i < 64; ++i) {
        if (i) s << ',';
        if (u.type == descriptor_type::constant_buffer) {
            const auto& b = static_cast<const buffer_range*>(u.descriptors)[i];
            s << address(b.buffer, b.offset);
        } else if (u.type == descriptor_type::buffer_shader_resource_view ||
                   u.type == descriptor_type::buffer_unordered_access_view || u.type == descriptor_type::acceleration_structure) {
            // These are raw GPU addresses in ReShade's native DX12 root hooks, NOT ID3D12Resource pointers.
            s << address({}, static_cast<const uint64_t*>(u.descriptors)[i]);
        } else if (u.type == descriptor_type::shader_resource_view || u.type == descriptor_type::unordered_access_view) {
            s << "{\"view\":\"" << hex(static_cast<const resource_view*>(u.descriptors)[i].handle) << "\"}";
        } else s << "null";
    }
    s << ']';
    add(s.str());
}
void on_vb(command_list* list, uint32_t first, uint32_t count, const resource* buffers, const uint64_t* offsets, const uint32_t* strides)
{
    if (!active(list)) return;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"vertex_buffers\",\"first\":" << first << ",\"buffers\":[";
    for (uint32_t i = 0; i < count && i < 32; ++i) {
        if (i) s << ',';
        s << "{\"address\":" << address(buffers[i], offsets ? offsets[i] : 0) << ",\"stride\":" << (strides ? strides[i] : 0) << '}';
    }
    s << ']'; add(s.str());
}
void on_ib(command_list* list, resource buffer, uint64_t offset, uint32_t size)
{
    if (!active(list)) return;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"index_buffer\",\"address\":" + address(buffer, offset) + ",\"index_bytes\":" + std::to_string(size));
}
bool on_draw(command_list* list, uint32_t count, uint32_t instances, uint32_t first, int32_t base, uint32_t first_instance)
{
    if (!active(list)) return false;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"draw_indexed\",\"count\":" + std::to_string(count) +
        ",\"instances\":" + std::to_string(instances) + ",\"first_index\":" + std::to_string(first) +
        ",\"base_vertex\":" + std::to_string(base) + ",\"first_instance\":" + std::to_string(first_instance));
    return false;
}
bool on_dispatch(command_list* list, uint32_t x, uint32_t y, uint32_t z)
{
    if (!active(list)) return false;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"dispatch\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"z\":" + std::to_string(z) +
        ",\"buffer_table_samples\":" + sample_compute_tables(list));
    return false;
}
bool on_build(command_list* list, acceleration_structure_type type, acceleration_structure_build_flags flags,
    uint32_t count, const acceleration_structure_build_input* inputs, resource scratch, uint64_t scratch_offset,
    resource_view source, resource_view dest, acceleration_structure_build_mode mode)
{
    if (!active(list, true)) return false;
    boot_hook(16, "first acceleration structure capture entered\r\n");
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"as_build\",\"type\":" << uint32_t(type) << ",\"flags\":" << uint32_t(flags)
      << ",\"mode\":" << uint32_t(mode) << ",\"source_va\":\"" << hex(source.handle) << "\",\"dest_va\":\"" << hex(dest.handle)
      << "\",\"scratch\":" << address(scratch, scratch_offset) << ",\"input_count\":" << count << ",\"inputs\":[";
    for (uint32_t i = 0; inputs && i < count && i < 128; ++i) {
        if (i) s << ',';
        const auto& g = inputs[i];
        s << "{\"geometry\":" << i << ",\"type\":" << uint32_t(g.type) << ",\"flags\":" << uint32_t(g.flags);
        if (g.type == acceleration_structure_build_input_type::triangles) {
            const auto& t = g.triangles;
            s << ",\"vertices\":" << address(t.vertex_buffer, t.vertex_offset) << ",\"vertex_count\":" << t.vertex_count
              << ",\"vertex_stride\":" << t.vertex_stride << ",\"vertex_format\":" << uint32_t(t.vertex_format)
              << ",\"indices\":" << address(t.index_buffer, t.index_offset) << ",\"index_count\":" << t.index_count
              << ",\"index_format\":" << uint32_t(t.index_format) << ",\"transform\":" << address(t.transform_buffer, t.transform_offset);
        } else if (g.type == acceleration_structure_build_input_type::instances) {
            const auto& t = g.instances;
            s << ",\"instances\":" << address(t.buffer, t.offset) << ",\"count\":" << t.count
              << ",\"array_of_pointers\":" << (t.array_of_pointers ? "true" : "false");
        } else if (g.type == acceleration_structure_build_input_type::aabbs) {
            const auto& t = g.aabbs;
            s << ",\"aabbs\":" << address(t.buffer, t.offset) << ",\"count\":" << t.count << ",\"stride\":" << t.stride;
        }
        s << '}';
    }
    s << "],\"inputs_truncated\":" << (count > 128 ? "true" : "false");
    add(s.str(), true);
    return false; // Always let the original build (and post-build queries) execute unchanged.
}
bool on_rays(command_list* list, resource rg, uint64_t rgo, uint64_t rgs, resource miss, uint64_t mo, uint64_t ms, uint64_t mst,
    resource hit, uint64_t ho, uint64_t hs, uint64_t hst, resource call, uint64_t co, uint64_t cs, uint64_t cst,
    uint32_t x, uint32_t y, uint32_t z)
{
    if (!active(list, true)) return false;
    std::lock_guard lock(mutex);
    std::ostringstream s;
    s << list_fields(list) << "\"event\":\"dispatch_rays\",\"x\":" << x << ",\"y\":" << y << ",\"z\":" << z << ",\"tables\":[";
    const resource buffers[] = { rg, miss, hit, call };
    const uint64_t offsets[] = { rgo, mo, ho, co }, sizes[] = { rgs, ms, hs, cs }, strides[] = { 0, mst, hst, cst };
    for (int i = 0; i < 4; ++i) {
        if (i) s << ',';
        s << "{\"address\":" << address(buffers[i], offsets[i]) << ",\"bytes\":" << sizes[i] << ",\"stride\":" << strides[i] << '}';
    }
    s << ']'; add(s.str(), true); return false;
}
void on_execute(command_queue* queue, command_list* list)
{
    if (!active(list) || !ours(queue->get_device())) return;
    std::lock_guard lock(mutex);
    add(list_fields(list) + "\"event\":\"execute\",\"queue\":\"" + hex(queue->get_native()) + "\"");
}

bool write_capture() // render thread, after recording stopped; no other callback writes trace/inventory
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[128];
    swprintf_s(name, 128, L"rt-capture-%04u%02u%02u-%02u%02u%02u-%03u-%lu", unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
        unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond), unsigned(now.wMilliseconds), GetCurrentProcessId());
    const auto dir = output_base / name;
    std::error_code ec;
    fs::create_directories(dir / "shaders", ec);
    if (ec) return false;
    std::ofstream out(dir / "capture.json", std::ios::binary);
    if (!out) return false;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    out << "{\"schema\":\"wwmi-rt-capture-v1\",\"probe_version\":\"0.1.2\",\"startup_mode\":\"dormant_until_F7\",\"base_commit\":\"845cec6\",\"full_rt_mesh_support\":false,"
        << "\"qpc_frequency\":" << frequency.QuadPart << ",\"dropped_events\":" << trace.dropped
        << ",\"dropped_metadata\":" << metadata_dropped
        << ",\"dropped_rt_events\":" << rt_trace.dropped
        << ",\"general_trace_truncated\":" << (trace_full.load() ? "true" : "false")
        << ",\"dropped_descriptors\":" << descriptor_dropped
        << ",\"limitations\":[\"CPU callback order is not GPU execution order\",\"No GPU buffer contents or SBT local records\","
        << "\"Bindings before capture starts are unknown\",\"Pre-F7 resources are queried lazily when a valid resource handle is bound; pre-F7 shader bytecode remains unknown\",\"Descriptor tables are sampled: first 64 slots/table, at most 64 buffer slots/dispatch\","
        << "\"Descriptor heap destruction is not observable; sampled associations are candidates\"],\"inventory\":[";
    for (size_t i = 0; i < inventory.size(); ++i) { if (i) out << ','; out << inventory[i]; }
    out << "],\"events\":[";
    bool comma = false;
    rt_capture::visit_sequenced(trace, rt_trace, [&](const std::string& row) {
        if (comma) out << ',';
        comma = true;
        out << '\n' << row;
    });
    out << "]}\n";
    out.close();
    if (!out) return false;
    for (const auto& [hash, bytes] : shaders) {
        std::ofstream shader(dir / "shaders" / (hex(hash) + ".dxil"), std::ios::binary);
        shader.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        shader.close();
        if (!shader) return false;
    }
    return true;
}
void on_present(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    if (probe_failed.load() || !queue) return;
    if (!present_seen) { boot_line("first present callback entered\r\n"); present_seen = true; }
    auto* dev = queue->get_device();
    if (!dev) return;
    if (!owner.load()) {
        boot_line("checking API after first present\r\n");
        if (dev->get_api() != device_api::d3d12) return;
        device* expected = nullptr;
        if (owner.compare_exchange_strong(expected, dev)) {
            boot_line("DX12 present reached; metadata hooks remain dormant until F7\r\n");
            wchar_t exe[32768];
            const DWORD size = GetModuleFileNameW(nullptr, exe, 32768);
            if (!size || size >= 32768) { probe_failed.store(true); return; }
            output_base = fs::path(exe).parent_path() / "wwmi_rt_captures";
        }
    }
    if (owner.load() != dev) return;
    DWORD pid = 0;
    if (const auto fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    const bool f7 = focused && (GetAsyncKeyState(VK_F7) & 0x8000);
    const bool f8 = focused && (GetAsyncKeyState(VK_F8) & 0x8000), f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
    const bool enable = f7 && !last_f7, capture = f8 && !last_f8, stop = f9 && !last_f9;
    last_f7 = f7; last_f8 = f8; last_f9 = f9;
    if (!enable && !capture && !stop && !recording.load()) return; // no capture lock on the dormant frame path
    std::lock_guard lock(mutex);
    ++frame;
    if (stop) {
        monitoring.store(false);
        recording.store(false);
        trace.clear(); rt_trace.clear(); inventory.clear(); trace_full.store(false); trace_rt_full.store(false);
        for (auto& [list, state] : lists) { (void)list; state.compute_tables = {}; state.compute_layout = 0; }
        reshade::log::message(reshade::log::level::info, "WWMI RT Capture: cancelled.");
        boot_line("F9: all capture hooks dormant\r\n");
    } else if (enable && !monitoring.load()) {
        boot_line("F7: enabling metadata hooks after game startup\r\n");
        // Destruction/reuse while dormant was deliberately unobserved. Never reuse
        // those associations when F7 starts a fresh monitoring interval.
        resources.clear(); pipelines.clear(); layouts.clear(); shaders.clear(); lists.clear(); inventory.clear(); descriptors.clear();
        shader_bytes = metadata_dropped = 0;
        metadata_bytes = 0;
        descriptor_writes = descriptor_dropped = 0;
        monitoring.store(true);
        reshade::log::message(reshade::log::level::info, "WWMI RT Capture 0.1.2: metadata enabled. Change character, then F8 captures six frames. F9 disables monitoring.");
    } else if (capture && !monitoring.load()) {
        boot_line("F8 ignored: press F7 to enable monitoring first\r\n");
    } else if (capture && !recording.load()) {
        boot_line("F8: starting six-frame capture\r\n");
        trace.clear(); rt_trace.clear(); inventory.clear(); trace_full.store(false); trace_rt_full.store(false);
        for (auto& [list, state] : lists) { (void)list; state.compute_tables = {}; state.compute_layout = 0; }
        for (const auto* entries : { &resources, &pipelines, &layouts })
            for (const auto& [handle, metadata] : *entries) { (void)handle; inventory.push_back(metadata.json); }
        end_frame = frame + 6;
        recording.store(true);
        reshade::log::message(reshade::log::level::info, "WWMI RT Capture: recording six frames.");
    } else if (recording.load() && (frame >= end_frame || trace_rt_full.load())) {
        recording.store(false);
        // One bounded file write after capture. No disk I/O occurs in draw/dispatch/build hooks.
        const bool ok = write_capture();
        boot_line(ok ? "capture saved\r\n" : "capture save failed\r\n");
        reshade::log::message(ok ? reshade::log::level::info : reshade::log::level::error,
            ok ? "WWMI RT Capture: saved to wwmi_rt_captures next to the game exe."
               : "WWMI RT Capture: save FAILED (check writable game directory and free disk space).");
    }
}
} // namespace

// A diagnostic callback must never cancel game work or propagate an allocation/IO exception.
template <auto Fn, bool Always = false> struct SafeCallback;
template <class R, class... Args, R (*Fn)(Args...), bool Always> struct SafeCallback<Fn, Always> {
    static R call(Args... args) noexcept
    {
        try { return rt_capture::invoke_when_enabled<Fn>(Always || monitoring.load(), args...); }
        catch (...) {
            monitoring.store(false);
            recording.store(false);
            boot_line("C++ exception: capture disabled; original game calls preserved\r\n");
            if (!probe_failed.exchange(true))
                reshade::log::message(reshade::log::level::error, "WWMI RT Capture: probe stopped after an exception. Game calls are passed through.");
            if constexpr (!std::is_void_v<R>) return R{};
        }
    }
};

extern "C" {
__declspec(dllexport) const char* NAME = "WWMI RT Capture";
__declspec(dllexport) const char* DESCRIPTION = "RT Capture 0.1.2: dormant startup; dedicated RT budget and lazy resource metadata. F7 enables, F8 captures, F9 disables.";
}
extern "C" __declspec(dllexport) bool AddonInit(HMODULE, HMODULE)
{
    try {
        open_boot_log();
        boot_line("WWMI RT Capture 0.1.2 AddonInit entered outside DllMain\r\n");
#define EVENT(e, f) reshade::register_event<reshade::addon_event::e>(SafeCallback<f>::call)
        reshade::register_event<reshade::addon_event::destroy_device>(SafeCallback<on_destroy_device, true>::call);
        EVENT(init_resource, on_init_resource); EVENT(destroy_resource, on_destroy_resource);
        EVENT(init_resource_view, on_view); EVENT(update_descriptor_tables, on_update_tables); EVENT(copy_descriptor_tables, on_copy_tables);
        EVENT(init_pipeline, on_init_pipeline); EVENT(destroy_pipeline, on_destroy_pipeline);
        EVENT(init_pipeline_layout, on_init_layout); EVENT(destroy_pipeline_layout, on_destroy_layout);
        EVENT(reset_command_list, on_reset); EVENT(destroy_command_list, on_destroy_list);
        EVENT(bind_pipeline, on_bind_pipeline); EVENT(bind_descriptor_tables, on_tables);
        EVENT(push_constants, on_constants); EVENT(push_descriptors, on_push);
        EVENT(bind_vertex_buffers, on_vb); EVENT(bind_index_buffer, on_ib);
        EVENT(draw_indexed, on_draw); EVENT(dispatch, on_dispatch);
        EVENT(build_acceleration_structure, on_build); EVENT(dispatch_rays, on_rays);
        EVENT(execute_command_list, on_execute);
        reshade::register_event<reshade::addon_event::present>(SafeCallback<on_present, true>::call);
#undef EVENT
        boot_line("callbacks registered; metadata, draw and RT hooks are dormant\r\n");
        return true;
    } catch (...) {
        monitoring.store(false);
        probe_failed.store(true);
        boot_line("AddonInit failed; monitoring disabled\r\n");
        return false;
    }
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE, HMODULE)
{
    monitoring.store(false);
    recording.store(false);
    boot_line("AddonUninit\r\n");
    const auto file = boot_log.exchange(INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    // No paths, files, capture locks, native API queries, or callback setup under the loader lock.
    if (reason == DLL_PROCESS_ATTACH && !reshade::register_addon(module)) return FALSE;
    if (reason == DLL_PROCESS_DETACH) reshade::unregister_addon(module);
    return TRUE;
}
