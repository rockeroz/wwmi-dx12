#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <string>
#include <vector>

namespace ModLoader {

unsigned SrvHeapSize();
unsigned SrvHeapCap();
void     Init(reshade::api::device* device);
void     Reload();
void     Tick(); // runs a pending Reload
void     GetLastStats(int& out_mods);

// A mod texture the host has loaded: SRV + resource + the file it came from.
struct TexResource {
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    bool                        valid = false;
    ID3D12Resource*             res = nullptr;
    std::wstring                path;
};

void                            LoadTextureOverrides();
bool                            SubstantiateWclResources();
const std::vector<TexResource>& TexResources();
struct TexEntry {
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    ID3D12Resource*             res = nullptr;
};
int             TexIndexOf(ID3D12Resource* res);
bool            TexEntryAt(int index, TexEntry* out);
unsigned        TexStoreGen();
ID3D12Resource* TexResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv);
void            TexTouch(int index);      // handed to a draw now
void            TexTouchRes(ID3D12Resource* res); // ... by its resource
void            SetTexTouch(bool on);     // off: neither records anything -- nothing reads it then
uint64_t        TexLastUse(int index);    // GetTickCount64 when it last was; 0 = never
constexpr unsigned kTexMips = 4;          // how far down the game draws its own texture there: 0, 1, 2, 3 or more
constexpr unsigned kTexMipUnknown = ~0u;
void            TexTouchAt(int index, unsigned mip); // handed to a draw where the game draws its own `mip` levels down
void            SetTexMips(bool on);                 // off: TexTouchAt records only what TexTouch does
unsigned        TexSharpest(int index, uint64_t since); // the fewest levels down it was handed at since then, else kTexMipUnknown
uint64_t        TexBytes();               // every texture in the store, at its allocation size
// Present thread, as SubstantiateWclResources: a texture described before anything can resolve to it; its entry
// taken out again (key gone, index dead, its SRV slot handed back for TexSrvFree once nothing can be copying it).
int      TexDescribe(ID3D12Resource* res, const wchar_t* name);
bool     TexForget(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE* srv, unsigned* epoch); // epoch: SrvEpoch() when the slot was handed out
unsigned SrvEpoch();
void     TexSrvFree(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch);

// One mod: the top-most folder under one of d3dx.ini's include_recursive roots holding an ini their engine parsed,
// with every such ini under it. An ini directly in a root is a mod of its own.
struct ModIni {
    std::wstring path; // absolute
    std::wstring ns;   // their engine's namespace for it
};
struct DiscoveredMod {
    std::wstring        dir;  // absolute mod folder
    std::wstring        name; // its folder from the root; the ini's file name when it sits in the root
    std::vector<ModIni> inis;
};
std::vector<DiscoveredMod> EnumerateMods();
}
