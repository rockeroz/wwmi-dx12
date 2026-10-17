#pragma once
// What the addon hands the ported engine: the DX12 objects a transition writes into, per frame and per
// draw scope, and the resource-state query. Hand-written; dx12_context.h is generated.
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace MigotoPort {

// Set by the addon each frame: the DX12 objects a transition writes into.
struct Dx12Bridge {
    ID3D12Device*              device = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr; // the game's current command list
    ID3D12CommandQueue*        queue = nullptr;
    unsigned                   rt_width = 0;
    unsigned                   rt_height = 0;
    DXGI_FORMAT                rt_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT                ds_format = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource*            back_buffer = nullptr;
    D3D12_RESOURCE_STATES      back_buffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    void (*restore_game_state)(void*) = nullptr;
    void* restore_user = nullptr;

    bool own_list = false; // true = ours ([Present]); false = the game's

    int (*vs_cb_root)(void* user, unsigned slot) = nullptr;
    void* vs_cb_root_user = nullptr;

    unsigned long owner_tid = 0;
};
void               SetDx12Bridge(const Dx12Bridge& b);
const Dx12Bridge&  GetDx12Bridge();
unsigned long long BridgeForeignListRefusals();          // times a thread was handed the global with its cl nulled
void               BridgePush(const Dx12Bridge& scoped); // DrawListScope ctor: `scoped` already carries cl/restore/roots
void               BridgePop();                          // DrawListScope dtor
unsigned           BridgeDepth();                        // open scopes, all threads
void               OweGameState(const Dx12Bridge& b);    // after our compute work on the game's list
void               PayGameState();                       // before anything that draws with the game's state
bool               BridgeNoteIssue(ID3D12GraphicsCommandList* cl, bool dispatch, unsigned a, unsigned b, unsigned c);
unsigned long long BridgeDisplacedCount();
unsigned long long BridgeWrongListDraws();
unsigned long long BridgeWrongListDispatches();

static const uint32_t kResStateUnknown = 0xFFFFFFFFu;
void                  SetResourceStateQuery(uint32_t (*fn)(void* d3d12_resource));
// kResStateUnknown when never transitioned or no query is installed; COMMON (0) is a real answer here.
uint32_t ObservedResourceState(void* d3d12_resource);
// The same with unknown folded into 0, for the copies that SKIP on 0 rather than guess a StateBefore.
unsigned QueryResourceState(void* d3d12_resource);

void                SetDumpDir(const wchar_t* dir);
const std::wstring& DumpDir();
unsigned long long  VbNullUnbinds();
void                NoteVbNullUnbind();
void                NoteEmittedDispatch(uint32_t x, uint32_t y, uint32_t z);
void                ResetEmittedDispatch();
unsigned            TakeEmittedDispatch(uint32_t* x, uint32_t* y, uint32_t* z); // count since the reset; resets
void                NoteVbBind(const void* state, int nonnull);
void                VbBindStats(unsigned* calls, unsigned* nonnull, uint64_t* last_ps);

struct ShimBufView {
    ID3D12Resource*    res = nullptr; // valid within this frame only; never stored
    unsigned long long va = 0;
    unsigned           size = 0;
    unsigned           stride = 0;
    unsigned           state = 0;
};
bool                  ShimBufferForCompute(ID3D12GraphicsCommandList* cl, char kind, unsigned slot, ShimBufView* out, const char** why,
    void* object = nullptr);
void*                 AcquireShimVsCb(unsigned slot);
void                  OwnBuffer(ID3D12Resource* res);
void                  DisownBuffer(ID3D12Resource* res);
bool                  OwnedBuffer(ID3D12Resource* res);
D3D12_RESOURCE_STATES OwnedTransition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES to);
void                  ListBuffersToCommon(ID3D12GraphicsCommandList* cl);
void                  FlushUavBarriers(ID3D12GraphicsCommandList* cl);
void                  ListBuffersForget(ID3D12GraphicsCommandList* cl);
void                  ReleaseShimObject(void* object);
// True for a buffer the GAME owns (a wrapper PushGameConstantBuffer and friends made) -- never a mod skeleton.
bool ShimObjectForeign(void* object);

inline thread_local bool g_touched_viewport = false;
inline thread_local bool g_touched_blend = false;
inline thread_local bool g_touched_stencil = false;
// [Present] run again for a present no game frame came before (port_api.cpp PresentRedraw): its draws only.
inline thread_local bool g_present_redraw = false;

bool TakeHeapsChanged();
} // namespace MigotoPort
