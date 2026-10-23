// The chain's identity searches: naming a buffer without a descriptor. See sk_internal.h.
#include "sk_internal.h"

// ---------------------------------------------------------------------------
//  EACH MOD'S OWN DECLARED SHAPEKEY HASHES, RESOLVED ONCE PER INI LOAD.
//
//  Which mod owns a buffer is a property of the INI, not of the dispatch. Resolved here once per mod
//  whenever PortApi::LoadGeneration() changes, the dispatch-time ownership question becomes an integer
//  compare against the mods that are enabled and on screen.
//
//  PUBLISHED THROUGH AN ATOMIC POINTER, superseded tables never freed: a reload is a keypress, the table
//  is a few hundred bytes, and a recording thread may still be reading the old one.
// ---------------------------------------------------------------------------
struct SkModHashTable {
    unsigned                                       gen;
    std::vector<std::vector<PortApi::ShapeKeySet>> sets; // per mod
};
static std::atomic<const SkModHashTable*> g_sk_mod_hashes{ nullptr };

// Called from Present, which is single-threaded, so the check-then-build needs no lock of its own.
void SkRefreshModHashes()
{
    const unsigned        gen = PortApi::LoadGeneration();
    const SkModHashTable* cur = g_sk_mod_hashes.load(std::memory_order_acquire);
    if (cur && cur->gen == gen && cur->sets.size() == g_geomods.size()) return;

    auto* t = new SkModHashTable();
    t->gen = gen;
    t->sets.resize(g_geomods.size());
    int    with_cb = 0;
    size_t n_sets = 0;
    for (size_t i = 0; i < g_geomods.size(); ++i) {
        t->sets[i] = PortApi::ModDeclaredShapeKeySets(g_geomods[i].wcl_ns.c_str());
        if (!t->sets[i].empty()) ++with_cb;
        n_sets += t->sets[i].size();
    }
    g_sk_mod_hashes.store(t, std::memory_order_release);
    logger::write("SKMODHASH: resolved %zu mod(s) at ini generation %u -- %d declare a shapekey callback, "
                  "%zu shapekey set(s) in all.",
        g_geomods.size(), gen, with_cb, n_sets);
}

static const std::vector<PortApi::ShapeKeySet>* SkModSets(int i)
{
    const SkModHashTable* t = g_sk_mod_hashes.load(std::memory_order_acquire);
    return (t && i >= 0 && (size_t)i < t->sets.size()) ? &t->sets[i] : nullptr;
}

// THE SET WHOSE ShapeKey*Callback DECLARES `cb`, or nullptr: a u0 of that hash is not this mod's.
static const PortApi::ShapeKeySet* SkSetOf(const std::vector<PortApi::ShapeKeySet>* sets, unsigned cb)
{
    if (!sets || !cb) return nullptr;
    for (const PortApi::ShapeKeySet& s : *sets)
        if (s.cb == cb) return &s;
    return nullptr;
}

bool SkModDeclaresCb(int i, unsigned cb)
{
    return SkSetOf(SkModSets(i), cb) != nullptr;
}

// ...and the ShapeKeyScale paired with it, which is how cs-u1 is identified.
unsigned SkModScaleFor(int i, unsigned cb)
{
    const PortApi::ShapeKeySet* s = SkSetOf(SkModSets(i), cb);
    return s ? s->scale : 0u;
}

// The callback hash of mod i that `u0hash` names; its only one when it declares a single set; else 0.
unsigned SkModCbFor(int i, unsigned u0hash)
{
    const std::vector<PortApi::ShapeKeySet>* sets = SkModSets(i);
    if (SkSetOf(sets, u0hash)) return u0hash;
    return (sets && sets->size() == 1) ? (*sets)[0].cb : 0u;
}

// ---------------------------------------------------------------------------
//  GEOMETRY-CHECKED IDENTITY -- the game's own dispatch names u0's SIZE.
//
//  DX12 has no descriptor-to-resource query, so the descriptor mirror can never be authoritative.
//  MEASURED (a morph dump): the clear before one character's Loader covered 7,325 x 256 = 1,875,200 B
//  and her Multiplier ran 2,442 groups (78,144 vertices x 24 B), while the slot's record named a
//  19,716 B buffer for the clear, the Loader and the Multiplier alike -- a previous occupant of the
//  offline slot, laundered by CopyDescriptors every frame. u1's record on the same list was right, so
//  the loss is per descriptor. The game's group counts are exact to one thread group and the ini's
//  declared hash is a function of the vanilla size, so between them the buffer is named WITHOUT a
//  descriptor. Nothing here is invented: the counts are the game's, the hash is the ini's.
// ---------------------------------------------------------------------------
// The last per-dword clear (g_clear_cs_hash) recorded on this thread. Dispatches on one command list
// are recorded by one thread, and the u0 clear precedes the Loader on the same list (MEASURED in every
// capture: clear u0, clear u1, Loader, Multiplier, consecutive), so a thread_local is exact and lock-free.
thread_local SkClearNote t_sk_clear;

// The byte range u0 must have for THIS dispatch, from its own geometry; 0/0 when nothing is known.
//   Loader:     the preceding clear covers groups x 64 threads x 4 B, so (gx-1)*256 < bytes <= gx*256.
//   Multiplier: groups = vertices/32 + 1 at 24 B per vertex, so ((gy-1)*32+1)*24 <= bytes <= gy*32*24.
void SkGeomExpect(const void* cl, uint64_t cs_hash, uint32_t gy, unsigned* lo, unsigned* hi)
{
    *lo = 0;
    *hi = 0;
    uint64_t lo64 = 0;
    uint64_t hi64 = 0;
    if (cs_hash == g_morph_cs_hash) {
        if (t_sk_clear.cl != cl || t_sk_clear.frame != g_frame_id || !t_sk_clear.gx) return;
        hi64 = (uint64_t)t_sk_clear.gx * 256ull;
        lo64 = hi64 - 255ull;
    } else if (cs_hash == 0xc1a7f1fec51b482fULL) { // ShapeKeyMultiplierCS, the DX12 hash on_dispatch tests
        if (!gy) return;
        hi64 = (uint64_t)gy * 32ull * 24ull;
        lo64 = ((uint64_t)(gy - 1) * 32ull + 1ull) * 24ull;
    } else {
        return;
    }
    if (hi64 > 0xFFFFFFFFull) return;
    *lo = (unsigned)lo64;
    *hi = (unsigned)hi64;
}

// THE DRAW NAMES THE INSTANCE. Per mod, the vertex-buffer resources its last claimed draws bound (the
// game's own IASetVertexBuffers, i.e. a VA, no descriptor involved). The character-screen instance and
// the in-world instance of one character's morph buffer are both alive (MEASURED, "2 live
// declared buffer(s) fit"), the game barriers neither on the dispatch's list, and only the draw says
// which one is being rendered. Kept for 4 frames, the same on-screen window the claim gate uses.
namespace {
struct SkDrawVbs {
    ID3D12Resource* res[8] = {};
    uint32_t        frame = 0;
};
std::mutex             g_drawvb_mtx;
std::vector<SkDrawVbs> g_drawvb; // per unit, grown to the load under g_drawvb_mtx
}

void SkNoteDrawVbs(int mod, ID3D12Resource* const* res, unsigned n)
{
    if (mod < 0 || !res) return;
    std::lock_guard<std::mutex> l(g_drawvb_mtx);
    if (g_drawvb.size() <= (size_t)mod) g_drawvb.resize(mod + 1);
    SkDrawVbs& d = g_drawvb[mod];
    for (unsigned i = 0; i < 8; ++i) d.res[i] = (i < n) ? res[i] : nullptr;
    d.frame = g_frame_id;
}

// The u0 buffers the replay wrote (RunShapeKeyPasses), by resource. A handful of entries, one per
// character instance the chain drives; never erased, aged by frame instead, so a destroyed resource's
// address reused by a new buffer answers false until the chain writes that one too.
namespace {
struct SkU0Written {
    ID3D12Resource* res;
    uint32_t        frame;
};
std::mutex               g_u0written_mtx;
std::vector<SkU0Written> g_u0written;
}

void SkNoteChainWroteU0(ID3D12Resource* res, uint32_t frame)
{
    if (!res) return;
    std::lock_guard<std::mutex> l(g_u0written_mtx);
    for (auto& w : g_u0written)
        if (w.res == res) {
            w.frame = frame;
            return;
        }
    g_u0written.push_back({ res, frame });
}

bool SkChainWroteU0Recently(ID3D12Resource* res, uint32_t frame)
{
    if (!res) return false;
    std::lock_guard<std::mutex> l(g_u0written_mtx);
    for (const auto& w : g_u0written)
        if (w.res == res) return (uint32_t)(frame - w.frame) <= 2u;
    return false;
}

static bool SkModDrewResource(int mod, ID3D12Resource* res)
{
    if (mod < 0 || !res) return false;
    std::lock_guard<std::mutex> l(g_drawvb_mtx);
    if ((size_t)mod >= g_drawvb.size()) return false;
    const SkDrawVbs& d = g_drawvb[mod];
    if (!d.frame || (uint32_t)(g_frame_id - d.frame) > 4u) return false;
    for (unsigned i = 0; i < 8; ++i)
        if (d.res[i] == res) return true;
    return false;
}

using SkFits = std::vector<std::pair<ID3D12Resource*, unsigned>>;

// Narrows `fits` to the one the game transitioned to UNORDERED_ACCESS on THIS list most recently, this
// frame or the previous one (CmdBlock::uav_recent), when exactly one of them was.
static void SkKeepBarrieredHere(command_list* cmd_list, SkFits& fits)
{
    SkFits here;
    if (const CmdBlock* blk = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list))) {
        for (auto n = blk->uav_recent.rbegin(); n != blk->uav_recent.rend() && here.empty(); ++n) {
            if (n->frame + 1 < g_frame_id) break;
            for (const auto& f : fits)
                if (reinterpret_cast<uint64_t>(f.first) == n->res) {
                    here.push_back(f);
                    break;
                }
        }
    }
    if (here.size() == 1) fits.swap(here);
}

// Narrows `fits` to the one whose tracked state includes UNORDERED_ACCESS, when exactly one does.
static void SkKeepInUav(SkFits& fits)
{
    SkFits uav;
    for (const auto& f : fits) {
        const uint32_t s = ResourceStateOf(f.first);
        if (s != MigotoPort::kResStateUnknown && (s & (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            uav.push_back(f);
    }
    if (uav.size() == 1) fits.swap(uav);
}

// Every LIVE buffer (g_va_to_res: inserted at creation, erased at destroy) whose size fits and whose
// desc hash a shapekey callback declares. Returns the count; the single fit, if there is one, in out.
// When a replaced buffer is still alive next to its replacement, the game's own barrier stream decides:
// it transitions u0 to UNORDERED_ACCESS before clearing it (on_barrier -> g_res_state).
int SkFindU0ByGeometry(command_list* cmd_list, unsigned lo, unsigned hi,
    ID3D12Resource** out_res, unsigned* out_bytes)
{
    SkFits fits;
    {
        std::lock_guard lock(g_mem_mutex);
        for (const auto& kv : g_va_to_res) {
            if (!kv.second.res || kv.second.size < lo || kv.second.size > hi) continue;
            if (!PortApi::ShapeKeyCallbackDeclaresHash(PortApi::GameComputeUavBufferHash(kv.second.size))) continue;
            fits.emplace_back(kv.second.res, kv.second.size);
        }
    }
    /* TWO LIVE BUFFERS OF THE DECLARED SIZE -- measured on the character screen as
       one character's in-world and character-screen instances alive together, "more than one fits" on
       1,999 dispatch pairs, with the chain refusing where one answer was right. The game's own
       barrier stream on THIS list says which: it transitions u0 to UNORDERED_ACCESS right before
       clearing it. The most recent such transition, this frame or the previous one, wins. */
    if (fits.size() > 1) SkKeepBarrieredHere(cmd_list, fits);
    if (fits.size() > 1) {
        /* STILL AMBIGUOUS: MEASURED on the character screen, the game barriers neither instance on the
           dispatch's list ("did not single one out"). The character's own draw does name it: the morph
           buffer is bound as a vertex buffer at its claimed draws (SkNoteDrawVbs). The fit that the
           on-screen mod declaring this hash drew within the last frames is the instance in use. */
        SkFits drawn;
        for (const auto& f : fits) {
            const unsigned h = PortApi::GameComputeUavBufferHash(f.second);
            for (int dmo = 0; dmo < (int)g_geomods.size(); ++dmo) {
                if (!ModGeomEnabled(dmo)) continue;
                if (!ModOnScreen(dmo)) continue;
                if (!PortApi::ModDeclaresHash(g_geomods[dmo].wcl_ns.c_str(), h)) continue;
                if (SkModDrewResource(dmo, f.first)) {
                    drawn.push_back(f);
                    break;
                }
            }
        }
        if (drawn.size() == 1) fits.swap(drawn);
    }
    if (fits.size() > 1) SkKeepInUav(fits);
    if (fits.size() == 1) {
        *out_res = fits[0].first;
        *out_bytes = fits[0].second;
    }
    return (int)fits.size();
}

/* u1 BY THE OWNER'S OWN DECLARATION -- for when the register has no record.

   MEASURED: one character's dispatches resolved u0 with `u0_size=0 ck_src=0 ck_age=-1` (the
   descriptor slot had NO record) and u0 was rescued by the geometry search, while u1 -- resolved
   through the same slot map one register along -- stayed UNBOUND for the whole session (u0_clear=1,
   u1_clear=0, with another character on the same screen at u1_clear=1). Earlier the same character had a record
   and u1 was bound, so this is per buffer INSTANCE, not per character. An unbound u1 leaves the ported
   engine's slot holding whoever pushed last, so one character's Loader accumulated into another's sums.

   Every LIVE buffer whose IDENTITY size hashes to the owner's own [TextureOverrideShapeKeyScale] value,
   minus our own relocations (the original is what the game binds; its relocation is re-derived by the
   caller). Disambiguated the way u0's geometry search is, minus the draw-VB rule -- u1 is never a
   vertex buffer. The VA distance from u0 is LOGGED, not used. Returns the count; the single fit out. */
int SkFindU1ByIdentity(command_list* cmd_list, unsigned want_scale, ID3D12Resource* u0,
    ID3D12Resource** out_res, unsigned* out_bytes, char* why, size_t why_len)
{
    SkFits   fits;
    uint64_t u0_va = 0;
    {
        std::lock_guard lock(g_mem_mutex);
        if (u0) {
            auto vi = g_res_to_va.find(reinterpret_cast<uint64_t>(u0));
            if (vi != g_res_to_va.end()) u0_va = vi->second;
        }
        for (const auto& kv : g_va_to_res) {
            if (!kv.second.res || !kv.second.size) continue;
            if (PortApi::GameComputeUavBufferHash(kv.second.size) != want_scale) continue; // identity = born size; ours are dropped below
            fits.emplace_back(kv.second.res, kv.second.size);
        }
    }
    // Drop our own relocations: they carry the widened size and never hash to the declared value, but a
    // relocation whose original was destroyed can still sit in the map under a recycled pointer.
    for (size_t k = 0; k < fits.size();)
        if (g_buffer_reloc.BirthBytes(reinterpret_cast<uint64_t>(fits[k].first), nullptr))
            fits.erase(fits.begin() + k);
        else
            ++k;
    const size_t n_ident = fits.size();
    if (fits.size() > 1) SkKeepBarrieredHere(cmd_list, fits);
    if (fits.size() > 1) SkKeepInUav(fits);
    if (why && why_len) {
        // The VA distance of each remaining fit from u0, signed, in bytes: the measurement for a
        // pool-neighbour rule if one is ever needed. Four fits at most, the rest counted.
        size_t pos = 0;
        pos += (size_t)_snprintf_s(why, why_len, _TRUNCATE, "%zu ident, %zu after barriers:", n_ident, fits.size());
        std::lock_guard lock(g_mem_mutex);
        for (size_t k = 0; k < fits.size() && k < 4 && pos < why_len; ++k) {
            auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(fits[k].first));
            const long long d = (vi != g_res_to_va.end() && u0_va) ? (long long)(vi->second - u0_va) : 0ll;
            const int       w = _snprintf_s(why + pos, why_len - pos, _TRUNCATE, " %p(%u B, %+lld from u0)",
                (void*)fits[k].first, fits[k].second, d);
            if (w < 0) break;
            pos += (size_t)w;
        }
    }
    if (fits.size() == 1) {
        *out_res = fits[0].first;
        *out_bytes = fits[0].second;
    }
    return (int)fits.size();
}
