#pragma once
#include <cstdint>
#include <reshade.hpp>
#include <d3d12.h>
#include "../mods/target_class.h"

void on_bind_pipeline(reshade::api::command_list* cmd_list, reshade::api::pipeline_stage type,
    reshade::api::pipeline pipeline);
void on_bind_vertex_buffers(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
    const reshade::api::resource* buffers, const uint64_t* offsets,
    const uint32_t* strides);
void on_bind_index_buffer(reshade::api::command_list* cmd_list, reshade::api::resource buffer,
    uint64_t offset, uint32_t index_size);
void on_bind_render_targets(reshade::api::command_list* cmd_list, uint32_t count,
    const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv);
void on_bind_viewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
    const reshade::api::viewport* vps);
void on_bind_scissor_rects(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
    const reshade::api::rect* rects);
void on_bind_pipeline_states(reshade::api::command_list* cmd_list, uint32_t count,
    const reshade::api::dynamic_state* states, const uint32_t* values);
void on_push_descriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout, uint32_t layout_param,
    const reshade::api::descriptor_table_update& update);
void on_bind_descriptor_tables(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout, uint32_t first, uint32_t count,
    const reshade::api::descriptor_table* tables);
// After ID3D12GraphicsCommandList::Reset: the list carries nothing, and neither does our record of it.
void                  on_reset_command_list(reshade::api::command_list* cmd_list);
void                  on_close_command_list(reshade::api::command_list* cmd_list);
ID3D12DescriptorHeap* GameHeap(reshade::api::command_list* cmd_list, int type);

// Where a ResolveBound stopped (step 0 = resolved) and what it saw on the way; filled only when asked for.
struct BindTrace {
    int             step = 0;
    uint64_t        layout = 0;
    uint8_t         param = 0xFF;
    bool            table = false;
    uint64_t        heap = 0;
    uint32_t        slot = 0;
    ID3D12Resource* res = nullptr;
    uint32_t        size = 0;
    uint32_t        offset = 0; // the register's offset in its table
    ID3D12Resource* table_res[8] = {};
    uint32_t        table_size[8] = {};
    uint32_t        table_wrote[8] = {};
    uint8_t         table_src[8] = {};
}; // the table's first 8 slots
bool ResolveBound(reshade::api::command_list* cmd_list, char stage, TargetClass kind, uint32_t reg,
    ID3D12Resource** out_res, uint64_t* out_va, uint32_t* out_size, BindTrace* trace = nullptr);

void on_push_constants(reshade::api::command_list*, reshade::api::shader_stage, reshade::api::pipeline_layout,
    uint32_t, uint32_t, uint32_t, const uint32_t*);
