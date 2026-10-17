// The per-instance store: what a shapekey dispatch publishes for its instance's draw. See sk_internal.h.
#include "sk_internal.h"
#include "../instrumentation/profiler.h" // ProfGpuWork -- this file's GPU copies, for the performance report's work rows
#include "../instrumentation/removal_pin.h"
#include "../migoto/dx12_bridge.h"
#include "../../src/gpu_progress.h"
#include "../../src/d3d12_name.h"

using SkStoreKey = std::pair<std::wstring, ID3D12Resource*>;
static std::mutex                      g_store_mtx;
static std::map<SkStoreKey, SkNsState> g_store;

// Look up an existing instance. Absent is a real answer, not a reason to create one.
_Requires_lock_held_(g_store_mtx) static SkNsState* SkFindLocked(const std::wstring& ns, ID3D12Resource* inst)
{
    if (ns.empty() || !inst) return nullptr;
    auto it = g_store.find(SkStoreKey(NsKey(ns), inst));
    return it == g_store.end() ? nullptr : &it->second; // node-based map: the pointer survives an insert
}

// Get or create. Only writers call this, and a writer naming a new instance is how an instance appears.
_Requires_lock_held_(g_store_mtx) static SkNsState* SkEmplaceLocked(const std::wstring& ns, ID3D12Resource* inst)
{
    if (ns.empty() || !inst) return nullptr;
    return &g_store[SkStoreKey(NsKey(ns), inst)];
}

/* Resolve (kind, reg) to the binding it names, or nullptr. RegisterIndex is the single bounds check. */
static SkBufferBinding* SkBindingSlot(SkNsState& e, TargetClass kind, unsigned reg)
{
    const unsigned i = RegisterIndex(kind, reg);
    return i == kNoRegister ? nullptr : &e.reg[i];
}

static bool SkBindingLive(TargetClass kind, const SkBufferBinding& b)
{
    if (!b.res) return false;
    return RegisterBoundByAddress(kind) ? b.va != 0 : b.bytes != 0;
}

bool SkGetBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out)
{
    std::lock_guard<std::mutex> lk(g_store_mtx);
    const SkNsState*            e = SkFindLocked(ns, inst);
    if (!e) return false;
    const SkBufferBinding* b = SkBindingSlot(*const_cast<SkNsState*>(e), kind, reg);
    if (!b || !SkBindingLive(kind, *b)) return false; /* nothing recorded is a real answer, not a default */
    *out = *b;
    return true;
}

struct SkZeroSource {
    ID3D12Resource*    buf = nullptr; // >= the largest buffer zeroed through this register so far
    unsigned           bytes = 0;
    unsigned long long cleared = 0;
    unsigned long long not_ours = 0;
    unsigned long long not_ready = 0;
};
static std::unordered_map<unsigned, SkZeroSource>        g_zero; // flat register index -> source; g_store_mtx
static std::vector<std::pair<ID3D12Resource*, uint64_t>> g_grave;
_Requires_lock_held_(g_store_mtx) static void SkRetireLocked(ID3D12Resource* r)
{
    if (r) g_grave.emplace_back(r, GpuStamp());
}
_Requires_lock_held_(g_store_mtx) static void SkSweepGraveLocked()
{
    /* destroy_resource, not Release: it also drops the buffer from ReShade's VA registry. */
    reshade::api::device* dev = g_reshade_device;
    for (size_t k = 0; k < g_grave.size();) {
        if (GpuPassed(g_grave[k].second)) {
            if (dev)
                dev->destroy_resource(resource{ reinterpret_cast<uint64_t>(g_grave[k].first) });
            else
                g_grave[k].first->Release();
            g_grave[k] = g_grave.back();
            g_grave.pop_back();
        } else
            ++k;
    }
}

static ID3D12Resource* SkMakeZeroSource(reshade::api::device* dev, unsigned bytes)
{
    if (!dev || !bytes) return nullptr;
    /* UPLOAD heap via cpu_access, which is GENERIC_READ (sdk d3d12_impl_type_convert.cpp:213). */
    resource h{};
    if (!dev->create_resource(resource_desc(bytes, memory_heap::upload, resource_usage::copy_source),
            nullptr, resource_usage::cpu_access, &h) ||
        !h.handle)
        return nullptr;
    void* mp = nullptr;
    if (!dev->map_buffer_region(h, 0, UINT64_MAX, map_access::write_only, &mp) || !mp) {
        dev->destroy_resource(h);
        return nullptr;
    }
    memset(mp, 0, bytes);
    dev->unmap_buffer_region(h);
    DevName(reinterpret_cast<ID3D12Resource*>(h.handle), L"wwmi shapekey zero source");
    return reinterpret_cast<ID3D12Resource*>(h.handle);
}

uint8_t SkZeroBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg,
    reshade::api::command_list* cmd_list, const char* why)
{
    const unsigned i = RegisterIndex(kind, reg);
    if (i == kNoRegister) return 0;
    const void*     cl = cmd_list ? reinterpret_cast<const void*>(cmd_list->get_native()) : nullptr; // the record path keys on the native list
    SkBufferBinding b{};
    if (!SkGetBinding(ns, inst, kind, reg, &b) || !b.res || !b.bytes) return 0; // nothing recorded: real answer
    bool live = false;
    {
        std::lock_guard lock(g_mem_mutex);
        live = g_res_to_va.find(reinterpret_cast<uint64_t>(b.res)) != g_res_to_va.end();
    }
    const unsigned  identity = g_buffer_reloc.IdentityBytes(b.res, b.bytes);
    const unsigned  hash = PortApi::GameComputeUavBufferHash(identity);
    const bool      ours = live && PortApi::ModDeclaresHash(ns.c_str(), hash);
    ID3D12Resource* src = nullptr;
    bool            need_build = false;
    /* DECIDE under the lock, BUILD outside it, INSTALL under it again. */
    {
        std::lock_guard<std::mutex> lk(g_store_mtx);
        SkZeroSource&               z = g_zero[i];
        if (!ours) {
            ++z.not_ours;
        } else if (cl) {
            if (z.buf && z.bytes < b.bytes) {
                SkRetireLocked(z.buf); // retired, NOT released -- see the note at g_grave
                z.buf = nullptr;
                z.bytes = 0;
            }
            if (z.buf) {
                src = z.buf;
                ++z.cleared;
            } else
                need_build = true;
        }
        SkSweepGraveLocked();
    }
    if (ours && cl && need_build) {
        ID3D12Resource*             nb = SkMakeZeroSource(g_reshade_device, b.bytes);
        std::lock_guard<std::mutex> lk(g_store_mtx);
        SkZeroSource&               z = g_zero[i];
        if (!nb) {
            ++z.not_ready;
        } else if (z.buf && z.bytes >= b.bytes) {
            g_reshade_device->destroy_resource(resource{ reinterpret_cast<uint64_t>(nb) });
            src = z.buf;
            ++z.cleared;
        } else {
            SkRetireLocked(z.buf);
            z.buf = nb;
            z.bytes = b.bytes;
            src = nb;
            ++z.cleared;
        }
    }
    if (!ours) {
        if (g_debug && LogBudget(why, ns, 6))
            logger::write("%s: REFUSED -- %c%u is %u B (identity %u B, hash %08x, live=%d), which ns='%ls' "
                          "does NOT declare. Not zeroing a buffer we cannot prove is ours.",
                why, TargetClassChar(kind), reg, b.bytes, identity, hash, (int)live, ns.c_str());
        return 2;
    }
    if (!cl) return 0;
    if (!src) return 3;
    {
        const resource dst{ reinterpret_cast<uint64_t>(b.res) };
        const resource zsrc{ reinterpret_cast<uint64_t>(src) };
        const bool     owned = MigotoPort::OwnedBuffer(b.res);
        if (owned)
            MigotoPort::OwnedTransition(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), b.res,
                D3D12_RESOURCE_STATE_COPY_DEST);
        else if (g_sk_clear_barrier)
            cmd_list->barrier(dst, resource_usage::unordered_access, resource_usage::copy_dest);
        cmd_list->copy_buffer_region(zsrc, 0, dst, 0, b.bytes);
        ProfGpuWork(PGW_SkCopy, b.bytes);
        if (!owned && g_sk_clear_barrier) cmd_list->barrier(dst, resource_usage::copy_dest, resource_usage::unordered_access);
        RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "sk-clear");
        RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), b.res, "sk-clear destination");
    }
    if (g_debug && LogBudget(why, ns, 6))
        logger::write("%s: zeroed %u B of %c%u (identity %u B, hash %08x, DECLARED) for ns='%ls' -- the "
                      "game's own per-dword clear does this to ITS buffer every frame; ours is a separate "
                      "resource it never sees.",
            why, b.bytes, TargetClassChar(kind), reg, identity, hash, ns.c_str());
    return 1;
}

void SkZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared,
    unsigned long long* not_ours, unsigned long long* not_ready)
{
    *cleared = *not_ours = *not_ready = 0;
    const unsigned i = RegisterIndex(kind, reg);
    if (i == kNoRegister) return;
    std::lock_guard<std::mutex> lk(g_store_mtx);
    auto                        it = g_zero.find(i);
    if (it == g_zero.end()) return;
    *cleared = it->second.cleared;
    *not_ours = it->second.not_ours;
    *not_ready = it->second.not_ready;
}

bool SkSetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
    ID3D12Resource* res, uint64_t va, unsigned bytes)
{
    const unsigned i = RegisterIndex(TargetClass::ConstantBuffer, reg);
    if (i == kNoRegister) return false;
    std::lock_guard<std::mutex> lk(g_store_mtx);
    SkNsState*                  e = SkEmplaceLocked(ns, inst);
    if (!e) return false;
    SkBufferBinding& b = e->snap[i][cs][tgy];
    b.res = res;
    b.va = va;
    b.bytes = bytes;
    return true;
}

bool SkGetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
    SkBufferBinding* out)
{
    const unsigned i = RegisterIndex(TargetClass::ConstantBuffer, reg);
    if (i == kNoRegister) return false;
    std::lock_guard<std::mutex> lk(g_store_mtx);
    SkNsState*                  e = SkFindLocked(ns, inst);
    if (!e) return false;
    auto si = e->snap.find(i);
    if (si == e->snap.end()) return false;
    auto ci = si->second.find(cs);
    if (ci == si->second.end()) return false;
    auto it = ci->second.find(tgy);
    if (it == ci->second.end() || !it->second.res || !it->second.va) return false;
    *out = it->second;
    return true;
}

void SkSnapshotCb0(reshade::api::command_list* cmd_list, const std::wstring& ns, ID3D12Resource* inst,
    ID3D12Resource* src, uint64_t va, unsigned size,
    unsigned long long cs_hash, unsigned tgy)
{
    if (!g_sk_cb0_snapshot || !cmd_list || ns.empty() || !inst || !src || !va || !size) return;

    uint64_t base = 0;
    {
        std::lock_guard<std::mutex> lock(g_mem_mutex);
        auto                        vi = g_res_to_va.find(reinterpret_cast<uint64_t>(src));
        if (vi == g_res_to_va.end()) return; // destroyed since resolve -- never deref
        base = vi->second;
    }
    const uint64_t off = (va > base) ? (va - base) : 0;

    const unsigned bytes = (size + 255u) & ~255u;
    if (bytes > 65536u || (off & 3u)) return;

    ID3D12Resource* snap = nullptr;
    ID3D12Resource* undersized = nullptr; // (`small` is a Windows macro)
    {
        SkBufferBinding prev{};
        if (SkGetSnap(ns, inst, 0, cs_hash, tgy, &prev)) {
            if (prev.bytes >= bytes)
                snap = prev.res;
            else
                undersized = prev.res;
        }
    }

    reshade::api::device* dev = cmd_list->get_device();
    if (!dev) return;

    if (!snap) {
        resource sh{};
        if (!dev->create_resource(resource_desc(bytes, memory_heap::default_,
                                      resource_usage::constant_buffer | resource_usage::copy_dest),
                nullptr, resource_usage::general, &sh) ||
            !sh.handle) {
            logger::write("CB0SNAP: create_resource FAILED for ns='%ls' (%u B) -- falling back to "
                          "the game's ring address, which is what was stale.",
                ns.c_str(), bytes);
            return;
        }
        snap = reinterpret_cast<ID3D12Resource*>(sh.handle);
        DevName(snap, L"wwmi cb snapshot");
        const uint64_t snap_gpu_va = snap->GetGPUVirtualAddress();
        if (undersized) {
            std::lock_guard<std::mutex> lk(g_store_mtx);
            SkRetireLocked(undersized);
        }
        SkSetSnap(ns, inst, 0, cs_hash, tgy, snap, snap_gpu_va, bytes);
        if (g_debug)
            logger::write("CB0SNAP: ns='%ls' inst=%p snapshot %u B  src=%p base=%llx va=%llx off=%llu cs=%016llx%s",
                ns.c_str(), (void*)inst, bytes, (void*)src, (unsigned long long)base,
                (unsigned long long)va, (unsigned long long)off, cs_hash,
                off == 0 ? "  [off=0 -- b0 pointed at the RING BASE, not a suballocation]" : "");
    }

    const resource snap_h{ reinterpret_cast<uint64_t>(snap) };
    const resource src_h{ reinterpret_cast<uint64_t>(src) };
    cmd_list->copy_buffer_region(src_h, off, snap_h, 0, bytes);
    ProfGpuWork(PGW_SkCopy, bytes);
    RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "sk-snap");
    RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), src, "sk-snap source");
}

void SkSetStateForNs(const std::wstring& ns, ID3D12Resource* inst, const SkComputeState& st)
{
    std::lock_guard<std::mutex> lk(g_store_mtx);
    SkNsState*                  e = SkEmplaceLocked(ns, inst);
    if (!e) return;
    for (unsigned i = 0; i < kRegisterFileTotal; ++i) e->reg[i] = st.reg[i];
    e->valid = st.valid;
}

bool SkGetStateForNs(const std::wstring& ns, ID3D12Resource* inst, SkComputeState* out)
{
    std::lock_guard<std::mutex> lk(g_store_mtx);
    SkNsState*                  e = SkFindLocked(ns, inst);
    if (!e || !e->valid) return false;
    for (unsigned i = 0; i < kRegisterFileTotal; ++i) out->reg[i] = e->reg[i];
    out->valid = true;
    return true;
}

void SkInvalidateSnapshotFor(ID3D12Resource* dead)
{
    if (!dead) return;
    {
        std::lock_guard<std::mutex> lk(g_store_mtx);
        for (auto kv = g_store.begin(); kv != g_store.end();) {
            SkNsState& e = kv->second;
            if (kv->first.second == dead) {
                for (auto& per_reg : e.snap)
                    for (auto& per_cs : per_reg.second)
                        for (auto& per_tgy : per_cs.second)
                            SkRetireLocked(per_tgy.second.res);
                kv = g_store.erase(kv);
                continue;
            }
            for (unsigned i = 0; i < kRegisterFileTotal; ++i)
                if (e.reg[i].res == dead) e.reg[i] = SkBufferBinding{};
            /* the snapshots are per queuing dispatch, so a dead one takes out its own key and no other */
            for (auto& per_reg : e.snap)
                for (auto& per_cs : per_reg.second)
                    for (auto it = per_cs.second.begin(); it != per_cs.second.end();)
                        if (it->second.res == dead)
                            it = per_cs.second.erase(it);
                        else
                            ++it;
            ++kv;
        }
    }
    SkForgetPendingFor(dead);
}
