#include "mod_loader.h"
#include "log.h"
#include "../reshade_addon/textures/tex_lod.h"         // TexLodInit: the LOD cache read again on F5
#include "../reshade_addon/textures/tex_look.h"        // TexLookInit: a seed dropped in since taken on F5
#include "../reshade_addon/migoto/port_api.h"          // their [Resource*] registry
#include "../reshade_addon/instrumentation/profiler.h" // ET:TexStore
#include <windows.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <string>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <cwctype> // towlower -- EnumerateMods' folder match
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;
namespace fs = std::filesystem;

static device* g_device = nullptr;
static bool    g_reload_pending = false;


static ComPtr<ID3D12DescriptorHeap>              g_srv_heap;
static std::vector<ComPtr<ID3D12DescriptorHeap>> g_srv_more; // further blocks of g_srv_heap_cap, as the library grows
static uint32_t                                  g_srv_heap_size = 0;
static uint32_t                                  g_srv_heap_cap = 0; // descriptors in g_srv_heap (and in each further block); 0 until it exists
namespace ModLoader {
unsigned SrvHeapSize()
{
    return g_srv_heap_size;
}
unsigned SrvHeapCap()
{
    return g_srv_heap_cap * (unsigned)(1 + g_srv_more.size());
}
}
static uint32_t                                 g_srv_descriptor_size = 0;
static std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> g_srv_free;      // slots given back by TexSrvFree
static unsigned                                 g_srv_epoch = 0; // bumped when every slot is handed out again from 0
// The next mod SRV slot; a further block is added when the ones there are used up. {0} if one could not be made.
static D3D12_CPU_DESCRIPTOR_HANDLE NextModSrv()
{
    if (!g_srv_heap || !g_srv_heap_cap) return { 0 };
    if (!g_srv_free.empty()) {
        const D3D12_CPU_DESCRIPTOR_HANDLE c = g_srv_free.back();
        g_srv_free.pop_back();
        return c;
    }
    const uint32_t        blk = g_srv_heap_size / g_srv_heap_cap;
    const uint32_t        off = g_srv_heap_size % g_srv_heap_cap;
    ID3D12DescriptorHeap* h = g_srv_heap.Get();
    if (blk) {
        while (g_srv_more.size() < blk) {
            ComPtr<ID3D12Device>             dev;
            ComPtr<ID3D12DescriptorHeap>     nb;
            const D3D12_DESCRIPTOR_HEAP_DESC d = g_srv_heap->GetDesc();
            if (FAILED(g_srv_heap->GetDevice(IID_PPV_ARGS(&dev))) || FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&nb)))) return { 0 };
            g_srv_more.push_back(nb);
            logger::write("ModLoader: mod SRV heap grown to %u descriptors (the library outgrew its first sizing)",
                g_srv_heap_cap * (unsigned)(1 + g_srv_more.size()));
        }
        h = g_srv_more[blk - 1].Get();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)off * g_srv_descriptor_size;
    ++g_srv_heap_size;
    return c;
}
static uint32_t SrvHeapCapacity(unsigned* dds_out)
{
    unsigned dds = 0;
    PortApi::ForEachCustomResource(
        [](const wchar_t*, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
            const size_t n = filename ? wcslen(filename) : 0;
            if (n >= 4 && _wcsicmp(filename + n - 4, L".dds") == 0) ++*static_cast<unsigned*>(user);
        },
        &dds);
    if (dds_out) *dds_out = dds;
    const uint32_t need = 2u * dds + 256u;
    const uint32_t cap = (need < 1024u) ? 1024u : ((need + 63u) & ~63u);
    return cap;
}
static void TexStoreAlloc(uint32_t cap); // below, with the store

void ModLoader::Init(device* device)
{
    g_device = device;
}

void ModLoader::GetLastStats(int& mods)
{
    static unsigned s_gen = ~0u;
    static int      s_mods = 0;
    const unsigned  gen = PortApi::LoadGeneration();
    if (gen != s_gen) {
        s_gen = gen;
        s_mods = (int)EnumerateMods().size();
    }
    mods = s_mods;
}

static void DoReload()
{
    if (!g_device) return;
    if (g_device->get_api() != reshade::api::device_api::d3d12) {
        logger::write("ModLoader: skipping reload -- device is not D3D12 (api=%u)", (unsigned)g_device->get_api());
        return;
    }
    TexLodInit(); // the LOD cache read again, and any seed dropped in since taken
    TexLookInit();
    g_srv_heap_size = 0;
    g_srv_free.clear();
    ++g_srv_epoch;
}

static std::vector<ModLoader::TexResource> g_tex_resources;
struct TexStore {
    static constexpr uint64_t kGone = 1; // a key taken out: lookups probe past it
    uint32_t                  cap = 0;
    uint32_t                  icap = 0;
    unsigned                  shift = 64;
    ModLoader::TexEntry*      entries = nullptr;
    std::atomic<uint64_t>*    used = nullptr;  // per index: GetTickCount64 when last handed to a draw
    std::atomic<uint64_t>*    used_at = nullptr; // per index x kTexMips: ... where the game drew its own that many mips down
    uint64_t*                 bytes = nullptr; // per index: its allocation size; present thread
    unsigned*                 srv_epoch = nullptr; // per index: g_srv_epoch when its SRV slot was handed out; present thread
    std::atomic<uint64_t>*    key = nullptr;
    std::atomic<uint32_t>*    val = nullptr;
    uint32_t                  slot(uint64_t k) const { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> shift) & (icap - 1); }
    void                      put_key(uint64_t k, uint32_t index)
    {
        for (uint32_t i = slot(k), n = 0; n < icap; ++n, i = (i + 1) & (icap - 1)) {
            const uint64_t was = key[i].load(std::memory_order_acquire);
            if (was == k || was == 0) {
                val[i].store(index, std::memory_order_release);
                key[i].store(k, std::memory_order_release); // publish the key LAST
                return;
            }
        }
    }
    void drop_key(uint64_t k)
    {
        for (uint32_t i = slot(k), n = 0; n < icap; ++n, i = (i + 1) & (icap - 1)) {
            const uint64_t was = key[i].load(std::memory_order_acquire);
            if (was == k) {
                key[i].store(kGone, std::memory_order_release);
                return;
            }
            if (was == 0) return;
        }
    }
};
static std::atomic<TexStore*> g_tex_store{ nullptr };
static std::atomic<uint32_t>  g_tex_count{ 0 };
static std::atomic<unsigned>  g_tex_gen{ 1 };
static std::atomic<uint64_t>  g_tex_bytes{ 0 };
static TexStore*              TexStoreMake(uint32_t cap)
{
    auto*    s = new TexStore();
    uint32_t icap = 4096u;
    unsigned bits = 12;
    while (icap < 4u * cap) {
        icap <<= 1;
        ++bits;
    }
    s->entries = new ModLoader::TexEntry[cap]();
    s->used = new std::atomic<uint64_t>[cap];
    s->bytes = new uint64_t[cap]();
    s->srv_epoch = new unsigned[cap]();
    s->used_at = new std::atomic<uint64_t>[(size_t)cap * ModLoader::kTexMips];
    for (uint32_t i = 0; i < cap; ++i) s->used[i].store(0);
    for (size_t i = 0; i < (size_t)cap * ModLoader::kTexMips; ++i) s->used_at[i].store(0);
    s->key = new std::atomic<uint64_t>[icap];
    s->val = new std::atomic<uint32_t>[icap];
    for (uint32_t i = 0; i < icap; ++i) {
        s->key[i].store(0);
        s->val[i].store(0);
    }
    s->shift = 64u - bits;
    s->icap = icap;
    s->cap = cap;
    return s;
}
static void TexStoreAlloc(uint32_t cap)
{
    if (g_tex_store.load(std::memory_order_acquire) || !cap) return;
    g_tex_store.store(TexStoreMake(cap), std::memory_order_release);
}
static void TexStoreClear()
{
    g_tex_gen.fetch_add(1, std::memory_order_acq_rel);
    g_tex_count.store(0, std::memory_order_release);
    g_tex_bytes.store(0, std::memory_order_release);
    if (TexStore* s = g_tex_store.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < s->icap; ++i) s->key[i].store(0, std::memory_order_release);
        for (uint32_t i = 0; i < s->cap; ++i) {
            s->used[i].store(0, std::memory_order_relaxed);
            s->bytes[i] = 0;
        }
        for (size_t i = 0; i < (size_t)s->cap * ModLoader::kTexMips; ++i) s->used_at[i].store(0, std::memory_order_relaxed);
    }
}
static void TexStorePublish(uint32_t index, const ModLoader::TexEntry& e, uint64_t bytes)
{
    TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!s || !e.res) return;
    if (index >= s->cap) {
        uint32_t cap = s->cap * 2;
        while (cap <= index) cap *= 2;
        TexStore*      g = TexStoreMake(cap);
        const uint32_t n = g_tex_count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < s->cap; ++i) {
            g->entries[i] = s->entries[i];
            g->used[i].store(s->used[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            for (unsigned k = 0; k < ModLoader::kTexMips; ++k)
                g->used_at[(size_t)i * ModLoader::kTexMips + k].store(
                    s->used_at[(size_t)i * ModLoader::kTexMips + k].load(std::memory_order_relaxed), std::memory_order_relaxed);
            g->bytes[i] = s->bytes[i];
            g->srv_epoch[i] = s->srv_epoch[i];
            if (s->entries[i].res) g->put_key((uint64_t)s->entries[i].res, i);
        }
        g_tex_store.store(g, std::memory_order_release);
        logger::write("ModLoader: texture store grown to %u entries", cap);
        s = g;
    }
    s->entries[index] = e;
    s->used[index].store(0, std::memory_order_relaxed);
    for (unsigned k = 0; k < ModLoader::kTexMips; ++k) s->used_at[(size_t)index * ModLoader::kTexMips + k].store(0, std::memory_order_relaxed);
    s->bytes[index] = bytes;
    s->srv_epoch[index] = g_srv_epoch;
    g_tex_bytes.fetch_add(bytes, std::memory_order_acq_rel);
    s->put_key((uint64_t)e.res, index);
    g_tex_count.store(index + 1, std::memory_order_release);
    g_tex_gen.fetch_add(1, std::memory_order_acq_rel);
}
unsigned ModLoader::TexStoreGen()
{
    return g_tex_gen.load(std::memory_order_acquire);
}
static std::atomic<bool> g_tex_touch{ false };
void                     ModLoader::SetTexTouch(bool on)
{
    g_tex_touch.store(on, std::memory_order_relaxed);
}
void ModLoader::TexTouch(int index)
{
    if (!g_tex_touch.load(std::memory_order_relaxed)) return;
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return;
    const uint64_t now = GetTickCount64();
    if (s->used[index].load(std::memory_order_relaxed) != now) s->used[index].store(now, std::memory_order_relaxed);
}
void ModLoader::TexTouchRes(ID3D12Resource* res)
{
    if (g_tex_touch.load(std::memory_order_relaxed)) TexTouch(TexIndexOf(res));
}
uint64_t ModLoader::TexLastUse(int index)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    return s && index >= 0 && (uint32_t)index < s->cap ? s->used[index].load(std::memory_order_relaxed) : 0;
}
static std::atomic<bool> g_tex_mips{ false };
void                     ModLoader::SetTexMips(bool on)
{
    g_tex_mips.store(on, std::memory_order_relaxed);
}
void ModLoader::TexTouchAt(int index, unsigned mip)
{
    TexTouch(index);
    if (mip == kTexMipUnknown || !g_tex_mips.load(std::memory_order_relaxed) || !g_tex_touch.load(std::memory_order_relaxed)) return;
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return;
    const uint64_t         now = GetTickCount64();
    std::atomic<uint64_t>& at = s->used_at[(size_t)index * kTexMips + (mip < kTexMips ? mip : kTexMips - 1)];
    if (at.load(std::memory_order_relaxed) != now) at.store(now, std::memory_order_relaxed);
}
unsigned ModLoader::TexSharpest(int index, uint64_t since)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return kTexMipUnknown;
    for (unsigned k = 0; k < kTexMips; ++k) {
        const uint64_t at = s->used_at[(size_t)index * kTexMips + k].load(std::memory_order_relaxed);
        if (at && at >= since) return k;
    }
    return kTexMipUnknown;
}
uint64_t ModLoader::TexBytes()
{
    return g_tex_bytes.load(std::memory_order_acquire);
}
int ModLoader::TexIndexOf(ID3D12Resource* res)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!res || !s) return -1;
    const uint64_t key = (uint64_t)res;
    for (uint32_t i = s->slot(key), n = 0; n < s->icap; ++n, i = (i + 1) & (s->icap - 1)) {
        const uint64_t k = s->key[i].load(std::memory_order_acquire);
        if (k == key) {
            const uint32_t v = s->val[i].load(std::memory_order_acquire);
            return (v < g_tex_count.load(std::memory_order_acquire)) ? (int)v : -1;
        }
        if (k == 0) return -1;
    }
    return -1;
}
bool ModLoader::TexEntryAt(int index, TexEntry* out)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!out || !s || index < 0 || (uint32_t)index >= g_tex_count.load(std::memory_order_acquire) || (uint32_t)index >= s->cap) return false;
    *out = s->entries[index];
    return out->res != nullptr && out->srv.ptr != 0;
}
ID3D12Resource* ModLoader::TexResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv)
{
    const TexStore* s = g_tex_store.load(std::memory_order_acquire);
    if (!srv.ptr || !s) return nullptr;
    const uint32_t n = (std::min)(g_tex_count.load(std::memory_order_acquire), s->cap);
    for (uint32_t i = 0; i < n; ++i)
        if (s->entries[i].srv.ptr == srv.ptr) return s->entries[i].res;
    return nullptr;
}
bool ModLoader::TexForget(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE* srv, unsigned* epoch)
{
    TexStore*  s = g_tex_store.load(std::memory_order_acquire);
    const int  index = TexIndexOf(res);
    if (!s || index < 0) return false;
    TexEntry& e = s->entries[index];
    if (srv) *srv = e.srv;
    if (epoch) *epoch = s->srv_epoch[index];
    g_tex_bytes.fetch_sub((std::min)(s->bytes[index], g_tex_bytes.load(std::memory_order_acquire)), std::memory_order_acq_rel);
    s->bytes[index] = 0;
    s->drop_key((uint64_t)res);
    std::atomic_ref<ID3D12Resource*>(e.res).store(nullptr, std::memory_order_release);
    if ((size_t)index < g_tex_resources.size()) {
        g_tex_resources[index].valid = false;
        g_tex_resources[index].res = nullptr;
    }
    g_tex_gen.fetch_add(1, std::memory_order_acq_rel);
    return true;
}
unsigned ModLoader::SrvEpoch()
{
    return g_srv_epoch;
}
void ModLoader::TexSrvFree(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch)
{
    if (srv.ptr && epoch == g_srv_epoch) g_srv_free.push_back(srv);
}
static bool g_tex_ovr_loaded = false;

// Ini parsing is owned by the ported engine; runs once it has loaded: the SRV heap, sized from what it declared.
void ModLoader::LoadTextureOverrides()
{
    if (g_tex_ovr_loaded) return;
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev) return;
    g_tex_ovr_loaded = true;
    if (!g_srv_heap) {
        unsigned                   n_dds = 0;
        const uint32_t             cap = SrvHeapCapacity(&n_dds);
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cap, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        HRESULT                    hr = dev->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_srv_heap));
        if (FAILED(hr)) {
            logger::write("ModLoader Error: Failed to create SRV heap hr=%08x", hr);
            return;
        }
        g_srv_descriptor_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_srv_heap_cap = cap;
        TexStoreAlloc(cap);
        logger::write("ModLoader: mod SRV heap sized %u for the library (%u .dds declared)", cap, n_dds);
    }
    logger::write("ModLoader: %zu mods (from the inis their engine parsed)", EnumerateMods().size());
}
static DXGI_FORMAT SrvFormatFor(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f; // already fully qualified
    }
}
// A 2D texture's SRV in a mod slot and its store entry; its index, or -1.
static int DescribeTexture(ID3D12Device* dev, ID3D12Resource* dx12, const wchar_t* name)
{
    const D3D12_RESOURCE_DESC       rd = dx12->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = SrvFormatFor(rd.Format);
    if (srv.Format != rd.Format)
        logger::write("PortRes: typeless resource format %d described as %d for the SRV",
            (int)rd.Format, (int)srv.Format);
    if (srv.Format == DXGI_FORMAT_UNKNOWN) {
        logger::write("PortRes: SKIPPING SRV -- resource format %d has no view-legal form",
            (int)rd.Format);
        return -1;
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = rd.MipLevels ? rd.MipLevels : 1;

    const D3D12_CPU_DESCRIPTOR_HANDLE h = NextModSrv();
    if (!h.ptr) return -1;
    dev->CreateShaderResourceView(dx12, &srv, h);

    g_tex_resources.push_back({ h, true, dx12, name });
    const uint32_t index = (uint32_t)g_tex_resources.size() - 1;
    TexStorePublish(index, ModLoader::TexEntry{ h, dx12 }, dev->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
    return (int)index;
}
int ModLoader::TexDescribe(ID3D12Resource* res, const wchar_t* name)
{
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev || !g_srv_heap || !res) return -1;
    if (res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return -1;
    const int known = TexIndexOf(res);
    return known >= 0 ? known : DescribeTexture(dev, res, name ? name : L"");
}
bool ModLoader::SubstantiateWclResources()
{
    PROF(PROF_ET_TexStore);
    ID3D12Device* dev = g_device ? reinterpret_cast<ID3D12Device*>(g_device->get_native()) : nullptr;
    if (!dev || !g_srv_heap) return false;
    {
        static unsigned s_store_gen = 0;
        static long     s_born = -1;
        static unsigned s_calls = 0;
        const unsigned  gen = PortApi::LoadGeneration();
        if (s_store_gen != gen) {
            s_store_gen = gen;
            TexStoreClear(); // the lock-free read side first, so no reader resolves into the old table
            g_tex_resources.clear();
            s_born = -1;
        }
        const long born = PortApi::WrapResourcesCreated();
        if (born == s_born && (++s_calls & 7u)) return true;
        s_born = born;
        s_calls = 0;
    }

    int registered = 0;
    struct Ctx {
        int*          registered;
        ID3D12Device* dev;
    } ctx{ &registered, dev };
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq_name, const wchar_t* filename, int type, bool substantiated,
            ID3D12Resource* dx12, void* user) {
            Ctx* c = (Ctx*)user;
            (void)type;
            (void)substantiated;
            if (!dx12) return; // not substantiated yet, or nothing to create
            const D3D12_RESOURCE_DESC rd = dx12->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return; // buffers are not swapped

            if (ModLoader::TexIndexOf(dx12) >= 0) return;

            if (DescribeTexture(c->dev, dx12, filename && *filename ? filename : fq_name) >= 0) ++*c->registered;
        },
        &ctx,
        [](ID3D12Resource* dx12, void*) { return dx12 && ModLoader::TexIndexOf(dx12) < 0; });

    if (registered > 0)
        logger::write("PortRes: described %d newly substantiated texture(s); %zu total, %u SRV slots used",
            registered, g_tex_resources.size(), g_srv_heap_size);
    return true;
}

const std::vector<ModLoader::TexResource>& ModLoader::TexResources()
{
    return g_tex_resources;
}
std::vector<ModLoader::DiscoveredMod> ModLoader::EnumerateMods()
{
    struct Found {
        fs::path root;
        ModIni   ini;
    };
    std::vector<Found> found;
    PortApi::ForEachModIni(
        [](const wchar_t* ini, const wchar_t* ns, const wchar_t* root, void* user) {
            static_cast<std::vector<Found>*>(user)->push_back({ root, { ini, ns } });
        },
        &found);
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    };
    std::unordered_set<std::wstring> ini_dirs;
    for (const auto& f : found) ini_dirs.insert(lower(fs::path(f.ini.path).parent_path().wstring()));
    std::map<fs::path, DiscoveredMod> mods;
    for (auto& f : found) {
        const fs::path folder = fs::path(f.ini.path).parent_path();
        fs::path       key = f.ini.path;
        fs::path       dir = f.root;
        fs::path       walk = f.root;
        for (const auto& part : folder.lexically_relative(f.root)) {
            if (part == L".") break;
            walk /= part;
            if (ini_dirs.count(lower(walk.wstring()))) {
                key = walk;
                dir = walk;
                break;
            }
        }
        DiscoveredMod& m = mods[key];
        m.dir = dir.wstring();
        m.name = (dir == f.root ? fs::path(f.ini.path).filename() : dir.lexically_relative(f.root)).wstring();
        m.inis.push_back(std::move(f.ini));
    }
    std::vector<DiscoveredMod> out;
    for (auto& kv : mods) {
        std::sort(kv.second.inis.begin(), kv.second.inis.end(),
            [](const ModIni& a, const ModIni& b) { return a.path < b.path; });
        out.push_back(std::move(kv.second));
    }
    return out;
}

void ModLoader::Tick()
{
    if (g_reload_pending) {
        DoReload();
        g_reload_pending = false;
    }
}
void ModLoader::Reload()
{
    g_reload_pending = true;
}
