#define NOMINMAX
#include "../reshade_addon/textures/tex_eager.h"
#include "../reshade_addon/config.h"
#include <d3d11.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>

// Actual worker, controlled CPU loading callbacks. No files or GPU are used.
static void Check(bool v, const char* why) { if (!v) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); } }
static std::mutex test_mutex;
static std::condition_variable test_cv;
static std::atomic<unsigned> generation{ 1 }, live_resources{ 0 };
static const std::thread::id caller = std::this_thread::get_id();
struct Stage { unsigned begins = 0, loads = 0, published = 0, failures = 0; bool entered = false, release = false; };
static std::map<unsigned, Stage> stages;
static std::map<std::pair<unsigned, std::wstring>, bool> pending;
struct Resource final : ID3D11Resource {
    std::atomic<ULONG> refs{ 1 };
    Resource() { ++live_resources; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) { --live_resources; delete this; } return n; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* out) override { *out = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
};
namespace logger { void write(const char*, ...) {} }
namespace PortApi {
unsigned LoadGeneration() { return generation.load(); }
EngineScope::EngineScope() {}
EngineScope::~EngineScope() {}
bool SubstantiateTexture(const wchar_t*) { return false; }
void ForEachUnsubstantiatedTexture(const wchar_t*, void (*)(const wchar_t*, const wchar_t*, void*), void*) {}
void BeginDeferredTextures(const wchar_t* ns, std::vector<DeferredTexture>& out)
{
    std::lock_guard lock(test_mutex);
    const auto gen = generation.load(); ++stages[gen].begins;
    if (std::wstring(ns) == L"empty") return;
    for (const wchar_t* suffix : { L"first", L"second", L"missing" }) {
        const std::wstring fq = std::wstring(ns) + L"/" + suffix;
        out.push_back({ fq, std::to_wstring(gen) + L":" + suffix, 0, 0, false });
        pending[{gen, fq}] = true;
    }
}
bool DeferredTexturePending(const DeferredTexture& t, unsigned gen)
{
    std::lock_guard lock(test_mutex);
    return gen == generation.load() && pending[{gen, t.fq_name}];
}
ID3D11Resource* LoadDeferredTexture(const DeferredTexture& t)
{
    Check(std::this_thread::get_id() != caller, "file loading must not run on the drawing caller");
    const unsigned gen = unsigned(std::stoul(t.filename));
    std::unique_lock lock(test_mutex);
    auto& s = stages[gen]; ++s.loads;
    if (t.filename.find(L":first") != std::wstring::npos) {
        s.entered = true; test_cv.notify_all();
        test_cv.wait(lock, [&] { return s.release; });
    }
    return t.filename.find(L":missing") == std::wstring::npos ? new Resource : nullptr;
}
bool PublishDeferredTexture(const wchar_t* fq, ID3D11Resource* res, unsigned gen)
{
    std::lock_guard lock(test_mutex);
    if (gen != generation.load() || !pending[{gen, fq}]) return false;
    pending[{gen, fq}] = false;
    if (res) { ++stages[gen].published; res->Release(); }
    else ++stages[gen].failures;
    return true;
}
} // namespace PortApi
static void Entered(unsigned gen)
{
    std::unique_lock lock(test_mutex);
    Check(test_cv.wait_for(lock, std::chrono::seconds(3), [&] { return stages[gen].entered; }), "worker starts the queued generation");
}
static void Release(unsigned gen)
{
    { std::lock_guard lock(test_mutex); stages[gen].release = true; }
    test_cv.notify_all();
}
static void Ready(TexEagerWorker& worker)
{
    for (unsigned i = 0; i < 3000 && !worker.ReadyForDraw(0, L"mod"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(worker.ReadyForDraw(0, L"mod"), "completed publication allows the mod draw");
}
int main()
{
    TexEagerWorker worker;
    Check(!worker.ReadyForDraw(0, L"mod"), "first draw defers without waiting for texture I/O");
    Entered(1);
    for (unsigned i = 0; i < 16; ++i) Check(!worker.ReadyForDraw(0, L"mod"), "polling while loading retains vanilla draw");
    {
        std::lock_guard lock(test_mutex);
        Check(stages[1].begins == 1 && stages[1].loads == 1, "repeated draws do not queue duplicate work");
        pending[{1, L"mod/second"}] = false; // another legacy route already supplied it
    }
    Release(1); Ready(worker);
    {
        std::lock_guard lock(test_mutex);
        Check(stages[1].loads == 2 && stages[1].published == 1 && stages[1].failures == 1,
            "skip supplied textures and complete failed files without hanging readiness");
        Check(!pending[{1, L"mod/missing"}], "failed file no longer needs a foreground retry");
    }
    generation.store(2);
    Check(!worker.ReadyForDraw(0, L"mod"), "reload warms again"); Entered(2);
    generation.store(3);
    Check(!worker.ReadyForDraw(0, L"mod"), "new generation has independent pending state");
    Release(2); Entered(3);
    Check(!worker.ReadyForDraw(0, L"mod"), "old worker completion cannot clear the new generation's pending state");
    Release(3); Ready(worker);
    {
        std::lock_guard lock(test_mutex);
        Check(stages[2].published == 0 && stages[3].published == 2 && stages[3].failures == 1,
            "stale resources are rejected and the new generation finishes all outcomes");
    }
    Check(worker.ReadyForDraw(5, L"empty"), "no textures means immediate readiness");
    g_tex_deferred = false;
    Check(worker.ReadyForDraw(9, L"legacy"), "explicit legacy mode preserves its behavior");
    worker.Stop();
    Check(live_resources == 0, "rejected stale resources and accepted resources are released");
    std::cout << "rt_tex_eager_test: PASS (actual worker, CPU callbacks, no GPU)\n";
}
