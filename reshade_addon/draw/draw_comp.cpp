// draw_comp.cpp -- one mod component drawn in place of a game draw: its [TextureOverride] section run
// by their engine, with this draw's hooks (RabbitFX latches, the section's drawindexed, the restore
// callback). See draw_inject.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/claim_state.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/skeleton.h"
#include "../mods/port_section.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cb_ring.h" // OBJCB-BORROW-PREV: copies of an object CB in a buffer of ours
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../mods/shader_alias.h"
#include "../hooks/pipeline_hooks.h"
#include "../rt/rt_hooks.h"
#include "draw_ctx.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include "../mods/wwmi_mod.h"
#include "../migoto/dx12_bridge.h"
#include "../textures/tex_crawl.h"

struct CompBoneCb {
    uint64_t va = 0;
    uint32_t frame = 0;
};
static std::mutex                               g_comp_bone_mtx;
static std::unordered_map<uint32_t, CompBoneCb> g_comp_bone; // key = (mod << 8) | comp

constexpr unsigned kSkMatchBones = 32;
struct SkBones {
    unsigned n = 0;
    float    t[kSkMatchBones * 3] = {};
};
struct SkCopyCb {
    uint64_t va = 0; // the component's bone CB this frame
    SkBones  bones;  // read at its draw: by the next frame the game may have released the CB
};
struct SkCopySeen {
    uint64_t              key = 0;
    unsigned              slot = 0;
    std::vector<SkCopyCb> cb4;
    std::vector<uint64_t> keys;
    std::vector<uint64_t> cb4_drawn;
};
struct SkCopyMod {
    uint32_t                          frame = 0;
    bool                              noted = false;
    bool                              off = false;
    unsigned                          oversplit = 0;
    std::vector<SkCopySeen>                    cur;
    std::vector<unsigned>                      prev_slots;
    std::vector<std::vector<SkBones>>          prev_bones;
    std::vector<std::pair<uint64_t, unsigned>> prev_keys; // last frame's vs-cb2 -> skeleton, joined ones included
};
constexpr unsigned            kSkOversplitFrames = 3;
static std::mutex             g_skcopy_mtx;
static std::vector<SkCopyMod> g_skcopy;
static unsigned               g_skcopy_gen = ~0u;

static SkBones SkBonesOf(ID3D12Resource* res, uint64_t va)
{
    float          m[kSkMatchBones * 12] = {};
    const uint32_t got = SnapshotGameCB(res, va, reinterpret_cast<uint8_t*>(m), sizeof m, true);
    SkBones        b;
    b.n = got / 48;
    for (unsigned i = 0; i < b.n; ++i) {
        b.t[i * 3] = m[i * 12 + 3];
        b.t[i * 3 + 1] = m[i * 12 + 7];
        b.t[i * 3 + 2] = m[i * 12 + 11];
    }
    return b;
}

static double SkBonesRms(const SkBones& a, const SkBones& b)
{
    const unsigned n = a.n < b.n ? a.n : b.n;
    if (!n) return -1.0;
    double d = 0;
    for (unsigned k = 0; k < n * 3; ++k) d += (double)(a.t[k] - b.t[k]) * (a.t[k] - b.t[k]);
    return std::sqrt(d / n);
}

static RootCbvBind ObjectCbOf(reshade::api::command_list* cl)
{
    int root = -1;
    {
        std::lock_guard l(g_layout_mutex);
        auto            it = g_layout_b2_root.find(CmdGfxLayout(cl));
        if (it != g_layout_b2_root.end()) root = it->second;
    }
    return (root >= 0 && root < 64) ? CmdCbv(cl).binds[root] : RootCbvBind{};
}

static void SkCopyRoll(SkCopyMod& m, int dm)
{
    m.prev_slots.clear();
    m.prev_bones.clear();
    size_t comps = 0;
    size_t active = 0;
    for (const SkCopySeen& e : m.cur) {
        m.prev_slots.push_back(e.slot);
        comps = e.cb4.size() > comps ? e.cb4.size() : comps;
        for (const SkCopyCb& cb : e.cb4)
            if (cb.va) {
                ++active;
                break;
            }
    }
    size_t most = 0;
    for (size_t c = 0; c < comps; ++c) {
        size_t n = 0;
        for (const SkCopySeen& e : m.cur)
            if (c < e.cb4.size() && e.cb4[c].va) ++n;
        most = n > most ? n : most;
    }
    m.oversplit = (most && active > most) ? m.oversplit + 1 : 0;
    if (m.oversplit >= kSkOversplitFrames) {
        m.off = true;
        logger::write("SKCOPIES mod=%d f=%u: for %u frames, %zu vs-cb2 addresses drew components no two of them share -- "
                      "vs-cb2 does not tell this character's copies apart; they share one skeleton until the next load",
            dm, m.frame, m.oversplit, active);
    }
    if (m.cur.size() >= 2) {
        unsigned top = 0;
        for (const SkCopySeen& e : m.cur) top = e.slot > top ? e.slot : top;
        m.prev_bones.assign(top + 1, std::vector<SkBones>());
        for (const SkCopySeen& e : m.cur) {
            std::vector<SkBones>& pb = m.prev_bones[e.slot];
            pb.resize(e.cb4.size());
            for (size_t c = 0; c < e.cb4.size(); ++c)
                if (e.cb4[c].va) pb[c] = e.cb4[c].bones;
        }
    }
    m.prev_keys.clear();
    for (const SkCopySeen& e : m.cur) {
        m.prev_keys.emplace_back(e.key, e.slot);
        for (const uint64_t k : e.keys) m.prev_keys.emplace_back(k, e.slot);
    }
    m.cur.clear();
}

static unsigned SkCopySlot(reshade::api::command_list* cl, int dm, int comp, ID3D12Resource* cb4_res, uint64_t cb4_va,
    bool shadow, bool* first_sight)
{
    *first_sight = false;
    if (!g_skel_copies || dm < 0 || comp < 0) return 0;
    const uint64_t key = ObjectCbOf(cl).va;
    if (!key) return 0;
    std::lock_guard l(g_skcopy_mtx);
    const unsigned  gen = PortApi::LoadGeneration();
    if (g_skcopy_gen != gen) {
        g_skcopy_gen = gen;
        g_skcopy.clear();
    }
    if (g_skcopy.size() <= (size_t)dm) g_skcopy.resize((size_t)dm + 1);
    SkCopyMod& m = g_skcopy[dm];
    if (!m.noted) {
        m.noted = true;
        *first_sight = true;
    }
    if (m.off) return 0;
    if (m.frame != g_frame_id) {
        SkCopyRoll(m, dm);
        m.frame = g_frame_id;
        if (m.off) return 0;
    }
    size_t at = m.cur.size();
    for (size_t i = 0; i < m.cur.size(); ++i)
        if (m.cur[i].key == key || std::find(m.cur[i].keys.begin(), m.cur[i].keys.end(), key) != m.cur[i].keys.end()) at = i;
    if (at == m.cur.size() && cb4_va)
        for (size_t i = 0; i < m.cur.size(); ++i) {
            SkCopySeen& o = m.cur[i];
            if (std::find(o.cb4_drawn.begin(), o.cb4_drawn.end(), cb4_va) == o.cb4_drawn.end()) continue;
            at = i;
            o.keys.push_back(key);
            if (DiagOnce("SKCOPIES-JOINED", dm))
                logger::write("SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx is drawn with bone CB %llx, already drawn this frame "
                              "under vs-cb2 %llx -- the same copy, on its skeleton #%u",
                    dm, comp, g_frame_id, (unsigned long long)key, (unsigned long long)cb4_va, (unsigned long long)o.key,
                    o.slot);
            if (ProbeArmed())
                logger::report("SKCOPY f=%u mod=%d comp=%d vs-cb2=%llx cb4=%llx%s -> skeleton #%u (its bone CB was drawn under "
                               "vs-cb2 %llx)",
                    g_frame_id, dm, comp, (unsigned long long)key, (unsigned long long)cb4_va, shadow ? " shadow" : "", o.slot,
                    (unsigned long long)o.key);
            break;
        }
    auto prev_slot_of = [&](uint64_t k) {
        unsigned s = ~0u;
        for (const auto& pk : m.prev_keys)
            if (pk.first == k) s = pk.second;
        return s;
    };
    const unsigned was = prev_slot_of(key);
    if (at == m.cur.size() && was != ~0u)
        for (size_t i = 0; i < m.cur.size(); ++i)
            if (m.cur[i].slot == was && prev_slot_of(m.cur[i].key) == was) {
                at = i;
                m.cur[i].keys.push_back(key);
                break;
            }
    if (at == m.cur.size()) {
        std::vector<char> taken;
        for (const SkCopySeen& e : m.cur) {
            if (taken.size() <= e.slot) taken.resize(e.slot + 1, 0);
            taken[e.slot] = 1;
        }
        auto        free_slot = [&](unsigned k) { return k >= taken.size() || !taken[k]; };
        unsigned    slot = ~0u;
        double      best = -1;
        const char* why = "a new copy";
        if (was != ~0u && free_slot(was)) {
            slot = was;
            why = "its skeleton of last frame";
        }
        if (slot == ~0u && !m.prev_bones.empty() && cb4_res && cb4_va) {
            const SkBones mine = SkBonesOf(cb4_res, cb4_va);
            for (unsigned k : m.prev_slots) {
                if (!free_slot(k) || k >= m.prev_bones.size()) continue;
                const std::vector<SkBones>& pbs = m.prev_bones[k];
                for (size_t c = 0; c < pbs.size(); ++c) {
                    if (c != (size_t)comp) continue;
                    const double d = SkBonesRms(mine, pbs[c]);
                    if (d >= 0 && (best < 0 || d < best)) {
                        best = d;
                        slot = k;
                    }
                }
            }
            if (slot != ~0u) why = "nearest bones of last frame";
        }
        if (slot == ~0u)
            for (unsigned k : m.prev_slots)
                if (free_slot(k)) {
                    slot = k;
                    why = "the copy of last frame";
                    break;
                }
        if (slot == ~0u) {
            slot = 0;
            while (!free_slot(slot)) ++slot;
        }
        m.cur.push_back(SkCopySeen{ key, slot });
        if (was != ~0u && was != slot && g_debug && LogBudget("SKCOPIES-MOVED", g_geomods[dm].wcl_ns, 64))
            logger::write("SKCOPIES-MOVED mod=%d comp=%d f=%u: vs-cb2 %llx drew on skeleton #%u last frame, now on #%u (%s, rms %.3f)",
                dm, comp, g_frame_id, (unsigned long long)key, was, slot, why, best);
        if (was == ~0u && m.prev_keys.size() >= 2 && DiagOnce("SKCOPIES-NEWKEY", dm))
            logger::write("SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx was not drawn last frame -- with copies on screen, "
                          "its address is new each frame, so only bones tell the copies apart",
                dm, comp, g_frame_id, (unsigned long long)key);
        if (slot && DiagOnce("SKCOPIES-SECOND", dm))
            logger::write("SKCOPIES mod=%d comp=%d f=%u: another copy of this character (vs-cb2 %llx) -- it merges and draws "
                          "with its own skeleton #%u",
                dm, comp, g_frame_id, (unsigned long long)key, slot);
        if (ProbeArmed())
            logger::report("SKCOPY f=%u mod=%d comp=%d vs-cb2=%llx cb4=%llx%s -> skeleton #%u (%s, rms %.3f)",
                g_frame_id, dm, comp, (unsigned long long)key, (unsigned long long)cb4_va, shadow ? " shadow" : "", slot,
                why, best);
    }
    SkCopySeen& e = m.cur[at];
    if (cb4_va && std::find(e.cb4_drawn.begin(), e.cb4_drawn.end(), cb4_va) == e.cb4_drawn.end()) e.cb4_drawn.push_back(cb4_va);
    if (!shadow && cb4_va) {
        if (e.cb4.size() <= (size_t)comp) e.cb4.resize((size_t)comp + 1);
        if (!e.cb4[comp].va) {
            e.cb4[comp].va = cb4_va;
            e.cb4[comp].bones = SkBonesOf(cb4_res, cb4_va);
        } else if (e.cb4[comp].va != cb4_va && DiagOnce("SKCOPIES-SHARED", dm)) {
            logger::write("SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx is drawn with two bone CBs (%llx, %llx) -- copies under "
                          "one per-object address share one skeleton",
                dm, comp, g_frame_id, (unsigned long long)key, (unsigned long long)e.cb4[comp].va,
                (unsigned long long)cb4_va);
        }
    }
    return e.slot;
}

static void PublishDrawShaders(reshade::api::command_list* cmd_list, uint64_t vs_hash)
{
    if (!g_publish_shader_state) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t ps = AliasShaderHash(ps_hash);
    PortApi::SetBoundShader('v', AliasShaderHash(vs_hash));
    PortApi::SetBoundShader('p', ps);
    if (!g_rfx_patch || !ps || PortApi::ShaderRegexLinked(ps) || !PortApi::ShaderRegexGroupLoaded(kRfxNamespace, kRfxRegexGroup)) return;
    unsigned ps_max = 0;
    bool     has_glow = false;
    if (GetRfxPatchVariant(cmd_list->get_device(), CmdGfx(cmd_list).gfx_pso, &ps_max, &has_glow, g_decensor))
        PortApi::LinkShaderRegexGroup(ps, kRfxNamespace, kRfxRegexGroup);
}

constexpr uint32_t                            kSectexGap = 30; // frames unseen before a texture's next sighting is logged again
static std::mutex                             g_sectex_mtx;
static std::unordered_map<uint64_t, uint32_t> g_sectex_last; // hash | unread << 32 -> last frame a section saw it

struct PsTexture {
    unsigned   reg = 0;
    bool       read = false;
    TableStart ts;
    uint32_t   offset = 0;
    ResInfo    ri;
};

// Each pixel-shader SRV register of the bound layout that the pipeline reads (with `unread`, the rest too) and
// `want(reg)` accepts, resolved to the resource behind it. False when the layout has no register map.
template <typename Want, typename Fn>
static bool ForEachPsTexture(reshade::api::command_list* cmd_list, bool unread, const DxilSrvRegs& ps_regs, Want&& want,
    Fn&& fn)
{
    const uint64_t layh = CmdGfxLayout(cmd_list);
    RegBind        t_ps[kSectionTexRegs];
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(layh);
        if (it == g_layout_regmap.end() || !it->second.built) return false;
        memcpy(t_ps, it->second.t_ps, sizeof t_ps);
    }
    device*     dev = cmd_list->get_device();
    const auto& root = CmdGfx(cmd_list).root;
    for (unsigned r = 0; r < kSectionTexRegs; ++r) {
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
        PsTexture t;
        t.reg = r;
        t.read = ps_regs.reads(r);
        if ((!t.read && !unread) || !want(r)) continue;
        t.ts = TableStartOf(dev, root[b.param].table);
        if (!t.ts) continue;
        t.offset = b.offset;
        if (!g_heap_to_res.find({ t.ts.heap, t.ts.base + b.offset }, t.ri) || !t.ri.res) continue;
        fn(t);
    }
    return true;
}

void CarryPsReads(const DrawArgs& da)
{
    if (!g_ps_slot_queries.load(std::memory_order_relaxed)) return;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    CmdState&                         gs = CmdGfx(cmd_list);
    if (!InSkinBand(gs.vb[0].va)) return;
    PROF(PROF_DI_PsCarry);
    const uint64_t layh = CmdGfxLayout(cmd_list);
    if (!layh) return;
    PsCarry&       pc = gs.ps_carry;
    const uint64_t gen = CmdGen(cmd_list);
    if (pc.layout != layh || pc.layout_gen != gen) {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(layh);
        if (it == g_layout_regmap.end() || !it->second.built) return;
        for (unsigned r = 0; r < kSectionTexRegs; ++r) {
            const RegBind& b = it->second.t_ps[r];
            pc.param[r] = b.param < 64 && b.is_table ? b.param : 0xFF;
            pc.offset[r] = b.offset;
        }
        pc.layout = layh;
        pc.layout_gen = gen;
    }
    const uint64_t reads = PipelinePsReadMask(gs.gfx_pso);
    for (unsigned r = 0; r < kSectionTexRegs; ++r)
        if (((reads >> r) & 1u) && pc.param[r] != 0xFF) pc.slot[r] = PsCarry::Slot{ gs.root[pc.param[r]].table, pc.offset[r], gen };
}

struct CarryKey {
    int      dm = 0;
    int      comp = 0;
    uint64_t ps = 0;
    unsigned reg = 0;
    bool     operator<(const CarryKey& o) const
    {
        return std::tie(dm, comp, ps, reg) < std::tie(o.dm, o.comp, o.ps, o.reg);
    }
};
struct CarriedTexture {
    ID3D12Resource* res = nullptr;
    uint32_t        hash = 0;
    unsigned        fmt = 0;
    uint32_t        frame = 0;
};
// What the list carry last published for one component's pass and register, referenced so it outlives the
// game's release of it.
class CarryMemo {
public:
    void Remember(const CarryKey& k, ID3D12Resource* res, uint32_t hash, unsigned fmt)
    {
        std::lock_guard l(m_mtx);
        Roll();
        CarriedTexture& e = m_map[k];
        if (e.res != res) {
            res->AddRef();
            if (e.res) e.res->Release();
            e.res = res;
        }
        e.hash = hash;
        e.fmt = fmt;
        e.frame = g_frame_id;
    }
    // With a reference the caller releases.
    bool Recall(const CarryKey& k, CarriedTexture& out)
    {
        std::lock_guard l(m_mtx);
        Roll();
        auto it = m_map.find(k);
        if (it == m_map.end()) return false;
        out = it->second;
        out.res->AddRef();
        return true;
    }

private:
    void Roll()
    {
        const unsigned gen = PortApi::LoadGeneration();
        if (gen == m_gen) return;
        m_gen = gen;
        for (auto& kv : m_map) kv.second.res->Release();
        m_map.clear();
    }
    std::mutex                         m_mtx;
    std::map<CarryKey, CarriedTexture> m_map;
    unsigned                           m_gen = 0;
};
static CarryMemo g_carry_memo;

static uint16_t PublishCarriedTextures(reshade::api::command_list* cmd_list, int dm, int comp, const DxilSrvRegs& ps_regs,
    uint16_t pushed, uint64_t ps_hash)
{
    const PsCarry& pc = CmdGfx(cmd_list).ps_carry;
    const uint64_t gen = CmdGen(cmd_list);
    device*        dev = cmd_list->get_device();
    uint16_t       carried = 0;
    for (unsigned r = 0; r < kSectionTexRegs; ++r) {
        if (ps_regs.reads(r) || ((pushed >> r) & 1u)) continue;
        const CarryKey       key{ dm, comp, ps_hash, r };
        const PsCarry::Slot& s = pc.slot[r];
        const TableStart     ts = s.table.handle && s.gen == gen ? TableStartOf(dev, s.table) : TableStart{};
        ResInfo              ri;
        const bool           listed = ts && g_heap_to_res.find({ ts.heap, ts.base + s.offset }, ri) && ri.res;
        CarriedTexture       ct;
        if (listed) {
            const uint32_t resident = TexHashOf(reinterpret_cast<uint64_t>(ri.res));
            const uint32_t hash = !resident || PortApi::TextureHashHasSections(resident) ? resident : TexLodTargetOf(resident);
            ct.res = ri.res;
            ct.hash = hash && PortApi::TextureHashHasSections(hash) ? hash : resident;
            ct.fmt = ri.view_handle ? (unsigned)dev->get_resource_view_desc(resource_view{ ri.view_handle }).format : 0u;
            g_carry_memo.Remember(key, ct.res, ct.hash, ct.fmt);
        }
        const bool recalled = !listed && g_carry_memo.Recall(key, ct);
        if (listed || recalled) {
            PortApi::PushGameTexture('p', r, ct.res, ct.hash, ct.fmt);
            carried |= (uint16_t)(1u << r);
        }
        if (recalled) ct.res->Release();
        const unsigned src = listed ? 1u : recalled ? 2u : 0u;
        if (g_debug && DiagChanged("SECTEX-CARRY", (int)(((unsigned)CompKey(dm, comp) * 16u + r) ^ (unsigned)(ps_hash * 0x9E3779B97F4A7C15ull >> 40)), (unsigned long long)ct.hash << 32 | ct.fmt << 2 | src))
            logger::write("SECTEX-CARRY f=%u mod=%d comp=%d ps-t%u ps=%016llx does not read it: %s%u -- %08x fmt=%u",
                g_frame_id, dm, comp, r, (unsigned long long)ps_hash,
                listed ? "the list's last character draw that read it, frame " : recalled ? "nothing on this list; remembered from frame " : "nothing on this list, nothing remembered, frame ",
                recalled ? ct.frame : g_frame_id, ct.hash, ct.fmt);
    }
    return carried;
}

static uint16_t PublishSectionTextures(reshade::api::command_list* cmd_list, int dm, int comp, uint32_t rtv_count)
{
    const DxilSrvRegs ps_regs = PipelineSrvRegs(CmdGfx(cmd_list).gfx_pso, 'p');
    uint16_t          pushed = 0;
    uint64_t          ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t pass = TexLookPlaceOf(g_geomods[dm].wcl_ns.c_str(), comp, ps_hash);
    const bool     every_slot = g_geomods[dm].mod.queries_ps_slots;
    device*        dev = cmd_list->get_device();
    ForEachPsTexture(cmd_list, g_debug, ps_regs, [](unsigned) { return true; }, [&](const PsTexture& t) {
        const unsigned    r = t.reg;
        const bool        read = t.read;
        const TableStart& ts = t.ts;
        const ResInfo&    ri = t.ri;
        const uint32_t    resident = TexHashOf(reinterpret_cast<uint64_t>(ri.res));
        if (read && resident)
            TexLookAtPlace(pass, r, resident, PortApi::TextureHashHasSections(resident) ? resident : TexLodKnown(resident));
        const uint32_t hash = !resident || PortApi::TextureHashHasSections(resident)
            ? resident
            : TexLodTargetOf(resident);
        const bool known = hash && PortApi::TextureHashHasSections(hash);
        if (!known && !(every_slot && read)) return;
        if (read) {
            const unsigned fmt = every_slot && ri.view_handle
                ? (unsigned)dev->get_resource_view_desc(resource_view{ ri.view_handle }).format
                : 0u;
            PortApi::PushGameTexture('p', r, ri.res, known ? hash : resident, fmt);
            pushed |= (uint16_t)(1u << r);
        }
        if (!known) return;
        if (g_debug) {
            bool appeared;
            {
                std::lock_guard l(g_sectex_mtx);
                uint32_t&       last = g_sectex_last[(uint64_t)hash | (read ? 0ull : 1ull << 32)];
                appeared = !last || g_frame_id - last > kSectexGap;
                last = g_frame_id;
            }
            if (appeared || g_sectex_watch.load(std::memory_order_relaxed) > 0) {
                char lod[24] = "";
                if (hash != resident) sprintf_s(lod, " (its mip %08x)", resident);
                logger::write("SECTEX f=%u mod=%d comp=%d ps-t%u rtv=%u: %08x%s res=%p at %llx+%u, written %u frame(s) "
                              "ago (src %u), ps=%016llx %s -- %s",
                    g_frame_id, dm, comp, r, rtv_count, hash, lod, (void*)ri.res, (unsigned long long)ts.heap,
                    ts.base + t.offset, g_frame_id - ri.wrote_frame, (unsigned)ri.wrote_src,
                    (unsigned long long)ps_hash,
                    !read ? "does NOT read it: not published" : ps_regs.parsed ? "reads it"
                                                                               : "(no PSV) published",
                    appeared ? "first sighting in 30+ frames" : "watch: a mod switched itself back on");
            }
        }
    });
    if (every_slot) pushed |= PublishCarriedTextures(cmd_list, dm, comp, ps_regs, pushed, ps_hash);
    return pushed;
}

void NoteMeshPlaces(const DrawArgs& da)
{
    const bool crawl = TexCrawlOn();
    if (!crawl && !TexLookHasMeshPlaces()) return;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12Resource*                   ibr = nullptr;
    ID3D12Resource*                   vbr = nullptr;
    uint64_t                          ibva = 0;
    uint64_t                          vbva = 0;
    uint32_t                          idxb = 2;
    uint32_t                          vstride = 0;
    {
        const auto& st = CmdGfx(cmd_list);
        if (!InSkinBand(st.vb[0].va) && !st.vb[6].res) return;
        ibr = st.ib.res;
        ibva = st.ib.va;
        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
        vbr = st.vb[0].res;
        vbva = st.vb[0].va;
        vstride = st.vb[0].stride;
    }
    if (!ibr || !vbr || !vstride) return;
    const uint32_t ib_hash = IbHash::Lookup(ibr, (uint32_t)(ibva - ibr->GetGPUVirtualAddress()),
        (da.first_index + da.index_count) * idxb, idxb, vbr, (uint32_t)(vbva - vbr->GetGPUVirtualAddress()), vstride,
        /*grow*/ true);
    if (!ib_hash) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t pass = TexLookMeshPassOf(ib_hash, da.first_index, da.index_count, ps_hash);
    const uint32_t mask = crawl ? ~0u : TexLookMeshMask(pass, kSectionTexRegs);
    if (!mask) return;
    const DxilSrvRegs ps_regs = PipelineSrvRegs(CmdGfx(cmd_list).gfx_pso, 'p');
    ForEachPsTexture(
        cmd_list, false, ps_regs, [&](unsigned r) { return ((mask >> r) & 1u) != 0; },
        [&](const PsTexture& t) {
            const uint32_t resident = TexHashOf(reinterpret_cast<uint64_t>(t.ri.res));
            if (!resident) return;
            const uint64_t place = TexLookSlotOf(pass, t.reg);
            if (crawl)
                TexCrawlNote(place, resident);
            else
                TexLookAtMeshPlace(place, resident,
                    PortApi::TextureHashHasSections(resident) ? resident : TexLodKnown(resident));
        });
}

bool DrawComponent(InjectFrame& fr, int dm, const WwmiMod::Component* wc)
{
    const DrawArgs&                   da = fr.da;
    DrawClaim&                        dc = fr.dc;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const  d3d12_cl = da.d3d12_cl;
    const uint32_t                    index_count = da.index_count;
    const uint32_t                    first_index = da.first_index;
    const int32_t                     vertex_offset = da.vertex_offset;
    const uint32_t                    first_instance = da.first_instance;
    const uint64_t                    vs_hash = da.vs_hash;
    const CmdRT&                      draw_rt = da.draw_rt;
    uint64_t&                         game_cb13_va = dc.cb13_va;
    ID3D12Resource*&                  port_cb4_res = dc.port_cb4_res;
    uint64_t&                         port_cb4_va = dc.port_cb4_va;
    ID3D12Resource*&                  port_cb3_res = dc.port_cb3_res;
    uint64_t&                         port_cb3_va = dc.port_cb3_va;
    int&                              game_cb13_root = dc.cb13_root;
    int&                              game_cb14_root = dc.cb14_root;
    bool&                             is_full_shadow_pass = dc.full_shadow;
    unsigned&                         reloc_vb_mask = fr.reloc_vb_mask;
    RelocXition(&reloc_xit)[9] = fr.reloc_xit;
    unsigned&  reloc_xit_n = fr.reloc_xit_n;
    const bool save_ib_set = fr.save_ib_set;
    bool&      x32_mid = fr.x32_mid;
    unsigned&  x32_mid_restores = fr.x32_mid_restores;
    unsigned&  x32_mid_skipped = fr.x32_mid_skipped;
    auto       restore_state = [&]() { RestoreGameState(fr); };
    PROF(PROF_DI_DrawComp);
    if (dm < 0 || dm >= (int)g_geomods.size()) return false;
    // Defensive gate for full-shadow and other routes that bypass ClaimDraw.
    if (!ModTexturesReady(dm)) return false;
    uint32_t did = 0;
    if (g_wcl_draw && wc) {
        // DI:CompSetup: everything before PortSect; ended right before it (a span, not a block).
        ProfScope           _di_compsetup(PROF_DI_CompSetup);
        ProfScope           _cs_prep(PROF_CS_Prep);
        const std::wstring& uns = g_geomods[dm].wcl_ns;
        const std::wstring  fq = PortComponentSection(dm, wc->index, wc->tail, uns);

        if (!fq.empty()) {
            bool      skip_orig = false;
            PortHooks hk;
            hk.handling_skip = [&] { skip_orig = true; };
            static std::wstring s_rfx_glowmap_fq;
            static std::wstring s_rfx_fxmap_fq;
            std::wstring&       rfx_glowmap_fq = s_rfx_glowmap_fq; // last GlowMap ref no Run has consumed yet
            std::wstring&       rfx_fxmap_fq = s_rfx_fxmap_fq;     // last FXMap ref: the cutout's input
            RfxDrawState        rfx_own;
            RfxDrawState&       rfx = fr.rfx_shared ? *fr.rfx_shared : rfx_own; // draw_inject.h
            RfxGlowState&       rfx_glow = rfx.glow;
            std::wstring&       rfx_diffuse_fq = rfx.diffuse_fq;
            std::wstring&       rfx_normal_fq = rfx.normal_fq;
            std::wstring&       rfx_light_fq = rfx.light_fq;
            std::wstring&       rfx_material_fq = rfx.material_fq;
            std::wstring&       rfx_cutout_fq = rfx.cutout_fq;
            std::wstring&       rfx_special_fq = rfx.special_fq;
            RfxTexLatch&        rfx_tex = rfx.tex;
            hk.resource_ref = [&](const std::wstring& lhs_fq, const std::wstring& rhs_fq) {
                if (lhs_fq.find(L"rabbitfx") == std::wstring::npos) return;
                if (lhs_fq.find(L"glowmap") != std::wstring::npos)
                    rfx_glowmap_fq = rhs_fq;
                else if (lhs_fq.find(L"fxmap") != std::wstring::npos)
                    rfx_fxmap_fq = rhs_fq;
                else if (lhs_fq.find(L"diffuse") != std::wstring::npos)
                    rfx_diffuse_fq = rhs_fq;
                else if (lhs_fq.find(L"normalmap") != std::wstring::npos)
                    rfx_normal_fq = rhs_fq;
                else if (lhs_fq.find(L"lightmap") != std::wstring::npos)
                    rfx_light_fq = rhs_fq;
                else if (lhs_fq.find(L"materialmap") != std::wstring::npos)
                    rfx_material_fq = rhs_fq;
                else if (lhs_fq.find(L"cutoutmap") != std::wstring::npos)
                    rfx_cutout_fq = rhs_fq;
                else if (lhs_fq.find(L"specialmap") != std::wstring::npos)
                    rfx_special_fq = rhs_fq;
            };
            hk.run_cmd = [&](const std::wstring& /*n*/, const std::wstring& target_fq) {
                if (target_fq.find(L"rabbitfx") == std::wstring::npos) return;
                // SetTextures (texture-binding half).
                if (target_fq.find(L"settextures") != std::wstring::npos) {
                    auto                        srv = [&](const std::wstring& fq) { return RfxMapSrv(fq, uns.c_str()); };
                    D3D12_CPU_DESCRIPTOR_HANDLE d = srv(rfx_diffuse_fq);
                    D3D12_CPU_DESCRIPTOR_HANDLE n = srv(rfx_normal_fq);
                    D3D12_CPU_DESCRIPTOR_HANDLE l = srv(rfx_light_fq);
                    D3D12_CPU_DESCRIPTOR_HANDLE m = srv(rfx_material_fq);
                    const bool                  pending = (!rfx_diffuse_fq.empty() && !d.ptr) || (!rfx_normal_fq.empty() && !n.ptr) ||
                        (!rfx_light_fq.empty() && !l.ptr) || (!rfx_material_fq.empty() && !m.ptr);
                    rfx_tex = pending ? RfxTexLatch{} : RfxTexLatch{ d, n, l, m }; // the draws below read the latch, not the strings
                    if (g_debug) {
                        const D3D12_CPU_DESCRIPTOR_HANDLE cut = srv(rfx_cutout_fq);
                        const D3D12_CPU_DESCRIPTOR_HANDLE spc = srv(rfx_special_fq);
                        const int                         k = CompKey(dm, wc ? wc->index : 0);
                        const int                         mask = (d.ptr ? 1 : 0) | (n.ptr ? 2 : 0) | (l.ptr ? 4 : 0) | (m.ptr ? 8 : 0) |
                            (cut.ptr ? 16 : 0) | (spc.ptr ? 32 : 0);
                        if (DiagChanged("RABBITFX-TEX", k, (unsigned long long)(unsigned)mask))
                            logger::write("RABBITFX-TEX f=%u mod=%d comp=%d diffuse=%ls(srv=%llx) "
                                          "normal=%ls(srv=%llx) light=%ls(srv=%llx) material=%ls(srv=%llx) "
                                          "cutout=%ls(srv=%llx, not bound on DX12) special=%ls(srv=%llx, not bound on DX12)%s",
                                g_frame_id, dm, wc ? wc->index : -1,
                                rfx_diffuse_fq.c_str(), (unsigned long long)d.ptr,
                                rfx_normal_fq.c_str(), (unsigned long long)n.ptr,
                                rfx_light_fq.c_str(), (unsigned long long)l.ptr,
                                rfx_material_fq.c_str(), (unsigned long long)m.ptr,
                                rfx_cutout_fq.c_str(), (unsigned long long)cut.ptr,
                                rfx_special_fq.c_str(), (unsigned long long)spc.ptr,
                                pending ? "  -- a named map is not loaded: the game's textures stay" : "");
                    }
                    return;
                }
                float                       h = PortApi::GetVar(L"rabbitfx\\h", 0.f);
                float                       br = PortApi::GetVar(L"rabbitfx\\brightness", 0.f);
                float                       ip = PortApi::GetVar(L"rabbitfx\\interpolate", 0.f);
                float                       s = PortApi::GetVar(L"rabbitfx\\s", 0.f);
                float                       v = PortApi::GetVar(L"rabbitfx\\v", 0.f);
                D3D12_CPU_DESCRIPTOR_HANDLE glow = rfx_glowmap_fq.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{ 0 }
                                                                          : WclModSrvByResourceFq(rfx_glowmap_fq);
                D3D12_CPU_DESCRIPTOR_HANDLE fxm = rfx_fxmap_fq.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{ 0 }
                                                                       : WclModSrvByResourceFq(rfx_fxmap_fq);
                rfx_glow.hue = (h / 360.0f) * ip;
                rfx_glow.sat = (s / 100.0f) * ip;
                rfx_glow.val = (v / 100.0f) * ip;
                rfx_glow.brightness = br;
                rfx_glow.map = glow;
                rfx_glow.fxmap = fxm;
                rfx_glow.active = (glow.ptr != 0); // brightness==0 is RECOLOUR, not "off" -- the ini writes o3 either way
                if (rfx_glow.active) ++g_rfx_glow_params;
                if (g_debug && DiagOnce("RABBITFX-INVOKE", CompKey(dm, wc ? wc->index : 0) * 8 + (int)((glow.ptr ? 1u : 0u) | (fxm.ptr ? 2u : 0u) | (br != 0.f ? 4u : 0u)))) {
                    logger::write("RABBITFX invoke mod=%d comp=%d h=%.1f s=%.1f v=%.1f brightness=%.2f interpolate=%.2f glowmap=%ls srv=%llx fxmap=%ls srv=%llx -> glow %s%s",
                        dm, wc ? wc->index : -1, h, s, v, br, ip, rfx_glowmap_fq.c_str(), (unsigned long long)glow.ptr,
                        rfx_fxmap_fq.c_str(), (unsigned long long)fxm.ptr,
                        rfx_glow.active ? "ARMED" : "inactive(no glow map)",
                        fxm.ptr ? "; FX map present" : "");
                }
            };
            ID3D12Resource* x24_vb6_res = nullptr;
            uint64_t        x24_vb6_va = 0;
            uint32_t        x24_vb6_bytes = 0;
            uint8_t         x24_vb6_verdict = 0;
            bool            rtpal_no_merge = false; // RTPAL published a palette for a component that merges no skeleton
            SectionDraw     sd{ fr, dm, wc, hk, did, rfx_glow, rfx_tex,
                x24_vb6_res, x24_vb6_va, x24_vb6_bytes, x24_vb6_verdict, rtpal_no_merge };
            hk.drawindexed = [&](uint32_t c, uint32_t s2, uint32_t b2) { SectionDrawIndexed(sd, c, s2, b2); };
            {
                static std::atomic<bool> once{ false };
                if (!once.exchange(true))
                    logger::write("PORTDRAW first component dispatch: mod=%d comp=%d fq='%ls'",
                        dm, wc->index, fq.c_str());
            }
            _cs_prep.End();
            std::optional<PortApi::EngineScope> port_scope;
            {
                PROF(PROF_DI_EngLock);
                port_scope.emplace();
            }
            ProfScope _cs_restore(PROF_CS_Restore);
            struct PortRestore {
                ID3D12GraphicsCommandList*             cl = nullptr;
                ID3D12DescriptorHeap*                  srv = nullptr;
                ID3D12DescriptorHeap*                  smp = nullptr;
                std::vector<std::pair<UINT, uint64_t>> tables;
                void*                                  inner_user = nullptr;
                bool*                                  mid = nullptr; // X32: raised around the inner call so restore_state knows it is mid-section
                ID3D12PipelineState*                   pso = nullptr;
            } port_restore;
            {
                const auto&                   root = CmdGfx(cmd_list).root;
                const uint64_t                layh = CmdGfxLayout(cmd_list);
                std::vector<LayoutTableParam> ltabs;
                if (layh) {
                    std::lock_guard l(g_layout_tables_mtx);
                    auto            it = g_layout_tables.find(layh);
                    if (it != g_layout_tables.end()) ltabs = it->second;
                }
                for (const auto& t : ltabs) {
                    const TableStart ts = TableStartOf(cmd_list->get_device(), root[t.param].table);
                    if (!ts) continue;
                    auto* hp = ts.Heap();
                    if (t.kind == 1 && !port_restore.srv) port_restore.srv = hp;
                    if (t.kind == 0 && !port_restore.smp) port_restore.smp = hp;
                    port_restore.tables.emplace_back((UINT)t.param, (uint64_t)root[t.param].table.handle);
                }
                if (ID3D12DescriptorHeap* gh = GameHeap(cmd_list, 0)) port_restore.srv = gh;
                if (ID3D12DescriptorHeap* gh = GameHeap(cmd_list, 1)) port_restore.smp = gh;
                port_restore.cl = d3d12_cl;
                port_restore.inner_user = &restore_state;
                port_restore.mid = &x32_mid; // X32
                {
                    port_restore.pso = reinterpret_cast<ID3D12PipelineState*>(CmdGfx(cmd_list).gfx_restore());
                }
            }
            static thread_local int t_cb4_root = -1;
            static thread_local int t_cb3_root = -1;
            t_cb4_root = game_cb13_root;
            t_cb3_root = game_cb14_root;
            std::optional<PortApi::DrawListScope> port_list;
            if (g_port_draw_list)
                port_list.emplace(d3d12_cl, [](void* u) {
                        auto* r = static_cast<PortRestore*>(u);
                        if (r->pso) r->cl->SetPipelineState(r->pso);
                        if (MigotoPort::TakeHeapsChanged()) {
                            if (r->srv) {
                                ID3D12DescriptorHeap* h[2] = { r->srv, r->smp };
                                r->cl->SetDescriptorHeaps(r->smp ? 2 : 1, h);
                            } else if (g_debug && DiagOnce("HEAPRESTORE-NOSRV", 0)) {
                                logger::write("HEAPRESTORE: our heaps were set on this list and the game's CBV_SRV_UAV heap is unknown -- ours stay set");
                            }
                        }
                        for (const auto& t : r->tables)
                            r->cl->SetGraphicsRootDescriptorTable(t.first,
                                D3D12_GPU_DESCRIPTOR_HANDLE{ t.second });
                        if (r->mid) *r->mid = true;
                        (*static_cast<decltype(restore_state)*>(r->inner_user))();
                        if (r->mid) *r->mid = false; }, &port_restore, [](void*, unsigned slot) -> int { return slot == 4 ? t_cb4_root : slot == 3 ? t_cb3_root
                                                                                                                                                        : -1; }, nullptr);
            struct PushCbGuard {
                ~PushCbGuard()
                {
                    PortApi::PushGameConstantBuffer('v', 4, nullptr, 0, 0);
                    PortApi::PushGameConstantBuffer('v', 3, nullptr, 0, 0);
                    PortApi::PushGameConstantBuffer('v', 2, nullptr, 0, 0);
                    if (g_push_game_vb)
                        PortApi::PushGameVertexBuffer(0, nullptr, 0, 0, 0, 0);
                }
            } push_cb_guard;
            _cs_restore.End();
            ProfScope _cs_push(PROF_CS_Push);
            ++g_port_sect_runs;
            if (port_cb4_res)
                SkelDumpNoteDrawCbs(port_cb4_res, port_cb4_va, game_cb13_root,
                    port_cb3_res, port_cb3_va, game_cb14_root);
            if (!port_cb4_res) ++g_port_sect_nocb;
            if (logger::enabled(1) && DiagOnce("PORTCB", CompKey(dm, wc->index))) {
                logger::write("PORTCB mod=%d comp=%d cb13res=%p va=%llx cb13root=%d  "
                              "cb14res=%p va=%llx cb14root=%d %s",
                    dm, wc->index, (void*)port_cb4_res, (unsigned long long)port_cb4_va,
                    game_cb13_root, (void*)port_cb3_res, (unsigned long long)port_cb3_va,
                    game_cb14_root,
                    port_cb4_res ? "" : "*** NO BONE CB -- vs-cb4 will read UNBOUND ***");
            }
            const uint64_t draw_pso = CmdGfx(cmd_list).gfx_pso;
            auto           cb_width = [&](unsigned reg) -> unsigned {
                uint32_t decl = 0;
                if (!PipelineVsCbufferBytes(draw_pso, reg, decl)) return kDx11SkeletonCbByteWidth;
                return decl == kBonesCbufferBytes ? kDx11SkeletonCbByteWidth : decl;
            };
            const RtPalResult pal = RtPalPublish(draw_pso, dm, wc, port_cb4_va);
            if (pal.published) {
                t_cb4_root = -1;
                rtpal_no_merge = pal.no_merge;
            }
            if (pal.no_cb3) t_cb3_root = -1; // RT M2: b3 is the pass-through VS's material buffer
            constexpr unsigned kNotSkeletonWidth = 256;
            if (!pal.published) PortApi::PushGameConstantBuffer('v', 4, port_cb4_res, port_cb4_va,
                pal.tried ? kNotSkeletonWidth : cb_width(4));
            unsigned w3 = cb_width(3);
            if (pal.tried && w3 == kDx11SkeletonCbByteWidth) {
                uint32_t   decl = 0;
                const bool parsed = PipelineVsCbufferBytes(draw_pso, 3, decl);
                if (DiagOnce("RTPAL-CB3", (int)(draw_pso ^ (draw_pso >> 32))))
                    logger::write("RTPAL-CB3 pso=%llx mod=%d comp=%d: b3 would have been published as a skeleton "
                                  "(declared %u B, %s) -- published at %u B instead",
                        (unsigned long long)draw_pso, dm, wc->index, decl,
                        parsed ? "read" : "declarations NOT read", kNotSkeletonWidth);
                w3 = kNotSkeletonWidth;
            }
            if (!pal.no_cb3) PortApi::PushGameConstantBuffer('v', 3, port_cb3_res, port_cb3_va, w3);
            {
                RootCbvBind obj = ObjectCbOf(cmd_list);
                // With ray tracing on, the game also draws characters from its skin cache (the posed vertices in vb0): those
                // passes bind neither a bone CB nor the object CB. An instance-skeleton mod (WWMI Tools 1.7: its section
                // asks `vs-cb2 == 3380.7777` for its instance) then draws nothing there and the game's original parts
                // show (Hsin "Linglang": 9 of 18 section runs a frame; mods without the instance check draw on). Such a
                // pass gets the object CB this component's skinned draw had this frame -- the same instance, the same
                // pool slot, its skeleton as that draw left it.
                // The skinned draw is not always earlier in the frame: Hsin's comp 0 draws in the base pass (skin cache, no
                // object CB) before its skinned draw, every frame, so a same-frame borrow found nothing and her original
                // comp 0 drew under the mod each frame (seen as flickering parts of the original). A component that ever
                // lacked one therefore keeps a copy of its object CB's contents; with none of this frame yet, the pass gets
                // the latest copy (at most kObjAge frames old: the same instance and pool slot, which is what the instance
                // capture reads -- the posed vertices come from the skin cache) in a buffer of ours that the GPU is done with
                // before it is reused (CbRing).
                constexpr uint32_t kObjAge = 8;
                constexpr uint32_t kObjBytes = kDx11ObjectCbByteWidth;
                static_assert(kObjBytes % CbRing::kSlot == 0 && kObjBytes / CbRing::kSlot <= CbRing::kBlock);
                struct ObjSeen {
                    RootCbvBind cb;
                    uint32_t    frame = 0;
                    bool        wanted = false; // a pass of this component once found no object CB of the frame
                    uint32_t    copy_frame = 0;
                    uint32_t    copy_n = 0;
                    uint8_t     copy[kObjBytes];
                };
                static std::mutex                            s_obj_m;
                static std::unordered_map<uint32_t, ObjSeen> s_obj; // (mod, comp) -> its last object CB
                static CbRing                                s_obj_ring;
                const uint32_t                               key = (uint32_t(dm) << 8) | (uint32_t(wc->index) & 0xff);
                if (obj.res && obj.va) {
                    std::lock_guard l(s_obj_m);
                    ObjSeen&        e = s_obj[key];
                    e.cb = obj;
                    e.frame = g_frame_id;
                    if (e.wanted) {
                        const uint32_t n = SnapshotGameCB(obj.res, obj.va, e.copy, kObjBytes);
                        if (n) {
                            e.copy_n = n;
                            e.copy_frame = g_frame_id;
                        }
                    }
                } else if (!port_cb4_res) {
                    std::lock_guard l(s_obj_m);
                    ObjSeen&        e = s_obj[key];
                    if (e.frame == g_frame_id && e.cb.res) {
                        obj = e.cb;
                        ++g_port_obj_borrowed;
                        if (logger::enabled(1) && DiagOnce("OBJCB-BORROW", (int)key))
                            logger::write("OBJCB-BORROW f=%u mod=%d comp=%d vs=%016llx: a pass with no bone and no object CB (the skin "
                                          "cache's posed vertices) gets this component's object CB of this frame (%llx) -- its instance "
                                          "capture sees the character's instance",
                                g_frame_id, dm, wc->index, (unsigned long long)vs_hash, (unsigned long long)obj.va);
                    } else {
                        if (!e.wanted && logger::enabled(1))
                            logger::write("OBJCB-WANT f=%u mod=%d comp=%d vs=%016llx: no object CB of this frame yet (its skinned "
                                          "draw comes later in the frame) -- its object CB is kept from now on, for the frames after",
                                g_frame_id, dm, wc->index, (unsigned long long)vs_hash);
                        e.wanted = true;
                        uint64_t        va = 0;
                        ID3D12Resource* ring = nullptr;
                        uint8_t*        p = nullptr;
                        if (e.copy_n && g_frame_id - e.copy_frame <= kObjAge && s_obj_ring.Ensure(g_d3d12_device, 1024 * 1024))
                            p = s_obj_ring.Take(&va, &ring, kObjBytes / CbRing::kSlot);
                        if (p) {
                            memcpy(p, e.copy, e.copy_n);
                            if (e.copy_n < kObjBytes) memset(p + e.copy_n, 0, kObjBytes - e.copy_n);
                            obj = { ring, va, kObjBytes };
                            ++g_port_obj_borrowed;
                            if (logger::enabled(1) && DiagOnce("OBJCB-BORROW-PREV", (int)key))
                                logger::write("OBJCB-BORROW-PREV f=%u mod=%d comp=%d vs=%016llx: no object CB of this frame yet -- "
                                              "given a copy of the one from frame %u (%u B)",
                                    g_frame_id, dm, wc->index, (unsigned long long)vs_hash, e.copy_frame, e.copy_n);
                        }
                    }
                }
                if (obj.res && obj.va) PortApi::PushGameConstantBuffer('v', 2, obj.res, obj.va, kDx11ObjectCbByteWidth);
            }

            if (g_push_game_vb) {
                uint64_t        gva[8] = {};
                uint32_t        gsz[8] = {};
                uint32_t        gstr[8] = {};
                ID3D12Resource* gres[8] = {};
                {
                    auto const& gs = CmdGfx(cmd_list);
                    for (unsigned s = 0; s < 8; ++s) {
                        gva[s] = gs.vb[s].va;
                        gsz[s] = gs.vb[s].size;
                        gstr[s] = gs.vb[s].stride;
                        gres[s] = gs.vb[s].res;
                    }
                }
                for (unsigned s = 0; s < 1; ++s) {
                    if (!gres[s] || !gva[s]) {
                        PortApi::PushGameVertexBuffer(s, nullptr, 0, 0, 0, 0);
                        continue;
                    }
                    const uint64_t base = gres[s]->GetGPUVirtualAddress();
                    const unsigned off = (gva[s] >= base) ? (unsigned)(gva[s] - base) : 0u;
                    PortApi::PushGameVertexBuffer(s, gres[s], base, gsz[s] + off, gstr[s], off);
                }
                if (logger::enabled(1) && ProbeArmed() && DiagOnce("PORTVB", CompKey(dm, wc ? wc->index : 0))) {
                    logger::write("PORTVB mod=%d comp=%d vb0res=%p va=%llx size=%u stride=%u %s",
                        dm, wc ? wc->index : -1, (void*)gres[0], (unsigned long long)gva[0],
                        gsz[0], gstr[0],
                        gres[0] ? "" : "*** NO vb0 -- `ref vb0` will capture NOTHING ***");
                }
            }

            if (!is_full_shadow_pass && wc && game_cb13_va) {
                std::lock_guard<std::mutex> l(g_comp_bone_mtx);
                g_comp_bone[(uint32_t)((dm << 8) | (wc->index & 0xff))] = { game_cb13_va, g_frame_id };
            }
            uint64_t sh_restore_va = 0;
            if (is_full_shadow_pass && wc &&
                game_cb13_root >= 0 && game_cb13_root < 64) {
                uint64_t own = 0;
                {
                    std::lock_guard<std::mutex> l(g_comp_bone_mtx);
                    auto                        it = g_comp_bone.find((uint32_t)((dm << 8) | (wc->index & 0xff)));
                    if (it != g_comp_bone.end() &&
                        (uint32_t)(g_frame_id - it->second.frame) <= 2u)
                        own = it->second.va;
                }
                if (!game_cb13_va) {
                    if (DiagOnce("SHADOWBONE-REFUSED", CompKey(dm, wc->index)))
                        logger::write("SHADOWBONE mod=%d comp=%d REFUSED: the game bound NO CB "
                                      "at root %d on this pass, so we cannot know the register "
                                      "is a bone CB here -- and would have nothing to restore",
                            dm, wc->index, game_cb13_root);
                }
                if (own && game_cb13_va && own != game_cb13_va) {
                    d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb13_root, own);
                    sh_restore_va = game_cb13_va; // put the game's back after our draw
                    if (logger::enabled(1)) {
                        if (DiagOnce("SHADOWBONE-RESTORED", CompKey(dm, wc->index)))
                            logger::write("SHADOWBONE mod=%d comp=%d root=%d inherited=%llx "
                                          "-> own=%llx (per-component CB restored)",
                                dm, wc->index, game_cb13_root,
                                (unsigned long long)game_cb13_va,
                                (unsigned long long)own);
                    }
                }
            }
            ID3D12Resource* sk_inst = nullptr;
            {
                auto const& gs = CmdGfx(cmd_list);
                if (gs.vb[6].stride) sk_inst = gs.vb[6].res;
            }
            bool           sk_first = false;
            const unsigned sk_copy = SkCopySlot(cmd_list, dm, wc->index, port_cb4_res, port_cb4_va, is_full_shadow_pass,
                &sk_first);
            if (g_sk_run_passes) {
                GpuBracket _gt(d3d12_cl, GT_ShapeKeys);
                RunShapeKeyPasses(uns, dm, sk_inst, cmd_list);
            }
            _cs_push.End();
            ProfScope             _cs_reloc(PROF_CS_Reloc);
            MorphDump::DrawRecord i8_row{};
            bool                  i8_pending = false;
            unsigned long long    i8_nulls_before = 0;
            if (d3d12_cl) {
                uint64_t        gva[8] = {};
                uint32_t        gstr[8] = {};
                ID3D12Resource* gres[8] = {};
                uint32_t        ifmt = 0;
                ID3D12Resource* ires = nullptr;
                {
                    auto const& gs = CmdGfx(cmd_list);
                    for (unsigned q = 0; q < 8; ++q) {
                        gva[q] = gs.vb[q].va;
                        gstr[q] = gs.vb[q].stride;
                        gres[q] = gs.vb[q].res;
                    }
                    ifmt = gs.ib.index_bytes;
                    ires = gs.ib.res;
                }
                SkNoteDrawVbs(dm, gres, 8);
                auto xit_in = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES to) -> bool {
                    if (!r || reloc_xit_n >= 9) return false;
                    reloc_xit[reloc_xit_n++] = { r, MigotoPort::OwnedTransition(d3d12_cl, r, to) };
                    return true;
                };
                SkBufferBinding       my_u0b{};
                const bool            have_my_u0 = SkGetBinding(g_geomods[dm].wcl_ns, sk_inst, TargetClass::UnorderedAccessView, 0, &my_u0b);
                ID3D12Resource* const my_u0 = my_u0b.res;
                uint8_t               v6 = 3;
                uint64_t              v6_sub_va = 0;
                uint32_t              v6_sub_bytes = 0;
                ID3D12Resource*       v6_sub_res = nullptr;
                uint64_t              v6_sub_off = 0;
                bool                  v6_xit = false;
                for (unsigned q = 0; q < 8; ++q) {
                    RelocBuf rb{};
                    if (!gstr[q] || !g_buffer_reloc.Resolve(gres[q], &rb)) {
                        if (q == 6) v6 = gstr[q] ? 0 : 3;
                        continue;
                    }
                    if (!have_my_u0 || rb.res != my_u0) {
                        if (q == 6) v6 = 2;
                        if (logger::enabled(1)) {
                            static const char kRelocBindSite = 0;
                            if (DiagOnce((const void*)&kRelocBindSite, dm * 8 + (int)q))
                                logger::write("RELOCBIND mod=%d vb%u: DECLINED -- the widened "
                                              "buffer for this resource (%p) is not this mod's "
                                              "u0 (%p, have=%d); binding it would hand this "
                                              "draw another character's morphs",
                                    dm, q, (void*)rb.res, (void*)my_u0,
                                    (int)have_my_u0);
                        }
                        continue;
                    }
                    const bool               xin = xit_in(rb.res, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                    D3D12_VERTEX_BUFFER_VIEW nv{ rb.va, (UINT)rb.bytes, gstr[q] };
                    d3d12_cl->IASetVertexBuffers(q, 1, &nv);
                    reloc_vb_mask |= (1u << q);
                    if (q == 6) {
                        v6 = 1;
                        v6_sub_va = rb.va;
                        v6_sub_bytes = (uint32_t)rb.bytes;
                        v6_sub_res = rb.res;
                        v6_xit = xin;
                        const uint64_t base = rb.res->GetGPUVirtualAddress();
                        v6_sub_off = (rb.va >= base) ? (rb.va - base) : 0;
                        x24_vb6_res = rb.res;
                        x24_vb6_va = rb.va;
                        x24_vb6_bytes = (uint32_t)rb.bytes;
                        x24_vb6_verdict = 1; // X24
                    }
                    if (logger::enabled(1)) {
                        if (DiagOnce("RELOCBIND", dm * 8 + (int)q))
                            logger::write("RELOCBIND mod=%d vb%u: %llx -> %llx/%llu (stride %u)"
                                          " -- the draw now reads the WIDENED buffer the chain "
                                          "writes",
                                dm, q, (unsigned long long)gva[q],
                                (unsigned long long)rb.va,
                                (unsigned long long)rb.bytes, gstr[q]);
                    }
                }
                RelocBuf rb{};
                if (ires && ifmt && save_ib_set && g_buffer_reloc.Resolve(ires, &rb)) {
                    xit_in(rb.res, D3D12_RESOURCE_STATE_INDEX_BUFFER);
                    D3D12_INDEX_BUFFER_VIEW ni{ rb.va, (UINT)rb.bytes,
                        ifmt == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
                    d3d12_cl->IASetIndexBuffer(&ni);
                }
                const bool i8 = MorphDump::Armed();
                const bool i9 = (v6 == 1) && v6_sub_res && MigotoPort::SkCapArmed();
                if (i8 || i9) {
                    const uint8_t is_depth = (draw_rt.rtv_count == 0 && draw_rt.has_dsv) ? 1 : 0;
                    const int     comp_ix = wc ? wc->index : -1;
                    if (i9) {
                        static std::atomic<unsigned> s_dseq{ 0 };
                        char                         prov[72];
                        sprintf_s(prov, "D f=%u m=%d c=%d l=%llx d=%u x=%d n=%u fi=%u",
                            g_frame_id, dm, comp_ix,
                            (unsigned long long)reinterpret_cast<uintptr_t>(d3d12_cl),
                            (unsigned)is_depth, (int)v6_xit, index_count, first_index);
                        MigotoPort::SkCapSnapDraw(g_d3d12_device, d3d12_cl, "vb6", v6_sub_res,
                            v6_sub_off, v6_sub_bytes,
                            v6_xit ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            1000u + s_dseq.fetch_add(1, std::memory_order_relaxed),
                            prov);
                    }
                    if (i8) {
                        MorphDump::DrawRecord& w = i8_row;
                        w.frame = g_frame_id;
                        w.mod_id = dm;
                        w.comp = comp_ix;
                        w.list = reinterpret_cast<uint64_t>(d3d12_cl);
                        w.index_count = index_count;
                        w.first_index = first_index;
                        w.vertex_offset = vertex_offset;
                        w.is_depth = is_depth;
                        w.vb6_res = reinterpret_cast<uint64_t>(gres[6]);
                        w.vb6_va = gva[6];
                        w.vb6_stride = gstr[6];
                        if (gres[6]) {
                            std::lock_guard lock(g_mem_mutex);
                            auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(gres[6]));
                            if (vi != g_res_to_va.end()) {
                                auto ri = g_va_to_res.find(vi->second);
                                if (ri != g_va_to_res.end()) w.vb6_bytes = ri->second.size;
                            }
                        }
                        w.vb6_verdict = v6;
                        w.sub_va = v6_sub_va;
                        w.sub_bytes = v6_sub_bytes;
                        w.my_u0 = reinterpret_cast<uint64_t>(my_u0);
                        w.vb0_res = reinterpret_cast<uint64_t>(gres[0]);
                        w.ns = g_geomods[dm].wcl_ns.c_str();
                        i8_pending = true;
                        i8_nulls_before = MigotoPort::VbNullUnbinds();
                    }
                }
            }
            _cs_reloc.End();
            ProfScope      _cs_secttex(PROF_CS_SectTex);
            const uint16_t sect_tex = PublishSectionTextures(cmd_list, dm, wc->index, draw_rt.rtv_count);
            if (MorphDump::Armed())
                hk.cs_enter = [](const std::wstring&, const std::wstring& t) {
                    if (t.find(L"skeletonmerger") != std::wstring::npos) ++t_x23_merges; };
            _cs_secttex.End();
            _di_compsetup.End();
            bool ran;
            {
                PROF(PROF_DI_PortSect);
                g_sections_this_frame.fetch_add(1, std::memory_order_relaxed); // GTEXMISS: sections before a miss
                std::optional<PortApi::SkelCopyScope> sk_scope;
                if (sk_copy || sk_first) sk_scope.emplace(uns.c_str(), sk_copy);
                PublishDrawShaders(cmd_list, vs_hash);
#ifdef WWMI_DEBUG
                const CmdState& bt_gs = CmdGfx(cmd_list);
                if (logger::enabled(1) && IbAnchorJustLearned(bt_gs.ib.va)) {
                    char bt_ia[768] = "";
                    int  bt_n = 0;
                    for (unsigned s = 0; s < 16 && bt_n >= 0 && bt_n < (int)sizeof(bt_ia); ++s)
                        if (bt_gs.vb[s].va)
                            bt_n += snprintf(bt_ia + bt_n, sizeof(bt_ia) - bt_n, " vb%u=%llx+%u(%p)", s,
                                (unsigned long long)bt_gs.vb[s].va, bt_gs.vb[s].size, (void*)bt_gs.vb[s].res);
                    logger::write("BINDTRACE mod=%d comp=%d f=%u: the first frame claimed on this index buffer -- the game's IA:%s "
                                  "ib=%llx+%u(%p); the section's commands and binds follow as CHAIN lines",
                        dm, wc->index, g_frame_id, bt_ia, (unsigned long long)bt_gs.ib.va, bt_gs.ib.size, (void*)bt_gs.ib.res);
                    t_trace_binds = true;
                }
#endif
                ran = PortRunSection(fq, hk, index_count, first_index, first_instance,
                    CmdGfx(cmd_list).gfx_pso, skip_orig);
                t_trace_binds = false;
            }
            for (unsigned r = 0; r < kSectionTexRegs; ++r)
                if (sect_tex & (1u << r)) PortApi::UnpushGameTexture('p', r);
            PROF(PROF_DI_CompPost); // to the end of this block, every return included
            if (x32_mid_restores) {
                g_x32_mid_total.fetch_add(x32_mid_restores, std::memory_order_relaxed);
                g_x32_mid_skipped_total.fetch_add(x32_mid_skipped, std::memory_order_relaxed);
                if (logger::enabled(1) && DiagOnce("X32-MID", CompKey(dm, wc->index)))
                    logger::write("X32 MID-RESTORE mod=%d comp=%d vs=%016llx: the section issued %u engine "
                                  "dispatch/draw(s) before its drawindexed and each one's restore callback "
                                  "ran restore_state (reloc mask %u); IA rebinds skipped on %u of them. %s",
                        dm, wc->index, (unsigned long long)vs_hash, x32_mid_restores, reloc_vb_mask,
                        x32_mid_skipped,
                        x32_mid_skipped == x32_mid_restores
                            ? "The injected draw keeps the RELOCBIND substitution."
                            : "A restore path skipped the mid-section guard: slot 6 went back to "
                              "the GAME's vanilla morph buffer -- RE-OPEN.");
            }
            if (i8_pending) { // X9: what the section did to the slots after the verdict
                const unsigned long long nb = MigotoPort::VbNullUnbinds();
                i8_row.ini_null_unbinds = (nb > i8_nulls_before) ? (uint32_t)(nb - i8_nulls_before) : 0u;
                i8_row.mid_restores = x32_mid_restores; // X32
                i8_row.mid_ia_skipped = x32_mid_skipped;
                MorphDump::RecordDraw(i8_row);
            }
            if (sh_restore_va)
                d3d12_cl->SetGraphicsRootConstantBufferView((UINT)game_cb13_root, sh_restore_va);
            if (logger::enabled(1)) {
                if (DiagOnce("COMP-NS", CompKey(dm, wc->index))) {
                    const std::wstring& u2 = g_geomods[dm].wcl_ns;
                    logger::write("CHAINPROF post-section mod=%d comp=%d ns='%ls' "
                                  "object_detected=%g state_id=%g mod_enabled=%g ran=%d",
                        dm, wc->index, u2.c_str(),
                        PortApi::GetVarNs(u2.c_str(), L"object_detected", -1.f),
                        PortApi::GetVarNs(u2.c_str(), L"state_id", -1.f),
                        PortApi::GetVarNs(u2.c_str(), L"mod_enabled", -1.f),
                        (int)ran);
                }
            }
            if (logger::enabled(1) && ran) {
                const float              en = PortApi::GetVarNs(g_geomods[dm].wcl_ns.c_str(), L"mod_enabled", -1.f);
                const unsigned long long v = ((unsigned long long)(en * 16.f + 64.f) << 8) | (did ? 2u : 0u) | (skip_orig ? 1u : 0u);
                const int                key = (int)(((uint64_t)(unsigned)dm << 24) ^ ((uint64_t)(unsigned)wc->index << 20) ^ (reinterpret_cast<uintptr_t>(sk_inst) >> 4));
                if (DiagChanged("MODDRAW", key, v))
                    logger::write("MODDRAW f=%u mod=%d comp=%d inst=%p: mod_enabled=%g drew=%u skip=%d",
                        g_frame_id, dm, wc->index, (void*)sk_inst, en, did, (int)skip_orig);
            }
            GeoMod& unit = g_geomods[dm];
            ++unit.sect_runs;
            if (ran && skip_orig) ++unit.skip_orig;
            if (ran) unit.replaced += did;
            if (ran) {
                g_draw_replace_count += did;
                g_wwmi_geom_count += did;
                if (skip_orig || did > 0) return true;
                ++g_port_fellback_count;
                {
                    // SECTION-NODRAW: an enabled mod's section ran and neither drew nor skipped -- the game's own draw goes
                    // through (with an instance-skeleton mod: its original parts on screen). Per (mod, comp, VS): what this
                    // pass binds, and what the mod's instance capture made of it.
                    const float en = PortApi::GetVarNs(uns.c_str(), L"mod_enabled", -1.f);
                    if (en != 0.f && logger::enabled(1) &&
                        DiagOnce("SECTION-NODRAW", (int)(((uint64_t)(unsigned)dm << 24) ^ ((uint64_t)(unsigned)wc->index << 20) ^ (vs_hash & 0xfffff) ^ (vs_hash >> 40)))) {
                        const auto&       gs = CmdGfx(cmd_list);
                        const RootCbvBind obj = ObjectCbOf(cmd_list);
                        logger::write("SECTION-NODRAW f=%u mod=%d comp=%d first=%u count=%u vs=%016llx pso=%llx rtv=%u dsv=%d vb0=%llx stride=%u inSkinBand=%d vb6=%llx | "
                                      "bones cb4=%p/%llx root %d, cb3=%p/%llx root %d | object cb (vs-cb2) %p/%llx | instance_valid=%g instance_id=%g instance_slot=%g "
                                      "object_detected=%g -- the game's draw goes through",
                            g_frame_id, dm, wc->index, first_index, index_count, (unsigned long long)vs_hash, (unsigned long long)gs.gfx_pso,
                            draw_rt.rtv_count, (int)draw_rt.has_dsv, (unsigned long long)gs.vb[0].va, gs.vb[0].stride, (int)InSkinBand(gs.vb[0].va),
                            (unsigned long long)gs.vb[6].va, (void*)port_cb4_res, (unsigned long long)port_cb4_va, game_cb13_root, (void*)port_cb3_res,
                            (unsigned long long)port_cb3_va, game_cb14_root, (void*)obj.res, (unsigned long long)obj.va,
                            PortApi::GetVarNs(uns.c_str(), L"instance_valid", -1.f), PortApi::GetVarNs(uns.c_str(), L"instance_id", -1.f),
                            PortApi::GetVarNs(uns.c_str(), L"instance_slot", -1.f), PortApi::GetVarNs(uns.c_str(), L"object_detected", -1.f));
                    }
                }
                restore_state();
                fr.mod_switched_off = PortApi::GetVarNs(uns.c_str(), L"mod_enabled", -1.f) == 0.f;
                return false;
            }
        }
    }
    return false;
}
