#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <reshade.hpp>
#include <d3d12.h>
#include "descriptor_shadow.h" // ResInfo -- the value type of the maps below

// Buffer GPU address -> resource, and back. Guarded by g_mem_mutex.
extern std::mutex                             g_mem_mutex;
extern std::unordered_map<uint64_t, ResInfo>  g_va_to_res;
extern std::unordered_map<uint64_t, uint64_t> g_res_to_va;

bool BufferLive(const void* res); // a buffer with a GPU address (g_res_to_va)

void     on_barrier(reshade::api::command_list* cmd_list, uint32_t count,
    const reshade::api::resource* res, const reshade::api::resource_usage* old_s,
    const reshade::api::resource_usage* new_s);
uint32_t ResourceStateOf(void* d3d12_resource);
bool     on_create_resource(reshade::api::device* dev, reshade::api::resource_desc& desc,
    reshade::api::subresource_data*, reshade::api::resource_usage);
void     on_init_resource(reshade::api::device* device, const reshade::api::resource_desc& desc,
    const reshade::api::subresource_data* initial_data,
    reshade::api::resource_usage initial_state, reshade::api::resource res);
void     on_destroy_resource(reshade::api::device* device, reshade::api::resource res);
bool     on_copy_buffer_region(reshade::api::command_list*, reshade::api::resource source, uint64_t source_offset,
    reshade::api::resource dest, uint64_t dest_offset, uint64_t size);
void     on_init_resource_view(reshade::api::device* device, reshade::api::resource res,
    reshade::api::resource_usage            usage_type,
    const reshade::api::resource_view_desc& desc,
    reshade::api::resource_view             view);
bool     on_update_descriptor_tables(reshade::api::device* device, uint32_t count,
    const reshade::api::descriptor_table_update* updates);
bool     on_copy_descriptor_tables(reshade::api::device* device, uint32_t count,
    const reshade::api::descriptor_table_copy* copies);

// Re-run the placement swap on the slot resH was last placed in. True = the slot now holds a mod SRV.
bool RestampResource(uint64_t resH);
void ResetDescriptorTracking();
// Put the game's own descriptor back in resH's recorded slot. True = restored. log=false for bulk callers.
bool UnstampResource(uint64_t resH, bool log = true);
// The same for one heap slot, whatever resource it holds. True = restored.
bool                                      UnstampSlot(uint64_t heap_h, uint32_t offset, bool log = false);
void                                      BackupBeforeStamp(uint64_t heap_h, uint32_t offset, uint64_t mod_srv);
std::vector<std::pair<HeapKey, uint64_t>> StampedCpuOnlySlots();
// The tracked heap slot a CPU descriptor handle points into. False = no heap we track holds it.
bool TrackedSlotOfCpu(uint64_t cpu, HeapKey* out);
struct UnstampTally {
    unsigned recorded = 0;
    unsigned cpu_only = 0;
    size_t   cpu_only_stamped = 0;
};
UnstampTally UnstampAll(void (*hold)(uint64_t res));
void         SetAddonDisabled(bool dis);

// A mod texture replaced by another: what the slots stamped with it get instead.
struct StampMove {
    ID3D12Resource*             to;
    D3D12_CPU_DESCRIPTOR_HANDLE to_srv;
    D3D12_CPU_DESCRIPTOR_HANDLE from_srv;
};
struct StampTally {
    unsigned moved_cpu_only = 0;
    unsigned moved_visible = 0;
    unsigned restored = 0; // the game's descriptor back
    unsigned nulled = 0;   // a null SRV: nothing of the game's to put back
    unsigned raced = 0;    // the game wrote the slot between the look and the write
    unsigned backups = 0;  // unstamp backups now naming the new SRV
};
// Every slot stamped with a key of `moves` holds its `to` instead; the unstamp backups follow.
StampTally MoveStamps(const std::unordered_map<ID3D12Resource*, StampMove>& moves);
// Mod textures whose last reference dropped, before the GPU fence frees them: no slot may keep one. Called after
// they left the texture store, so a stamp after the walk sees that (StampedStoreTexture).
StampTally UnstampDying(const std::unordered_set<ID3D12Resource*>& dying);
// Drawing threads, right after stamping a texture taken from the store into a slot: if the store let go of it
// meanwhile, the slot is given back as UnstampDying would have.
void StampedStoreTexture(uint64_t heap, uint32_t offset, ID3D12Resource* res);
// A null view of a DXIL resource kind (0 = 2D), in a CPU-only heap of its own.
D3D12_CPU_DESCRIPTOR_HANDLE NullSrvDescriptor(ID3D12Device* dev, uint8_t kind);
