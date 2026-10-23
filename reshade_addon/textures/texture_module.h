#pragma once
#include <bitset>
#include <reshade.hpp>
#include <d3d12.h>
#include <mutex>

inline constexpr uint64_t kTexHashFloorPx = 256ull * 256ull;

#include <shared_mutex>
#include <set>
#include <deque>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <string>
#include <thread>
#include <condition_variable>
#include "../../src/dxgi_bc.h"
#include "tex_look.h"

inline uint32_t TexTypelessBC(uint32_t f)
{
    if (f >= 70 && f <= 72) return 70;
    if (f >= 73 && f <= 75) return 73;
    if (f >= 76 && f <= 78) return 76;
    if (f >= 79 && f <= 81) return 79;
    if (f >= 82 && f <= 84) return 82;
    if (f >= 94 && f <= 96) return 94;
    if (f >= 97 && f <= 99) return 97;
    if (f == 87 || f == 91) return 90;
    if (f == 88 || f == 93) return 92;
    if (f == 28 || f == 29) return 27;
    return f;
}

// ---- shared texture state (inline => single instance across all TUs) ----------
struct TexInfo {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t mips;
    uint32_t arr;
    bool     hashable = false;
}; // no RT/DS/UAV usage and mip0 at or above kTexHashFloorPx: an upload can name it
inline std::shared_mutex                      g_tex_mutex;
inline std::unordered_map<uint64_t, TexInfo>  g_tex_dims;    // resource handle -> dims
inline std::unordered_map<uint64_t, uint32_t> g_res_texhash; // ID3D12Resource* -> 3dmigoto hash
// Stamps per rule hash, for the audit below (GlobalTexSwap increments; a stamp is rare, ~3 a frame).
inline std::mutex                             g_swap_by_hash_mtx;
inline std::unordered_map<uint32_t, uint32_t> g_swap_by_hash;
inline std::unordered_set<uint64_t>           g_modtex_res;
inline std::unordered_set<uint64_t>           g_res_inherited;

inline bool TexHashKnown(uint64_t res)
{
    std::shared_lock l(g_tex_mutex);
    return g_res_texhash.count(res) != 0;
}
// Bumped under g_tex_mutex by every write to g_res_texhash: a thread's remembered hash holds while it is unchanged.
inline std::atomic<uint32_t> g_res_texhash_gen{ 1 };
struct TexHashMemo {
    uint64_t res;
    uint32_t hash;
    uint32_t gen;
};
inline TexHashMemo& TexHashMemoAt(uint64_t res)
{
    thread_local TexHashMemo t_memo[1024] = {};
    return t_memo[((res >> 4) ^ (res >> 14)) & 1023];
}
inline uint32_t TexHashOf(uint64_t res) // 0 = never hashed
{
    const uint32_t gen = g_res_texhash_gen.load(std::memory_order_acquire);
    TexHashMemo&   m = TexHashMemoAt(res);
    if (m.res == res && m.gen == gen) return m.hash;
    uint32_t h = 0;
    {
        std::shared_lock l(g_tex_mutex);
        auto             it = g_res_texhash.find(res);
        if (it != g_res_texhash.end()) h = it->second;
    }
    m = TexHashMemo{ res, h, gen };
    return h;
}
inline bool TexDimsOf(uint64_t res, TexInfo* out)
{
    std::shared_lock l(g_tex_mutex);
    auto             it = g_tex_dims.find(res);
    if (it == g_tex_dims.end()) return false;
    *out = it->second;
    return true;
}

inline bool                  g_disable_tex = false;
inline bool                  g_global_tex = true; // PIPELINE 1: placement-time swap (copy/update desc hooks)
inline bool                  g_tex_inplace_stamp = true;
inline bool                  g_tex_read_slots = true;
inline std::atomic<uint32_t> g_tex_inject_count{ 0 }; // per-draw swaps (claimed draws)
inline std::atomic<uint32_t> g_global_tex_count{ 0 }; // descriptor-placement swaps
inline std::atomic<uint32_t> g_global_tex_miss{ 0 };  // placements with a rule but an EMPTY selection (GTEXMISS)

inline std::atomic<bool> g_texovr_ready{ false };
struct TexRules {
    std::unordered_map<uint32_t, int> hash_to_rule; // tex hash -> occurrence index of its [TextureOverrideTexture*]
    std::set<uint32_t>                targets;      // vanilla hashes the mods replace (sections and t=<hash> filenames)
};
inline std::atomic<const TexRules*> g_tex_rules{ nullptr };
inline const TexRules&              TexRulesNow()
{
    static const TexRules none;
    const TexRules*       r = g_tex_rules.load(std::memory_order_acquire);
    return r ? *r : none;
}
void TexRulesPublish();
bool TexIsTarget(uint32_t hash, void* unused);

inline std::set<uint64_t> g_match_profiles;
inline std::mutex         g_match_mtx;

uint32_t TexIdentityHash(uint32_t data_hash, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr, uint32_t dxgi_fmt);
void     TexMipIdentityFromUpload(ID3D12Resource* dest, uint32_t sub, const uint8_t* src);
void     TexMipIdentityForget(uint64_t res); // on destroy: whatever was held for it

int      TexSelectIdx(uint32_t hash, unsigned mip);
// How many mips down the game draws `hash` against the mod target `target` (A20): 0 for the target itself, else its proven low's mip; unknown otherwise.
unsigned TexDrawnMip(uint32_t hash, uint32_t target);
uint32_t TexRuleHashFor(uint32_t hash);
// The mod target `hash` is a lower mip or (A13) a look-alike of, from the texture cache, else 0.
uint32_t TexLodTargetOf(uint32_t hash);
uint32_t ResolveBoundTextures(uint64_t heap, uint32_t base, uint32_t num, ID3D12Resource** res, uint32_t* hash,
    uint32_t cap, const std::bitset<512>* want = nullptr);
// Resolve a matched texture hash to a mod SRV via its rule's conditional assigns (last match wins).
D3D12_CPU_DESCRIPTOR_HANDLE TexReplacement(uint32_t hash);
int                         TexSelectIdxNs(uint32_t hash, const wchar_t* ns, unsigned mip);
// Resolve a fully-qualified mod resource name ("ns\resourcename") -> its loaded mod-texture SRV ({0} if unknown).
D3D12_CPU_DESCRIPTOR_HANDLE WclModSrvByResourceFq(const std::wstring& res_fq);
D3D12_CPU_DESCRIPTOR_HANDLE RfxMapSrv(const std::wstring& res_fq, const wchar_t* ns);
// At descriptor placement: if res's hash matches a rule, overwrite the heap slot with the mod SRV. True = swapped.
bool        GlobalTexSwap(ID3D12Device* dev, uint64_t heap_handle, uint32_t slot, ID3D12Resource* res);
const char* GlobalTexSwapDecline();
void        TexPlacementReport();
void        TexSnapshotSelections(const wchar_t* ns);
void        TexSnapshotsMove(const std::unordered_map<int, int>& index_to); // store indices of replaced textures
void        TexRetryMissed(const char* why, bool quiet_if_none = false);
// Holds a resource for the next TexRetryMissed: one of its slots was given back to the game.
void TexHoldForRetry(uint64_t res);
// A mod switched itself off: its stamped slots in CPU-only heaps get the game's descriptor back and are held.
void                         TexUnstampSwitchedOff(const wchar_t* ns);
void                         TexSwapsSwitched(bool were_live);
void                         TexLookAlikesSwitched(bool on); // A13 changed: slots of look-alike textures swapped or given back
void                         TexWatchSwitchedOff(const wchar_t* ns);
void                         TexWatchSwitchedOn();
bool                         TexOffWatching();
uint32_t                     TexOffRuleOf(ID3D12Resource* res); // the watched mod's rule this game texture swaps under, else 0
inline std::atomic<unsigned> g_off_leaks{ 0 };                  // slot reads by draws not handed back
inline std::atomic<unsigned> g_off_kept{ 0 };                   // hand-backs that left the mod texture in place
// Hash an uploaded texture's mip0 (3dmigoto identity) -> g_res_texhash[dest]; updates rolling cache.
void TexHashFromUpload(ID3D12Resource* dest, const uint8_t* src, size_t src_row_pitch);
void TexTargetAudit();
class TexLodWorker {
public:
    void Queue(ID3D12Resource* dest, uint32_t crc, uint32_t w, uint32_t h, uint32_t fmt, bool is_target,
        const std::vector<uint8_t>& bc);
    void Stop();
    ~TexLodWorker()
    {
        if (thread_.joinable()) thread_.detach();
    }

private:
    struct Job {
        ID3D12Resource* dest;
        uint32_t        crc;
        bool            is_target;
        TexLookCut      cut;
    };
    void                    Run();
    std::deque<Job>         queue_; // under mtx_
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stop_ = false; // under mtx_
    std::thread             thread_;       // started and joined under mtx_
};
extern TexLodWorker g_tex_lod_worker;

int  RfxTexClass(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr);
bool RfxSetTexturesDecision(const int cls[3], bool have_d, bool have_n, bool have_l, bool have_m,
    uint8_t& d_mask, uint8_t& n_mask, uint8_t& l_mask, uint8_t& m_mask,
    char flags_out[8]);

// ---- reshade hooks owned by this module (registered in AddonInit) -------------
bool on_update_texture_region(reshade::api::device*, const reshade::api::subresource_data& data,
    reshade::api::resource dest, uint32_t dest_subresource,
    const reshade::api::subresource_box*);
bool on_copy_buffer_to_texture(reshade::api::command_list*, reshade::api::resource source,
    uint64_t source_offset, uint32_t row_length, uint32_t slice_height,
    reshade::api::resource dest, uint32_t dest_subresource,
    const reshade::api::subresource_box*);
bool on_copy_texture_region(reshade::api::command_list*, reshade::api::resource source,
    uint32_t               source_subresource, const reshade::api::subresource_box*,
    reshade::api::resource dest, uint32_t dest_subresource,
    const reshade::api::subresource_box*, reshade::api::filter_mode);
bool on_copy_resource(reshade::api::command_list*, reshade::api::resource source, reshade::api::resource dest);
void DrainTexReadback();
// A game list submitted: the texture readbacks recorded on it are timed by a fence signalled behind it.
void TexReadbackOnExecute(reshade::api::command_queue* queue, reshade::api::command_list* list);
