#pragma once
#include "shapekey_chain.h"

// ---- the per-instance store (sk_store.cpp) ----------------------------------------------------------

struct SkNsState {
    SkBufferBinding reg[kRegisterFileTotal];

    std::unordered_map<unsigned,
        std::unordered_map<unsigned long long, std::unordered_map<unsigned, SkBufferBinding>>>
        snap;

    bool valid = false;
};

/* The name the replay path reads: "the whole compute state" and "this owner's bindings" are one thing. */
using SkComputeState = SkNsState;

inline const SkBufferBinding& SkReg(const SkNsState& s, TargetClass c, unsigned slot)
{
    const unsigned i = RegisterIndex(c, slot);
    assert(i != kNoRegister && "SkReg called with a slot outside the class's register file");
    return s.reg[i];
}

inline SkBufferBinding& SkRegMut(SkNsState& s, TargetClass c, unsigned slot)
{
    const unsigned i = RegisterIndex(c, slot);
    assert(i != kNoRegister && "SkRegMut called with a slot outside the class's register file");
    return s.reg[i];
}

// Publish a whole-state snapshot for the owner's instance. The register file only; its cb0 copies stay.
void SkSetStateForNs(const std::wstring& ns, ID3D12Resource* inst, const SkComputeState& st);
// The instance's last published register file; false when it has none.
bool SkGetStateForNs(const std::wstring& ns, ID3D12Resource* inst, SkComputeState* out);
// Copy the dispatch's cb0 on the game's own command list, keyed by the queuing dispatch.
void SkSnapshotCb0(reshade::api::command_list* cmd_list, const std::wstring& ns, ID3D12Resource* inst,
    ID3D12Resource* src, uint64_t va, unsigned size,
    unsigned long long cs_hash, unsigned tgy);
// Drop the queued lists of an instance whose buffer the game destroyed (sk_queue.cpp).
void SkForgetPendingFor(ID3D12Resource* dead);

// ---- submission order (sk_submit_order.cpp) --------------------------------------------------------

struct SkDispAnchor {
    std::atomic<const void*> cl{ nullptr };
    std::atomic<uint32_t>    frame{ 0 };
};
inline SkDispAnchor g_sk_disp[2];
// The dispatch list recorded FOR THAT EXACT FRAME, or nullptr when none was seen.
inline const void* SkDispListForFrame(uint32_t f)
{
    const SkDispAnchor& a = g_sk_disp[f & 1];
    return (a.frame.load(std::memory_order_relaxed) == f)
        ? a.cl.load(std::memory_order_relaxed)
        : nullptr;
}

// The index at which `list` was submitted in `frame`; -1 when it was not (or the frame has aged out).
int SubmitIndexOf(uint32_t frame, const void* list);
// Lists seen by on_execute_command_list, for the SHAPEKEY-ORDER line. Counted only under g_debug.
extern std::atomic<unsigned long long> g_sub_seen;

bool     SkModDeclaresCb(int i, unsigned cb);
unsigned SkModScaleFor(int i, unsigned cb);
unsigned SkModCbFor(int i, unsigned u0hash);

struct SkClearNote {
    const void* cl = nullptr;
    uint32_t    gx = 0;
    uint32_t    frame = 0;
};
extern thread_local SkClearNote t_sk_clear;

// The byte range u0 must have for THIS dispatch, from its own geometry; 0/0 when nothing is known.
void SkGeomExpect(const void* cl, uint64_t cs_hash, uint32_t gy, unsigned* lo, unsigned* hi);
int  SkFindU0ByGeometry(reshade::api::command_list* cmd_list, unsigned lo, unsigned hi,
    ID3D12Resource** out_res, unsigned* out_bytes);
// Live buffers whose identity size hashes to the owner's declared scale value, minus our relocations.
int SkFindU1ByIdentity(reshade::api::command_list* cmd_list, unsigned want_scale, ID3D12Resource* u0,
    ID3D12Resource** out_res, unsigned* out_bytes, char* why, size_t why_len);
