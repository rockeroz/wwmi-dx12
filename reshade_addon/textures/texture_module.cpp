// ===========================================================================
//  Texture module implementation. See texture_module.h.
// ===========================================================================
#include "texture_module.h"
#include "tex_lod.h"
#include "../instrumentation/removal_pin.h"
#include "../config.h"
#include "../migoto/port_api.h" // their [TextureOverride*] dispatch and [Resource*] registry
#include "tex_crawl.h"
#include "../mods/ib_hash.h"    // ForgetRange: a whole-buffer copy replaces the mesh hashes inside it
#include "../instrumentation/profiler.h"
#include "../addon_state.h" // g_res_to_slot / HeapKey (late re-stamp slot map)
#include "../../src/log.h"
#include "../../src/d3d12_buffer.h"
#include "../../src/mod_loader.h"
#include <filesystem>
#include <string>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <cwchar> // wcstoul / _wcsnicmp -- parse texture filenames as wide (CJK-safe)
#include <wrl.h>
#include <thread>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <smmintrin.h>                 // _mm_stream_load_si128 -- reads from the write-combined upload heap
#include "../hooks/resource_tracker.h" // RestampResource -- the late re-stamp the lineage paths fire
#include "../hooks/cmd_state.h"        // CmdGen / t_list_resets -- when a readback's list reached its queue
#include "../instrumentation/tex_timing.h"
#include "../migoto/dx12_bridge.h" // DumpDir: D15 writes under it
#include <cstdio>

using namespace reshade::api;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// A texture whose bytes never passed an upload hook: its mip0 copied to a readback buffer, hashed frames later.
class TexReadback {
public:
    bool Schedule(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
        ID3D12Resource* dst, const D3D12_RESOURCE_DESC& sd);
    bool ScheduleUpload(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
        uint64_t src_off, uint64_t bytes, ID3D12Resource* dst, uint32_t sub, const uint32_t* cpu_crc);
    void OnExecute(command_queue* queue, command_list* list);
    void Drain();
    void Forget(ID3D12Resource* dst)
    {
        std::lock_guard l(mtx_);
        tried_.erase(dst);
    } // destroyed: its address is free again
private:
    struct QueueFence {
        ID3D12CommandQueue* queue = nullptr;
        ComPtr<ID3D12Fence> fence;
        uint64_t            next = 0;
    };
    struct Pending {
        ID3D12Resource*        dst = nullptr;
        uint32_t               off = 0;
        uint32_t               rowPitch = 0;
        uint32_t               sub = 0;         // an upload's mip
        bool                   compare = false; // D16: cpu_crc is the processor's read of the same upload
        uint32_t               cpu_crc = 0;
        ComPtr<ID3D12Resource> own;            // not in the shared buffer
        const void*            list = nullptr; // the game's list the copy is recorded on
        uint64_t               gen = 0;        // ...in this recording of it
        QueueFence*            qf = nullptr;   // the queue it was submitted to
        uint32_t               tid = 0;        // ...by this thread
        uint64_t               resets = 0;     // ...at this t_list_resets
        ULONGLONG              exec_ms = 0;
        uint64_t               value = 0; // signalled on qf behind the submission; the copy has run once the fence reaches it
    };
    struct Spare {
        ComPtr<ID3D12Resource> buf;
        uint64_t               bytes = 0;
    };
    bool        Reserve(ID3D12Device* dev, uint64_t total, bool once, Pending& p, ID3D12Resource** buf);
    void        KeepSpare(ComPtr<ID3D12Resource> b);
    static void Arrived(const Pending& p, const uint8_t* data);
    QueueFence* FenceOf(ID3D12CommandQueue* q);
    template <class Behind>
    unsigned SignalBehind(QueueFence* qf, Behind behind);
    static constexpr uint32_t           kBytes = 48u * 1024 * 1024;
    ID3D12Device*                       dev_ = nullptr;
    ComPtr<ID3D12Resource>              rb_;
    uint32_t                            cursor_ = 0; // the shared buffer is free from here; back to 0 once no copy into it is outstanding
    std::vector<Pending>                pending_;
    std::vector<Spare>                  spare_;           // own buffers whose readback was hashed or dropped, for the next that does not fit
    ULONGLONG                           own_used_ms_ = 0; // when an own buffer was last taken; the spares go after 10 s without one
    std::deque<QueueFence>              fences_; // one per queue: queues finish out of order
    std::atomic<uint32_t>               unsignalled_{ 0 }; // pending_ entries with no signal behind them yet
    std::unordered_set<ID3D12Resource*> tried_;
    std::mutex                          mtx_;
};
static TexReadback g_tex_readback;

static std::atomic<uint32_t> g_ts_tex2tex{ 0 };       // on_copy_texture_region calls past the asset gate
static std::atomic<uint32_t> g_ts_stream{ 0 };        // ...same format, no box, same mip, different size (a stream copy)
static std::atomic<uint32_t> g_ts_stream_nobig{ 0 };  // ...whose bigger side had no hash yet (LineageAlias could not alias)
static std::atomic<uint32_t> g_ts_buf_mip0{ 0 };      // buffer->texture mip0 uploads hashed
static std::atomic<uint32_t> g_ts_buf_mipk{ 0 };      // buffer->texture mip K uploads past the floor
static std::atomic<uint32_t> g_ts_mipk_target{ 0 };   // ...on a resource whose mip0 is a target (recorded or already known)
static std::atomic<uint32_t> g_ts_mipk_held{ 0 };     // ...held because mip0 was not known yet
static std::atomic<uint32_t> g_ts_held_resolved{ 0 }; // held entries resolved at mip0 (recorded or not a target)
static std::atomic<uint32_t> g_ts_target_mip0{ 0 };   // mip0 identifications of a rule target
static std::atomic<uint32_t> g_ts_rb_late{ 0 };       // readbacks whose fence went in a second after the submission, not behind a later one
static std::atomic<uint32_t> g_ts_up_gpu{ 0 };        // buffer->texture uploads hashed from a GPU copy
static std::atomic<uint32_t> g_ts_up_same{ 0 };       // ...whose hash matched the processor's read (D16)
static std::atomic<uint32_t> g_ts_up_diff{ 0 };       // ...and did not
static std::atomic<uint32_t> g_ts_rb_own{ 0 };        // readbacks that did not fit the shared buffer and created their own
static std::atomic<uint64_t> g_ts_rb_own_us{ 0 };     // ...time spent creating them, on the game's threads
static std::atomic<uint32_t> g_ts_rb_waits{ 0 };      // readbacks that waited 2 ms or more for the lock
static std::atomic<uint32_t> g_ts_rb_reused{ 0 };     // readbacks that took a spare own buffer instead of creating one
static std::atomic<uint32_t> g_up_slow_run{ 0 };      // consecutive upload reads slower than kUpSlowMBps
static std::atomic<bool>     g_up_gpu{ false };       // the processor reads the game's upload memory too slowly: GPU copies from now on

// BC formats: collapse unorm/srgb to the TYPELESS base (the resource format 3dmigoto hashes).
uint32_t TexIdentityHash(uint32_t data_hash, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr, uint32_t dxgi_fmt)
{
    uint32_t desc[11] = { w, h, mips, arr, TexTypelessBC(dxgi_fmt), 1, 0, /*Usage*/ 0, /*Bind*/ 8, /*CPU*/ 0, /*Misc*/ 0 };
    return PortApi::Crc32c(data_hash, desc, sizeof(desc));
}

static int WclTexSelect(uint32_t hash)
{
    return ModLoader::TexIndexOf(PortApi::SelectTextureOverride(hash));
}
static int Touched(int sel, unsigned mip)
{
    ModLoader::TexTouchAt(sel, mip);
    return sel;
}
unsigned TexDrawnMip(uint32_t hash, uint32_t target)
{
    if (!g_tex_distance || !hash) return ModLoader::kTexMipUnknown;
    if (hash == target) return 0;
    const unsigned k = TexLodMipOf(hash);
    return k ? k : ModLoader::kTexMipUnknown;
}
D3D12_CPU_DESCRIPTOR_HANDLE WclModSrvByResourceFq(const std::wstring& res_fq)
{
    ModLoader::TexEntry e;
    return ModLoader::TexEntryAt(Touched(ModLoader::TexIndexOf(PortApi::FindCustomResource(res_fq.c_str())), ModLoader::kTexMipUnknown), &e)
        ? e.srv
        : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
}
D3D12_CPU_DESCRIPTOR_HANDLE RfxMapSrv(const std::wstring& res_fq, const wchar_t* ns)
{
    if (res_fq.empty()) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    const uint32_t      sw = PortApi::SwitchOfResource(res_fq.c_str());
    const int           sel = sw ? TexSelectIdxNs(sw, ns, ModLoader::kTexMipUnknown) : -1;
    ModLoader::TexEntry e;
    if (sel < 0 || !ModLoader::TexEntryAt(sel, &e)) return WclModSrvByResourceFq(res_fq);
    if (g_debug && DiagChanged("RFX-SWITCH", (int)(sw & 0x7fffffff), (unsigned long long)(unsigned)sel)) {
        const bool named = sel == ModLoader::TexIndexOf(PortApi::FindCustomResource(res_fq.c_str()));
        logger::write("RFX-SWITCH %ls: [TextureOverride] hash=%08x switches its variants; bound its selection %d (%s)",
            res_fq.c_str(), sw, sel, named ? "the named one" : "NOT the named one: the switch wins");
    }
    return e.srv;
}
std::atomic<int> g_tex_flicker_watch{ 0 }; // frames remaining, armed by the overlay
struct FlickerSlot {
    uint32_t hash;
    int      sel;
    int      logged;
};
static thread_local FlickerSlot t_flick[512] = {};
static void                     FlickerWatch(uint32_t hash, int sel)
{
    if (g_tex_flicker_watch.load(std::memory_order_relaxed) <= 0) return;
    FlickerSlot& s = t_flick[hash & 511];
    if (s.hash != hash) {
        s.hash = hash;
        s.sel = sel;
        s.logged = 0;
        return;
    } // new occupant of this slot
    if (s.sel == sel) return;
    const int was = s.sel;
    s.sel = sel;
    if (s.logged >= 4) return; // a flipping texture proves itself in a few lines
    ++s.logged;
    // Their section name, so a flipping texture names the mod responsible rather than just a hash.
    wchar_t owner[256] = {};
    PortApi::TextureOverrideSection(hash, owner, std::size(owner));
    logger::write("TEXFLICKER hash=%08lx  %d -> %d   [%ls]",
        (unsigned long)hash, was, sel, owner[0] ? owner : L"(no section)");
}

// Resolve the selected mod TexResource index for a vanilla hash (honors $var/key conditionals). -1 = none.
int TexSelectIdx(uint32_t hash, unsigned mip)
{
    int                 wsel = WclTexSelect(hash);
    ModLoader::TexEntry e;
    const int           sel = (wsel >= 0 && ModLoader::TexEntryAt(wsel, &e)) ? wsel : -1; // else: no section, or vanilla
    FlickerWatch(hash, sel);
    return Touched(sel, mip);
}
int TexSelectIdxNs(uint32_t hash, const wchar_t* ns, unsigned mip)
{
    struct Sel {
        uint32_t hash;
        int      sel;
        unsigned load_gen;
        unsigned inputs_gen;
        unsigned tex_gen;
        long     churn;
    };
    if (!PortApi::SelectionCacheOk()) {
        const int           wsel = ModLoader::TexIndexOf(PortApi::SelectTextureOverrideNs(hash, ns));
        ModLoader::TexEntry e;
        return Touched((wsel >= 0 && ModLoader::TexEntryAt(wsel, &e)) ? wsel : -1, mip);
    }
    thread_local std::unordered_map<uint64_t, Sel> t_sel;
    thread_local const wchar_t*                    t_ns = nullptr;
    thread_local unsigned                          t_ns_gen = ~0u;
    thread_local uint64_t                          t_ns_hash = 0;
    const unsigned                                 load_gen = PortApi::LoadGeneration();
    if (ns != t_ns || load_gen != t_ns_gen) {
        uint64_t h = 0xcbf29ce484222325ull;
        if (ns)
            for (const wchar_t* c = ns; *c; ++c) h = (h ^ (uint64_t)*c) * 0x100000001b3ull;
        t_ns = ns;
        t_ns_gen = load_gen;
        t_ns_hash = h;
    }
    const uint64_t key = (t_ns_hash ^ hash) * 0x9E3779B97F4A7C15ull;
    const unsigned inputs_gen = PortApi::SelectionInputsGen();
    const unsigned tex_gen = ModLoader::TexStoreGen();
    const long     churn = PortApi::WrapChurn();
    auto           it = t_sel.find(key);
    if (it != t_sel.end() && it->second.hash == hash && it->second.load_gen == load_gen && it->second.inputs_gen == inputs_gen &&
        it->second.tex_gen == tex_gen && it->second.churn == churn)
        return Touched(it->second.sel, mip);
    bool                cacheable = false;
    const int           wsel = ModLoader::TexIndexOf(PortApi::SelectTextureOverrideNs(hash, ns, &cacheable));
    ModLoader::TexEntry e;
    const int           sel = (wsel >= 0 && ModLoader::TexEntryAt(wsel, &e)) ? wsel : -1;
    if (cacheable)
        t_sel[key] = Sel{ hash, sel, load_gen, inputs_gen, tex_gen, churn };
    else if (it != t_sel.end())
        t_sel.erase(it);
    return Touched(sel, mip);
}
uint32_t TexRuleHashFor(uint32_t hash)
{
    if (!hash) return 0;
    const TexRules& tr = TexRulesNow();
    if (tr.hash_to_rule.find(hash) != tr.hash_to_rule.end()) return hash;
    const uint32_t tgt = TexLodLookup(hash);
    return (tgt && tr.hash_to_rule.find(tgt) != tr.hash_to_rule.end()) ? tgt : 0;
}
uint32_t TexLodTargetOf(uint32_t hash)
{
    return hash ? TexLodLookup(hash) : 0;
}
uint32_t ResolveBoundTextures(uint64_t heap, uint32_t base, uint32_t num, ID3D12Resource** res, uint32_t* hash,
    uint32_t cap, const std::bitset<512>* want)
{
    static_assert(kTexScanMax == 512, "the read-slot mask is sized by kTexScanMax");
    FlatHeapMap::Entry* const e = g_heap_to_res.entry_of(heap);
    uint32_t                  n = 0;
    for (uint32_t s = 0; s < cap; ++s) {
        const uint32_t slot = base + s;
        if (slot >= num) break;
        ResInfo    ri;
        const bool read = !want || (s < kTexScanMax && want->test(s));
        res[s] = (read && e && g_heap_to_res.find_in(e, slot, ri)) ? ri.res : nullptr;
        hash[s] = 0;
        n = s + 1;
    }
    const uint32_t gen = g_res_texhash_gen.load(std::memory_order_acquire);
    uint32_t       miss[kTexScanMax];
    uint32_t       nm = 0;
    for (uint32_t s = 0; s < n; ++s) {
        if (!res[s]) continue;
        const uint64_t     r = reinterpret_cast<uint64_t>(res[s]);
        const TexHashMemo& m = TexHashMemoAt(r);
        if (m.res == r && m.gen == gen)
            hash[s] = m.hash;
        else if (nm < kTexScanMax)
            miss[nm++] = s;
    }
    if (nm) {
        std::shared_lock lt(g_tex_mutex); // PURE READ, once per table
        for (uint32_t k = 0; k < nm; ++k) {
            const uint64_t r = reinterpret_cast<uint64_t>(res[miss[k]]);
            auto           h = g_res_texhash.find(r);
            hash[miss[k]] = h != g_res_texhash.end() ? h->second : 0;
            TexHashMemoAt(r) = TexHashMemo{ r, hash[miss[k]], gen };
        }
    }
    return n;
}
D3D12_CPU_DESCRIPTOR_HANDLE TexReplacement(uint32_t hash)
{
    ModLoader::TexEntry e;
    return ModLoader::TexEntryAt(TexSelectIdx(hash, ModLoader::kTexMipUnknown), &e) ? e.srv : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
}

// Resources whose placement found a rule but an empty selection -- see TexRetryMissed.
static std::mutex                   g_tex_missed_mtx;
static std::unordered_set<uint64_t> g_tex_missed; // a destroyed resource leaves it (TexMipIdentityForget)

struct PlaceStat {
    uint32_t                                placed = 0;
    uint32_t                                missed = 0;
    std::set<std::pair<uint64_t, uint32_t>> slots;
};
static std::mutex                              g_place_mtx;
static std::unordered_map<uint32_t, PlaceStat> g_place_stats;
static void                                    NotePlacement(uint32_t rule, uint64_t heap, uint32_t slot, bool ok)
{
    if (!g_debug) return;
    std::lock_guard l(g_place_mtx);
    PlaceStat&      s = g_place_stats[rule];
    ++s.placed;
    if (!ok) ++s.missed;
    s.slots.insert({ heap, slot });
}
void TexPlacementReport()
{
    if (!g_debug) return;
    std::lock_guard l(g_place_mtx);
    for (const auto& [rule, s] : g_place_stats)
        if (s.missed)
            logger::write("TEXPLACE rule=%08x placed=%u missed=%u distinct_slots=%zu (last 600 frames)", rule, s.placed,
                s.missed, s.slots.size());
    g_place_stats.clear();
}

struct SelSnap {
    int      sel;
    uint32_t frame;
};
static std::shared_mutex                     g_sel_snap_mtx;
static std::unordered_map<uint32_t, SelSnap> g_sel_snap;
static int                                   SnapshotSelection(uint32_t rule)
{
    std::shared_lock l(g_sel_snap_mtx);
    auto             it = g_sel_snap.find(rule);
    if (it == g_sel_snap.end() || g_frame_id - it->second.frame > 120u) return -1;
    return it->second.sel;
}

static thread_local char t_tex_decline[96];
const char*              GlobalTexSwapDecline()
{
    return t_tex_decline;
}
bool GlobalTexSwap(ID3D12Device* dev, uint64_t heap_handle, uint32_t slot, ID3D12Resource* res)
{
    t_tex_decline[0] = 0;
    if (g_addon_disabled) {
        strcpy_s(t_tex_decline, "addon disabled");
        return false;
    }
    if (!g_global_tex || g_disable_tex || !g_texovr_ready || !dev || !res || !heap_handle) {
        sprintf_s(t_tex_decline, "global_tex=%d disable_tex=%d texovr_ready=%d", (int)g_global_tex, (int)g_disable_tex,
            (int)g_texovr_ready);
        return false;
    }
    if (TexRulesNow().hash_to_rule.empty()) {
        strcpy_s(t_tex_decline, "no texture rules");
        return false;
    }
    uint32_t hash = 0;
    bool     candidate = false;
    {
        std::shared_lock l(g_tex_mutex); // PURE READ (per-descriptor, hot) -> shared so worker threads parallelize
        auto             h = g_res_texhash.find(reinterpret_cast<uint64_t>(res));
        if (h != g_res_texhash.end()) hash = h->second;
        if (g_tex_lineage) {
            auto d = g_tex_dims.find(reinterpret_cast<uint64_t>(res));
            if (d != g_tex_dims.end()) candidate = d->second.hashable;
        }
    } // any texture an upload can name, not BC only:
    //   a mod may replace an R8 mask
    if (candidate) g_res_to_slot.put(reinterpret_cast<uint64_t>(res), { heap_handle, slot });
    uint32_t       rule_hash = TexRuleHashFor(hash);
    constexpr bool sticky = false; // GTEXRATE log field, kept stable
    if (!rule_hash) {              // not a mod texture / variant of one
        sprintf_s(t_tex_decline, "hash %08x has no rule", hash);
        return false;
    }
    ModLoader::TexEntry rep_e; // the SRV to stamp AND the resource it views
    int                 sel = TexSelectIdx(rule_hash, TexDrawnMip(hash, rule_hash));
    if (sel < 0) {
        sel = SnapshotSelection(rule_hash);
        if (sel >= 0 && g_debug && DiagOnce("TEXSEL-SNAP", (int)rule_hash))
            logger::write("TEXSEL-SNAP rule=%08x: its section selects nothing at placement; used the selection %d made at its mod's draw-time state",
                rule_hash, sel);
    }
    if (!ModLoader::TexEntryAt(sel, &rep_e) || rep_e.srv.ptr == 0) { // TexReplacement's test: honors $var/key conditionals
        g_heap_to_res.set_miss({ heap_handle, slot }, g_frame_id);
        ++g_global_tex_miss;
        TexTiming::NoteNeeded(rule_hash);
        if (g_debug) {
            const int           wsel = WclTexSelect(rule_hash);
            ModLoader::TexEntry we;
            const bool          have = wsel >= 0 && ModLoader::TexEntryAt(wsel, &we);
            sprintf_s(t_tex_decline, "rule %08x: section selects %d, %s", rule_hash, wsel,
                wsel < 0 ? "no texture (conditional or no section)" : !have ? "entry missing"
                    : we.srv.ptr                                            ? "entry has SRV"
                                                                            : "entry not loaded yet (no SRV)");
        }
        NotePlacement(rule_hash, heap_handle, slot, false);
        if (candidate) {
            std::lock_guard l(g_tex_missed_mtx); // its slot is recorded: retry it later
            g_tex_missed.insert(reinterpret_cast<uint64_t>(res));
        }
        if (g_debug) {
            static ProbeBudget missed;
            const int          n = missed.index(2000);
            if (n >= 0)
                logger::write("GTEXMISS f=%u n=%u rule=%08x boundHash=%08x heap=%llx slot=%u sections_run=%u",
                    g_frame_id, (unsigned)n, rule_hash, hash, (unsigned long long)heap_handle, slot,
                    g_sections_this_frame.load(std::memory_order_relaxed));
            else if (n == 2000)
                logger::write("GTEXMISS capped at 2000");
        }
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rep = rep_e.srv;
    auto*                             heap = reinterpret_cast<ID3D12DescriptorHeap*>(heap_handle);
    uint32_t                          inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE       dst = { heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * inc };
    BackupBeforeStamp(heap_handle, slot, rep.ptr);
    dev->CopyDescriptorsSimple(1, dst, rep, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_heap_to_res.set_swapped({ heap_handle, slot }, rep_e.res); // the frame dump publishes what the GPU reads
    StampedStoreTexture(heap_handle, slot, rep_e.res);
    ++g_global_tex_count;
    TexTiming::NoteSwapped(rep_e.res, rule_hash);
    NotePlacement(rule_hash, heap_handle, slot, true);
    {
        std::lock_guard l(g_swap_by_hash_mtx);
        ++g_swap_by_hash[rule_hash];
    }
    if (g_debug && ProbeArmed() && DiagOnce("GTEXSWAP", (int)rule_hash))
        logger::write("GTEXSWAP rule_hash=%08x boundHash=%08x slot=%u", rule_hash, hash, slot);

    if (g_debug) {
        static ProbeBudget emitted;
        const int          n = emitted.index(4000);
        if (n >= 0)
            logger::write("GTEXRATE f=%u n=%u rule=%08x boundHash=%08x slot=%u sticky=%d",
                g_frame_id, (unsigned)n, rule_hash, hash, slot, sticky ? 1 : 0);
        else if (n == 4000)
            logger::write("GTEXRATE capped at 4000 stamps -- the volume IS the finding");
    }
    return true;
}

void TexHoldForRetry(uint64_t res)
{
    if (!res) return;
    std::lock_guard l(g_tex_missed_mtx);
    g_tex_missed.insert(res);
}

void TexRetryMissed(const char* why, bool quiet_if_none)
{
    if (!g_global_tex || g_disable_tex) return;
    std::unordered_set<uint64_t> held;
    {
        std::lock_guard l(g_tex_missed_mtx);
        held.swap(g_tex_missed);
    }
    if (held.empty()) return;
    uint32_t swapped = 0;
    for (uint64_t r : held)
        if (RestampResource(r)) ++swapped; // a miss again re-enters the set
    if (g_debug && (swapped || !quiet_if_none))
        logger::write("TEXRETRY f=%u: %s -- %zu held placement(s) retried, %u now swapped",
            g_frame_id, why, held.size(), swapped);
}

static std::unordered_set<uint32_t> TexRulesOfNs(const wchar_t* ns);

void TexSnapshotSelections(const wchar_t* ns)
{
    if (!ns || !*ns) return;
    std::vector<std::pair<uint32_t, int>> sel;
    for (uint32_t r : TexRulesOfNs(ns)) sel.emplace_back(r, TexSelectIdx(r, ModLoader::kTexMipUnknown));
    std::unique_lock l(g_sel_snap_mtx);
    for (const auto& [r, s] : sel)
        if (s >= 0)
            g_sel_snap[r] = SelSnap{ s, g_frame_id };
        else
            g_sel_snap.erase(r);
}

void TexSnapshotsMove(const std::unordered_map<int, int>& index_to)
{
    if (index_to.empty()) return;
    std::unique_lock l(g_sel_snap_mtx);
    for (auto& [rule, snap] : g_sel_snap) {
        const auto it = index_to.find(snap.sel);
        if (it != index_to.end()) snap.sel = it->second;
    }
}

static std::unordered_set<uint32_t> TexRulesOfNs(const wchar_t* ns)
{
    std::unordered_set<uint32_t> rules;
    for (const auto& kv : TexRulesNow().hash_to_rule)
        if (PortApi::TextureOverrideInNs(kv.first, ns)) rules.insert(kv.first);
    return rules;
}

void TexUnstampSwitchedOff(const wchar_t* ns)
{
    if (g_addon_disabled || !ns || !*ns) return;
    const std::unordered_set<uint32_t> rules = TexRulesOfNs(ns);
    if (rules.empty()) return;
    const auto                                t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<HeapKey, uint64_t>> stamped = StampedCpuOnlySlots();
    std::erase_if(stamped, [&](const auto& s) { return !rules.count(TexRuleHashFor(TexHashOf(s.second))); });
    unsigned restored = 0;
    for (const auto& [key, res] : stamped) {
        if (UnstampSlot(key.heap, key.offset)) {
            ++restored;
            TexHoldForRetry(res);
            continue;
        }
        ResInfo ri{};
        HeapKey src{};
        if (!g_heap_to_res.find(key, ri)) continue;
        const bool tracked = TrackedSlotOfCpu(ri.view_handle, &src);
        logger::write("TEXOFF-KEPT f=%u heap %llx slot %u res=%p: source view %llx (%s heap %llx slot %u), slot written by src %u",
            g_frame_id, (unsigned long long)key.heap, key.offset, (void*)res, (unsigned long long)ri.view_handle,
            tracked ? "tracked" : "untracked", (unsigned long long)src.heap, src.offset, (unsigned)ri.wrote_src);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logger::write("TEXOFF f=%u ns='%ls': switched itself off -- %u of %zu stamped slot(s) in CPU-only heaps under its "
                  "%zu texture rule(s) given the game's descriptor back (%.2f ms)",
        g_frame_id, ns, restored, stamped.size(), rules.size(), ms);
}

void TexSwapsSwitched(bool were_live)
{
    const bool live = g_global_tex && !g_disable_tex;
    if (live == were_live || g_addon_disabled) return;
    if (live) {
        TexRetryMissed("texture swaps back on (C1/C2)");
        return;
    }
    const UnstampTally t = UnstampAll(TexHoldForRetry);
    logger::write("C1/C2: texture swaps off -- %u stamped slot(s) given the game's texture back, and %u of %zu still "
                  "stamped in CPU-only heaps",
        t.recorded, t.cpu_only, t.cpu_only_stamped);
}

static std::mutex                   g_off_mtx;
static std::unordered_set<uint32_t> g_off_rules;
static std::atomic<bool>            g_off_watching{ false };

void TexWatchSwitchedOff(const wchar_t* ns)
{
    if (!g_debug || !ns || !*ns) return;
    std::unordered_set<uint32_t> rules = TexRulesOfNs(ns);
    const size_t                 n = rules.size();
    {
        std::lock_guard l(g_off_mtx);
        g_off_rules.swap(rules);
    }
    g_off_leaks.store(0);
    g_off_kept.store(0);
    g_off_watching.store(n != 0);
    logger::write("OFFWATCH f=%u ns='%ls': switched itself off -- every draw is checked for a slot still stamped "
                  "under one of its %zu texture rule(s)",
        g_frame_id, ns, n);
}

void TexWatchSwitchedOn()
{
    if (!g_off_watching.exchange(false)) return;
    logger::write("OFFWATCH f=%u: switched back on -- %u slot read(s) of its textures by draws not handed back, "
                  "%u hand-back(s) that left its texture in place",
        g_frame_id, g_off_leaks.load(), g_off_kept.load());
}

bool TexOffWatching()
{
    return g_off_watching.load(std::memory_order_relaxed);
}

uint32_t TexOffRuleOf(ID3D12Resource* res)
{
    const uint32_t rule = TexRuleHashFor(TexHashOf(reinterpret_cast<uint64_t>(res)));
    if (!rule) return 0;
    std::lock_guard l(g_off_mtx);
    return g_off_rules.count(rule) ? rule : 0;
}

void TexTargetAudit()
{
    struct Ent {
        std::wstring leaf;
        std::wstring folder;
    };
    std::unordered_map<uint32_t, Ent> ents;
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool, const wchar_t* folder, void* user) {
            auto* m = (std::unordered_map<uint32_t, Ent>*)user;
            if (!hash || !fq) return;
            const wchar_t* leaf = wcsrchr(fq, L'\\');
            leaf = leaf ? leaf + 1 : fq;
            if (_wcsnicmp(leaf, L"texture", 7) != 0) return; // [TextureOverrideTexture*] only, as TexRulesPublish
            (*m)[hash] = { leaf, folder ? folder : L"" };
        },
        &ents);
    std::unordered_map<uint32_t, unsigned> seen;
    {
        std::shared_lock l(g_tex_mutex);
        for (auto& kv : g_res_texhash) ++seen[kv.second];
    }
    std::unordered_map<uint32_t, uint32_t> swapped;
    {
        std::lock_guard l(g_swap_by_hash_mtx);
        swapped = g_swap_by_hash;
    }
    std::map<std::wstring, std::vector<uint32_t>> by_mod;
    for (auto& kv : ents) by_mod[kv.second.folder].push_back(kv.first);
    logger::write("TEXAUDIT: %zu texture targets over %zu mods; %zu resources carry a hash; %zu rule hashes ever stamped",
        ents.size(), by_mod.size(), seen.size(), swapped.size());
    for (auto& kv : by_mod) {
        std::vector<uint32_t>& hashes = kv.second;
        std::sort(hashes.begin(), hashes.end(), [&](uint32_t a, uint32_t b) { return ents[a].leaf < ents[b].leaf; });
        unsigned     n_seen = 0;
        unsigned     n_sw = 0;
        std::wstring never;
        std::wstring unswapped;
        wchar_t      b[128];
        for (uint32_t h : hashes) {
            const unsigned s = seen.count(h) ? seen[h] : 0;
            const unsigned w = swapped.count(h) ? swapped[h] : 0;
            if (s) ++n_seen;
            if (w) ++n_sw;
            if (!s) {
                swprintf_s(b, L" %ls=%08x", ents[h].leaf.c_str(), h);
                never += b;
            } else if (!w) {
                swprintf_s(b, L" %ls=%08x(x%u)", ents[h].leaf.c_str(), h, s);
                unswapped += b;
            }
        }
        const wchar_t* fl = wcsrchr(kv.first.c_str(), L'\\');
        logger::write("TEXAUDIT %ls: targets=%zu seen=%u stamped=%u | never seen:%ls | seen, never stamped:%ls",
            fl ? fl + 1 : kv.first.c_str(), hashes.size(), n_seen, n_sw,
            never.empty() ? L" -" : never.c_str(), unswapped.empty() ? L" -" : unswapped.c_str());
    }
}

bool TexIsTarget(uint32_t hash, void*)
{
    return TexRulesNow().hash_to_rule.count(hash) != 0;
}

void TexRulesPublish()
{
    TexRules* tr = new TexRules;
    struct Ctx {
        TexRules* tr;
        int       n;
    } ctx{ tr, 0 };
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t* folder, void* user) {
            (void)ranged;
            (void)folder;
            Ctx* c = (Ctx*)user;
            if (!hash || !fq) return;
            const wchar_t* leaf = wcsrchr(fq, L'\\');
            leaf = leaf ? leaf + 1 : fq;
            if (_wcsnicmp(leaf, L"texture", 7) != 0) return; // [TextureOverrideTexture*] only
            c->tr->hash_to_rule.emplace(hash, c->n++);
            c->tr->targets.insert(hash);
        },
        &ctx);
    const size_t from_sections = tr->targets.size();
    PortApi::ForEachCustomResource(
        [](const wchar_t*, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
            const fs::path     f = filename ? filename : L"";
            const std::wstring nm = f.filename().wstring();
            const size_t       pos = nm.find(L"t=");
            if (pos == std::wstring::npos || _wcsicmp(f.parent_path().filename().c_str(), L"Textures") != 0) return;
            const uint32_t h = (uint32_t)wcstoul(nm.c_str() + pos + 2, nullptr, 16);
            if (h) static_cast<TexRules*>(user)->targets.insert(h);
        },
        tr);
    static std::vector<const TexRules*> s_retired;
    if (const TexRules* old = g_tex_rules.exchange(tr, std::memory_order_acq_rel)) s_retired.push_back(old);
    std::string s;
    char        b[16];
    for (uint32_t h : tr->targets) {
        snprintf(b, sizeof b, "%08x ", h);
        s += b;
    }
    logger::write("TexOverride runtime ready: %zu hashes from engine sections; TEXTARGETS (%zu): %s",
        from_sections, tr->targets.size(), s.c_str());
}

int RfxTexClass(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr)
{
    (void)arr;
    const bool square = (w == h);
    const bool bc7 = (fmt >= 94 && fmt <= 99);
    const bool bgra = (fmt >= 87 && fmt <= 91);
    uint32_t   full_chain = 0;
    for (uint32_t s = h; s; s >>= 1) ++full_chain;
    if (bc7 && square && h > 1 && (mips > 8 || mips == full_chain)) return 3; // [TextureOverrideDiffuseNormalTexture]
    if (bgra && square && h < 512 && mips > 1) return 3;                      // [TextureOverride<Name>SpecialTexture]
    if (fmt == 61 && square && h > 1 && mips > 1) return 4;                   // [TextureOverrideMaterialmap]
    if (bc7 && square && mips == 1) return 4;                                 // [TextureOverrideMaterialmapHair]
    if (bgra && square && h == 1 && mips == 1) return 5;                      // [TextureOverrideMagicDot]
    return 0;
}

bool RfxSetTexturesDecision(const int cls[3], bool have_d, bool have_n, bool have_l, bool have_m,
    uint8_t& d_mask, uint8_t& n_mask, uint8_t& l_mask, uint8_t& m_mask,
    char flags_out[8])
{
    d_mask = n_mask = l_mask = m_mask = 0;
    const bool x = cls[0] == 3;
    const bool y = (cls[1] == 3 || cls[1] == 5);
    const bool z = cls[2] == 3;
    const bool w = cls[2] == 4;
    const bool more = ((int)x + (int)y + (int)z + (int)w) > 1; // treg3.x: the flags sum past 1
    int        f = 0;
    if (x) flags_out[f++] = 'x';
    if (y) flags_out[f++] = 'y';
    if (z) flags_out[f++] = 'z';
    if (w) flags_out[f++] = 'w';
    flags_out[f] = 0;
    // 1st sample (t0)
    if (x) {
        if (more) {
            if (have_n) n_mask |= 1;
        } else if (have_d)
            d_mask |= 1;
    } else if (have_l)
        l_mask |= 1;
    // 2nd sample (t1)
    if (y) {
        if (have_d) d_mask |= 2;
    } else if (have_l)
        l_mask |= 2;
    // 3rd sample (t2)
    if (z) {
        if (have_d) d_mask |= 4;
    } else if (w) {
        if (have_m)
            m_mask |= 4;
        else if (have_l)
            l_mask |= 4;
    }
    if (w) {
        if (more) {
            if (have_d) d_mask |= 8;
        } else if (have_m)
            m_mask |= 8;
    } else if (more && !x) {
        if (have_n) n_mask |= 8;
    }
    return (d_mask | n_mask | l_mask | m_mask) != 0;
}

// ---- MIP-EXACT IDENTITIES ----------------------------------------------------------
// The resident the game streams in at distance is a resource whose mip0 IS the asset's mip K, with N-K
// mips, so its identity hash -- crc32c of that data seeding the reconstructed D3D11 desc -- is EXACTLY
// computable from the full texture's mip K as it uploads. The content match (tex_lod) measured a
// resemblance at 32x32 and could not tell one character's template map from another's (23 of 545
// aliases were ambiguous); this needs no signature. Recorded for every mip K of a mod target whose
// standalone size still clears the hashing floor; mips that arrive before mip0 wait in g_mip_pending
// until mip0's identity says whether the texture is a target at all.
namespace {
std::mutex                                                               g_mip_pending_mtx;
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> g_mip_pending; // dest -> (mip, identity)
std::mutex                                                               g_feed_pending_mtx;
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> g_feed_pending; // bigger resource -> (small identity, mips between)
void                                                                     MipExactRecord(ID3D12Resource* dest, uint32_t sub, uint32_t id, uint32_t rule)
{
    bool           degenerate = false;
    const uint32_t prev = TexLodAddMipExact(id, rule, sub, &degenerate);
    if (g_debug && DiagOnce("MIPEXACT", (int)(id ^ (sub << 28))))
        logger::write("MIPEXACT dest=%p mip=%u identity=%08x -> rule=%08x (%s)", (void*)dest, sub, id, rule,
            !prev ? "new record" : prev == rule ? "the content match had agreed"
                : degenerate                    ? "DEGENERATE: an exact record for another rule stood"
                                                : "REPLACES a content match that pointed elsewhere");
    if (!degenerate) return;
    std::vector<uint64_t> carriers;
    {
        std::shared_lock l(g_tex_mutex);
        for (const auto& kv : g_res_texhash)
            if (kv.second == id) carriers.push_back(kv.first);
    }
    for (uint64_t r : carriers) UnstampResource(r);
}
}
void TexMipIdentityForget(uint64_t res)
{
    {
        std::lock_guard l(g_mip_pending_mtx);
        g_mip_pending.erase(res);
    }
    {
        std::lock_guard l(g_feed_pending_mtx);
        g_feed_pending.erase(res);
    }
    {
        std::lock_guard l(g_tex_missed_mtx);
        g_tex_missed.erase(res);
    }
    g_tex_readback.Forget(reinterpret_cast<ID3D12Resource*>(res));
}
struct TexAssetLayout {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t rows = 0;
    uint32_t tight_row = 0;
    uint32_t pitch = 0;
    bool     floor = false;
};
static bool TexAssetLayoutOf(const D3D12_RESOURCE_DESC& d, uint32_t mip, TexAssetLayout& o)
{
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || mip >= d.MipLevels) return false;
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    const uint32_t fmt = (uint32_t)d.Format;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const uint32_t bpp = bb ? 0 : DxgiPlainBytesPerPixel(fmt);
    if (!bb && !bpp) return false;
    o.w = std::max<uint32_t>(1u, (uint32_t)d.Width >> mip);
    o.h = std::max<uint32_t>(1u, d.Height >> mip);
    o.rows = bb ? (o.h + 3) / 4 : o.h;
    o.tight_row = bb ? ((o.w + 3) / 4) * bb : o.w * bpp;
    o.pitch = (o.tight_row + 255u) & ~255u;
    o.floor = (uint64_t)o.w * o.h >= kTexHashFloorPx;
    return true;
}

static std::vector<uint8_t> TightFromUpload(const uint8_t* src, size_t pitch, const TexAssetLayout& L)
{
    std::vector<uint8_t> tight;
    {
        PROF(PROF_CB_Alloc);
        tight.resize((size_t)L.tight_row * L.rows);
    }
    PROF(PROF_CB_Copy);
    for (uint32_t r = 0; r < L.rows; ++r) {
        const uint8_t* s = src + (size_t)r * pitch;
        uint8_t*       d = tight.data() + (size_t)r * L.tight_row;
        size_t         n = L.tight_row;
        for (; n && ((uintptr_t)s & 15); --n) *d++ = *s++;
        for (; n >= 64; n -= 64, s += 64, d += 64) {
            const __m128i a = _mm_stream_load_si128((__m128i*)(s));
            const __m128i b = _mm_stream_load_si128((__m128i*)(s + 16));
            const __m128i c = _mm_stream_load_si128((__m128i*)(s + 32));
            const __m128i e = _mm_stream_load_si128((__m128i*)(s + 48));
            _mm_storeu_si128((__m128i*)(d), a);
            _mm_storeu_si128((__m128i*)(d + 16), b);
            _mm_storeu_si128((__m128i*)(d + 32), c);
            _mm_storeu_si128((__m128i*)(d + 48), e);
        }
        for (; n >= 16; n -= 16, s += 16, d += 16) _mm_storeu_si128((__m128i*)d, _mm_stream_load_si128((__m128i*)s));
        for (; n; --n) *d++ = *s++;
    }
    return tight;
}

static void DumpIfDegenerate(uint32_t hash, const std::vector<uint8_t>& tight, const TexAssetLayout& L, uint32_t fmt,
    uint32_t sub, uint32_t of)
{
    if (!g_dump_degenerates || !TexLodDegenerate(hash)) return;
    static std::mutex                   s_mtx;
    static std::unordered_set<uint32_t> s_done;
    {
        std::lock_guard l(s_mtx);
        if (!s_done.insert(hash).second) return;
    }
    const std::wstring dir = MigotoPort::DumpDir() + L"\\degenerates";
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%08x.dds", dir.c_str(), hash);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    uint32_t dxgi = fmt;
    switch (dxgi) {
    case 27:
    case 70:
    case 73:
    case 76:
    case 79:
    case 82:
    case 94:
    case 97:
        ++dxgi; // TYPELESS -> UNORM (BC6H: UF16), which a viewer opens
        break;
    case 90:
        dxgi = 87;
        break;
    }
    const bool bc = DxgiBcBlockBytes(fmt) != 0;
    uint32_t   hdr[37] = {}; // "DDS ", DDS_HEADER, DDS_HEADER_DXT10
    hdr[0] = 0x20534444;
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000 : 0x8);
    hdr[3] = L.h;
    hdr[4] = L.w;
    hdr[5] = bc ? (uint32_t)tight.size() : L.tight_row;
    hdr[7] = 1;
    hdr[19] = 32;
    hdr[20] = 0x4;
    hdr[21] = 0x30315844;
    hdr[27] = 0x1000;
    hdr[32] = dxgi;
    hdr[33] = 3;
    hdr[35] = 1;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
        logger::write("DEGENERATE-DUMP %08x: cannot write %S", hash, path);
        return;
    }
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(tight.data(), 1, tight.size(), f);
    fclose(f);
    if (sub)
        logger::write("DEGENERATE-DUMP %08x %ux%u fmt=%u: mip %u of %08x -> %S", hash, L.w, L.h, fmt, sub, of, path);
    else
        logger::write("DEGENERATE-DUMP %08x %ux%u fmt=%u: a resident's mip0 -> %S", hash, L.w, L.h, fmt, path);
}

void TexMipIdentityFromUpload(ID3D12Resource* dest, uint32_t sub, const uint8_t* src)
{
    if (!dest || !src || sub == 0 || !g_texovr_ready || !g_texlod_cache) return;
    const D3D12_RESOURCE_DESC d = dest->GetDesc();
    TexAssetLayout            L;
    if (!TexAssetLayoutOf(d, sub, L) || !L.floor) return;
    const uint32_t             fmt = (uint32_t)d.Format;
    const uint32_t             w = L.w;
    const uint32_t             h = L.h;
    const std::vector<uint8_t> tight = TightFromUpload(src, L.pitch, L);
    uint32_t                   data_hash;
    {
        PROF(PROF_CB_Crc);
        data_hash = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    const uint32_t id = TexIdentityHash(data_hash, w, h, d.MipLevels - sub,
        d.DepthOrArraySize ? d.DepthOrArraySize : 1, fmt);
    const uint32_t h0 = TexHashOf(reinterpret_cast<uint64_t>(dest));
    if (!h0) { // mip0 not seen yet: hold it
        DumpIfDegenerate(id, tight, L, fmt, sub, 0);
        std::lock_guard l(g_mip_pending_mtx);
        g_mip_pending[reinterpret_cast<uint64_t>(dest)].emplace_back(sub, id);
        g_ts_mipk_held.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t rule = TexRuleHashFor(h0);
    if (rule) g_ts_mipk_target.fetch_add(1, std::memory_order_relaxed);
    const uint32_t high = rule ? rule : TexCrawlOn() ? h0 : 0;
    if (high && id != high && id != h0) MipExactRecord(dest, sub, id, high);
    DumpIfDegenerate(id, tight, L, fmt, sub, rule ? rule : h0);
}

TexLodWorker g_tex_lod_worker;

void TexLookAlikesSwitched(bool on)
{
    std::vector<uint64_t> carriers;
    {
        std::shared_lock l(g_tex_mutex);
        for (const auto& kv : g_res_texhash)
            if (TexLodLinked(kv.second) && !TexLodKnown(kv.second)) carriers.push_back(kv.first);
    }
    unsigned changed = 0;
    for (uint64_t r : carriers) changed += on ? RestampResource(r) : UnstampResource(r, false);
    logger::write("TexLod: A13 %s -- %zu resident(s) carry a look-alike, %u slot(s) %s", on ? "on" : "off", carriers.size(),
        changed, on ? "now hold the mod texture" : "given back to the game");
}

void TexLodWorker::Run()
{
    for (;;) {
        Job j;
        {
            std::unique_lock l(mtx_);
            if (!cv_.wait_for(l, std::chrono::seconds(1), [this] { return stop_ || !queue_.empty(); })) {
                l.unlock();
                TexLodFlushIfQuiet(2000);
                TexCrawlFlushIfQuiet(10000);
                TexLookFlushIfQuiet(10000);
                continue;
            }
            if (stop_) return;
            j = std::move(queue_.front());
            queue_.pop_front();
        }
        const bool known_before = TexLodKnown(j.crc) || TexLodLookup(j.crc);
        {
            PROF(PROF_CB_TexLod);
            TexLookOnCut(j.crc, j.cut, j.is_target);
        }
        if (known_before || !(TexLodKnown(j.crc) || TexLodLookup(j.crc))) continue;
        const uint64_t key = reinterpret_cast<uint64_t>(j.dest);
        bool           still = false;
        {
            std::lock_guard l(g_tex_mutex); // the resource must still hold this texture
            auto            it = g_res_texhash.find(key);
            if (it != g_res_texhash.end() && it->second == j.crc) {
                g_modtex_res.insert(key);
                still = true;
            }
        }
        if (still) RestampResource(key);
    }
}

void TexLodWorker::Stop()
{
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TexLodWorker::Queue(ID3D12Resource* dest, uint32_t crc, uint32_t w, uint32_t h, uint32_t fmt, bool is_target,
    const std::vector<uint8_t>& bc)
{
    if (!TexLookWants(crc, w, h, fmt, is_target)) return; // decided for this content already
    {
        std::lock_guard l(mtx_);
        if (stop_) return;
    }
    TexLookCut cut = TexLookCutOf(bc.data(), bc.size(), w, h, fmt); // the patches' blocks queued, not the whole mip
    if (cut.blocks.empty()) return;
    {
        std::lock_guard l(mtx_);
        if (stop_) return;
        queue_.push_back(Job{ dest, crc, is_target, std::move(cut) });
        if (!thread_.joinable()) thread_ = std::thread(&TexLodWorker::Run, this);
    }
    cv_.notify_one();
}

void TexHashFromUpload(ID3D12Resource* dest, const uint8_t* src, size_t src_row_pitch)
{
    if (!dest || !src) return;
    auto           d = dest->GetDesc();
    TexAssetLayout L;
    if (!TexAssetLayoutOf(d, 0, L) || !L.floor) return;
    const uint32_t w = L.w;
    const uint32_t h = L.h;
    const uint32_t fmt = (uint32_t)d.Format;
    if (src_row_pitch == 0) src_row_pitch = L.pitch; // DX12 copy: 256-aligned rows
    if (src_row_pitch < L.tight_row) src_row_pitch = L.tight_row;
    std::vector<uint8_t> tight = TightFromUpload(src, src_row_pitch, L);
    uint32_t             data_hash;
    {
        PROF(PROF_CB_Crc);
        data_hash = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    uint32_t       crc = TexIdentityHash(data_hash, w, h, d.MipLevels ? d.MipLevels : 1,
        d.DepthOrArraySize ? d.DepthOrArraySize : 1, fmt);
    const uint32_t prev = TexHashOf(reinterpret_cast<uint64_t>(dest));
    g_th_all.fetch_add(1, std::memory_order_relaxed);
    g_th_bytes.fetch_add(tight.size(), std::memory_order_relaxed);
    if (prev) {
        g_th_rehash.fetch_add(1, std::memory_order_relaxed);
        if (prev == crc) g_th_same.fetch_add(1, std::memory_order_relaxed);
    }
    bool inherited = false;
    bool restamped = false;
    {
        std::lock_guard l(g_tex_mutex);
        g_res_texhash[reinterpret_cast<uint64_t>(dest)] = crc;
        g_res_texhash_gen.fetch_add(1, std::memory_order_acq_rel);
        inherited = g_res_inherited.erase(reinterpret_cast<uint64_t>(dest)) != 0;
    }
    if (prev && prev != crc) {
        const uint32_t r_prev = TexRuleHashFor(prev);
        const uint32_t r_now = TexRuleHashFor(crc);
        if (r_prev && r_now && r_prev != r_now) {
            logger::write("TEXID-REFUTED res=%p %ux%u m%u (%s): believed %08x -> rule %08x, its mip0 hashes to %08x -> rule %08x",
                (void*)dest, w, h, d.MipLevels, inherited ? "inherited" : "hashed", prev, r_prev, crc, r_now);
            if (prev != r_prev) TexLodRefute(prev, r_prev, "the resource's own mip0 named a different target");
            RestampResource(reinterpret_cast<uint64_t>(dest)); // its recorded slot now swaps as what it is
            restamped = true;
        } else if (r_prev && !r_now && inherited) {
            {
                std::lock_guard l(g_tex_mutex);
                g_res_texhash[reinterpret_cast<uint64_t>(dest)] = prev;
                g_res_texhash_gen.fetch_add(1, std::memory_order_acq_rel);
            }
            if (g_debug && DiagOnce("TEXID-UNCONFIRMED", (int)r_prev))
                logger::write("TEXID-UNCONFIRMED res=%p %ux%u m%u: believed %08x -> rule %08x, its mip0 hashes to %08x (no target) -- belief kept",
                    (void*)dest, w, h, d.MipLevels, prev, r_prev, crc);
        }
    }
    DumpIfDegenerate(crc, tight, L, fmt, 0, 0);
    const TexRules& tr = TexRulesNow();
    bool            tex_is_target = tr.hash_to_rule.count(crc) != 0;
    g_tex_lod_worker.Queue(dest, crc, w, h, fmt, tex_is_target, tight);
    if (tex_is_target || tr.targets.count(crc) || TexLodKnown(crc) || TexLodLookup(crc)) {
        std::lock_guard l(g_tex_mutex);
        g_modtex_res.insert(reinterpret_cast<uint64_t>(dest));
    }
    if (!restamped && prev != crc && TexRuleHashFor(crc)) {
        const bool ok = RestampResource(reinterpret_cast<uint64_t>(dest));
        if (g_debug && DiagOnce("TEXRESTAMP", (int)TexRuleHashFor(crc)))
            logger::write("TEXRESTAMP res=%p rule=%08x: %s", (void*)dest, TexRuleHashFor(crc),
                ok ? "its slot now holds the mod SRV"
                   : "not swapped (no slot recorded, the slot reused, or no mod SRV yet -- then TEXRETRY)");
    }
    if (tex_is_target) {                                                                    // this texture is one the mod replaces -> remember
        uint64_t        prof = ((uint64_t)w << 40) | ((uint64_t)h << 16) | (fmt & 0xffffu); //   its size/format
        std::lock_guard l(g_match_mtx);
        g_match_profiles.insert(prof); //   to target readback
    }
    std::vector<std::pair<uint32_t, uint32_t>> held;
    {
        std::lock_guard l(g_mip_pending_mtx);
        auto            it = g_mip_pending.find(reinterpret_cast<uint64_t>(dest));
        if (it != g_mip_pending.end()) {
            held.swap(it->second);
            g_mip_pending.erase(it);
        }
    }
    g_ts_held_resolved.fetch_add((uint32_t)held.size(), std::memory_order_relaxed);
    // Stream-copy edges parked on this resource before its identity was known (LineageAlias): resolve them now.
    std::vector<std::pair<uint32_t, uint32_t>> fed;
    {
        std::lock_guard l(g_feed_pending_mtx);
        auto            it = g_feed_pending.find(reinterpret_cast<uint64_t>(dest));
        if (it != g_feed_pending.end()) {
            fed.swap(it->second);
            g_feed_pending.erase(it);
        }
    }
    for (const auto& e : fed) TexLodFeeds(e.first, crc, tex_is_target || TexCrawlOn() ? crc : 0, e.second);
    if (!held.empty()) {
        const uint32_t rule = TexRuleHashFor(crc);
        const uint32_t high = rule ? rule : TexCrawlOn() ? crc : 0;
        if (high)
            for (const auto& m : held)
                if (m.second != high && m.second != crc) MipExactRecord(dest, m.first, m.second, high);
    }
    if (tex_is_target) {                                          // TEXSTREAM: this resource is a rule target as uploaded -- its mips past the floor
        g_ts_target_mip0.fetch_add(1, std::memory_order_relaxed); // should each leave a record
        if (g_debug) {
            uint32_t past_floor = 0;
            for (uint32_t k = 1; k < d.MipLevels; ++k) {
                TexAssetLayout Lk;
                if (TexAssetLayoutOf(d, k, Lk) && Lk.floor) ++past_floor;
            }
            logger::write("TEXSTREAM-TARGET rule=%08x %ux%u m%u fmt=%u mipsPastFloor=%u heldBeforeMip0=%zu",
                crc, w, h, d.MipLevels, fmt, past_floor, held.size());
        }
    }
}

bool on_update_texture_region(device*, const subresource_data& data, resource dest, uint32_t dest_subresource, const subresource_box*)
{
    if (g_addon_disabled) return false;
    if (dest_subresource == 0 && dest.handle != 0) // mip0 only
        TexHashFromUpload(reinterpret_cast<ID3D12Resource*>(dest.handle), (const uint8_t*)data.data, data.row_pitch);
    return false;
}

static void DiagTexPath(const char* path, ID3D12Resource* dest, ID3D12Resource* src, uint32_t dest_sub)
{
    if (!g_debug || !dest || dest_sub != 0) return;
    auto d = dest->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (!DxgiIsBc((uint32_t)d.Format)) return; // BC char-textures only
    if ((uint64_t)d.Width * d.Height < kTexHashFloorPx) return;
    if (TexHashKnown(reinterpret_cast<uint64_t>(dest))) return;
    static std::atomic<int> n{ 0 };
    if (n.fetch_add(1) > 60) return;
    int srcHeap = -1;
    if (src) {
        D3D12_HEAP_PROPERTIES hp = {};
        D3D12_HEAP_FLAGS      hf = {};
        src->GetHeapProperties(&hp, &hf);
        srcHeap = (int)hp.Type;
    }
    logger::write("TEXPATH %s dest=%p w=%u h=%u fmt=%u srcHeap=%d (1=DEFAULT 2=UPLOAD 3=READBACK)",
        path, (void*)dest, (uint32_t)d.Width, d.Height, (uint32_t)d.Format, srcHeap);
}

static void PropagateTexLineage(uint64_t srcH, uint64_t dstH, bool same_image)
{
    if (!g_tex_lineage || srcH == dstH) return;
    const uint32_t shash = TexHashOf(srcH);
    const uint32_t rule = TexRuleHashFor(shash);
    if (!shash || (!rule && !same_image)) return; // a copy holding its source's image (whole, or streamed down) is that texture, target or not
    bool tagged = false;
    {
        std::lock_guard l(g_tex_mutex);
        auto            it = g_res_texhash.find(dstH);
        if (it == g_res_texhash.end() || it->second != shash) {
            g_res_texhash[dstH] = shash; // dest now swaps as the mod target on next placement
            g_res_texhash_gen.fetch_add(1, std::memory_order_acq_rel);
            g_modtex_res.insert(dstH);
            g_res_inherited.insert(dstH); // a belief until its own mip0 says otherwise
            ++g_tex_lineage_count;
            tagged = true;
        }
    }
    if (tagged && rule) RestampResource(dstH);
}

static void LineageAlias(uint64_t smallerH, uint64_t biggerH, uint32_t steps)
{
    const uint32_t sId = TexHashOf(smallerH);
    const uint32_t bId = TexHashOf(biggerH);
    if (!sId || sId == bId) return;
    if (!bId) { // the bigger side is not hashed yet: park the edge
        std::lock_guard l(g_feed_pending_mtx);
        auto&           v = g_feed_pending[biggerH];
        if (std::none_of(v.begin(), v.end(), [&](const auto& e) { return e.first == sId; })) v.emplace_back(sId, steps);
        return;
    }
    const bool known_before = TexLodKnown(sId) != 0;
    TexLodFeeds(sId, bId, TexRulesNow().hash_to_rule.count(bId) || TexCrawlOn() ? bId : 0, steps);
    if (!known_before && TexLodKnown(sId)) RestampResource(smallerH); // recognised now -> re-fire its placement swap
}

bool on_copy_texture_region(command_list* cl, resource source, uint32_t src_sub, const subresource_box* src_box,
    resource dest, uint32_t dest_subresource, const subresource_box* dst_box, reshade::api::filter_mode)
{
    PROF(PROF_CopyTexRegion);
    if (g_addon_disabled) return false;
    if (g_disable_tex || !g_texovr_ready) return false;
    if (!source.handle || !dest.handle || !cl) return false;
    auto*               dst = reinterpret_cast<ID3D12Resource*>(dest.handle);
    auto*               src = reinterpret_cast<ID3D12Resource*>(source.handle);
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    uint32_t            fmt = (uint32_t)dd.Format;
    uint32_t            w = (uint32_t)dd.Width;
    uint32_t            h = dd.Height;
    {
        TexAssetLayout L;
        if (!TexAssetLayoutOf(dd, 0, L) || !L.floor) return false;
    } // the same assets the hash covers
    g_ts_tex2tex.fetch_add(1, std::memory_order_relaxed);
    if (sd.Format == dd.Format && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && !src_box && !dst_box) {
        uint32_t sMipW = (uint32_t)(sd.Width >> src_sub);
        uint32_t sMipH = (uint32_t)(sd.Height >> src_sub);
        uint32_t dMipW = (uint32_t)(dd.Width >> dest_subresource);
        uint32_t dMipH = (uint32_t)(dd.Height >> dest_subresource);
        bool     same_mip = sMipW && sMipH && sMipW == dMipW && sMipH == dMipH;
        bool     diff_res = (sd.Width != dd.Width || sd.Height != dd.Height);
        if (same_mip && diff_res) {
            bool                       dst_smaller = (dd.Width < sd.Width); // stream-out: dst is the lower-LOD variant
            const D3D12_RESOURCE_DESC& big = dst_smaller ? sd : dd;
            const D3D12_RESOURCE_DESC& sm = dst_smaller ? dd : sd;
            uint32_t                   steps = 0;
            while ((big.Width >> steps) > sm.Width) ++steps;
            if ((big.Width >> steps) != sm.Width || (big.Height >> steps) != sm.Height) steps = 0;
            g_ts_stream.fetch_add(1, std::memory_order_relaxed);
            if (!TexHashOf(dst_smaller ? source.handle : dest.handle)) g_ts_stream_nobig.fetch_add(1, std::memory_order_relaxed);
            PropagateTexLineage(source.handle, dest.handle, dst_smaller); // tag the fresh variant (the chain)
            if (dst_smaller)
                LineageAlias(dest.handle, source.handle, steps);
            else
                LineageAlias(source.handle, dest.handle, steps);
        }
    }

    // -----------------------------------------------------

    bool already = TexHashKnown(dest.handle);
    if (already) {
        std::shared_lock l(g_tex_mutex);
        if (g_res_inherited.count(dest.handle)) already = false;
    } // a belief gets measured
    bool profMatch;
    {
        uint64_t        prof = ((uint64_t)w << 40) | ((uint64_t)h << 16) | (fmt & 0xffffu);
        std::lock_guard l(g_match_mtx);
        profMatch = g_match_profiles.count(prof) != 0;
    }
    bool dimsEq = (sd.Width == dd.Width && sd.Height == dd.Height && sd.Format == dd.Format);
    if (g_debug && (fmt >= 94 && fmt <= 99) && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (uint64_t)sd.Width * sd.Height >= 256ull * 256ull) {
        static ProbeBudget n;
        if (n.take(601)) {
            const uint32_t sh = TexHashOf(source.handle);
            const uint32_t dh = TexHashOf(dest.handle);
            logger::trace("STREAMCOPY src=%llx[%ux%u m%u s%u h=%08x] -> dst=%llx[%ux%u m%u s%u h=%08x] dimsEq=%d box=%d",
                (unsigned long long)source.handle, (uint32_t)sd.Width, (uint32_t)sd.Height, sd.MipLevels, src_sub, sh,
                (unsigned long long)dest.handle, w, h, dd.MipLevels, dest_subresource, dh, (int)dimsEq,
                (int)(src_box != nullptr || dst_box != nullptr));
        }
    }
    if (src_sub != 0 || dest_subresource != 0 || src_box || dst_box) return false; // full mip0->mip0 only
    if (!dimsEq) return false;                                                     // identical mip0
    PropagateTexLineage(source.handle, dest.handle, true);
    if (already || !profMatch) return false;

    auto* dev = reinterpret_cast<ID3D12Device*>(cl->get_device()->get_native());
    auto* d3dcl = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    if (!dev || !d3dcl) return false;
    if (!g_tex_readback.Schedule(dev, cl, d3dcl, src, dst, sd)) return false;
    RemovalPin::Ours(d3dcl, "tex-readback");
    if (g_debug) {
        static ProbeBudget n;
        if (n.take(61))
            logger::write("TEXRB schedule dst=%p w=%u h=%u fmt=%u", (void*)dst, w, h, fmt);
    }
    return false;
}

void DrainTexReadback()
{
    if (g_debug) { // TEXSTREAM: the arrival paths, cumulative, every 600 frames
        static uint32_t tick = 0;
        if (++tick % 600 == 0)
            logger::write("TEXSTREAM tex2tex=%u stream=%u streamBigUnknown=%u | buf mip0=%u mipK=%u mipKtarget=%u held=%u heldResolved=%u | targets=%u | readbackLate=%u | "
                          "uploadsByGpuCopy=%u (same=%u differ=%u) | readbackOwnBuffers created=%u (%.1f ms) reused=%u lockWaits2ms=%u",
                g_ts_tex2tex.load(), g_ts_stream.load(), g_ts_stream_nobig.load(), g_ts_buf_mip0.load(), g_ts_buf_mipk.load(),
                g_ts_mipk_target.load(), g_ts_mipk_held.load(), g_ts_held_resolved.load(), g_ts_target_mip0.load(),
                g_ts_rb_late.load(), g_ts_up_gpu.load(), g_ts_up_same.load(), g_ts_up_diff.load(), g_ts_rb_own.load(),
                g_ts_rb_own_us.load() / 1000.0, g_ts_rb_reused.load(), g_ts_rb_waits.load());
    }
    g_tex_readback.Drain();
}

void TexReadbackOnExecute(command_queue* queue, command_list* list)
{
    g_tex_readback.OnExecute(queue, list);
}

bool TexReadback::Reserve(ID3D12Device* dev, uint64_t total, bool once, Pending& p, ID3D12Resource** buf)
{
    const auto      t_wait = std::chrono::steady_clock::now();
    std::lock_guard l(mtx_);
    {
        const uint64_t waited_us =
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_wait).count();
        if (waited_us >= 2000) {
            g_ts_rb_waits.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> logged{ 0 };
            if (g_debug && logged.fetch_add(1) < 20)
                logger::write("TEXRB-WAIT thread %u waited %.1f ms for the readback lock to record a %llu B readback",
                    GetCurrentThreadId(), waited_us / 1000.0, (unsigned long long)total);
        }
    }
    if (once && tried_.count(p.dst)) return false; // one-shot per resource
    if (!rb_) {
        CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &rb_);
        if (!rb_) return false;
    }
    dev_ = dev;
    const uint32_t aligned = (cursor_ + 511u) & ~511u;
    if ((uint64_t)aligned + total <= kBytes) {
        p.off = aligned;
        cursor_ = aligned + (uint32_t)total;
    } else {
        size_t best = spare_.size();
        for (size_t i = 0; i < spare_.size(); ++i)
            if (spare_[i].bytes >= total && (best == spare_.size() || spare_[i].bytes < spare_[best].bytes)) best = i;
        if (best != spare_.size()) {
            p.own = std::move(spare_[best].buf);
            spare_[best] = std::move(spare_.back());
            spare_.pop_back();
            g_ts_rb_reused.fetch_add(1, std::memory_order_relaxed);
        } else {
            uint64_t bytes = 64u * 1024;
            while (bytes < total) bytes <<= 1;
            const auto t0 = std::chrono::steady_clock::now();
            CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, &p.own);
            const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g_ts_rb_own.fetch_add(1, std::memory_order_relaxed);
            g_ts_rb_own_us.fetch_add(us, std::memory_order_relaxed);
            static std::atomic<int> logged{ 0 };
            if (g_debug && us >= 2000 && logged.fetch_add(1) < 20)
                logger::write("TEXRB-OWN %llu B readback buffer created in %.1f ms on thread %u -- the shared one is full to %u of %u B with "
                              "%zu readback(s) pending",
                    (unsigned long long)bytes, us / 1000.0, GetCurrentThreadId(), cursor_, kBytes, pending_.size());
        }
        own_used_ms_ = GetTickCount64();
        if (!p.own) return false;
    }
    *buf = p.own ? p.own.Get() : rb_.Get();
    pending_.push_back(p);
    unsignalled_.fetch_add(1, std::memory_order_relaxed);
    if (once) tried_.insert(p.dst);
    return true;
}

void TexReadback::KeepSpare(ComPtr<ID3D12Resource> b)
{
    if (!b) return;
    const uint64_t bytes = b->GetDesc().Width;
    spare_.push_back(Spare{ std::move(b), bytes });
}

bool TexReadback::Schedule(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
    ID3D12Resource* dst, const D3D12_RESOURCE_DESC& sd)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT                               rows = 0;
    UINT64                             rowSize = 0;
    UINT64                             total = 0;
    dev->GetCopyableFootprints(&sd, 0, 1, 0, &fp, &rows, &rowSize, &total);
    Pending p;
    p.dst = dst;
    p.rowPitch = fp.Footprint.RowPitch;
    p.list = list;
    p.gen = CmdGen(list);
    ID3D12Resource* own_or_shared = nullptr;
    if (!Reserve(dev, total, true, p, &own_or_shared)) return false;
    dst->AddRef(); // keep alive until Drain hashes it
    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = own_or_shared;
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = fp;
    d.PlacedFootprint.Offset = p.off;
    D3D12_TEXTURE_COPY_LOCATION s = {};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr); // src already copy-source-ready, no render pass: no barrier
    return true;
}

bool TexReadback::ScheduleUpload(ID3D12Device* dev, command_list* list, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
    uint64_t src_off, uint64_t bytes, ID3D12Resource* dst, uint32_t sub, const uint32_t* cpu_crc)
{
    Pending p;
    p.dst = dst;
    p.sub = sub;
    p.list = list;
    p.gen = CmdGen(list);
    if (cpu_crc) {
        p.compare = true;
        p.cpu_crc = *cpu_crc;
    }
    ID3D12Resource* buf = nullptr;
    if (!Reserve(dev, bytes, false, p, &buf)) return false;
    dst->AddRef();
    cl->CopyBufferRegion(buf, p.off, src, src_off, bytes); // an upload heap is always copy-source-ready
    return true;
}

void TexReadback::Arrived(const Pending& p, const uint8_t* data)
{
    if (p.compare) {
        TexAssetLayout L;
        if (TexAssetLayoutOf(p.dst->GetDesc(), p.sub, L)) {
            const std::vector<uint8_t> tight = TightFromUpload(data, L.pitch, L);
            const uint32_t             gpu_crc = PortApi::Crc32c(0, tight.data(), tight.size());
            (gpu_crc == p.cpu_crc ? g_ts_up_same : g_ts_up_diff).fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> n{ 0 };
            if (gpu_crc != p.cpu_crc && n.fetch_add(1) < 20)
                logger::write("TEXUP-DIFF res=%p mip %u %ux%u: the GPU copy hashes %08x, the processor's read %08x",
                    (void*)p.dst, p.sub, L.w, L.h, gpu_crc, p.cpu_crc);
        }
    }
    if (p.sub)
        TexMipIdentityFromUpload(p.dst, p.sub, data);
    else
        TexHashFromUpload(p.dst, data, p.rowPitch);
}

TexReadback::QueueFence* TexReadback::FenceOf(ID3D12CommandQueue* q)
{
    for (QueueFence& f : fences_)
        if (f.queue == q) return &f;
    QueueFence f;
    if (!dev_ || FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence)))) return nullptr;
    q->AddRef();
    f.queue = q;
    fences_.push_back(std::move(f));
    return &fences_.back();
}

template <class Behind>
unsigned TexReadback::SignalBehind(QueueFence* qf, Behind behind)
{
    bool any = false;
    for (const Pending& p : pending_) any |= p.qf == qf && !p.value && behind(p);
    if (!any || FAILED(qf->queue->Signal(qf->fence.Get(), qf->next + 1))) return 0;
    ++qf->next;
    unsigned n = 0;
    for (Pending& p : pending_)
        if (p.qf == qf && !p.value && behind(p)) {
            p.value = qf->next;
            unsignalled_.fetch_sub(1, std::memory_order_relaxed);
            ++n;
        }
    return n;
}

void TexReadback::OnExecute(command_queue* queue, command_list* list)
{
    if (!queue || !list || unsignalled_.load(std::memory_order_relaxed) == 0) return;
    auto* const    q = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    const uint32_t tid = GetCurrentThreadId();
    const uint64_t gen = CmdGen(list);
    std::lock_guard l(mtx_);
    for (QueueFence& f : fences_)
        if (f.queue == q) SignalBehind(&f, [&](const Pending& p) { return p.tid == tid && p.resets != t_list_resets; });
    for (Pending& p : pending_)
        if (!p.exec_ms && p.list == list && p.gen == gen) {
            p.qf = FenceOf(q);
            p.tid = tid;
            p.resets = t_list_resets;
            p.exec_ms = GetTickCount64();
            if (!p.qf) unsignalled_.fetch_sub(1, std::memory_order_relaxed);
        }
}

void TexReadback::Drain()
{
    std::vector<Pending>         arrived;
    std::vector<ID3D12Resource*> dropped_dst;
    uint32_t                     mapped_end = 0;
    {
        std::lock_guard l(mtx_);
        const ULONGLONG now = GetTickCount64();
        for (QueueFence& f : fences_)
            g_ts_rb_late.fetch_add(SignalBehind(&f, [&](const Pending& p) { return now - p.exec_ms >= 1000; }),
                std::memory_order_relaxed);
        size_t keep = 0;
        for (size_t i = 0; i < pending_.size(); ++i) {
            Pending&   p = pending_[i];
            const bool dropped = !p.exec_ms && CmdGen(p.list) != p.gen;
            const bool ran = p.value ? p.qf->fence->GetCompletedValue() >= p.value : (p.exec_ms && !p.qf && now - p.exec_ms >= 2000);
            if (!dropped && !ran) {
                if (keep != i) pending_[keep] = std::move(p);
                ++keep;
                continue;
            }
            if (dropped) unsignalled_.fetch_sub(1, std::memory_order_relaxed);
            if (ran) {
                arrived.push_back(std::move(p));
            } else {
                dropped_dst.push_back(p.dst);
                KeepSpare(std::move(p.own));
            }
        }
        pending_.erase(pending_.begin() + keep, pending_.end());
        mapped_end = cursor_;
    }
    void* shared = nullptr;
    for (const Pending& p : arrived) {
        if (p.own) {
            void* q = nullptr;
            if (SUCCEEDED(p.own->Map(0, nullptr, &q)) && q) {
                Arrived(p, (const uint8_t*)q);
                D3D12_RANGE wr = { 0, 0 };
                p.own->Unmap(0, &wr);
            }
            continue;
        }
        if (!shared) {
            D3D12_RANGE rr = { 0, mapped_end };
            if (FAILED(rb_->Map(0, &rr, &shared))) shared = nullptr;
        }
        if (shared) Arrived(p, (const uint8_t*)shared + p.off);
    }
    if (shared) {
        D3D12_RANGE wr = { 0, 0 };
        rb_->Unmap(0, &wr);
    }
    {
        std::lock_guard l(mtx_);
        for (Pending& p : arrived) KeepSpare(std::move(p.own));
        if (!spare_.empty() && GetTickCount64() - own_used_ms_ >= 10000) spare_.clear();
        if (std::none_of(pending_.begin(), pending_.end(), [](const Pending& p) { return !p.own; })) cursor_ = 0;
    }
    for (const Pending& p : arrived) p.dst->Release();
    for (ID3D12Resource* r : dropped_dst) r->Release();
}
bool on_copy_resource(command_list*, resource source, resource dest)
{
    if (g_addon_disabled) return false;
    if (dest.handle) {
        auto* d = reinterpret_cast<ID3D12Resource*>(dest.handle);
        if (const uint64_t va = d->GetGPUVirtualAddress()) {
            IbHash::ForgetRange(va, d->GetDesc().Width);
            return false;
        }
    }
    if (g_texovr_ready && source.handle && dest.handle) { // lineage for whole-resource copies too
        auto*               d = reinterpret_cast<ID3D12Resource*>(dest.handle);
        auto*               s = reinterpret_cast<ID3D12Resource*>(source.handle);
        D3D12_RESOURCE_DESC dd = d->GetDesc();
        TexAssetLayout      L; // the same assets the hash covers (on_copy_texture_region's test), not BC only
        if (TexAssetLayoutOf(dd, 0, L) && L.floor && s->GetDesc().Format == dd.Format)
            PropagateTexLineage(source.handle, dest.handle, true);
    }
    DiagTexPath("copy_resource", reinterpret_cast<ID3D12Resource*>(dest.handle),
        reinterpret_cast<ID3D12Resource*>(source.handle), 0);
    return false;
}

static constexpr uint32_t kUpProbeBytes = 16u * 1024;
static constexpr double   kUpSlowMBps = 256.0;
static std::atomic<uint32_t> g_up_probe_sink{ 0 };

static bool UploadReadSlow(const uint8_t* p, uint64_t span)
{
    static const double us_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / (double)f.QuadPart;
    }();
    const size_t   n0 = (size_t)std::min<uint64_t>(span, kUpProbeBytes);
    size_t         n = n0;
    const uint8_t* s = p;
    __m128i        acc = _mm_setzero_si128();
    LARGE_INTEGER  a;
    LARGE_INTEGER  b;
    {
        PROF(PROF_CB_Copy);
        QueryPerformanceCounter(&a);
        for (; n && ((uintptr_t)s & 15); --n, ++s) acc = _mm_xor_si128(acc, _mm_cvtsi32_si128(*s));
        for (; n >= 16; n -= 16, s += 16) acc = _mm_xor_si128(acc, _mm_stream_load_si128((__m128i*)s));
        QueryPerformanceCounter(&b);
    }
    g_up_probe_sink.fetch_xor((uint32_t)_mm_cvtsi128_si32(acc), std::memory_order_relaxed);
    const size_t read = n0 - n;
    const double us = (double)(b.QuadPart - a.QuadPart) * us_per_tick;
    const double mbps = us > 0 ? (double)read / us : 1e9;
    const bool   slow = mbps < kUpSlowMBps;
#ifdef WWMI_DEBUG
    {
        static std::atomic<int> logged{ 0 };
        if (g_debug && (slow || logged.load(std::memory_order_relaxed) < 6) && logged.fetch_add(1) < 24)
            logger::write("TEXUP-PROBE %zu B of the game's upload memory in %.1f us: %.0f MB/s%s", read, us, mbps,
                slow ? " -- slow" : "");
    }
#endif
    if (!slow) {
        g_up_slow_run.store(0, std::memory_order_relaxed);
        return false;
    }
    if (g_up_slow_run.fetch_add(1, std::memory_order_relaxed) + 1 >= 3 && !g_up_gpu.exchange(true))
        logger::write("TEXUP: the processor reads the game's upload memory at %.1f MB/s (Resizable BAR puts it in video memory) -- "
                      "texture uploads are hashed from a GPU copy from now on",
            mbps);
    return true;
}

static bool HashUploadOnGpu(command_list* cl, ID3D12Resource* src, uint64_t off, uint64_t span, ID3D12Resource* dst, uint32_t sub,
    const uint8_t* compare_from, const TexAssetLayout& L)
{
    if (sub && (!g_texovr_ready || !g_texlod_cache)) return true;
    auto* dev = reinterpret_cast<ID3D12Device*>(cl->get_device()->get_native());
    auto* d3dcl = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    if (!dev || !d3dcl) return false;
    uint32_t cpu_crc = 0;
    if (compare_from) {
        const std::vector<uint8_t> tight = TightFromUpload(compare_from, L.pitch, L);
        cpu_crc = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    if (!g_tex_readback.ScheduleUpload(dev, cl, d3dcl, src, off, span, dst, sub, compare_from ? &cpu_crc : nullptr)) return false;
    RemovalPin::Ours(d3dcl, "tex-upload-readback");
    g_ts_up_gpu.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool on_copy_buffer_to_texture(command_list* cl, resource source, uint64_t source_offset, uint32_t /*row_length*/, uint32_t /*slice_height*/, resource dest, uint32_t dest_subresource, const subresource_box* dst_box)
{
    PROF(PROF_CopyBufToTex);
    if (g_addon_disabled) return false;
    if (!dest.handle || !cl) return false;

    auto*               dst = reinterpret_cast<ID3D12Resource*>(dest.handle);
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    TexAssetLayout      L;
    if (!TexAssetLayoutOf(dd, dest_subresource, L) || !L.floor) return false;
    // a partial upload (an atlas region) is not the texture, and its source holds only the region
    if (dst_box && (dst_box->left || dst_box->top || dst_box->right != L.w || dst_box->bottom != L.h)) return false;

    if (source.handle == 0) return false;
    if (dest_subresource != 0 && !g_mip_exact) return false;
    auto*                 srcRes = reinterpret_cast<ID3D12Resource*>(source.handle);
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS      hf = {};
    srcRes->GetHeapProperties(&hp, &hf);
    if (hp.Type != D3D12_HEAP_TYPE_UPLOAD) {
        DiagTexPath("buf2tex(non-upload)", dst, srcRes, dest_subresource);
        return false;
    }
    // every row the hasher reads must lie inside the upload buffer (the placed footprint's pitch is assumed)
    if (source_offset + (uint64_t)L.pitch * (L.rows - 1) + L.tight_row > srcRes->GetDesc().Width) return false;
    if (dest_subresource != 0) {
        // Only a target's mips are worth a map: a known mip0 that is no rule hash and no alias is nothing.
        const uint32_t h0 = TexHashOf(reinterpret_cast<uint64_t>(dst));
        if (h0 && !TexRuleHashFor(h0) && !TexCrawlOn()) return false;
    }
    const uint64_t         span = (uint64_t)L.pitch * (L.rows - 1) + L.tight_row;
    std::atomic<uint32_t>& arrivals = dest_subresource == 0 ? g_ts_buf_mip0 : g_ts_buf_mipk;
    if (!g_tex_upload_gpu && g_up_gpu.load(std::memory_order_relaxed) &&
        HashUploadOnGpu(cl, srcRes, source_offset, span, dst, dest_subresource, nullptr, L)) {
        arrivals.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void*   ptr = nullptr;
    HRESULT map_hr;
    {
        PROF(PROF_CB_Map);
        map_hr = srcRes->Map(0, nullptr, &ptr);
    }
    if (SUCCEEDED(map_hr) && ptr) {
        arrivals.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* up = (const uint8_t*)ptr + source_offset;
        const bool     gpu = g_tex_upload_gpu || UploadReadSlow(up, span);
        if (!gpu || !HashUploadOnGpu(cl, srcRes, source_offset, span, dst, dest_subresource, g_tex_upload_gpu ? up : nullptr, L)) {
            if (dest_subresource == 0)
                TexHashFromUpload(dst, up, 0 /*256-aligned*/);
            else
                TexMipIdentityFromUpload(dst, dest_subresource, up);
        }
        PROF(PROF_CB_Map);
        srcRes->Unmap(0, nullptr);
    }
    return false;
}
