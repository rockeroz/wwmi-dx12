#include "../reshade_addon/rt/rt_build_contract.h"
#include "../reshade_addon/rt/rt_record_plan.h"
#include "../reshade_addon/hooks/compute_root_state.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
using uint = unsigned int;
#include "../reshade_addon/rt/shaders/SkinWeights.hlsli"

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main()
{
    // A failed candidate is never committed: the retry must still be a build.
    rtm::BuildContract state;
    std::vector<rtm::GeometryContract> a{ { 1, 100, 300, 42, 6, 12, 0x1000, false } };
    Check(state.NeedsBuild(a, false, 0), "first generation builds");
    state.Commit(a);
    Check(!state.NeedsBuild(a, false, 1), "unchanged generation can update");
    auto b = a;
    b[0].vertices = 200;
    Check(state.NeedsBuild(b, false, 1), "VertexCount change rebuilds");
    // Simulated allocation failure: retain the old published contract.
    Check(state.geometries == a && state.NeedsBuild(b, false, 1), "retry after allocation failure still rebuilds");
    b = a; b[0].flags = 2;
    Check(state.NeedsBuild(b, false, 1), "geometry flags change rebuilds");
    b = a; b[0].index_generation++;
    Check(state.NeedsBuild(b, false, 1), "new index contents rebuild");
    Check(state.NeedsBuild(a, true, 1), "explicit build honored");
    Check(state.NeedsBuild(a, false, 300), "periodic rebuild honored");

    rtm::RecordPlan plan;
    Check(plan.Reserve(3, 6, 100, 30), "first instance reserves its records");
    Check(plan.Reserve(3, 6, 100, 30), "second instance may share the same BLAS records");
    Check(!plan.Reserve(3, 6, 200, 30), "same records cannot name a different BLAS");
    Check(!plan.Reserve(6, 6, 100, 30), "partially overlapping records refused");
    Check(!plan.Contains(9), "failed reservation leaves no partial plan");
    Check(plan.Contains(3) && plan.Contains(8), "native instance sharing modified records is detected");
    Check(!plan.Reserve(UINT64_MAX, 2, 100, 30), "bounds check cannot wrap");
    Check(!plan.Reserve(29, 2, 100, 30), "last record bounds checked");

    ComputeRootState roots;
    roots.Signature(10);
    roots.Descriptor(0, ComputeRootState::Kind::table, 0x1000);
    roots.Descriptor(1, ComputeRootState::Kind::acceleration_structure, 0x2000);
    const uint32_t constants[] = { 7, 8 };
    roots.Constants(2, 3, 2, constants);
    const uint32_t replacement = 9;
    roots.Constants(2, 4, 1, &replacement);
    Check(roots.args[2].defined == ((1ull << 3) | (1ull << 4)), "only defined constants restored");
    Check(roots.args[2].words[3] == 7 && roots.args[2].words[4] == 9, "partial constant updates retained");
    roots.Signature(10);
    Check(roots.args[1].value == 0x2000, "same signature preserves roots");
    roots.Signature(11);
    Check(roots.args[1].kind == ComputeRootState::Kind::none && roots.args[2].defined == 0, "new signature invalidates all old roots");
    roots.Constants(1, 63, 2, constants);
    Check(!roots.complete, "out-of-range root state prevents injection");
    roots = {};
    Check(roots.layout == 0 && roots.complete, "reset discards the prior recording");

    for (uint weight : { 1u, 128u, 254u, 255u }) {
        const float weighted_identity = 10.0f * float(weight) / 255.0f;
        const float weighted_translation = 16.0f * float(weight) / 255.0f;
        Check(std::abs(weighted_identity * RtSkinWeightScale(weight) - 10.0f) < 0.0001f, "identity preserved for partial weights");
        Check(std::abs(weighted_translation * RtSkinWeightScale(weight) - 16.0f) < 0.0001f, "translation normalised with partial weights");
    }
    Check(RtSkinWeightScale(0) == 0 && std::isfinite(RtSkinWeightScale(0)), "zero weights finite");
    std::cout << "rt_safety_test: PASS\n";
}
