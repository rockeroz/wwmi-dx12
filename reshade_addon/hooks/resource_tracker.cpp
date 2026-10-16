// Resource, view and descriptor tracking: the shadow of the game's own bookkeeping.
// See resource_tracker.h.
#include "../mods/ib_hash.h"
#include "../addon_common.h"
#include "resource_tracker.h"
#include "../textures/texture_module.h"     // kTexHashFloorPx
#include "../instrumentation/diagnostics.h" // g_texnoswap_arm / _n / _gen -- TEXNOSWAP
#include "../instrumentation/removal_pin.h" // DEAD-REF: a game buffer we named, destroyed before our list executed
#include "../instrumentation/hot_path.h"
#include "../mods/claim_state.h" // ForgetCharMeshVb0 / IbAnchorForget -- what is learned inside a buffer
#include "../rt/rt_hooks.h"      // RtForgetSkinOutRange
#include <unordered_set>
#include <cstring>
#include <wrl/client.h>

// Buffer VA -> resource (for VA-based lookups in the bind hooks).
std::mutex                             g_mem_mutex; // declared in resource_tracker.h
std::unordered_map<uint64_t, ResInfo>  g_va_to_res; // (the chain resolves VAs too)
std::unordered_map<uint64_t, uint64_t> g_res_to_va;

bool BufferLive(const void* res)
{
    std::lock_guard lock(g_mem_mutex);
    return g_res_to_va.find(reinterpret_cast<uint64_t>(res)) != g_res_to_va.end();
}

void on_barrier(command_list* cmd_list, uint32_t count, const resource* res, const resource_usage* /*old_s*/, const resource_usage* new_s)
{
    PROF(PROF_Barrier);
    HotPath::GpuNoteBarriers(count);
    CmdBlock* const blk = g_cmd_blocks.get(reinterpret_cast<uintptr_t>(cmd_list));
    for (uint32_t i = 0; i < count; ++i)
        if (res[i].handle) {
            g_res_state.put(res[i].handle, (uint32_t)new_s[i]);
            if (blk && ((uint32_t)new_s[i] & (uint32_t)resource_usage::unordered_access)) {
                auto&  v = blk->uav_recent;
                size_t old = 0;
                while (old < v.size() && v[old].frame + 1 < g_frame_id) ++old;
                if (old) v.erase(v.begin(), v.begin() + old);
                v.push_back(UavNote{ res[i].handle, g_frame_id });
            }
        }
}

uint32_t ResourceStateOf(void* res)
{
    if (ModLoader::TexIndexOf(reinterpret_cast<ID3D12Resource*>(res)) >= 0) return (uint32_t)D3D12_RESOURCE_STATE_COMMON;
    uint32_t state = 0;
    if (g_res_state.find(reinterpret_cast<uint64_t>(res), state)) return state;
    return MigotoPort::kResStateUnknown;
}

bool on_create_resource(device*, resource_desc& desc, subresource_data*, resource_usage)
{
    if (g_is_dx12 && !g_addon_disabled &&
        desc.type == resource_type::buffer &&
        (desc.usage & resource_usage::unordered_access) != resource_usage::unordered_access) {
        const uint64_t sz = desc.buffer.size;
        if (sz && sz <= 0xFFFFFFFFull) {
            const unsigned h = PortApi::GameComputeUavBufferHash((unsigned)sz);
            if (h && PortApi::ShapeKeyCallbackDeclaresHash(h)) {
                desc.usage |= resource_usage::unordered_access;
                if (ProbeArmed() && DiagOnce("UAVFLAG", (int)h))
                    logger::write("UAVFLAG: buffer %llu B (desc %08x) created WITHOUT "
                                  "ALLOW_UNORDERED_ACCESS -- added it, so WWMI's atomic_iadd into u0 has "
                                  "a valid view. Size and placement unchanged.",
                        (unsigned long long)sz, h);
                return true; // desc modified
            }
        }
    }

    return false;
}

void on_init_resource(device*, const resource_desc& desc, const subresource_data* init_data, resource_usage, resource res)
{
    if (g_addon_disabled) return;
    g_init_res_all.fetch_add(1, std::memory_order_relaxed);
    if (desc.type == resource_type::buffer) {
        ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
        if (d3d12_res) {
            uint64_t va = d3d12_res->GetGPUVirtualAddress();
            if (va != 0) {
                g_init_res_buf.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(g_mem_mutex);
                g_va_to_res[va] = { (uint32_t)desc.buffer.size, d3d12_res, va };
                g_res_to_va[res.handle] = va;
            }
        }
    } else if (desc.type == resource_type::texture_2d) {
        {
            std::lock_guard lock(g_tex_mutex);
            g_tex_dims[res.handle] = { desc.texture.width, desc.texture.height, (uint32_t)desc.texture.format,
                desc.texture.levels ? desc.texture.levels : 1u,
                desc.texture.depth_or_layers ? desc.texture.depth_or_layers : 1u, // mips/arr for RfxTexClass
                (uint32_t)(desc.usage & (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access)) == 0 &&
                    (uint64_t)desc.texture.width * desc.texture.height >= kTexHashFloorPx };
        }
        if (g_debug) {
            uint32_t tf = (uint32_t)desc.texture.format;
            uint32_t tw = desc.texture.width;
            uint32_t th = desc.texture.height;
            if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
                static ProbeBudget sn;
                if (sn.take(601))
                    logger::trace("STREAMNEW res=%llx %ux%u mips=%u fmt=%u initData=%d",
                        (unsigned long long)res.handle, tw, th, desc.texture.levels, tf, (int)(init_data != nullptr));
            }
        }
        if (init_data && init_data[0].data && res.handle)
            TexHashFromUpload(reinterpret_cast<ID3D12Resource*>(res.handle), (const uint8_t*)init_data[0].data, init_data[0].row_pitch);
    }
}

bool on_copy_buffer_region(command_list* cmd_list, resource source, uint64_t source_offset, resource dest, uint64_t dest_offset, uint64_t size)
{
    PROF(PROF_CopyBufRegion);
    RtNoteCopy(cmd_list, source, source_offset, dest, dest_offset, size); // RT M3: where the shader tables come from
    if (!dest.handle || !size) return false;
    if (const uint64_t va = reinterpret_cast<ID3D12Resource*>(dest.handle)->GetGPUVirtualAddress())
        IbHash::ForgetRange(va + dest_offset, size); // a mesh streamed over this region replaces the hashes taken inside it
    return false;
}

void on_destroy_resource(device*, resource res)
{
    RemovalPin::OnDestroyResource(reinterpret_cast<const void*>(res.handle));
    uint64_t gone_va = 0;
    uint64_t gone_bytes = 0;
    {
        std::lock_guard lock(g_mem_mutex);
        auto            it = g_res_to_va.find(res.handle);
        if (it != g_res_to_va.end()) {
            // Record BEFORE erasing: this is the only moment the handle is known to have been ours.
            MorphDump::NoteResourceErased(res.handle);
            gone_va = it->second;
            auto vi = g_va_to_res.find(gone_va);
            if (vi != g_va_to_res.end()) gone_bytes = vi->second.size;
            g_va_to_res.erase(it->second);
            g_res_to_va.erase(it);
        }
    }
    ForgetCharMeshVb0(gone_va, gone_bytes);    // a learned character VB0 dies with its buffer
    IbHash::ForgetRange(gone_va, gone_bytes);  // and so do the mesh hashes taken inside it
    IbAnchorForget(gone_va, gone_bytes);       // and the index-buffer anchor learned inside it
    RtForgetSkinOutRange(gone_va, gone_bytes); // and the RT skin rings inside it
    if (gone_va && gone_bytes) {
        std::lock_guard l(g_skincache_mtx); // and the skin-cache outputs
        std::erase_if(g_skincache_out, [&](const SkinOut& o) { return o.va >= gone_va && o.va < gone_va + gone_bytes; });
    }
    bool tex_known;
    {
        std::shared_lock l(g_tex_mutex);
        tex_known = g_res_texhash.count(res.handle) || g_modtex_res.count(res.handle) ||
            g_res_inherited.count(res.handle) || g_tex_dims.count(res.handle);
    }
    if (tex_known) {
        std::lock_guard l(g_tex_mutex);
        if (g_res_texhash.erase(res.handle)) g_res_texhash_gen.fetch_add(1, std::memory_order_acq_rel);
        g_modtex_res.erase(res.handle);
        g_res_inherited.erase(res.handle);
        g_tex_dims.erase(res.handle);
    }
    TexMipIdentityForget(res.handle);
    {
        std::lock_guard l(g_res_to_mod_mtx);
        g_res_to_mod_srv.erase(res.handle);
    }
    if (g_reloc_forget_on_destroy) g_buffer_reloc.ForgetResource((uint64_t)res.handle);
    g_res_to_slot.erase(res.handle); // re-stamp must not target a reused ptr's old slot
    g_res_state.erase(res.handle);
    SkInvalidateSnapshotFor(reinterpret_cast<ID3D12Resource*>(res.handle));
    ForgetGameCB(reinterpret_cast<ID3D12Resource*>(res.handle));
}

static void TrackView(device* device, resource_view view)
{
    if (view.handle == 0) return;
    g_view_to_res.init(reinterpret_cast<ID3D12Device*>(device->get_native())); // once: the page granularity
    resource res = device->get_resource_from_view(view);
    if (res.handle == 0) {
        g_view_to_res.forget(view.handle);
        return;
    }
    auto            res_desc = device->get_resource_desc(res);
    ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
    if (res_desc.type == resource_type::buffer) {
        ResInfo info = { (uint32_t)res_desc.buffer.size, d3d12_res,
            d3d12_res->GetGPUVirtualAddress(), false, 0, view.handle };
        info.is_buffer = true; // decided HERE, where res_desc.type is authoritative
        g_view_to_res.put(view.handle, info);
    } else if (res_desc.type == resource_type::texture_2d) {
        // Any texture an upload can name (TexInfo::hashable's test), not BC only: a mod may replace an R8 mask.
        const bool cand = (uint32_t)(res_desc.usage & (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access)) == 0 && (uint64_t)res_desc.texture.width * res_desc.texture.height >= kTexHashFloorPx; // texture_module.h
        g_view_to_res.put(view.handle, { 0, d3d12_res, 0, cand, 0, view.handle });
    } else
        g_view_to_res.forget(view.handle);
}

static void RecordViewInTrackedHeap(resource res, resource_view view);

void on_init_resource_view(device* device, resource res, resource_usage, const resource_view_desc& desc, resource_view view)
{
    PROF(PROF_InitView);
    g_init_view_all.fetch_add(1, std::memory_order_relaxed);
    if (!g_is_dx12 || view.handle == 0) return;
    if (res.handle)
        TrackView(device, view);
    else
        g_view_to_res.forget(view.handle);
    RecordViewInTrackedHeap(res, view);
    if (res.handle == 0) return;

    if (g_debug && desc.type == resource_view_type::texture_2d) {
        uint32_t tw = 0;
        uint32_t th = 0;
        uint32_t tf = 0;
        {
            TexInfo di{};
            if (TexDimsOf(res.handle, &di)) {
                tw = di.width;
                th = di.height;
                tf = di.format;
            }
        }
        if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
            static ProbeBudget sv;
            if (sv.take(601))
                logger::trace("STREAMSRV res=%llx %ux%u fmt=%u firstMip=%u mipCount=%u",
                    (unsigned long long)res.handle, tw, th, tf, desc.texture.first_level, desc.texture.levels);
        }
    }
}

static void TexNoSwapNote(uint64_t heap, uint32_t off, ID3D12Resource* res, const char* how)
{
    TexInfo di{};
    if (!res || !TexDimsOf(reinterpret_cast<uint64_t>(res), &di) || !di.hashable) return;
    const uint32_t h = TexHashOf(reinterpret_cast<uint64_t>(res));
    const uint32_t rule = TexRuleHashFor(h);
    if (h && !rule) return;
    bool shaped = false;
    {
        std::lock_guard l(g_match_mtx);
        for (uint32_t k = 0; k <= 4 && !shaped; ++k)
            shaped = g_match_profiles.count((((uint64_t)di.width << k) << 40) | (((uint64_t)di.height << k) << 16) |
                         (di.format & 0xffffu)) != 0;
    }
    if (!shaped) return;
    {
        static std::mutex                   m;
        static std::unordered_set<uint64_t> seen;
        static unsigned                     gen = ~0u;
        std::lock_guard                     l(m);
        const unsigned                      g = g_texnoswap_gen.load(std::memory_order_relaxed);
        if (gen != g) {
            gen = g;
            seen.clear();
        }
        if (!seen.insert(reinterpret_cast<uint64_t>(res)).second) return;
    }
    if (g_texnoswap_n.fetch_add(1, std::memory_order_relaxed) >= 400) return;
    logger::write("TEXNOSWAP f=%u heap=%llx slot=%u by %s: res=%p %ux%u m%u fmt=%u hash=%08x rule=%08x -- %s",
        g_frame_id, (unsigned long long)heap, off, how, (void*)res, di.width, di.height, di.mips, di.format,
        h, rule, !h ? "no identity yet" : "a target, no mod SRV selected");
}

bool on_update_descriptor_tables(device* device, uint32_t count, const descriptor_table_update* updates)
{
    PROF(PROF_DescUpdate);
    if (g_addon_disabled) return false;
    for (uint32_t u = 0; u < count; ++u) {
        const auto& update = updates[u];
        if (update.type == descriptor_type::constant_buffer && update.descriptors) {
            descriptor_heap cheap = { 0 };
            uint32_t        cbase = 0;
            device->get_descriptor_heap_offset(update.table, update.binding, 0, &cheap, &cbase);
            if (cheap.handle) {
                auto* brs = static_cast<const buffer_range*>(update.descriptors);
                for (uint32_t i = 0; i < update.count; ++i) {
                    if (!brs[i].buffer.handle) continue;
                    auto* br = reinterpret_cast<ID3D12Resource*>(brs[i].buffer.handle);
                    // size == UINT64_MAX means "rest of the buffer" -- resolve it so the byte-size probe is real.
                    uint64_t sz = brs[i].size;
                    if (sz == UINT64_MAX) {
                        D3D12_RESOURCE_DESC rd = br->GetDesc();
                        sz = (rd.Width > brs[i].offset) ? (rd.Width - brs[i].offset) : 0;
                    }
                    g_heap_cbv.put({ cheap.handle, cbase + i },
                        { br, br->GetGPUVirtualAddress() + brs[i].offset, (uint32_t)sz });
                }
            }
            continue;
        }
        if ((update.type != descriptor_type::shader_resource_view &&
                update.type != descriptor_type::unordered_access_view &&
                update.type != descriptor_type::buffer_shader_resource_view &&
                update.type != descriptor_type::buffer_unordered_access_view) ||
            !update.descriptors) continue;
        descriptor_heap heap = { 0 };
        uint32_t        base_offset = 0;
        device->get_descriptor_heap_offset(update.table, update.binding, 0, &heap, &base_offset);
        if (heap.handle == 0) continue;
        auto*                     rvs = static_cast<const resource_view*>(update.descriptors);
        auto*                     dev_u = reinterpret_cast<ID3D12Device*>(device->get_native());
        FlatHeapMap::Entry* const de = g_heap_to_res.enter(heap.handle); // once per update, not per descriptor
        for (uint32_t i = 0; i < update.count; ++i) {
            ResInfo                  vinfo;
            FlatHeapMap::Slot* const dp = FlatHeapMap::at(de, base_offset + i);
            if (!g_view_to_res.find(rvs[i].handle, vinfo)) {
                FlatHeapMap::erase_at(dp);
            } else {
                FlatHeapMap::set_at(dp, rvs[i].handle, g_view_seq.load(std::memory_order_acquire), g_frame_id, 1, nullptr);
                if (update.type == descriptor_type::shader_resource_view && vinfo.tex_cand && // hashable textures only
                    !RtHoldVanilla() &&                                                       //   RTHOLD: no swaps while held
                    !GlobalTexSwap(dev_u, heap.handle, base_offset + i, vinfo.res) &&         //   -> skip swap lock otherwise
                    g_texnoswap_arm.load(std::memory_order_relaxed) > 0)
                    TexNoSwapNote(heap.handle, base_offset + i, vinfo.res, "update");
            }
        }
    }
    return false;
}

static inline D3D12_CPU_DESCRIPTOR_HANDLE HeapCpuStart(uint64_t heap_handle)
{
    if (!heap_handle) return { 0 };
    return reinterpret_cast<ID3D12DescriptorHeap*>(heap_handle)->GetCPUDescriptorHandleForHeapStart();
}

bool TrackedSlotOfCpu(uint64_t cpu, HeapKey* out)
{
    static uint32_t s_inc = 0;
    if (!cpu || !g_d3d12_device) return false;
    if (!s_inc) s_inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t inc = s_inc;
    bool           hit = false;
    g_heap_to_res.for_each_heap([&](uint64_t heap, uint64_t lo, uint32_t n, bool, FlatHeapMap::Slot*) {
        if (!lo || cpu < lo || cpu >= lo + (uint64_t)n * inc) return false;
        *out = HeapKey{ heap, (uint32_t)((cpu - lo) / inc) };
        hit = true;
        return true;
    });
    return hit;
}

static void RecordViewInTrackedHeap(resource res, resource_view view)
{
    HeapKey key{};
    if (!TrackedSlotOfCpu(view.handle, &key)) return;
    ResInfo                  vinfo{};
    const bool               have = res.handle && g_view_to_res.find(view.handle, vinfo);
    FlatHeapMap::Slot* const sp = g_heap_to_res.slot(key.heap, key.offset, false);
    if (g_debug) {
        static std::atomic<unsigned> s_n{ 0 };
        static std::atomic<unsigned> s_stale{ 0 };
        ResInfo                      old{};
        if (g_heap_to_res.find(key, old) && old.res != reinterpret_cast<ID3D12Resource*>(res.handle))
            s_stale.fetch_add(1, std::memory_order_relaxed);
        const unsigned k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((k & 0xFFFF) == 0)
            logger::write("VIEW-IN-HEAP-N: %u views created straight into tracked heaps, %u over a record naming "
                          "another resource -- the record now follows the view",
                k, s_stale.load(std::memory_order_relaxed));
    }
    if (!have) {
        FlatHeapMap::erase_at(sp);
        return;
    }
    FlatHeapMap::set_at(sp, view.handle, g_view_seq.load(std::memory_order_acquire), g_frame_id, 3, nullptr);
}

bool on_copy_descriptor_tables(device* device, uint32_t count, const descriptor_table_copy* copies)
{
    PROF(PROF_DescCopy);
    if (g_addon_disabled) return false;
    auto* d3d12_dev = reinterpret_cast<ID3D12Device*>(device->get_native());
    // Also a device constant, not a per-call query.
    static uint32_t s_inc = 0;
    if (!s_inc && d3d12_dev) s_inc = d3d12_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t      inc = s_inc ? s_inc : 32u;
    uint64_t            prev_s = 0;
    uint64_t            prev_d = 0;
    FlatHeapMap::Entry* prev_se = nullptr;
    FlatHeapMap::Entry* prev_de = nullptr;
    uint64_t            prev_s_lo = 0;
    for (uint32_t u = 0; u < count; ++u) {
        const auto&     copy = copies[u];
        descriptor_heap s_heap = { 0 };
        uint32_t        s_off = 0;
        descriptor_heap d_heap = { 0 };
        uint32_t        d_off = 0;
        device->get_descriptor_heap_offset(copy.source_table, copy.source_binding, 0, &s_heap, &s_off);
        device->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, 0, &d_heap, &d_off);
        if (s_heap.handle == 0 || d_heap.handle == 0) continue;
        if (s_heap.handle != prev_s) {
            prev_se = g_heap_to_res.entry_of(s_heap.handle); // once per heap per call, not per descriptor
            prev_s_lo = prev_se ? prev_se->lo.load(std::memory_order_relaxed) : HeapCpuStart(s_heap.handle).ptr;
            prev_s = s_heap.handle;
        }
        if (d_heap.handle != prev_d) {
            prev_de = g_heap_to_res.enter(d_heap.handle);
            prev_d = d_heap.handle;
        }
        FlatHeapMap::Entry* const se = prev_se;
        FlatHeapMap::Entry* const de = prev_de;
        if (!de) continue;
        const uint64_t s_lo = prev_s_lo;
        if (!s_lo) continue;
        const uint32_t now = g_view_seq.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < copy.count; ++i) {
            FlatHeapMap::Slot* const dp = FlatHeapMap::at(de, d_off + i);
            if (!dp) break;
            uint64_t        src = s_lo + (uint64_t)(s_off + i) * inc;
            uint32_t        seq = now;
            ID3D12Resource* swapped = nullptr;
            if (const FlatHeapMap::Slot* const sp = FlatHeapMap::at(se, s_off + i)) {
                const uint32_t sq = sp->seq.load(std::memory_order_acquire);
                if (sq) {
                    src = sp->src.load(std::memory_order_relaxed);
                    seq = sq;
                }
                swapped = sp->swapped.load(std::memory_order_relaxed);
            }
            FlatHeapMap::set_at(dp, src, seq, g_frame_id, 2, swapped);
        }
    }
    return false;
}

void ResetDescriptorTracking()
{
    g_heap_to_res.clear_all();
    g_heap_cbv.erase_if([](const HeapCbv&) { return true; });
    g_res_to_slot.erase_if([](const HeapKey&) { return true; });
    logger::write("A1: addon re-enabled -- descriptor slot records cleared (stale while bypassed); they refill as the game writes descriptors");
}

bool UnstampResource(uint64_t resH, bool log)
{
    if (!g_d3d12_device || !resH) return false;
    HeapKey key{ 0, 0 };
    if (!g_res_to_slot.find(resH, key)) return false;
    if (!key.heap) return false;
    ResInfo ri;
    if (!g_heap_to_res.find(key, ri)) return false;
    if (ri.res != reinterpret_cast<ID3D12Resource*>(resH)) return false; // slot reused
    return UnstampSlot(key.heap, key.offset, log);
}

namespace {
struct SlotBackup {
    uint32_t idx;
    uint64_t mod_srv;
};
std::mutex                                                g_bk_mtx;
constexpr uint32_t                                        kBkBlock = 16384; // one CPU-only heap per block; blocks are added as needed
std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> g_bk_heaps;
std::unordered_map<HeapKey, SlotBackup, HeapKeyHash>      g_bk_of;
std::vector<uint32_t>                                     g_bk_free;
uint32_t                                                  g_bk_next = 0;

uint32_t DescInc()
{
    static const uint32_t inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return inc;
}
D3D12_CPU_DESCRIPTOR_HANDLE BackupSlot(uint32_t idx)
{ // under g_bk_mtx
    return { g_bk_heaps[idx / kBkBlock]->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)(idx % kBkBlock) * DescInc() };
}
void BackupsOfRetiredHeap(uint64_t heap)
{
    std::lock_guard l(g_bk_mtx);
    for (auto it = g_bk_of.begin(); it != g_bk_of.end();)
        if (it->first.heap == heap) {
            g_bk_free.push_back(it->second.idx);
            it = g_bk_of.erase(it);
        } else
            ++it;
}

bool RestoreFromBackup(const HeapKey& key, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    std::lock_guard l(g_bk_mtx);
    auto            it = g_bk_of.find(key);
    if (it == g_bk_of.end()) return false;
    const SlotBackup bk = it->second;
    g_bk_of.erase(it);
    g_bk_free.push_back(bk.idx);
    if (memcmp(reinterpret_cast<const void*>(dst.ptr), reinterpret_cast<const void*>(bk.mod_srv), DescInc()) != 0) {
        if (g_debug && DiagOnce("UNSTAMP-NOTOURS", (int)(key.offset ^ (key.heap >> 4))))
            logger::write("UNSTAMP-NOTOURS heap %llx slot %u: recorded as stamped, but it no longer holds the mod SRV's "
                          "bytes -- the game wrote it after the stamp; the slot is left alone",
                (unsigned long long)key.heap, key.offset);
        return false;
    }
    g_d3d12_device->CopyDescriptorsSimple(1, dst, BackupSlot(bk.idx), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}
}

void BackupBeforeStamp(uint64_t heap_h, uint32_t offset, uint64_t mod_srv)
{
    if (!g_d3d12_device || !heap_h || !mod_srv || g_heap_to_res.shader_visible(heap_h)) return;
    const HeapKey key{ heap_h, offset };
    ResInfo       ri;
    if (g_heap_to_res.find(key, ri) && ri.swapped) return;
    const D3D12_CPU_DESCRIPTOR_HANDLE slot{ reinterpret_cast<ID3D12DescriptorHeap*>(heap_h)->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)offset * DescInc() };
    std::lock_guard                   l(g_bk_mtx);
    g_heap_to_res.retire_hook.store(BackupsOfRetiredHeap, std::memory_order_release);
    auto     it = g_bk_of.find(key);
    uint32_t idx;
    if (it != g_bk_of.end())
        idx = it->second.idx;
    else if (!g_bk_free.empty()) {
        idx = g_bk_free.back();
        g_bk_free.pop_back();
    } else {
        if (g_bk_next == g_bk_heaps.size() * kBkBlock) {
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
            D3D12_DESCRIPTOR_HEAP_DESC                   hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kBkBlock, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
            if (FAILED(g_d3d12_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h)))) return;
            g_bk_heaps.push_back(h);
        }
        idx = g_bk_next++;
    }
    g_d3d12_device->CopyDescriptorsSimple(1, BackupSlot(idx), slot, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_bk_of[key] = { idx, mod_srv };
}

bool UnstampSlot(uint64_t heap_h, uint32_t offset, bool log)
{
    if (!g_d3d12_device || !heap_h) return false;
    const HeapKey key{ heap_h, offset };
    ResInfo       ri;
    if (!g_heap_to_res.find(key, ri)) return false;
    if (!ri.res || !ri.swapped) return false; // never stamped
    const uint64_t                    resH = reinterpret_cast<uint64_t>(ri.res);
    auto*                             heap = reinterpret_cast<ID3D12DescriptorHeap*>(key.heap);
    const uint32_t                    inc = g_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint64_t                    lo = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const D3D12_CPU_DESCRIPTOR_HANDLE dst{ (SIZE_T)(lo + (uint64_t)key.offset * inc) };
    HeapKey                           src{};
    const bool                        src_tracked = ri.view_handle && TrackedSlotOfCpu(ri.view_handle, &src);
    static thread_local int           t_depth = 0;
    if (src_tracked && !(src == key) && t_depth < 2) {
        ResInfo sri;
        if (g_heap_to_res.find(src, sri) && sri.swapped) {
            ++t_depth;
            UnstampSlot(src.heap, src.offset, false);
            --t_depth;
        }
    }
    ResInfo    sri{};
    const bool src_ok = src_tracked
        ? !g_heap_to_res.shader_visible(src.heap) && g_heap_to_res.find(src, sri) && sri.res == ri.res && !sri.swapped
        : g_view_to_res.find(ri.view_handle, sri) && sri.res == ri.res;
    if (!ri.view_handle || ri.view_handle == dst.ptr || !src_ok) {
        if (!g_heap_to_res.shader_visible(key.heap) && RestoreFromBackup(key, dst)) {
            g_heap_to_res.set_swapped(key, nullptr);
            if (log) logger::write("UNSTAMP res=%p slot %u: the game's descriptor restored from the copy kept before the stamp", (void*)resH, key.offset);
            return true;
        }
        if (log) logger::write("UNSTAMP res=%p slot %u: no unstamped CPU-only source for the game's descriptor -- the mod SRV stays", (void*)resH, key.offset);
        return false;
    }
    g_d3d12_device->CopyDescriptorsSimple(1, dst, { (SIZE_T)ri.view_handle }, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_heap_to_res.set_swapped(key, nullptr);
    if (log) logger::write("UNSTAMP res=%p slot %u: the game's descriptor restored (degenerate identity)", (void*)resH, key.offset);
    return true;
}

std::vector<std::pair<HeapKey, uint64_t>> StampedCpuOnlySlots()
{
    std::vector<std::pair<HeapKey, uint64_t>> out;
    g_heap_to_res.for_each_heap([&](uint64_t h, uint64_t, uint32_t n, bool vis, FlatHeapMap::Slot* arr) {
        if (vis) return false;
        for (uint32_t o = 0; arr && o < n; ++o) {
            if (!arr[o].swapped.load(std::memory_order_acquire)) continue;
            ResInfo ri;
            if (g_heap_to_res.find({ h, o }, ri)) out.push_back({ HeapKey{ h, o }, reinterpret_cast<uint64_t>(ri.res) });
        }
        return false;
    });
    return out;
}

UnstampTally UnstampAll(void (*hold)(uint64_t res))
{
    UnstampTally          t;
    std::vector<uint64_t> recorded;
    g_res_to_slot.for_each([&](const uint64_t& r, const HeapKey&) { recorded.push_back(r); });
    for (uint64_t r : recorded) {
        if (!UnstampResource(r, /*log*/ false)) continue;
        ++t.recorded;
        if (hold) hold(r);
    }
    const std::vector<std::pair<HeapKey, uint64_t>> sources = StampedCpuOnlySlots();
    t.cpu_only_stamped = sources.size();
    for (const auto& [key, res] : sources) {
        if (!UnstampSlot(key.heap, key.offset)) continue;
        ++t.cpu_only;
        if (hold) hold(res);
    }
    return t;
}

void SetAddonDisabled(bool dis)
{
    if (dis == g_addon_disabled.load()) return;
    if (dis) {
        const UnstampTally t = UnstampAll(nullptr);
        g_addon_disabled.store(true);
        logger::write("A1: addon DISABLED -- %u stamped slot(s) given the game's texture back, and %u of %zu still "
                      "stamped in CPU-only heaps",
            t.recorded, t.cpu_only, t.cpu_only_stamped);
    } else {
        ResetDescriptorTracking(); // BEFORE the hooks resume: nothing may read a stale slot
        g_addon_disabled.store(false);
    }
}

bool RestampResource(uint64_t resH)
{
    if (!g_d3d12_device || !resH || !g_tex_lineage) return false;
    if (RtHoldVanilla()) { // RTHOLD: held as a miss; RtPresentTick retries the set when the hold ends
        TexHoldForRetry(resH);
        return false;
    }
    HeapKey key{ 0, 0 };
    if (!g_res_to_slot.find(resH, key) || !key.heap) {
        if (g_debug && DiagOnce("RESTAMP-NOSLOT", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: no slot recorded for it", (void*)resH);
        return false;
    }
    ID3D12Resource* res = nullptr;
    {
        ResInfo ri;
        if (g_heap_to_res.find(key, ri)) res = ri.res;
    }
    if (res != reinterpret_cast<ID3D12Resource*>(resH)) {
        if (g_debug && DiagOnce("RESTAMP-REUSED", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: its slot heap=%llx off=%u now holds %p", (void*)resH,
                (unsigned long long)key.heap, (unsigned)key.offset, (void*)res);
        return false;
    }
    const bool ok = GlobalTexSwap(g_d3d12_device, key.heap, key.offset, res);
    if (!ok && g_debug && DiagOnce("RESTAMP-REFUSED", (int)(resH >> 4)))
        logger::write("RESTAMP-FAIL res=%p: slot heap=%llx off=%u is its own, the swap itself declined (%s)", (void*)resH,
            (unsigned long long)key.heap, (unsigned)key.offset, GlobalTexSwapDecline()[0] ? GlobalTexSwapDecline() : "miss: no mod SRV");
    return ok;
}
