// claim_state.cpp -- character recognition state + helpers (see claim_state.h).
#include "../addon_common.h"
#include "claim_state.h"
#include "skeleton.h" // g_merged_comps -- the declared component set an anchor must see in full
#include "../../src/fnv.h"
#include <map>

std::unordered_map<std::wstring, int>      g_ns_to_mod;
std::deque<GeoMod>                         g_geomods;
bool                                       g_geom_loaded = false;
std::unordered_map<uint64_t, CharVbOrigin> g_char_mesh_vb0;
std::mutex                                 g_char_mesh_mtx;

void ForgetCharMeshVb0(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    unsigned n = 0;
    {
        std::lock_guard l(g_char_mesh_mtx);
        for (auto it = g_char_mesh_vb0.begin(); it != g_char_mesh_vb0.end();)
            if (it->first >= va && it->first - va < bytes) {
                it = g_char_mesh_vb0.erase(it);
                ++n;
            } else
                ++it;
    }
    if (n && logger::enabled(2))
        logger::write("CHARVB: forgot %u learned VB0(s) in destroyed buffer va=%llx bytes=%llu", n,
            (unsigned long long)va, (unsigned long long)bytes);
}

// ---- INDEX-BUFFER ANCHOR (claim_state.h) -------------------------------------------------------
namespace {
// A set of component indices, any number of them.
struct CompSet {
    std::vector<uint64_t> w;
    void                  set(int i)
    {
        const size_t k = (size_t)i / 64;
        if (w.size() <= k) w.resize(k + 1, 0);
        w[k] |= 1ull << (i % 64);
    }
    bool empty() const
    {
        for (uint64_t x : w)
            if (x) return false;
        return true;
    }
    bool covers(const CompSet& d) const
    {
        for (size_t k = 0; k < d.w.size(); ++k)
            if ((d.w[k] & (k < w.size() ? w[k] : 0)) != d.w[k]) return false;
        return true;
    }
    unsigned count() const
    {
        unsigned n = 0;
        for (uint64_t x : w)
            while (x) {
                x &= x - 1;
                ++n;
            }
        return n;
    }
};
struct IbEvidence {
    uint32_t frame = 0;
    int      mod = -1;
    CompSet  seen;
}; // this frame, this buffer
struct IbAnchorRec {
    int      mod = -1;
    uint32_t learned = 0;
    uint32_t confirmed = 0;
    uint64_t bytes = 0;
};
// ORDERED, so IbAnchorForget can erase a destroyed buffer's address range the way ForgetRange does.
std::map<uint64_t, IbEvidence>  g_ib_ev;
std::map<uint64_t, IbAnchorRec> g_ib_anchor;
std::mutex                      g_ib_anchor_mtx;
constexpr size_t                kIbAnchorCap = 256;    // a character is one entry; a sweep past this is a leak guard
constexpr uint32_t              kIbAnchorStale = 1800; // ~30 s unconfirmed -- the buffer is not on screen any more

// The components an anchor must see in full: the mod's components that share comp_idx's ini `hash=` (one index
// buffer). A mod whose parts live in two buffers (Suoming Summer: components 0-6 in one, component 7 in another)
// never shows every component in one buffer, so asking for the whole mod never learned it. Components without a
// hash= group together (key 0), as before. Rebuilt when the component list or the loaded units change (F10).
CompSet ModDeclaredComps(int mod_id, int comp_idx, unsigned* mod_total = nullptr)
{
    static std::mutex                            m;
    static size_t                                built_for = (size_t)-1;
    static unsigned                              built_epoch = ~0u;
    static std::unordered_map<uint64_t, CompSet> sets; // (mod << 32) | ib hash
    static std::unordered_map<int, unsigned>     totals; // every component of the mod, whatever its buffer
    std::lock_guard                              l(m);
    const unsigned                               epoch = g_mods_epoch.load(std::memory_order_acquire);
    if (built_for != g_merged_comps.size() || built_epoch != epoch) {
        sets.clear();
        totals.clear();
        for (const MergedComp& c : g_merged_comps)
            if (c.comp_idx >= 0) {
                ++totals[c.mod_id];
                const WwmiMod::Component* wc = WmodComp(c.mod_id, c.comp_idx);
                sets[((uint64_t)(uint32_t)c.mod_id << 32) | (wc ? wc->ib_hash : 0u)].set(c.comp_idx);
            }
        built_for = g_merged_comps.size();
        built_epoch = epoch;
    }
    if (mod_total) {
        auto t = totals.find(mod_id);
        *mod_total = t == totals.end() ? 0u : t->second;
    }
    const WwmiMod::Component* wc = WmodComp(mod_id, comp_idx);
    auto it = sets.find(((uint64_t)(uint32_t)mod_id << 32) | (wc ? wc->ib_hash : 0u));
    return it == sets.end() ? CompSet{} : it->second;
}
} // namespace

bool IbAnchorNoteAndAsk(uint64_t ib_va, uint64_t ib_bytes, int mod_id, int comp_idx)
{
    if (!ib_va || mod_id < 0 || comp_idx < 0) return false;
    unsigned      mod_total = 0;
    const CompSet declared = ModDeclaredComps(mod_id, comp_idx, &mod_total);
    if (declared.empty()) return false;
    std::lock_guard l(g_ib_anchor_mtx);
    IbEvidence&     e = g_ib_ev[ib_va];
    if (e.frame != g_frame_id || e.mod != mod_id) {
        e.frame = g_frame_id;
        e.mod = mod_id;
        e.seen = CompSet{};
    }
    e.seen.set(comp_idx);
    // A lone component in a buffer of its own (Suoming's component 7) is one (first, count) window: by itself that
    // is weak evidence. It is anchored only while another buffer of the same mod is anchored and on screen.
    bool enough = e.seen.covers(declared);
    if (enough && declared.count() == 1 && mod_total > 1) {
        enough = false;
        for (const auto& [va, a] : g_ib_anchor)
            if (va != ib_va && a.mod == mod_id && g_frame_id - a.confirmed <= 2) {
                enough = true;
                break;
            }
    }
    if (enough) {
        IbAnchorRec& a = g_ib_anchor[ib_va];
        a.bytes = ib_bytes;
        if (a.mod != mod_id) {
            a.mod = mod_id;
            a.learned = g_frame_id;
            g_ib_anchor_learned.fetch_add(1, std::memory_order_relaxed);
            logger::write("IBANCHOR learned: index buffer %llx carries mod %d's COMPLETE declared component "
                          "set for its hash= (%u of the mod's %u components) in frame %u -- its draws are claimed "
                          "from the next frame on, with no vertex content consulted",
                (unsigned long long)ib_va, mod_id, declared.count(), mod_total, g_frame_id);
        }
        a.confirmed = g_frame_id;
    }
    if (g_ib_anchor.size() > kIbAnchorCap)
        for (auto it = g_ib_anchor.begin(); it != g_ib_anchor.end();)
            if (g_frame_id - it->second.confirmed > kIbAnchorStale)
                it = g_ib_anchor.erase(it);
            else
                ++it;
    auto it = g_ib_anchor.find(ib_va);
    if (it == g_ib_anchor.end() || it->second.mod != mod_id) return false;
    return it->second.learned < g_frame_id;
}

bool IbAnchorResolve(uint64_t index_va, uint32_t index_bytes, int* mod_id, uint64_t* ib_base_va)
{
    if (!index_va || !index_bytes) return false;
    std::lock_guard l(g_ib_anchor_mtx);
    auto            it = g_ib_anchor.upper_bound(index_va);
    if (it == g_ib_anchor.begin()) return false;
    --it; // the greatest anchored base at or below the address
    const uint64_t off = index_va - it->first;
    if (!it->second.bytes || off >= it->second.bytes || (off % index_bytes) != 0 || it->second.mod < 0) return false;
    *mod_id = it->second.mod;
    *ib_base_va = it->first;
    return true;
}

bool IbAnchorJustLearned(uint64_t index_va)
{
    if (!index_va) return false;
    std::lock_guard l(g_ib_anchor_mtx);
    auto            it = g_ib_anchor.upper_bound(index_va);
    if (it == g_ib_anchor.begin()) return false;
    --it;
    return index_va - it->first < it->second.bytes && it->second.learned + 1 == g_frame_id;
}

void IbAnchorForget(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    std::lock_guard l(g_ib_anchor_mtx);
    g_ib_ev.erase(g_ib_ev.lower_bound(va), g_ib_ev.lower_bound(va + bytes));
    g_ib_anchor.erase(g_ib_anchor.lower_bound(va), g_ib_anchor.lower_bound(va + bytes));
}

std::unordered_set<uint32_t> g_frame_skel;   // character skeleton CONTENT hashes this frame
std::unordered_set<uint64_t> g_char_lod_vb0; // character LOD mesh buffers (persistent)
std::mutex                   g_char_skel_mtx;
uint32_t                     HashBoneCB(ID3D12Resource* res, uint64_t va)
{
    PROF(PROF_DI_BoneHash);
    uint8_t        buf[1536];
    const uint32_t n = SnapshotGameCB(res, va, buf, sizeof(buf));
    if (!n) return 0;
    const uint32_t h = Fnv1a32(buf, n);
    return h ? h : 1;
}
static void CollectBoundModTexHashes(command_list* cmd_list, uint32_t* out, int cap, int& n)
{
    n = 0;
    const TexRules& tr = TexRulesNow();
    if (tr.targets.empty()) return;
    PROF(PROF_DI_TexScan); // the per-draw table walk behind char_tex
    const auto& root = CmdGfx(cmd_list).root;
    uint32_t    scan_len[64];
    LayoutTexScanLens(CmdGfxLayout(cmd_list), scan_len);
    for (int p = 0; p < 64; ++p) {
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        uint32_t        num = ts.Heap()->GetDesc().NumDescriptors;
        ID3D12Resource* res[kTexScanMax];
        uint32_t        hs[kTexScanMax];
        const uint32_t  maxS = ResolveBoundTextures(ts.heap, ts.base, num, res, hs, scan_len[p]);
        for (uint32_t s = 0; s < maxS; ++s) {
            const uint32_t h = !hs[s] || tr.targets.count(hs[s]) ? hs[s] : TexLodTargetOf(hs[s]);
            if (h && tr.targets.count(h)) {
                if (n < cap) out[n++] = h;
                if (n >= cap) return;
            }
        }
    }
}
bool DrawBindsModTexture(command_list* cmd_list)
{
    uint32_t hs[8];
    int      n = 0;
    CollectBoundModTexHashes(cmd_list, hs, 8, n);
    return n > 0;
}

std::mutex                                          g_claimed_ibhash_mtx;
std::unordered_set<uint32_t>                        g_claimed_ibhash;
std::mutex                                          g_skip_vbhash_mtx;
std::shared_ptr<const std::unordered_set<uint32_t>> g_skip_vbhash_snap;
std::atomic<bool>                                   g_skip_vbhash_any{ false };
std::atomic<uint32_t>                               g_skip_vbhash_drops{ 0 };

std::atomic<uint64_t> g_skin_band_lo{ 0x1000000000ULL }; // non-static: diagnostics.cpp filters the
std::atomic<uint64_t> g_skin_band_hi{ 0x1100000000ULL }; // frame dump to char-band draws with these
void                  UpdateSkinBand()
{
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    {
        std::lock_guard l(g_char_mesh_mtx);
        for (const auto& kv : g_char_mesh_vb0) {
            const uint64_t v = kv.first;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    if (mn == ~0ull) return;                                      // nothing learned yet -> keep fallback
    g_skin_band_lo.store(mn & ~0xFFFFFFFFull);                    // round min down to 4GB boundary
    g_skin_band_hi.store((mx & ~0xFFFFFFFFull) + 0x200000000ull); // cover max + ~8GB pool headroom
}

std::mutex g_hb_mtx;
uint64_t   g_hb_vbmin = ~0ull;
uint64_t   g_hb_vbmax = 0;
uint32_t   g_hb_large = 0;
uint32_t   g_hb_pool = 0;

bool ModGeomEnabled(int md)
{
    if (md < 0 || md >= (int)g_geomods.size()) return true;
    if (g_wcl_draw && !g_geomods[md].wcl_ns.empty()) {
        return g_geomods[md].geom_enabled;
    }
    // Vars live under the ENGINE namespace.
    constexpr float kUnset = -1e30f;
    const float     v = PortApi::GetVarNs(g_geomods[md].wcl_ns.c_str(), L"enable_mods", kUnset);
    if (v == kUnset) return true; // mod ships no enable toggle -> auto-on (author intent)
    return v != 0.0f;             // manual mod -> honor $enable_mods (0 until the mod's enable key)
}
