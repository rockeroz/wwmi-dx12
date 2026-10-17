#pragma once
#include <cstdint>
#include <unordered_map>
#include <reshade.hpp>
#include "dxil_psv.h"

// The shader hashes of a PSO (vs or cs, ps, gs; 0 = none), read without a lock -- the bind hook's path.
void PsoBoundHashes(uint64_t pso, uint64_t* vs_or_cs, uint64_t* ps, uint64_t* gs);
// The PSO's decensor-only variant, lock-free; 0 = none (yet), or its pixel shader has not drawn a character.
// The first ask queues the build.
uint64_t PsoDecensorVariant(reshade::api::device* dev, uint64_t pso);
void     PsoSetDecensorVariant(uint64_t pso, uint64_t variant);
// A claimed character draw runs on `pso`: its pixel shader is one PsoDecensorVariant answers for, from now on.
void PsoNoteCharacterDraw(uint64_t pso);
// The PSO's UID-cut variant for a back buffer of `target` (UidHide::Target), lock-free; 0 = none (yet), or the
// PSO does not draw UI text. The first ask for a target queues the build.
uint64_t PsoUidVariant(reshade::api::device* dev, uint64_t pso, uint32_t target);
void     PsoSetUidVariant(uint64_t pso, uint32_t target, uint64_t variant);
// The ps-tN its pixel shader may read, bit r for t0..t63 (DxilSrvRegs::reads), lock-free; all set when unknown.
uint64_t PipelinePsReadMask(uint64_t pso);
// A copy of the set for `pso` / stage ('p' or 'v'), under the guard; an unparsed default when unknown.
DxilSrvRegs PipelineSrvRegs(uint64_t pso, char stage);
bool        PipelineVsCbufferBytes(uint64_t pso, unsigned reg, uint32_t& bytes);
// The PSO's rasterizer cull mode, recorded at creation for every PSO; false when it stated none.
bool PipelineCullMode(uint64_t pso, reshade::api::cull_mode* out);

bool on_create_pipeline(reshade::api::device*, reshade::api::pipeline_layout,
    uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects);
void on_init_pipeline(reshade::api::device* device, reshade::api::pipeline_layout layout,
    uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline pipeline);
void on_destroy_pipeline(reshade::api::device* device, reshade::api::pipeline pipeline);
// What the two hooks above cost since the last take: calls and the time spent in them, all threads summed.
struct PipelineHookStats {
    uint64_t created;
    uint64_t created_us;
    uint64_t created_max_us;
    uint64_t destroyed;
    uint64_t destroyed_us;
};
PipelineHookStats TakePipelineHookStats();
