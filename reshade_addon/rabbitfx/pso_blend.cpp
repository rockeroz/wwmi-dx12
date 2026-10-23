// pso_blend.cpp -- custom-material blend PSO derivation and the shader patch variants (see header).
#include <reshade.hpp>
#include <d3d12.h>
#include <d3d11.h> // D3D11_BLEND_DESC: the state-only [CustomShader] seam hands over the PARSED desc
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <utility>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <sstream>
#include <cstring>

#include "../../src/log.h"
#include "../../src/fnv.h"
#include "../config.h"
#include "pso_blend.h"
#include "rfx_dxil.h"
#include "../hooks/pipeline_hooks.h" // PsoSetDecensorVariant
#include "../migoto/dx12_bridge.h"   // MigotoPort::DumpDir -- where the cutout's shader corpus goes
#include "../mods/wwmi_runtime.h"    // WwmiReadFile
#include "../build_stamp.h"          // WWMI_RFX_SRC_HASH: a cached patch is reused only by the patcher that wrote it

using namespace reshade::api;

using CodeBlob = std::shared_ptr<const std::vector<uint8_t>>;
struct PsoClone {
    std::vector<pipeline_subobject>   subs;  // owns nothing directly; data() points into `store` / `code`
    std::vector<std::vector<uint8_t>> store; // backing storage for every element array / string
    std::vector<CodeBlob>             code;  // the shaders' bytecode, shared with every clone of the same shader
    pipeline_layout                   layout = { 0 };
    int                               blend_idx = -1; // index of the blend_state subobject in `subs`
};
static std::unordered_map<uint64_t, PsoClone> g_pso_clone; // src PSO handle -> deep copy, until the PSO is destroyed
static std::mutex                             g_pso_clone_mtx;
static size_t                                 g_pso_clone_bytes = 0; // under g_pso_clone_mtx; the shared code not included
static unsigned                               g_pso_clone_forgotten = 0;
static unsigned                               g_variants_released = 0;
static size_t                                 CloneBytes(const PsoClone& c)
{
    size_t n = 0;
    for (const auto& v : c.store) n += v.size();
    return n;
}

// One copy of each shader's bytecode, however many PSOs use it; it lives as long as a clone holds it.
static std::unordered_map<uint64_t, std::weak_ptr<const std::vector<uint8_t>>> g_code_pool;
static std::mutex                                                              g_code_pool_mtx;
static size_t                                                                  g_code_pool_prune_at = 1024; // under g_code_pool_mtx
static CodeBlob                                                                PooledCode(const void* p, size_t n)
{
    const uint64_t  key = Fnv1a64(p, n) ^ (uint64_t)n;
    std::lock_guard l(g_code_pool_mtx);
    auto&           slot = g_code_pool[key];
    if (CodeBlob b = slot.lock(); b && b->size() == n && memcmp(b->data(), p, n) == 0) return b;
    CodeBlob b = std::make_shared<const std::vector<uint8_t>>((const uint8_t*)p, (const uint8_t*)p + n);
    slot = b;
    if (g_code_pool.size() >= g_code_pool_prune_at) {
        for (auto it = g_code_pool.begin(); it != g_code_pool.end();) it = it->second.expired() ? g_code_pool.erase(it) : std::next(it);
        g_code_pool_prune_at = g_code_pool.size() * 2 + 1024;
    }
    return b;
}
// A variant remembers its SOURCE so PsoCloneForget can find it under the composite key.
struct PsoVariant {
    uint64_t             src;
    ID3D12PipelineState* pso;
};
static std::unordered_map<uint64_t, PsoVariant> g_blend_variant; // (src^blendkey) -> variant
static std::mutex                               g_blend_variant_mtx;
static std::unordered_map<uint64_t, PsoVariant> g_layout_variant; // (src^layoutkey) -> variant; pso nullptr = could not be made
static std::mutex                               g_layout_variant_mtx;
// Sources whose clone was missing when a variant was asked for, logged once each (under the variant mutex).
static std::unordered_set<uint64_t> g_blend_miss_logged;
// Source PSO -> its decensored pixel shader, once the decensor-only variant has settled with a patch.
static std::unordered_map<uint64_t, CodeBlob> g_decensor_ps;
static std::mutex                             g_decensor_ps_mtx;
// Source PSO | decensor << 60 -> its RabbitFX-patched pixel shader, once that variant has settled.
static std::unordered_map<uint64_t, CodeBlob> g_rfx_ps;
static std::mutex                             g_rfx_ps_mtx;

static CodeBlob DecensoredPs(uint64_t src_pso)
{
    std::lock_guard l(g_decensor_ps_mtx);
    auto            it = g_decensor_ps.find(src_pso);
    return it == g_decensor_ps.end() ? nullptr : it->second;
}

static CodeBlob RfxPatchedPs(uint64_t src_pso, bool decensor)
{
    std::lock_guard l(g_rfx_ps_mtx);
    auto            it = g_rfx_ps.find(src_pso | ((uint64_t)decensor << 60));
    return it == g_rfx_ps.end() ? nullptr : it->second;
}

static void ForgetPatchedPs(uint64_t src_pso)
{
    {
        std::lock_guard d(g_decensor_ps_mtx);
        g_decensor_ps.erase(src_pso);
    }
    std::lock_guard r(g_rfx_ps_mtx);
    g_rfx_ps.erase(src_pso);
    g_rfx_ps.erase(src_pso | (1ull << 60));
}

static void SwapPixelShader(std::vector<pipeline_subobject>& subs, const CodeBlob& ps, shader_desc& storage)
{
    for (auto& so : subs)
        if (so.type == pipeline_subobject_type::pixel_shader && so.count == 1 && so.data) {
            storage = *static_cast<const shader_desc*>(so.data);
            storage.code = ps->data();
            storage.code_size = ps->size();
            so.data = &storage;
            return;
        }
}

static size_t SubobjElemSize(pipeline_subobject_type t)
{
    using T = pipeline_subobject_type;
    switch (t) {
    case T::vertex_shader:
    case T::hull_shader:
    case T::domain_shader:
    case T::geometry_shader:
    case T::pixel_shader:
    case T::compute_shader:
    case T::amplification_shader:
    case T::mesh_shader:
    case T::raygen_shader:
    case T::any_hit_shader:
    case T::closest_hit_shader:
    case T::miss_shader:
    case T::intersection_shader:
    case T::callable_shader: return sizeof(shader_desc);
    case T::input_layout: return sizeof(input_element);
    case T::stream_output_state: return sizeof(stream_output_desc);
    case T::blend_state: return sizeof(blend_desc);
    case T::rasterizer_state: return sizeof(rasterizer_desc);
    case T::depth_stencil_state: return sizeof(depth_stencil_desc);
    case T::primitive_topology: return sizeof(primitive_topology);
    case T::depth_stencil_format: return sizeof(format);
    case T::render_target_formats: return sizeof(format);
    case T::dynamic_pipeline_states: return sizeof(dynamic_state);
    case T::sample_mask:
    case T::sample_count:
    case T::viewport_count:
    case T::max_vertex_count:
        return sizeof(uint32_t);
    default: return 0;
    }
}

// Deep-copy a subobject array into a self-contained PsoClone (pointers fixed up to `store`).
static bool DeepCopySubobjects(uint32_t n, const pipeline_subobject* src, PsoClone& c)
{
    c.subs.resize(n);
    c.store.reserve(n * 4 + 8); // element arrays + per-shader code/entry + semantic strings
    auto blob = [&](const void* p, size_t bytes) -> void* {
        c.store.emplace_back((const uint8_t*)p, (const uint8_t*)p + bytes); // moving the outer vec keeps inner data() valid
        return c.store.back().data();
    };
    for (uint32_t i = 0; i < n; ++i) {
        pipeline_subobject so = src[i];
        size_t             esz = SubobjElemSize(so.type);
        if (esz == 0 || so.data == nullptr || so.count == 0) {
            c.subs[i] = so;
            continue;
        }
        void* base = blob(so.data, esz * so.count);
        so.data = base;
        switch (so.type) {
        case pipeline_subobject_type::vertex_shader:
        case pipeline_subobject_type::hull_shader:
        case pipeline_subobject_type::domain_shader:
        case pipeline_subobject_type::geometry_shader:
        case pipeline_subobject_type::pixel_shader:
        case pipeline_subobject_type::compute_shader:
        case pipeline_subobject_type::amplification_shader:
        case pipeline_subobject_type::mesh_shader: {
            auto* sd = static_cast<shader_desc*>(base);
            for (uint32_t e = 0; e < so.count; ++e) {
                if (sd[e].code && sd[e].code_size) {
                    c.code.push_back(PooledCode(sd[e].code, sd[e].code_size));
                    sd[e].code = c.code.back()->data();
                }
                if (sd[e].entry_point) sd[e].entry_point = (const char*)blob(sd[e].entry_point, strlen(sd[e].entry_point) + 1);
                if (sd[e].spec_constants && sd[e].spec_constant_ids)
                    sd[e].spec_constant_ids = (const uint32_t*)blob(sd[e].spec_constant_ids, sd[e].spec_constants * sizeof(uint32_t));
                if (sd[e].spec_constants && sd[e].spec_constant_values)
                    sd[e].spec_constant_values = (const uint32_t*)blob(sd[e].spec_constant_values, sd[e].spec_constants * sizeof(uint32_t));
            }
            break;
        }
        case pipeline_subobject_type::input_layout: {
            auto* el = static_cast<input_element*>(base);
            for (uint32_t e = 0; e < so.count; ++e)
                if (el[e].semantic) el[e].semantic = (const char*)blob(el[e].semantic, strlen(el[e].semantic) + 1);
            break;
        }
        case pipeline_subobject_type::blend_state: c.blend_idx = (int)i; break;
        default: break;
        }
        c.subs[i] = so;
    }
    return true;
}

static blend_op BlendOpFromTok(const std::string& t)
{
    if (t == "SUBTRACT") return blend_op::subtract;
    if (t == "REV_SUBTRACT") return blend_op::reverse_subtract;
    if (t == "MIN") return blend_op::min;
    if (t == "MAX") return blend_op::max;
    return blend_op::add;
}
static blend_factor BlendFacFromTok(const std::string& t, bool alpha)
{
    if (t == "ZERO") return blend_factor::zero;
    if (t == "ONE") return blend_factor::one;
    if (t == "SRC_COLOR") return blend_factor::source_color;
    if (t == "INV_SRC_COLOR") return blend_factor::one_minus_source_color;
    if (t == "SRC_ALPHA") return blend_factor::source_alpha;
    if (t == "INV_SRC_ALPHA") return blend_factor::one_minus_source_alpha;
    if (t == "DEST_ALPHA") return blend_factor::dest_alpha;
    if (t == "INV_DEST_ALPHA") return blend_factor::one_minus_dest_alpha;
    if (t == "DEST_COLOR") return blend_factor::dest_color;
    if (t == "INV_DEST_COLOR") return blend_factor::one_minus_dest_color;
    if (t == "SRC_ALPHA_SAT") return blend_factor::source_alpha_saturate;
    if (t == "BLEND_FACTOR") return alpha ? blend_factor::constant_alpha : blend_factor::constant_color;
    if (t == "INV_BLEND_FACTOR") return alpha ? blend_factor::one_minus_constant_alpha : blend_factor::one_minus_constant_color;
    if (t == "SRC1_COLOR") return blend_factor::source1_color;
    if (t == "INV_SRC1_COLOR") return blend_factor::one_minus_source1_color;
    if (t == "SRC1_ALPHA") return blend_factor::source1_alpha;
    if (t == "INV_SRC1_ALPHA") return blend_factor::one_minus_source1_alpha;
    return blend_factor::one;
}

static const char* TokFromD3D11Fac(D3D11_BLEND f)
{
    switch (f) {
    case D3D11_BLEND_ZERO: return "ZERO";
    case D3D11_BLEND_ONE: return "ONE";
    case D3D11_BLEND_SRC_COLOR: return "SRC_COLOR";
    case D3D11_BLEND_INV_SRC_COLOR: return "INV_SRC_COLOR";
    case D3D11_BLEND_SRC_ALPHA: return "SRC_ALPHA";
    case D3D11_BLEND_INV_SRC_ALPHA: return "INV_SRC_ALPHA";
    case D3D11_BLEND_DEST_ALPHA: return "DEST_ALPHA";
    case D3D11_BLEND_INV_DEST_ALPHA: return "INV_DEST_ALPHA";
    case D3D11_BLEND_DEST_COLOR: return "DEST_COLOR";
    case D3D11_BLEND_INV_DEST_COLOR: return "INV_DEST_COLOR";
    case D3D11_BLEND_SRC_ALPHA_SAT: return "SRC_ALPHA_SAT";
    case D3D11_BLEND_BLEND_FACTOR: return "BLEND_FACTOR";
    case D3D11_BLEND_INV_BLEND_FACTOR: return "INV_BLEND_FACTOR";
    case D3D11_BLEND_SRC1_COLOR: return "SRC1_COLOR";
    case D3D11_BLEND_INV_SRC1_COLOR: return "INV_SRC1_COLOR";
    case D3D11_BLEND_SRC1_ALPHA: return "SRC1_ALPHA";
    case D3D11_BLEND_INV_SRC1_ALPHA: return "INV_SRC1_ALPHA";
    default: return nullptr; // unmapped: say so, never silently blend wrong
    }
}
static const char* TokFromD3D11Op(D3D11_BLEND_OP o)
{
    switch (o) {
    case D3D11_BLEND_OP_ADD: return "ADD";
    case D3D11_BLEND_OP_SUBTRACT: return "SUBTRACT";
    case D3D11_BLEND_OP_REV_SUBTRACT: return "REV_SUBTRACT";
    case D3D11_BLEND_OP_MIN: return "MIN";
    case D3D11_BLEND_OP_MAX: return "MAX";
    default: return nullptr;
    }
}

ID3D12PipelineState* GetBlendVariantFromDesc(reshade::api::device* dev, uint64_t src_pso,
    const D3D11_BLEND_DESC* bdesc, const float bf[4], bool decensor)
{
    if (!dev || !src_pso || !bdesc) return nullptr;
    const D3D11_RENDER_TARGET_BLEND_DESC& r = bdesc->RenderTarget[0];
    if (!r.BlendEnable) return nullptr; // `blend = disable`: no variant wanted, draw with the game's PSO

    const char* cop = TokFromD3D11Op(r.BlendOp);
    const char* csrc = TokFromD3D11Fac(r.SrcBlend);
    const char* cdst = TokFromD3D11Fac(r.DestBlend);
    const char* aop = TokFromD3D11Op(r.BlendOpAlpha);
    const char* asrc = TokFromD3D11Fac(r.SrcBlendAlpha);
    const char* adst = TokFromD3D11Fac(r.DestBlendAlpha);
    if (!cop || !csrc || !cdst || !aop || !asrc || !adst) {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("PSOBLEND: unmapped D3D11 blend enum in a state-only [CustomShader] "
                          "(op=%d src=%d dst=%d) -- drawing OPAQUE rather than guessing",
                (int)r.BlendOp, (int)r.SrcBlend, (int)r.DestBlend);
        }
        return nullptr;
    }
    const std::string cmode = std::string(cop) + " " + csrc + " " + cdst;
    const std::string amode = std::string(aop) + " " + asrc + " " + adst;
    return GetBlendVariant(dev, src_pso, cmode, amode, bf, decensor);
}

ID3D12PipelineState* GetBlendVariant(reshade::api::device* dev, uint64_t src_pso,
    const std::string& blend_mode, const std::string& alpha_mode, const float bf[4], bool decensor)
{
    if (!dev || !src_pso || blend_mode.empty()) return nullptr;
    const bool dbg = g_custom_blend_debug_zero;
    uint64_t   bkey = Fnv1a64(blend_mode.data(), blend_mode.size(), dbg ? 0xD000000000000001ULL : kFnv64Basis);
    bkey = (bkey ^ 0x5bd1e995u) * kFnv64Prime;
    bkey = Fnv1a64(alpha_mode.data(), alpha_mode.size(), bkey);
    const CodeBlob dps = decensor ? DecensoredPs(src_pso) : nullptr;
    uint64_t       key = src_pso ^ (bkey * 2654435761ULL) ^ (dps ? 0x9E3779B97F4A7C15ULL : 0);
    {
        std::lock_guard l(g_blend_variant_mtx);
        auto            it = g_blend_variant.find(key);
        if (it != g_blend_variant.end()) return it->second.pso;
    }

    std::string ctoks[3];
    {
        std::istringstream is(blend_mode);
        is >> ctoks[0] >> ctoks[1] >> ctoks[2];
    }
    std::string atoks[3];
    if (!alpha_mode.empty()) {
        std::istringstream is(alpha_mode);
        is >> atoks[0] >> atoks[1] >> atoks[2];
    } else {
        atoks[0] = ctoks[0];
        atoks[1] = ctoks[1];
        atoks[2] = ctoks[2];
    }

    pipeline_layout                 layout;
    std::vector<pipeline_subobject> subs;
    int                             bidx;
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(src_pso);
        if (it == g_pso_clone.end() || it->second.blend_idx < 0) {
            std::lock_guard lv(g_blend_variant_mtx);
            if (g_blend_miss_logged.insert(src_pso).second)
                logger::write("CUSTOMBLEND-MISS src=%llx: %s -- the piece draws OPAQUE",
                    src_pso, it == g_pso_clone.end() ? "no clone of this PSO (created before the addon)" : "the clone has no blend_state subobject");
            return nullptr;
        }
        layout = it->second.layout;
        subs = it->second.subs;
        bidx = it->second.blend_idx; // shallow copy: data ptrs still point into the persistent `store`
    }
    auto bd = std::make_unique<blend_desc>(*static_cast<const blend_desc*>(subs[bidx].data));
    for (int rt = 0; rt < 8; ++rt) {
        bd->blend_enable[rt] = true;
        if (dbg) { // SHOW BACKGROUND: result = 0*src + 1*dst = whatever is behind the piece
            bd->color_blend_op[rt] = bd->alpha_blend_op[rt] = blend_op::add;
            bd->source_color_blend_factor[rt] = bd->source_alpha_blend_factor[rt] = blend_factor::zero;
            bd->dest_color_blend_factor[rt] = bd->dest_alpha_blend_factor[rt] = blend_factor::one;
        } else {
            bd->color_blend_op[rt] = BlendOpFromTok(ctoks[0]);
            bd->source_color_blend_factor[rt] = BlendFacFromTok(ctoks[1], false);
            bd->dest_color_blend_factor[rt] = BlendFacFromTok(ctoks[2], false);
            bd->alpha_blend_op[rt] = BlendOpFromTok(atoks[0]);
            bd->source_alpha_blend_factor[rt] = BlendFacFromTok(atoks[1], true);
            bd->dest_alpha_blend_factor[rt] = BlendFacFromTok(atoks[2], true);
        }
    }
    // blend_constant here is informational only -- D3D12 takes the factor from OMSetBlendFactor, not the PSO.
    bd->blend_constant[0] = bf[0];
    bd->blend_constant[1] = bf[1];
    bd->blend_constant[2] = bf[2];
    bd->blend_constant[3] = bf[3];
    subs[bidx].data = bd.get();
    shader_desc dps_desc{};
    if (dps) SwapPixelShader(subs, dps, dps_desc);

    reshade::api::pipeline out = { 0 };
    const auto             t0 = std::chrono::steady_clock::now();
    bool                   ok = dev->create_pipeline(layout, (uint32_t)subs.size(), subs.data(), &out);
    const double           ms = logger::ms_since(t0);
    ID3D12PipelineState*   variant = ok ? reinterpret_cast<ID3D12PipelineState*>(out.handle) : nullptr;
    {
        std::lock_guard lv(g_blend_variant_mtx);
        auto            it = g_blend_variant.find(key);
        if (it != g_blend_variant.end()) {
            if (variant) dev->destroy_pipeline(out);
            return it->second.pso;
        }
        g_blend_variant[key] = PsoVariant{ src_pso, variant };
    }
    logger::write("CUSTOMBLEND variant src=%llx blend='%s' alpha='%s' iniFactor=%.2f,%.2f,%.2f,%.2f (factor dynamic) decensored=%d -> %s, "
                  "built on the drawing thread in %.1f ms",
        src_pso, blend_mode.c_str(), alpha_mode.empty() ? "(=color)" : alpha_mode.c_str(), bf[0], bf[1], bf[2], bf[3], (int)(dps != nullptr),
        variant ? "OK" : "FAILED(create)", ms);
    return variant;
}

ID3D12PipelineState* GetInputLayoutVariant(reshade::api::device* dev, uint64_t src_pso,
    const D3D11_INPUT_ELEMENT_DESC* elements, uint32_t count, bool decensor, bool rfx)
{
    if (!dev || !src_pso || !elements || !count) return nullptr;
    uint64_t lkey = kFnv64Basis;
    for (uint32_t i = 0; i < count; ++i) {
        const D3D11_INPUT_ELEMENT_DESC& e = elements[i];
        if (e.SemanticName) lkey = Fnv1a64(e.SemanticName, strlen(e.SemanticName), lkey);
        const uint32_t v[3] = { e.SemanticIndex, (uint32_t)e.Format, e.AlignedByteOffset };
        lkey = Fnv1a64(v, sizeof v, lkey);
    }
    const CodeBlob dps = rfx ? RfxPatchedPs(src_pso, decensor) : decensor ? DecensoredPs(src_pso) : nullptr;
    if (rfx && !dps) return nullptr;
    const uint64_t salt = !dps ? 0
        : !rfx                 ? 0x9E3779B97F4A7C15ULL
        : decensor             ? 0xD6E8FEB86659FD93ULL
                               : 0xC2B2AE3D27D4EB4FULL;
    const uint64_t key = src_pso ^ (lkey * 2654435761ULL) ^ salt;
    {
        std::lock_guard l(g_layout_variant_mtx);
        if (auto it = g_layout_variant.find(key); it != g_layout_variant.end()) return it->second.pso;
    }

    pipeline_layout                 layout = {};
    std::vector<pipeline_subobject> subs;
    const char*                     why = nullptr;
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(src_pso);
        if (it == g_pso_clone.end()) {
            why = "no clone of this PSO (created before the addon)";
        } else {
            layout = it->second.layout;
            subs = it->second.subs;
        }
    }
    int il = -1;
    for (size_t s = 0; !why && s < subs.size(); ++s)
        if (subs[s].type == pipeline_subobject_type::input_layout) il = (int)s;
    if (!why && (il < 0 || subs[il].count != count || !subs[il].data))
        why = "the PSO's input layout is not the one the override was made from";
    std::vector<input_element> ie;
    ID3D12PipelineState*       variant = nullptr;
    double                     ms = 0.0;
    if (!why) {
        const auto* src = static_cast<const input_element*>(subs[il].data);
        ie.assign(src, src + count);
        for (uint32_t i = 0; i < count; ++i) {
            ie[i].semantic = elements[i].SemanticName;
            ie[i].semantic_index = elements[i].SemanticIndex;
            ie[i].format = (format)elements[i].Format;
            ie[i].offset = elements[i].AlignedByteOffset;
        }
        subs[il].data = ie.data();
        shader_desc dps_desc{};
        if (dps) SwapPixelShader(subs, dps, dps_desc);
        reshade::api::pipeline out = { 0 };
        const auto             t0 = std::chrono::steady_clock::now();
        if (dev->create_pipeline(layout, (uint32_t)subs.size(), subs.data(), &out))
            variant = reinterpret_cast<ID3D12PipelineState*>(out.handle);
        else
            why = "create_pipeline failed";
        ms = logger::ms_since(t0);
    }
    {
        std::lock_guard l(g_layout_variant_mtx);
        if (auto it = g_layout_variant.find(key); it != g_layout_variant.end()) {
            if (variant) dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(variant) });
            return it->second.pso;
        }
        g_layout_variant[key] = PsoVariant{ src_pso, variant };
    }
    char desc[512];
    size_t n = 0;
    desc[0] = 0;
    for (uint32_t i = 0; i < count && n + 48 < sizeof desc; ++i) {
        const int w = _snprintf_s(desc + n, sizeof desc - n, _TRUNCATE, " %s%u+%u:f%u",
            elements[i].SemanticName ? elements[i].SemanticName : "?", elements[i].SemanticIndex,
            elements[i].AlignedByteOffset, (unsigned)elements[i].Format);
        if (w < 0) break;
        n += (size_t)w;
    }
    logger::write("ILAYOUT variant src=%llx elems=%u ps=%s ->%s%s, built on the drawing thread in %.1f ms:%s",
        (unsigned long long)src_pso, count, !dps ? "game" : rfx ? (decensor ? "rabbitfx+decensored" : "rabbitfx") : "decensored",
        variant ? " OK" : " NONE, drawn with the game's layout -- ", variant ? "" : why, ms, desc);
    return variant;
}

struct RfxpVariant {
    ID3D12PipelineState* pso;
    unsigned             max_reg;
    bool                 has_glow;
}; // max_reg: the source PS's highest t-register
static std::unordered_map<uint64_t, RfxpVariant> g_rfxp_variant; // src -> variant; pso nullptr = a settled no
static std::unordered_set<uint64_t>              g_rfxp_miss_logged;
static std::mutex                                g_rfxp_variant_mtx;
struct RfxpJob {
    reshade::api::device*   dev;
    uint64_t                src_pso0;
    uint64_t                key;
    bool                    decensor;
    PsoClone                clone;
    bool                    inject = true;
    std::vector<RfxDxilCut> cuts;       // not empty: a UID-cut job (inject and decensor false)
    uint32_t                target = 0; // its back buffer size, width << 16 | height
};
static std::deque<RfxpJob>          g_rfxp_queue;   // under g_rfxp_variant_mtx
static std::unordered_set<uint64_t> g_rfxp_pending; // keys queued or in flight -- under g_rfxp_variant_mtx
// UID-cut jobs: (source PSO, target) queued or in flight, and every variant made, until the source is destroyed.
// Under g_rfxp_variant_mtx.
static std::set<std::pair<uint64_t, uint32_t>>                 g_uid_pending;
static std::unordered_multimap<uint64_t, ID3D12PipelineState*> g_uid_variants;
static std::condition_variable      g_rfxp_cv;
static std::atomic<bool>            g_rfxp_stop{ false };
struct RfxpThread {
    std::thread t;
    ~RfxpThread()
    {
        if (t.joinable()) t.detach();
    }
};
static RfxpThread g_rfxp_worker; // start (a draw thread) vs stop: under g_rfxp_thread_mtx
static std::mutex g_rfxp_thread_mtx;
struct RfxpPs {
    std::vector<uint8_t> code;
    unsigned             max_reg = 0;
    bool                 has_glow = false;
    bool                 cached = false;
}; // empty code = a patch that failed: said once, never retried
static std::unordered_map<uint64_t, std::shared_ptr<RfxpPs>> g_rfxp_ps;             // WORKER THREAD ONLY
static unsigned                                              g_rfxp_ps_patched = 0; // under g_rfxp_variant_mtx
static unsigned                                              g_rfxp_ps_failed = 0;
static unsigned                                              g_rfxp_variants = 0;

struct RfxCacheHdr {
    uint32_t magic;
    uint32_t version;
    uint64_t src_hash;
    uint32_t src_size;
    uint32_t code_size;
    uint32_t max_reg;
    uint32_t has_glow;
};
constexpr uint32_t kRfxCacheMagic = 0x43584652; // "RFXC"

static std::wstring RfxCachePath(uint64_t ps_key)
{
    wchar_t leaf[40];
    swprintf_s(leaf, L"\\%016llx.rfx", (unsigned long long)ps_key);
    return g_shader_cache_dir + leaf;
}

// True for a file this build wrote for this shader -- with code, or empty for a patch that did not apply.
static bool RfxCacheLoad(uint64_t ps_key, uint64_t src_hash, size_t src_size, RfxpPs& out)
{
    if (g_shader_cache_dir.empty()) return false;
    const std::vector<uint8_t> f = WwmiReadFile(RfxCachePath(ps_key).c_str());
    RfxCacheHdr                h;
    if (f.size() < sizeof h) return false;
    memcpy(&h, f.data(), sizeof h);
    if (h.magic != kRfxCacheMagic || h.version != WWMI_RFX_SRC_HASH || h.src_hash != src_hash ||
        h.src_size != (uint32_t)src_size || f.size() != sizeof h + (size_t)h.code_size) return false;
    out.code.assign(f.begin() + sizeof h, f.end());
    out.max_reg = h.max_reg;
    out.has_glow = h.has_glow != 0;
    return true;
}

static void RfxCacheStore(uint64_t ps_key, uint64_t src_hash, size_t src_size, const RfxpPs& p)
{
    if (g_shader_cache_dir.empty()) return;
    CreateDirectoryW(g_shader_cache_dir.c_str(), nullptr);
    const std::wstring path = RfxCachePath(ps_key);
    const std::wstring tmp = path + L".tmp";
    const HANDLE       f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const RfxCacheHdr h{ kRfxCacheMagic, WWMI_RFX_SRC_HASH, src_hash, (uint32_t)src_size, (uint32_t)p.code.size(),
        p.max_reg, p.has_glow ? 1u : 0u };
    DWORD             wr = 0;
    bool              ok = WriteFile(f, &h, sizeof h, &wr, nullptr) && wr == sizeof h;
    if (ok && !p.code.empty()) ok = WriteFile(f, p.code.data(), (DWORD)p.code.size(), &wr, nullptr) && wr == p.code.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

#ifdef WWMI_DEBUG
static void RfxPatchDump(uint64_t ps_hash, const char* suffix, const void* p, size_t n)
{
    if (!p || !n) return;
    const std::wstring dir = MigotoPort::DumpDir() + L"\\RfxDxil";
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%016llx%S", dir.c_str(), (unsigned long long)ps_hash, suffix);
    const HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, p, (DWORD)n, &wr, nullptr);
    CloseHandle(f);
}
#endif

static void RfxPatchBuild(RfxpJob& job)
{
    reshade::api::device* const dev = job.dev;
    const uint64_t              src_pso0 = job.src_pso0;
    const uint64_t              key = job.key;
    const bool                  decensor = job.decensor;
    const bool                  inject = job.inject;
    const bool                  cut = !job.cuts.empty();
    const char* const           label = cut ? "UIDCUT" : (inject ? "RFXPATCH" : "RFXDECENSOR");
    unsigned                    max_reg = 0;
    bool                        glow = false;
    CodeBlob                    dps;
    CodeBlob                    pps;
    auto                        settle = [&](ID3D12PipelineState* v) {
        bool orphan = false;
        {
            std::lock_guard l(g_rfxp_variant_mtx);
            if (cut) {
                if (!g_uid_pending.erase({ src_pso0, job.target }))
                    orphan = true;
                else {
                    if (v) {
                        g_uid_variants.emplace(src_pso0, v);
                        ++g_rfxp_variants;
                    }
                    PsoSetUidVariant(src_pso0, job.target, reinterpret_cast<uint64_t>(v));
                }
            } else if (!g_rfxp_pending.erase(key))
                orphan = true;
            else {
                g_rfxp_variant[key] = RfxpVariant{ v, max_reg, v && glow };
                if (v) ++g_rfxp_variants;
                if (!inject) PsoSetDecensorVariant(src_pso0, reinterpret_cast<uint64_t>(v));
                if (!inject && v && dps) {
                    std::lock_guard d(g_decensor_ps_mtx);
                    g_decensor_ps[src_pso0] = dps;
                }
                if (inject && v && pps) {
                    std::lock_guard r(g_rfx_ps_mtx);
                    g_rfx_ps[key] = pps;
                }
            }
        }
        if (orphan && v) {
            logger::write("%s variant src=%llx decensor=%d: the source PSO was destroyed while its patch was in flight -- variant released", label, src_pso0, (int)decensor);
            dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(v) });
        }
    };
    std::vector<pipeline_subobject>& subs = job.clone.subs;
    // A GBuffer pass only: the DX11 regex matches the component's two 7-RT passes and neither depth pass.
    if (inject)
        for (const auto& so : subs)
            if (so.type == pipeline_subobject_type::render_target_formats) {
                const auto* fm = static_cast<const format*>(so.data);
                if (so.count < 4 || fm[3] == format::unknown) return settle(nullptr);
                break;
            }
    pipeline_subobject* ps_so = nullptr;
    for (auto& so : subs)
        if (so.type == pipeline_subobject_type::pixel_shader && so.count >= 1 && so.data) {
            ps_so = &so;
            break;
        }
    if (!ps_so) return settle(nullptr);
    const shader_desc src_ps = *static_cast<const shader_desc*>(ps_so->data);
    if (!src_ps.code || !src_ps.code_size) return settle(nullptr);
    const uint64_t ps_hash = Fnv1a64(src_ps.code, src_ps.code_size);
    const uint64_t ps_key = ps_hash ^ (decensor ? 0x9E3779B97F4A7C15ull : 0) ^ (inject ? 0 : 0xC2B2AE3D27D4EB4Full) ^
        (cut ? Fnv1a64(job.cuts.data(), job.cuts.size() * sizeof(RfxDxilCut)) : 0); // the store holds every patch of a PS

    std::shared_ptr<RfxpPs> patched;
    if (auto it = g_rfxp_ps.find(ps_key); it != g_rfxp_ps.end())
        patched = it->second;
    else {
        patched = std::make_shared<RfxpPs>();
        patched->cached = RfxCacheLoad(ps_key, ps_hash, src_ps.code_size, *patched);
        bool ok = patched->cached && !patched->code.empty();
        if (patched->cached) logger::trace("RFXCACHE ps=%016llx decensor=%d inject=%d: %zu bytes from the disk cache", (unsigned long long)ps_hash, (int)decensor, (int)inject, patched->code.size());
        if (!patched->cached) {
            std::string     why;
            const ULONGLONG t0 = GetTickCount64();
            RfxDxilResult   r;
            const bool      init_ok = RfxDxilInit((g_cso_dir + L"\\dxc").c_str(), why);
            RfxDxilSpec     spec{ kRfxGlowReg, kRfxCutoutReg, kRfxParamsReg, decensor, inject };
            spec.cuts = job.cuts;
            ok = init_ok && RfxDxilPatch(src_ps.code, src_ps.code_size, spec, r);
            if (ok) {
                patched->code = std::move(r.code);
                patched->max_reg = r.srv_max_reg;
                patched->has_glow = r.has_glow;
            } else if (!r.why.empty())
                why = r.why;
            if (cut)
                logger::write("UIDCUT ps=%016llx (%zu bytes) for %ux%u: %s in %llu ms -- %s", (unsigned long long)ps_hash, (size_t)src_ps.code_size,
                    job.target >> 16, job.target & 0xFFFFu, ok ? "PATCHED" : "NOT patched", (unsigned long long)(GetTickCount64() - t0),
                    ok ? r.why.c_str() : why.c_str());
            else if (!inject) {
                if (ok)
                    logger::write("RFXDECENSOR ps=%016llx (%zu bytes): PATCHED in %llu ms -- %s", (unsigned long long)ps_hash, (size_t)src_ps.code_size,
                        (unsigned long long)(GetTickCount64() - t0), r.why.c_str());
                else
                    logger::trace("RFXDECENSOR ps=%016llx: NOT patched -- %s", (unsigned long long)ps_hash, why.c_str());
            } else {
                logger::write("RFXPATCH ps=%016llx (%zu bytes): %s in %llu ms -- %s", (unsigned long long)ps_hash, (size_t)src_ps.code_size,
                    ok ? "PATCHED" : "NOT patched", (unsigned long long)(GetTickCount64() - t0), ok ? r.why.c_str() : why.c_str());
#ifdef WWMI_DEBUG
                if (logger::unlimited()) {
                    RfxPatchDump(ps_hash, ".ps.cso", src_ps.code, src_ps.code_size);
                    RfxPatchDump(ps_hash, ".before.ll", r.ll_before.data(), r.ll_before.size());
                    RfxPatchDump(ps_hash, ".after.ll", r.ll_after.data(), r.ll_after.size());
                    RfxPatchDump(ps_hash, ".patched.cso", patched->code.data(), patched->code.size());
                }
#endif
            }
            if (r.settled) RfxCacheStore(ps_key, ps_hash, src_ps.code_size, *patched);
        }
        g_rfxp_ps.emplace(ps_key, patched);
        std::lock_guard l(g_rfxp_variant_mtx);
        if (ok)
            ++g_rfxp_ps_patched;
        else
            ++g_rfxp_ps_failed;
    }
    if (patched->code.empty()) return settle(nullptr);
    max_reg = patched->max_reg;
    glow = patched->has_glow;
    if (!cut) {
        if (!inject)
            dps = PooledCode(patched->code.data(), patched->code.size());
        else
            pps = PooledCode(patched->code.data(), patched->code.size());
    }

    shader_desc ps = src_ps;
    ps.code = patched->code.data();
    ps.code_size = patched->code.size();
    ps_so->data = &ps;
    ps_so->count = 1;
    reshade::api::pipeline out = { 0 };
    const bool             ok = dev->create_pipeline(job.clone.layout, (uint32_t)subs.size(), subs.data(), &out);
    ID3D12PipelineState*   variant = ok ? reinterpret_cast<ID3D12PipelineState*>(out.handle) : nullptr;
    logger::write("%s variant src=%llx ps=%016llx decensor=%d -> %s", label, src_pso0, (unsigned long long)ps_hash, (int)decensor, variant ? "OK" : "FAILED(create)");
    if (!variant && patched->cached) {
        DeleteFileW(RfxCachePath(ps_key).c_str());
        logger::write("RFXCACHE ps=%016llx: its cached patch built no pipeline -- the cache file is removed, patched afresh next launch", (unsigned long long)ps_hash);
    }
    return settle(variant);
}

static void RfxPatchWorker()
{
    for (;;) {
        RfxpJob job;
        {
            std::unique_lock l(g_rfxp_variant_mtx);
            g_rfxp_cv.wait(l, [] { return g_rfxp_stop.load(std::memory_order_acquire) || !g_rfxp_queue.empty(); });
            if (g_rfxp_stop.load(std::memory_order_acquire)) return;
            job = std::move(g_rfxp_queue.front());
            g_rfxp_queue.pop_front();
        }
        RfxPatchBuild(job);
    }
}

void RfxPatchWorkerStop()
{
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        g_rfxp_stop.store(true, std::memory_order_release);
        g_rfxp_queue.clear();
        g_rfxp_pending.clear();
    } // an in-flight build then settles as an orphan
    g_rfxp_cv.notify_all();
    std::lock_guard t(g_rfxp_thread_mtx);
    if (g_rfxp_worker.t.joinable()) {
        g_rfxp_worker.t.join();
        logger::write("RFXPATCH worker stopped (device destroyed / addon unloading)");
    }
}

static void RfxpWake()
{
    {
        std::lock_guard t(g_rfxp_thread_mtx);
        if (!g_rfxp_worker.t.joinable() && !g_rfxp_stop.load(std::memory_order_acquire)) {
            g_rfxp_worker.t = std::thread(RfxPatchWorker);
            logger::write("RFXPATCH worker started");
        }
    }
    g_rfxp_cv.notify_one();
}

ID3D12PipelineState* GetRfxPatchVariant(reshade::api::device* dev, uint64_t src_pso0, unsigned* ps_max_srv_reg, bool* has_glow, bool decensor)
{
    if (ps_max_srv_reg) *ps_max_srv_reg = 0;
    if (has_glow) *has_glow = false;
    if (!dev || !src_pso0) return nullptr;
    const uint64_t src_pso = src_pso0 | ((uint64_t)decensor << 60);
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        auto            it = g_rfxp_variant.find(src_pso);
        if (it != g_rfxp_variant.end()) {
            if (ps_max_srv_reg) *ps_max_srv_reg = it->second.max_reg;
            if (has_glow) *has_glow = it->second.has_glow;
            return it->second.pso;
        }
        if (g_rfxp_pending.count(src_pso)) return nullptr;
    } // in flight: this draw goes unpatched

    RfxpJob job{ dev, src_pso0, src_pso, decensor, {} };
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(src_pso0);
        if (it == g_pso_clone.end()) {
            // As CUSTOMBLEND-MISS: said once, not cached, retried per draw.
            std::lock_guard lv(g_rfxp_variant_mtx);
            if (g_rfxp_miss_logged.insert(src_pso0).second)
                logger::write("RFXPATCH variant src=%llx -> none: no clone of this PSO (created before the addon)", src_pso0);
            return nullptr;
        }
        job.clone.layout = it->second.layout;
        DeepCopySubobjects((uint32_t)it->second.subs.size(), it->second.subs.data(), job.clone);
    }
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        if (g_rfxp_stop.load(std::memory_order_acquire)) return nullptr;
        if (!g_rfxp_pending.insert(src_pso).second) return nullptr; // another draw thread queued it first
        g_rfxp_queue.push_back(std::move(job));
        logger::trace("RFXPATCH queued src=%llx decensor=%d (drawn unpatched until built; %zu queued)", src_pso0, (int)decensor, g_rfxp_queue.size());
    }
    RfxpWake();
    return nullptr;
}

void RfxDecensorQueue(reshade::api::device* dev, uint64_t src_pso0)
{
    if (!dev || !src_pso0) return;
    const uint64_t key = src_pso0 | (2ull << 60);
    RfxpJob        job{ dev, src_pso0, key, true, {}, false };
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(src_pso0);
        if (it == g_pso_clone.end()) {
            logger::trace("RFXDECENSOR src=%llx -> none: no clone of this PSO", src_pso0);
            return;
        }
        job.clone.layout = it->second.layout;
        DeepCopySubobjects((uint32_t)it->second.subs.size(), it->second.subs.data(), job.clone);
    }
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        if (g_rfxp_stop.load(std::memory_order_acquire)) return;
        if (!g_rfxp_pending.insert(key).second) return;
        g_rfxp_queue.push_back(std::move(job));
    }
    RfxpWake();
}

void RfxUidQueue(reshade::api::device* dev, uint64_t src_pso0, uint32_t target, std::vector<RfxDxilCut> cuts)
{
    if (!dev || !src_pso0 || cuts.empty()) return;
    RfxpJob job{ dev, src_pso0, src_pso0 | (3ull << 60), false, {}, false, std::move(cuts), target };
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(src_pso0);
        if (it == g_pso_clone.end()) {
            logger::trace("UIDCUT src=%llx -> none: no clone of this PSO", src_pso0);
            return;
        }
        job.clone.layout = it->second.layout;
        DeepCopySubobjects((uint32_t)it->second.subs.size(), it->second.subs.data(), job.clone);
    }
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        if (g_rfxp_stop.load(std::memory_order_acquire)) return;
        if (!g_uid_pending.insert({ src_pso0, target }).second) return;
        g_rfxp_queue.push_back(std::move(job));
    }
    RfxpWake();
}

RfxPatchStats GetRfxPatchStats()
{
    std::lock_guard l(g_rfxp_variant_mtx);
    return RfxPatchStats{ g_rfxp_ps_patched, g_rfxp_ps_failed, g_rfxp_variants };
}

static const char* CmpName(compare_op c)
{
    switch (c) {
    case compare_op::never: return "never";
    case compare_op::less: return "less";
    case compare_op::equal: return "equal";
    case compare_op::less_equal: return "less_equal";
    case compare_op::greater: return "greater";
    case compare_op::not_equal: return "not_equal";
    case compare_op::greater_equal: return "greater_equal";
    case compare_op::always: return "always";
    }
    return "?";
}
static const char* StOpName(stencil_op o)
{
    switch (o) {
    case stencil_op::keep: return "keep";
    case stencil_op::zero: return "zero";
    case stencil_op::replace: return "replace";
    case stencil_op::increment_saturate: return "incr_sat";
    case stencil_op::decrement_saturate: return "decr_sat";
    case stencil_op::invert: return "invert";
    case stencil_op::increment: return "incr";
    case stencil_op::decrement: return "decr";
    }
    return "?";
}
unsigned PsoCloneWriteShaders(uint64_t pso_handle, const wchar_t* dir)
{
    if (!dir || !dir[0]) return 0;
    std::vector<uint8_t>     blob[2];
    static const char* const kStage[2] = { "vs", "ps" };
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(pso_handle);
        if (it == g_pso_clone.end()) return 0;
        for (const auto& so : it->second.subs) {
            const int k = so.type == pipeline_subobject_type::vertex_shader ? 0 : so.type == pipeline_subobject_type::pixel_shader ? 1
                                                                                                                                   : -1;
            if (k < 0 || so.count < 1 || !so.data) continue;
            const auto* sd = static_cast<const shader_desc*>(so.data);
            if (sd->code && sd->code_size) blob[k].assign((const uint8_t*)sd->code, (const uint8_t*)sd->code + sd->code_size);
        }
    }
    unsigned wrote = 0;
    for (int k = 0; k < 2; ++k) {
        if (blob[k].empty()) continue;
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%016llx.%S.cso", dir, (unsigned long long)HashShader(blob[k].data(), blob[k].size()), kStage[k]);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") != 0 || !f) continue;
        fwrite(blob[k].data(), 1, blob[k].size(), f);
        fclose(f);
        ++wrote;
    }
    return wrote;
}

unsigned PsoCloneDescribeDepthStencil(uint64_t pso_handle, char* out, size_t cap)
{
    if (!out || !cap) return 0;
    depth_stencil_desc ds{};
    bool               have = false;
    {
        std::lock_guard l(g_pso_clone_mtx);
        auto            it = g_pso_clone.find(pso_handle);
        if (it != g_pso_clone.end())
            for (const auto& so : it->second.subs)
                if (so.type == pipeline_subobject_type::depth_stencil_state && so.count >= 1) {
                    ds = *static_cast<const depth_stencil_desc*>(so.data);
                    have = true;
                    break;
                }
    }
    if (!have) return 0;
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "depth %s write=%d func=%s | stencil %s read=%02x write=%02x ref=%u "
        "front func=%s pass=%s fail=%s zfail=%s back func=%s pass=%s fail=%s zfail=%s",
        ds.depth_enable ? "on" : "off", (int)ds.depth_write_mask, CmpName(ds.depth_func),
        ds.stencil_enable ? "on" : "off", (unsigned)ds.front_stencil_read_mask, (unsigned)ds.front_stencil_write_mask,
        (unsigned)ds.front_stencil_reference_value,
        CmpName(ds.front_stencil_func), StOpName(ds.front_stencil_pass_op), StOpName(ds.front_stencil_fail_op),
        StOpName(ds.front_stencil_depth_fail_op),
        CmpName(ds.back_stencil_func), StOpName(ds.back_stencil_pass_op), StOpName(ds.back_stencil_fail_op),
        StOpName(ds.back_stencil_depth_fail_op));
    return n > 0 ? (unsigned)n : 0u;
}

// Deep-copy a PSO's subobjects (called from on_init_pipeline); kept until the PSO is destroyed.
void PsoCloneStore(uint64_t pso_handle, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject* subobjects)
{
    PsoClone c;
    c.layout = layout;
    if (!DeepCopySubobjects(subobject_count, subobjects, c) || c.blend_idx < 0) return;
    ForgetPatchedPs(pso_handle);
    std::lock_guard l(g_pso_clone_mtx);
    // A handle already here is a recycled one whose destroy we did not see: the new PSO replaces it.
    if (auto old = g_pso_clone.find(pso_handle); old != g_pso_clone.end()) {
        g_pso_clone_bytes -= CloneBytes(old->second);
        g_pso_clone.erase(old);
    }
    g_pso_clone_bytes += CloneBytes(c);
    g_pso_clone.emplace(pso_handle, std::move(c));
}

void PsoCloneForget(reshade::api::device* dev, uint64_t pso_handle)
{
    {
        std::lock_guard l(g_pso_clone_mtx);
        if (auto e = g_pso_clone.find(pso_handle); e != g_pso_clone.end()) {
            g_pso_clone_bytes -= CloneBytes(e->second);
            g_pso_clone.erase(e);
            ++g_pso_clone_forgotten;
        }
    }
    std::vector<ID3D12PipelineState*> dead;
    {
        std::lock_guard l(g_blend_variant_mtx);
        g_blend_miss_logged.erase(pso_handle);
        for (auto it = g_blend_variant.begin(); it != g_blend_variant.end();) {
            if (it->second.src == pso_handle) {
                if (it->second.pso) dead.push_back(it->second.pso);
                it = g_blend_variant.erase(it);
            } else
                ++it;
        }
    }
    {
        std::lock_guard l(g_layout_variant_mtx);
        for (auto it = g_layout_variant.begin(); it != g_layout_variant.end();) {
            if (it->second.src == pso_handle) {
                if (it->second.pso) dead.push_back(it->second.pso);
                it = g_layout_variant.erase(it);
            } else
                ++it;
        }
    }
    {
        std::lock_guard l(g_rfxp_variant_mtx);
        g_rfxp_miss_logged.erase(pso_handle);
        ForgetPatchedPs(pso_handle);
        constexpr uint64_t kSrcMask = ~(7ull << 60);
        for (unsigned d = 0; d < 8; ++d) {
            const uint64_t key = pso_handle | ((uint64_t)d << 60);
            if (auto it = g_rfxp_variant.find(key); it != g_rfxp_variant.end()) {
                if (it->second.pso) dead.push_back(it->second.pso);
                g_rfxp_variant.erase(it);
            }
            g_rfxp_pending.erase(key);
        }
        for (auto it = g_rfxp_queue.begin(); it != g_rfxp_queue.end();)
            if ((it->key & kSrcMask) == pso_handle)
                it = g_rfxp_queue.erase(it);
            else
                ++it;
        for (auto it = g_uid_pending.lower_bound({ pso_handle, 0 }); it != g_uid_pending.end() && it->first == pso_handle;) it = g_uid_pending.erase(it);
        const auto made = g_uid_variants.equal_range(pso_handle);
        for (auto it = made.first; it != made.second; ++it) dead.push_back(it->second);
        g_uid_variants.erase(made.first, made.second);
    }
    for (ID3D12PipelineState* v : dead) {
        if (dev)
            dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(v) });
        else
            v->Release();
    }
    if (!dead.empty()) {
        std::lock_guard l(g_pso_clone_mtx);
        g_variants_released += (unsigned)dead.size();
    }
}

PsoCloneStats PsoCloneGetStats()
{
    size_t code_blobs = 0;
    size_t code_bytes = 0;
    {
        std::lock_guard l(g_code_pool_mtx);
        for (const auto& kv : g_code_pool)
            if (CodeBlob b = kv.second.lock()) {
                ++code_blobs;
                code_bytes += b->size();
            }
    }
    std::lock_guard l(g_pso_clone_mtx);
    return PsoCloneStats{ g_pso_clone.size(), g_pso_clone_bytes, code_blobs, code_bytes, g_pso_clone_forgotten, g_variants_released };
}
