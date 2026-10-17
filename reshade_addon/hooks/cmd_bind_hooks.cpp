// The bind hooks: everything the game binds, written down as it binds it. See cmd_bind_hooks.h.
#include "../addon_common.h"
#include "cmd_bind_hooks.h"
#include "../instrumentation/removal_pin.h"
#include "../migoto/dx12_bridge.h"
#include "../instrumentation/diag.h"
#include "../instrumentation/hot_path.h"
#include "pipeline_hooks.h" // the PSO -> shader-hash table on_bind_pipeline stamps from
#include "../hud/uid_hide.h"

void on_bind_pipeline(command_list* cmd_list, pipeline_stage type, pipeline pipeline)
{
    PROF(PROF_BindPipeline);
    uint64_t vc = 0;
    uint64_t ps = 0;
    uint64_t gs = 0;
    PsoBoundHashes(pipeline.handle, &vc, &ps, &gs); // lock-free
    if ((uint32_t)(type & pipeline_stage::vertex_shader) != 0) {
        cmd_list->set_private_data(s_iid_vs_hash, vc);
        // Track the bound graphics PSO so the custom-material path can derive a blend variant of it.
        auto& st = CmdGfx(cmd_list);
        st.gfx_pso = pipeline.handle;
        st.gfx_set = 0;
        if (ps && !g_addon_disabled) {
            uint64_t v = g_hide_uid ? PsoUidVariant(cmd_list->get_device(), pipeline.handle, g_uid_hide.Target()) : 0;
            if (!v && g_decensor) v = PsoDecensorVariant(cmd_list->get_device(), pipeline.handle);
            if (v) {
                reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native())->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(v));
                st.gfx_set = v;
            }
        }
    }
    if ((uint32_t)(type & pipeline_stage::compute_shader) != 0) {
        cmd_list->set_private_data(s_iid_cs_hash, vc);
        {
            CmdGfx(cmd_list).cmp_pso = pipeline.handle;
        }
    }
    if ((uint32_t)(type & pipeline_stage::pixel_shader) != 0)
        cmd_list->set_private_data(s_iid_ps_hash, ps);
    if ((uint32_t)(type & pipeline_stage::geometry_shader) != 0)
        cmd_list->set_private_data(s_iid_gs_hash, gs);
}

void on_bind_vertex_buffers(command_list* cmd_list, uint32_t first, uint32_t count, const resource* buffers, const uint64_t* offsets, const uint32_t* strides)
{
    PROF(PROF_BindVB);
    auto&          state = CmdGfx(cmd_list);
    const uint32_t n = first < 16 ? std::min<uint32_t>(count, 16 - first) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (buffers[i].handle == 0) continue;
        ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(buffers[i].handle);
        const uint64_t  off = offsets ? offsets[i] : 0;
        const UINT64    width = d3d12_res->GetDesc().Width;
        state.vb[first + i] = {
            .va = d3d12_res->GetGPUVirtualAddress() + off,
            .size = (width > off) ? (uint32_t)(width - off) : 0u,
            .stride = strides ? strides[i] : 0,
            .res = d3d12_res,
            .res_width = width,
        };
    }
}

// Track the game's bound index buffer so we can restore it after our override.
void on_bind_index_buffer(command_list* cmd_list, resource buffer, uint64_t offset, uint32_t index_size)
{
    PROF(PROF_BindIB);
    auto& state = CmdGfx(cmd_list);
    if (buffer.handle != 0) {
        auto* d3d12_res = reinterpret_cast<ID3D12Resource*>(buffer.handle);
        state.ib = {
            .va = d3d12_res->GetGPUVirtualAddress() + offset,
            .size = (uint32_t)d3d12_res->GetDesc().Width - (uint32_t)offset,
            .index_bytes = index_size, // 2 or 4
            .res = d3d12_res,
        };
    } else {
        state.ib.va = 0;
        state.ib.res = nullptr;
    }
}

void on_bind_render_targets(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv)
{
    PROF(PROF_BindRT);
    const uint32_t gen = dsv.handle ? g_rt_pass_gen.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    CmdRT&         e = CmdRt(cmd_list);
    e = { count, dsv.handle != 0, dsv.handle, gen };
    const uint32_t n = (count < 8u) ? count : 8u;
    for (uint32_t i = 0; i < n; ++i) e.rtv[i] = rtvs ? rtvs[i].handle : 0;
    e.valid = true; // AFTER the aggregate assignment above, which resets it
}

void on_bind_viewports(command_list* cmd_list, uint32_t first, uint32_t count, const viewport* vps)
{
    if (!cmd_list || !vps || first != 0) return; // partial updates are not a thing in this game
    auto&          st = CmdGfx(cmd_list);
    const uint32_t n = count < 16 ? count : 16;
    for (uint32_t i = 0; i < n; ++i)
        st.vp[i] = { vps[i].x, vps[i].y, vps[i].width, vps[i].height, vps[i].min_depth, vps[i].max_depth };
    st.vp_count = n;
}

void on_bind_scissor_rects(command_list* cmd_list, uint32_t first, uint32_t count, const rect* rects)
{
    if (!g_restore_nonpso) return;
    if (!cmd_list || !rects || first != 0) return;
    auto&          st = CmdGfx(cmd_list);
    const uint32_t n = count < 16 ? count : 16;
    for (uint32_t i = 0; i < n; ++i)
        st.scissor[i] = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
    st.scissor_count = n;
}

void on_bind_pipeline_states(command_list* cmd_list, uint32_t count, const dynamic_state* states, const uint32_t* values)
{
    if (!g_restore_nonpso) return;
    if (!cmd_list || !states || !values) return;
    auto& st = CmdGfx(cmd_list);
    for (uint32_t i = 0; i < count; ++i) {
        switch (states[i]) {
        case dynamic_state::primitive_topology:
            st.topology = values[i];
            break;
        case dynamic_state::blend_constant: {
            // ReShade packs the blend constant as one RGBA8 word; D3D12 wants four floats.
            const uint32_t v = values[i];
            st.blend_factor[0] = ((v) & 0xff) / 255.0f;
            st.blend_factor[1] = ((v >> 8) & 0xff) / 255.0f;
            st.blend_factor[2] = ((v >> 16) & 0xff) / 255.0f;
            st.blend_factor[3] = ((v >> 24) & 0xff) / 255.0f;
            st.blend_factor_set = true;
            break;
        }
        case dynamic_state::front_stencil_reference_value:
            st.stencil_ref = values[i];
            st.stencil_ref_set = true;
            break;
        default: break;
        }
    }
}

bool ResolveBound(command_list* cmd_list, char stage, TargetClass kind, uint32_t reg,
    ID3D12Resource** out_res, uint64_t* out_va, uint32_t* out_size, BindTrace* trace)
{
    if (out_res) *out_res = nullptr;
    if (out_va) *out_va = 0;
    if (out_size) *out_size = 0;
    auto fail = [trace](int step) { if (trace) trace->step = step; return false; };
    if (!cmd_list) return fail(1);

    const uint64_t lay = (stage == 'c') ? CmdCmpLayout(cmd_list) : CmdGfxLayout(cmd_list);
    if (trace) trace->layout = lay;
    if (!lay) return fail(2);

    RegBind rb;
    {
        std::lock_guard l(g_layout_regmap_mtx);
        auto            it = g_layout_regmap.find(lay);
        if (it == g_layout_regmap.end() || !it->second.built) return fail(3);
        const LayoutRegMap& m = it->second;
        const bool          cs = (stage == 'c');
        switch (kind) {
        case TargetClass::ShaderResource:
            if (reg >= std::size(m.t_cs)) return fail(4);
            rb = cs ? m.t_cs[reg] : m.t_gfx[reg];
            break;
        case TargetClass::UnorderedAccessView:
            if (reg >= std::size(m.u_cs)) return fail(4);
            rb = cs ? m.u_cs[reg] : m.u_gfx[reg];
            break;
        case TargetClass::ConstantBuffer:
            if (reg >= std::size(m.b_cs)) return fail(4);
            rb = cs ? m.b_cs[reg] : m.b_gfx[reg];
            break;
        default: return fail(4);
        }
    }
    if (trace) {
        trace->param = rb.param;
        trace->table = rb.is_table;
    }
    if (rb.param == 0xFF || rb.param >= 64) return fail(5);

    // ROOT DESCRIPTOR: recorded per root param as we saw it pushed.
    if (!rb.is_table) {
        ID3D12Resource* res = nullptr;
        uint64_t        va = 0;
        uint32_t        sz = 0;
        if (kind == TargetClass::ConstantBuffer) {
            const RootCbvBind& cb = CmdCbv(cmd_list).binds[rb.param];
            res = cb.res;
            va = cb.va;
            sz = cb.size;
        } else {
            const RootParam& rp = CmdGfx(cmd_list).root[rb.param];
            res = rp.res;
            va = rp.va;
        }
        if (trace) trace->res = res;
        if (!res && !va) return fail(6);
        // Liveness + the authoritative VA in one lookup, the same rule every publish site follows.
        if (res) {
            std::lock_guard lock(g_mem_mutex);
            auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(res));
            if (vi == g_res_to_va.end()) return fail(7);
            if (!va) va = vi->second;
        }
        if (sz == 0xFFFFFFFFu && res) {
            const D3D12_RESOURCE_DESC rd = res->GetDesc();
            const uint64_t            base = res->GetGPUVirtualAddress();
            const uint64_t            off = (va >= base) ? (va - base) : 0;
            sz = (rd.Width > off) ? (uint32_t)std::min<uint64_t>(rd.Width - off, 0xFFFFFFFEull) : 0;
        }
        if (kind == TargetClass::ConstantBuffer && sz) {
            sz = (sz + 15u) & ~15u;
            if (sz > 65536u) sz = 65536u;
        }
        if (out_res) *out_res = res;
        if (out_va) *out_va = va;
        if (out_size) *out_size = sz;
        return true;
    }

    // DESCRIPTOR TABLE: the bound table for that root param, plus the offset the layout declared.
    const RootParam&       rp = CmdGfx(cmd_list).root[rb.param];
    const descriptor_table dt = (stage == 'c') ? rp.compute_table() : rp.table;
    if (!dt.handle) return fail(8);
    descriptor_heap heap = { 0 };
    uint32_t        slot = 0;
    cmd_list->get_device()->get_descriptor_heap_offset(dt, rb.offset, 0, &heap, &slot);
    if (trace) {
        trace->heap = heap.handle;
        trace->slot = slot;
        trace->offset = rb.offset;
        for (uint32_t k = 0; k < 8 && slot >= rb.offset; ++k) {
            ResInfo t{};
            if (g_heap_to_res.find({ heap.handle, slot - rb.offset + k }, t)) {
                trace->table_res[k] = reinterpret_cast<ID3D12Resource*>(t.res);
                trace->table_size[k] = t.size;
                trace->table_wrote[k] = t.wrote_frame;
                trace->table_src[k] = t.wrote_src;
            }
        }
    }
    if (!heap.handle) return fail(9);
    ResInfo    ri{};
    const bool have_ri = g_heap_to_res.find({ heap.handle, slot }, ri);
    if (trace) {
        trace->res = reinterpret_cast<ID3D12Resource*>(ri.res);
        trace->size = ri.size;
    }
    if (!have_ri || !ri.res || !ri.size) return fail(10);
    uint64_t va = 0;
    {
        std::lock_guard lock(g_mem_mutex);
        auto            vi = g_res_to_va.find(reinterpret_cast<uint64_t>(ri.res));
        if (vi != g_res_to_va.end()) va = vi->second;
    }
    if (!va) return fail(11); // destroyed since the bind: unbound, not a crash
    if (out_res) *out_res = reinterpret_cast<ID3D12Resource*>(ri.res);
    if (out_va) *out_va = va;
    if (out_size) *out_size = ri.size;
    return true;
}

void on_push_descriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t layout_param, const descriptor_table_update& update)
{
    PROF(PROF_PushDesc);
    if (g_addon_disabled) return;
    if (update.count == 0 || layout_param >= 64) return;
    if ((uint32_t)(stages & shader_stage::vertex) != 0 || (uint32_t)(stages & shader_stage::all_graphics) != 0) {
        CmdGfxLayout(cmd_list) = layout.handle;
    }
    if ((uint32_t)(stages & shader_stage::compute) != 0 || stages == shader_stage::all) {
        CmdCmpLayout(cmd_list) = layout.handle;
    }

    if (update.type == descriptor_type::acceleration_structure) { // RT M3: a TLAS bound as a root SRV (the view is its address)
        CmdGfx(cmd_list).root[layout_param].view_va = static_cast<const resource_view*>(update.descriptors)[0].handle;
        CmdGfx(cmd_list).root[layout_param].desc_type = 2;
    }
    if (update.type == descriptor_type::shader_resource_view || update.type == descriptor_type::unordered_access_view) {
        CmdGfx(cmd_list).root[layout_param].view_va = static_cast<const resource_view*>(update.descriptors)[0].handle; // RT M3
        resource res = cmd_list->get_device()->get_resource_from_view(static_cast<const resource_view*>(update.descriptors)[0]);
        if (res.handle != 0) {
            auto*      d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
            RootParam& rp = CmdGfx(cmd_list).root[layout_param];
            rp.va = d3d12_res->GetGPUVirtualAddress();
            rp.res = d3d12_res;
            rp.desc_type = (update.type == descriptor_type::unordered_access_view) ? 3 : 2; // SRV=2, UAV=3
        }
    } else if (update.type == descriptor_type::constant_buffer) {
        // CBV: descriptors point to buffer_range records (resource + offset + size)
        const auto* ranges = static_cast<const buffer_range*>(update.descriptors);
        if (ranges[0].buffer.handle != 0) {
            auto*    d3d12_res = reinterpret_cast<ID3D12Resource*>(ranges[0].buffer.handle);
            uint64_t va = d3d12_res->GetGPUVirtualAddress() + ranges[0].offset;
            uint64_t bytes = ranges[0].size;
            if (bytes == UINT64_MAX) {
                const D3D12_RESOURCE_DESC rd = d3d12_res->GetDesc();
                bytes = (rd.Width > ranges[0].offset) ? (rd.Width - ranges[0].offset) : 0ull;
            }
            const uint32_t sz = (bytes > UINT32_MAX) ? 0u : (uint32_t)bytes; /* 0 = extent unknown */
            auto&          cbvs = CmdCbv(cmd_list);
            cbvs.binds[layout_param] = { d3d12_res, va, sz };
        }
    }
}

void on_close_command_list(command_list* cmd_list)
{
    PROF(PROF_CmdClose);
    HotPath::OnClose(cmd_list);
    if (!cmd_list) return;
    auto* nl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    MigotoPort::ListBuffersToCommon(nl);
    RemovalPin::OnClose(nl);
}

void on_reset_command_list(command_list* cmd_list)
{
    PROF(PROF_CmdReset);
    if (!cmd_list) return;
    ++t_list_resets;
    HotPath::OnReset(cmd_list);
    RemovalPin::OnReset(reinterpret_cast<const void*>(cmd_list->get_native())); // the tally is per recording
    MigotoPort::ListBuffersForget(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()));
    const uintptr_t key = reinterpret_cast<uintptr_t>(cmd_list);
    CmdBlock*       b = g_cmd_blocks.get(key);
    if (b) {
        ++b->gen; // a new recording (CmdGen): independent of the clear A/B below
        for (HeapBind& hb : b->heaps) hb.fresh = false;
    }
    if (!g_cmd_reset_clears) return;
    if (b) {
        b->gfx = CmdState{};
        b->cbv = CmdCbvBinds{};
        b->rt = CmdRT{};
        b->gfx_layout = 0;
        b->cmp_layout = 0;
    }
    g_cmd_resets.fetch_add(1, std::memory_order_relaxed);
}

static void NoteGameHeap(command_list* cmd_list, uint64_t h)
{
    if (!h) return;
    CmdHeaps& hs = CmdHeap(cmd_list);
    for (const HeapBind& hb : hs)
        if (hb.fresh && h >= hb.lo && h < hb.hi) return;
    descriptor_heap dh = { 0 };
    uint32_t        base = 0;
    cmd_list->get_device()->get_descriptor_heap_offset({ h }, 0, 0, &dh, &base);
    auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(dh.handle);
    if (!heap) return;
    const D3D12_DESCRIPTOR_HEAP_DESC d = heap->GetDesc();
    const int                        k = d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 0 : d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1
                                                                                                                                             : -1;
    if (k < 0) return;
    auto*          dev = reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native());
    const uint64_t lo = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    hs[k] = { .heap = heap, .lo = lo, .hi = lo + (uint64_t)d.NumDescriptors * dev->GetDescriptorHandleIncrementSize(d.Type), .fresh = true };
}

ID3D12DescriptorHeap* GameHeap(command_list* cmd_list, int type)
{
    HeapBind& hb = CmdHeap(cmd_list)[type];
    if (!hb.heap) return nullptr;
    if (hb.fresh) return hb.heap;
    descriptor_heap dh = { 0 };
    uint32_t        base = 0;
    cmd_list->get_device()->get_descriptor_heap_offset({ hb.lo }, 0, 0, &dh, &base);
    if (dh.handle != reinterpret_cast<uint64_t>(hb.heap)) {
        hb = {};
        return nullptr;
    }
    if (g_debug && DiagOnce("HEAPCARRY", type))
        logger::write("HEAPCARRY: no %s table bound yet in this recording -- the heap this list used last time is still alive and is the one restored",
            type ? "sampler" : "CBV_SRV_UAV");
    return hb.heap;
}

void on_bind_descriptor_tables(command_list* cmd_list, shader_stage stages, [[maybe_unused]] pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table* tables)
{
    PROF(PROF_BindDescTables);
    if (g_addon_disabled) return;
    auto&          state = CmdGfx(cmd_list);
    const bool     to_compute = ((uint32_t)(stages & shader_stage::all_compute) != 0);
    const uint32_t n = first < 64 ? std::min<uint32_t>(count, 64 - first) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        RootParam& rp = state.root[first + i];
        rp.table = tables[i];
        NoteGameHeap(cmd_list, tables[i].handle);
        if (!to_compute) continue;
        rp.ctable = tables[i];
#ifdef WWMI_DEBUG
        rp.ctable_note = { .layout = layout.handle, .frame = g_frame_id, .seq = ++state.ctable_seq_next };
#endif
    }
}
