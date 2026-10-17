// port_api: the engine's variables and custom resources, read and written by the addon.
#include "port_api_internal.h"
#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "dx12_wrap.h"
#include "dx12_pipeline.h"
#include "port_keygate.h"
#include <DDSTextureLoader.h>
#include "../../src/log.h"

#include <string>
#include <unordered_map>

namespace PortApi {

static std::unordered_map<const CustomResource*, unsigned> g_deferred_pending; // under the engine lock: -> the load it was deferred in

void ForEachVariable(void (*cb)(const wchar_t* fq_name, float value, bool persist, void* user), void* user)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !cb) return;
    for (auto& kv : command_list_globals) {
        const CommandListVariable& v = kv.second;
        const bool                 persist = !!(v.flags & VariableFlags::PERSIST);
        cb(v.name.c_str(), v.fval, persist, user);
    }
}

void EngineValues::Take()
{
    vars.clear();
    vars.reserve(command_list_globals.size());
    for (const auto& kv : command_list_globals) vars.push_back(kv.second.fval);
    params = G->iniParams;
    shader_runs.clear();
    shader_runs.reserve(customShaders.size());
    for (const auto& kv : customShaders) shader_runs.emplace_back(kv.second.frame_no, kv.second.executions_this_frame);
}

void EngineValues::PutBack() const
{
    size_t i = 0;
    for (auto& kv : command_list_globals) {
        if (i == vars.size()) break;
        kv.second.fval = vars[i++];
    }
    G->iniParams = params;
    size_t s = 0;
    for (auto& kv : customShaders) {
        if (s == shader_runs.size()) break;
        kv.second.frame_no = shader_runs[s].first;
        kv.second.executions_this_frame = shader_runs[s++].second;
    }
}

static std::wstring VarKey(const wchar_t* fq_name)
{
    std::wstring k = fq_name;
    for (auto& ch : k) {
        if (ch == L'/') ch = L'\\';
        ch = (wchar_t)towlower(ch);
    }
    if (!k.empty() && k[0] == L'$') return k;
    if (!k.empty() && k[0] != L'\\') k.insert(k.begin(), L'\\');
    return L"$" + k;
}

// ns + name -> their fully-qualified key. The separator lives here, once, instead of at 15 call sites.
static std::wstring JoinNs(const wchar_t* ns, const wchar_t* name)
{
    std::wstring s = ns ? ns : L"";
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s + (name ? name : L"");
}

float GetVarNs(const wchar_t* ns, const wchar_t* name, float def)
{
    return GetVar(JoinNs(ns, name).c_str(), def);
}

bool SetVarNs(const wchar_t* ns, const wchar_t* name, float value)
{
    return SetVar(JoinNs(ns, name).c_str(), value);
}

float GetVar(const wchar_t* fq_name, float def)
{
    if (!g_loaded || !fq_name) return def;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    auto                                  it = command_list_globals.find(VarKey(fq_name));
    return (it == command_list_globals.end()) ? def : it->second.fval;
}

bool SetVar(const wchar_t* fq_name, float value)
{
    if (!g_loaded || !fq_name) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    auto                                  it = command_list_globals.find(VarKey(fq_name));
    if (it == command_list_globals.end()) return false;
    if (memcmp(&it->second.fval, &value, sizeof value) != 0 && MigotoPort::SelectionVarWatched(&it->second.fval))
        MigotoPort::BumpSelectionInputs();
    it->second.fval = value;
    return true;
}

float* VarSlotNs(const wchar_t* ns, const wchar_t* name)
{
    if (!g_loaded) return nullptr;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    auto                                  it = command_list_globals.find(VarKey(JoinNs(ns, name).c_str()));
    return it == command_list_globals.end() ? nullptr : &it->second.fval;
}

void SetVarSlot(float* slot, float value)
{
    if (!g_loaded || !slot) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (memcmp(slot, &value, sizeof value) != 0 && MigotoPort::SelectionVarWatched(slot))
        MigotoPort::BumpSelectionInputs();
    *slot = value;
}

bool VarSlotHolds(const float* slot, unsigned gen, float value)
{
    const volatile unsigned& live_gen = g_generation;
    if (!slot || gen != live_gen) return false;
    const float held = *static_cast<const volatile float*>(slot);
    return gen == live_gen && memcmp(&held, &value, sizeof value) == 0;
}

ID3D12Resource* FindCustomResource(const wchar_t* fq_name)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name) return nullptr;
    auto it = customResources.find(fq_name);
    if (it == customResources.end()) return nullptr;

    it->second.Substantiate(g_dev.GetPassThroughOrigDevice1(),
        (D3D11_BIND_FLAG)0, (D3D11_RESOURCE_MISC_FLAG)0);
    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(it->second.resource);
    return wr ? wr->dx12 : nullptr;
}

ID3D12Resource* CustomResourceDx12(const void* custom)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !custom) return nullptr;
    CustomResource* r = const_cast<CustomResource*>(static_cast<const CustomResource*>(custom));
    r->Substantiate(g_dev.GetPassThroughOrigDevice1(), (D3D11_BIND_FLAG)0, (D3D11_RESOURCE_MISC_FLAG)0);
    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(r->resource);
    return wr ? wr->dx12 : nullptr;
}

bool GetBufferResource(const wchar_t* fq_name, BufferResourceInfo* out)
{
    if (!out) return false;
    *out = BufferResourceInfo{};

    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name) return false;
    auto it = customResources.find(fq_name);
    if (it == customResources.end()) return false;

    {
        const CustomResourceType t = it->second.override_type;
        const bool               declared_buffer = (t == CustomResourceType::BUFFER ||
            t == CustomResourceType::STRUCTURED_BUFFER ||
            t == CustomResourceType::RAW_BUFFER);
        bool                     buf_file = false;
        const std::wstring&      fn = it->second.filename;
        if (fn.size() >= 4 && _wcsicmp(fn.c_str() + fn.size() - 4, L".buf") == 0) buf_file = true;

        if (!it->second.substantiated && !declared_buffer && !buf_file) return false;

        if (!it->second.substantiated)
            it->second.Substantiate(g_dev.GetPassThroughOrigDevice1(),
                (D3D11_BIND_FLAG)0, (D3D11_RESOURCE_MISC_FLAG)0);
    }

    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(it->second.resource);
    if (!wr || !wr->dx12) return false;
    if (wr->dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false; // textures go through the SRV walk

    out->dx12 = wr->dx12;
    out->gpu_va = (unsigned long long)wr->dx12->GetGPUVirtualAddress() + wr->gpu_va;

    if (auto* wb = static_cast<MigotoPort::WrapBuffer*>(wr)) {
        out->byte_width = wb->desc.ByteWidth;
        out->stride = wb->desc.StructureByteStride;
    }
    if (it->second.stride) out->stride = it->second.stride;
    if (!out->byte_width && it->second.buf_size) out->byte_width = it->second.buf_size;
    return true;
}

size_t BufferInitialData(const wchar_t* fq_name, void* out, size_t cap)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name || !out) return 0;
    auto it = customResources.find(fq_name);
    if (it == customResources.end() || !it->second.initial_data) return 0;
    const size_t n = (std::min)(cap, it->second.initial_data_size);
    memcpy(out, it->second.initial_data, n);
    return n;
}

bool UploadBufferResource(const wchar_t* fq_name, const void* data, size_t bytes)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name || !data || !bytes) return false;
    auto it = customResources.find(fq_name);
    if (it == customResources.end()) return false;
    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(it->second.resource);
    if (!wr || !wr->dx12 || wr->dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS      hf{};
    if (SUCCEEDED(wr->dx12->GetHeapProperties(&hp, &hf)) && hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
        void*             p = nullptr;
        const D3D12_RANGE none{ 0, 0 };
        if (FAILED(wr->dx12->Map(0, &none, &p)) || !p) return false;
        memcpy(p, data, bytes);
        const D3D12_RANGE wrote{ 0, bytes };
        wr->dx12->Unmap(0, &wrote);
        return true;
    }
    MigotoPort::QueueUpload(it->second.resource, data, bytes);
    return true;
}

void* AcquireBufferObject(const wchar_t* fq_name)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    BufferResourceInfo                    bi;
    if (!GetBufferResource(fq_name, &bi)) return nullptr; // substantiates only a buffer, as above
    auto it = customResources.find(fq_name);
    if (it == customResources.end() || !it->second.resource) return nullptr;
    it->second.resource->AddRef();
    return it->second.resource;
}

void ForEachUnsubstantiatedTexture(const wchar_t* ns, void (*cb)(const wchar_t* fq_name, const wchar_t* filename, void* user),
    void*                                         user)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !ns || !*ns || !cb) return;
    const std::wstring prefix = std::wstring(L"resource\\") + ns + L"\\";
    for (auto& kv : customResources) {
        const CustomResource& r = kv.second;
        if (r.substantiated || r.resource || r.view) continue;
        const std::wstring& fn = r.filename;
        if (fn.size() < 4 || _wcsicmp(fn.c_str() + fn.size() - 4, L".dds") != 0) continue;
        if (_wcsnicmp(kv.first.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        cb(kv.first.c_str(), fn.c_str(), user);
    }
}

void BeginDeferredTextures(const wchar_t* ns, std::vector<DeferredTexture>& out)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !ns || !*ns) return;
    const std::wstring prefix = std::wstring(L"resource\\") + ns + L"\\";
    for (auto& kv : customResources) {
        CustomResource& r = kv.second;
        if (r.substantiated || r.resource || r.view) continue;
        const std::wstring& fn = r.filename;
        if (fn.size() < 4 || _wcsicmp(fn.c_str() + fn.size() - 4, L".dds") != 0) continue;
        if (_wcsnicmp(kv.first.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        r.substantiated = true;                                                      // Substantiate's first act, so no draw loads it
        r.bind_flags = (D3D11_BIND_FLAG)(r.bind_flags | D3D11_BIND_SHADER_RESOURCE); // the flag its assignment would add
        if (r.override_bind_flags != CustomResourceBindFlags::INVALID)
            r.bind_flags = (D3D11_BIND_FLAG)r.override_bind_flags;
        if (r.override_misc_flags != ResourceMiscFlags::INVALID)
            r.misc_flags = (D3D11_RESOURCE_MISC_FLAG)r.override_misc_flags;
        out.push_back({ kv.first, fn, (unsigned)r.bind_flags, (unsigned)r.misc_flags,
            r.override_color_space == CustomColorSpace::SRGB });
        g_deferred_pending[&r] = g_generation;
    }
}

void LoadDeferredNow(CustomResource* r)
{
    if (!r || r->resource || r->view) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    auto                                  it = g_deferred_pending.find(r);
    if (it == g_deferred_pending.end()) return;
    const bool this_load = it->second == g_generation;
    g_deferred_pending.erase(it);
    if (!g_loaded || !this_load || r->resource || r->view) return;
    ID3D11Resource* res = LoadDeferredTexture({ std::wstring(), r->filename, (unsigned)r->bind_flags, (unsigned)r->misc_flags,
        r->override_color_space == CustomColorSpace::SRGB });
    if (!res) return;
    r->resource = res;
    r->device = g_dev.GetPassThroughOrigDevice1();
    r->is_null = false;
    MigotoPort::BumpSelectionInputs();
    const auto& names = CustomResourceNameIndex();
    const auto  n = names.find(r);
    logger::write("TEXEAGER-NOW %ls: a copy or a compute bind read it before the background load had -- loaded now, as on DX11",
        n != names.end() ? n->second.c_str() : r->filename.c_str());
}

ID3D11Resource* LoadDeferredTexture(const DeferredTexture& t)
{
    EnsureCOM();
    ID3D11Resource* res = nullptr;
    const HRESULT   hr = DirectX::CreateDDSTextureFromFileEx(g_dev.GetPassThroughOrigDevice1(), t.filename.c_str(), 0,
        D3D11_USAGE_DEFAULT, t.bind_flags, 0, t.misc_flags, t.force_srgb,
        &res, nullptr, nullptr);
    if (FAILED(hr)) logger::write("PORT: failed to load custom texture resource %ls: 0x%08x", t.filename.c_str(), (unsigned)hr);
    return SUCCEEDED(hr) ? res : nullptr;
}

bool PublishDeferredTexture(const wchar_t* fq_name, ID3D11Resource* res)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name || !res) return false;
    auto it = customResources.find(fq_name);
    if (it == customResources.end() || it->second.resource || it->second.view) return false;
    CustomResource& r = it->second;
    r.resource = res;
    r.device = g_dev.GetPassThroughOrigDevice1();
    r.is_null = false; // LoadFromFile's outcome
    g_deferred_pending.erase(&r);
    MigotoPort::BumpSelectionInputs();
    return true;
}

bool SubstantiateTexture(const wchar_t* fq_name)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || !fq_name) return false;
    auto it = customResources.find(fq_name);
    if (it == customResources.end() || it->second.substantiated) return false;
    it->second.Substantiate(g_dev.GetPassThroughOrigDevice1(), D3D11_BIND_SHADER_RESOURCE, (D3D11_RESOURCE_MISC_FLAG)0);
    return it->second.resource != nullptr;
}

void ForEachCustomResource(void (*cb)(const wchar_t*, const wchar_t*, int, bool, ID3D12Resource*, void*),
    void* user, bool (*want)(ID3D12Resource*, void*))
{
    if (!g_loaded || !cb) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    for (auto& kv : customResources) {
        MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(kv.second.resource);
        ID3D12Resource*           dx12 = wr ? wr->dx12 : nullptr;
        if (want && !want(dx12, user)) continue;
        cb(kv.first.c_str(), kv.second.filename.c_str(), (int)kv.second.override_type,
            kv.second.substantiated, dx12, user);
    }
}
} // namespace PortApi
