#pragma once
#include "../addon_state.h"
#include "../instrumentation/diag.h" // WWMI_DEBUG_FN
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// ---- bone CB constants --------------------------------------------------------
constexpr uint32_t kBoneStride = 48; // float3x4
constexpr uint32_t kMaxBones = 256;
constexpr uint32_t kBoneCBBytes = kMaxBones * kBoneStride; // 12288

struct MergedComp {
    int              mod_id = 0; // which geometry mod (index into g_geomods) owns this component
    int              comp_idx = -1;
    uint32_t         first_index = 0; // game match_first_index
    uint32_t         match_count = 0; // game match_index_count -- disambiguates (first_index alone collides)
    int              remap_size = 0;  // # of local bones in this component's RemappedSkeleton
    bool             hide = false;
    bool             vanilla = false;
    bool             freeze = false;
    std::vector<int> with;      // other mods' components on this range of the same mesh: their sections run on its draw too
    std::vector<int> run_order; // this slot and `with`, as DX11 runs them: match_priority, then section name
};
inline std::vector<MergedComp>                        g_merged_comps;
inline std::unordered_map<uint64_t, int>              g_first_to_comp;
inline std::unordered_map<uint64_t, std::vector<int>> g_first_to_shared;
inline uint64_t                                       FirstCountKey(uint32_t first_index, uint32_t match_count)
{
    return ((uint64_t)first_index << 32) | match_count;
}
inline bool g_merged_loaded = false;
inline bool g_merged_enable = true;
// Live A/B toggles for the uncertain WWMI-faithfulness knobs (flip in-game, no rebuild):
inline bool g_restore_vbib = true; // restore VB0-4 + IB after our draws (3dmigoto auto-restore)
inline bool g_restore_cb = true;   // restore bone CB root 13/14 after our draws
inline bool g_restore_nonpso = true;
inline bool g_hide_unclaimed_char = false;
inline bool g_skip_no_bone13 = false;

inline std::atomic<uint32_t> g_init_res_all{ 0 };  // init_resource, every type
inline std::atomic<uint32_t> g_init_res_buf{ 0 };  // ... of which buffers WITH a GPU VA (what the map takes)
inline std::atomic<uint32_t> g_init_view_all{ 0 }; // init_resource_view, every type

inline ComPtr<ID3D12Resource> g_identity_cb; // 256 identity float3x4, shared (bind-pose freeze)
inline uint64_t               g_identity_cb_va = 0;

// ---- functions ----------------------------------------------------------------
void     FinalizeMergedRemap();
bool     LooksLikeBone(const float* m);
uint32_t BoneRootMask(reshade::api::command_list* cmd_list);
uint32_t SnapshotGameCB(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t max_bytes,
    bool map_on_demand = true);
bool     ReadRootCbv(reshade::api::command_list* cmd_list, uint32_t param, void* dst, uint32_t n);
void     ForgetGameCB(ID3D12Resource* res);

WWMI_DEBUG_FN void SkelDumpNoteDrawCbs(ID3D12Resource* cb4_res, uint64_t cb4_va, int cb4_root,
    ID3D12Resource* cb3_res, uint64_t cb3_va, int cb3_root) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void SkeletonChainDumpTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void SkeletonDumpTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q) WWMI_DEBUG_BODY();
