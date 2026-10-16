// CPU tests for reshade_addon/rt/rt_registry.h (no D3D12). Build: g++ -std=c++20 -Wall -Wextra -Werror
// <Windows.h> without NOMINMAX defines these as macros; the header must still compile (MSVC C2589).
// The standard headers come first (some standard libraries #undef the two), as in the add-on.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <vector>
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#include "../reshade_addon/rt/rt_registry.h"
#undef min
#undef max
#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);           \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

using namespace rtm;

static BlasGeom Geom(uint64_t vb, uint64_t ib, uint32_t count, uint32_t verts = 87374)
{
    BlasGeom g;
    g.vb = vb;
    g.ib = ib;
    g.vertex_count = verts;
    g.vertex_stride = 12;
    g.vertex_format = 6;
    g.index_count = count;
    g.index_width = 4;
    return g;
}
static DrawNote Draw(uint64_t vb0, uint64_t ib_at, uint32_t count, bool identified = true, int mod = 0)
{
    DrawNote d;
    d.mod = mod;
    d.comp = 0;
    d.identified = identified;
    d.vb0 = vb0;
    d.vb0_bytes = 87374 * 12;
    d.vb0_stride = 12;
    d.ib_at = ib_at;
    d.index_count = count;
    return d;
}

static void blas_then_draw()
{
    Registry r;
    const auto ev = r.NoteBlas({ Geom(0x1000000, 0x5000, 24111), Geom(0x1000000, 0x5000 + 24111 * 4, 27075) }, 10);
    CHECK(ev.empty());
    CHECK(r.Count() == 0);
    const auto e = r.NoteDraw(Draw(0x1000000, 0x5000 + 24111 * 4, 27075), 10);
    CHECK(e.has_value());
    CHECK(e && e->is_new);
    CHECK(r.Count() == 1);
    CHECK(r.IsSkinOut(0x1000000 + 100, 4));
    CHECK(!r.IsSkinOut(0x1000000 + 87374 * 12, 4)); // one past the end
    CHECK(r.SkinOutMod(0x1000000, 87374 * 12, nullptr) == 0);
    CHECK(!r.Rings()[0].weak);
    // the same geometry again only confirms
    const auto again = r.NoteDraw(Draw(0x1000000, 0x5000 + 24111 * 4, 27075), 11);
    CHECK(again && !again->is_new);
    CHECK(r.Count() == 1);
}

static void draw_then_blas()
{
    Registry r;
    CHECK(!r.NoteDraw(Draw(0x2000000, 0x9000, 1428), 3).has_value());
    const auto ev = r.NoteBlas({ Geom(0x2000000, 0x9000, 1428) }, 4);
    CHECK(ev.size() == 1);
    CHECK(r.Count() == 1);
}

static void mismatches_do_not_learn()
{
    Registry r;
    r.NoteBlas({ Geom(0x3000000, 0xA000, 500) }, 1);
    CHECK(!r.NoteDraw(Draw(0x3000004, 0xA000, 500), 1)); // other vertex base
    CHECK(!r.NoteDraw(Draw(0x3000000, 0xA000, 501), 1)); // other count
    DrawNote d = Draw(0x3000000, 0xA000, 500);
    d.vb0_stride = 16; // a bind-pose stream, not the stride-12 positions
    CHECK(!r.NoteDraw(d, 1));
    BlasGeom t = Geom(0x4000000, 0xB000, 9);
    t.transformed = true;
    r.NoteBlas({ t }, 1);
    CHECK(!r.NoteDraw(Draw(0x4000000, 0xB000, 9), 1));
    CHECK(r.Count() == 0);
}

static void stale_sides_expire()
{
    Registry r;
    r.NoteBlas({ Geom(0x5000000, 0xC000, 30) }, 1);
    CHECK(!r.NoteDraw(Draw(0x5000000, 0xC000, 30), 1 + Registry::kKeepFrames + 5).has_value());
    CHECK(r.Count() == 0);
}

static void rings_expire_and_forget()
{
    Registry r;
    r.NoteRing(0x6000000, 1200, 2, false, 5);
    CHECK(r.Count() == 1);
    const uint32_t g = r.Gen();
    r.Forget(0x6000000 + 600, 4);
    CHECK(r.Count() == 0);
    CHECK(r.Gen() != g);
    r.NoteRing(0x7000000, 1200, 2, false, 5);
    r.Expire(5 + Registry::kRingExpire + 1);
    CHECK(r.Count() == 0);
}

static void weak_and_identified()
{
    Registry r;
    r.NoteBlas({ Geom(0x8000000, 0xD000, 64) }, 1);
    auto w = r.NoteDraw(Draw(0x8000000, 0xD000, 64, false, 7), 1);
    CHECK(w && w->ring.weak);
    auto s = r.NoteDraw(Draw(0x8000000, 0xD000, 64, true, 3), 2);
    CHECK(s && !s->ring.weak && s->ring.mod == 3);
    // a later weak note never downgrades it
    r.NoteDraw(Draw(0x8000000, 0xD000, 64, false, 9), 3);
    CHECK(!r.Rings()[0].weak && r.Rings()[0].mod == 3);
}

static void wraparound_overlap()
{
    CHECK(!Overlaps(UINT64_MAX - 4, 100, 0, 8));
    CHECK(Overlaps(UINT64_MAX - 4, 100, UINT64_MAX - 1, 1));
    CHECK(!Overlaps(10, 0, 10, 5));
}

static void bones()
{
    std::vector<uint8_t> buf(48 * 10 + 16, 0);
    // 16 junk bytes, then 6 identity-ish matrices, then zeros
    for (int b = 0; b < 6; ++b) {
        float m[12] = { 1, 0, 0, 0.5f * b, 0, 1, 0, 0, 0, 0, 1, -2 };
        std::memcpy(buf.data() + 16 + b * 48, m, sizeof m);
    }
    const BoneRun run = BestBoneRun(buf.data(), buf.size());
    CHECK(run.count == 6 && run.offset == 16);
    float bad[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 }; // rows 0 and 1 parallel
    CHECK(!LooksAffine3x4(bad));
    std::vector<uint8_t> a(64, 1), b2(64, 1);
    b2[5] = 2;
    b2[63] = 9;
    CHECK(ChangedWords(a, b2) == 2);
}

static void judge()
{
    CsJudge j;
    // an unknown shader: kPerFrame dispatches per frame, the window moving through the frame's dispatch order
    CHECK(j.ShouldInspect(0xAA, 1, 1000));  // idx 0
    CHECK(j.ShouldInspect(0xAA, 1, 1000));  // idx 1
    CHECK(!j.ShouldInspect(0xAA, 1, 1000)); // idx 2
    CHECK(!j.ShouldInspect(0xAA, 1, 1001)); // next frame: window 2..3
    CHECK(!j.ShouldInspect(0xAA, 1, 1001));
    CHECK(j.ShouldInspect(0xAA, 1, 1001));
    // the 7th dispatch of every frame (idx 6) is reached by frame since+3
    bool reached = false;
    for (uint32_t f = 1002; f < 1000 + CsJudge::kJudgeFrames && !reached; ++f)
        for (uint32_t i = 0; i < 8; ++i) {
            const bool look = j.ShouldInspect(0xAA, 1, f);
            if (look && i == 6) reached = true;
        }
    CHECK(reached);
    CHECK(!j.ShouldInspect(0xAA, 1, 1000 + CsJudge::kJudgeFrames));
    CHECK(j.ShouldInspect(0xAA, 2, 1000 + CsJudge::kJudgeFrames)); // rings changed: judged again
    j.Verdict(0xAA, true);
    // a ring shader: right after the change, then only on observation frames
    CHECK(j.ShouldInspect(0xAA, 2, 1001 + CsJudge::kJudgeFrames));
    CHECK(!j.ShouldInspect(0xAA, 2, 1500));
    CHECK(j.ShouldInspect(0xAA, 2, 1800));
    CHECK(j.ShouldInspect(0xAA, 2, 1801));
    CHECK(j.ShouldInspect(0xAA, 2, 1830));
    CHECK(!j.ShouldInspect(0xAA, 2, 1831));
    CHECK(j.SkinShaders() == 1);
    CHECK(IsObservationFrame(3600) && !IsObservationFrame(3602));
}

static void signatures()
{
    // the Hsin body windows from the 0.1.2 capture: 8 geometries over one vertex base
    const uint32_t first[8] = { 0, 24111, 51186, 68460, 134256, 135684, 209574, 247680 };
    const uint32_t count[8] = { 24111, 27075, 17274, 65796, 1428, 73890, 38106, 107763 };
    std::vector<CompWindow> comps;
    for (int i = 0; i < 8; ++i) comps.push_back({ 0, i, first[i], count[i] });
    comps.push_back({ 1, 0, 0, 20496 }); // another mod: a lone accessory
    std::vector<BlasGeom> geoms;
    const uint64_t        ib = 0x1c578d3200;
    for (int i = 0; i < 8; ++i) geoms.push_back(Geom(0x1d20c00000, ib + uint64_t(first[i]) * 4, count[i]));
    auto hits = MatchSignatures(geoms, comps);
    CHECK(hits.size() == 1 && hits[0].mod == 0 && hits[0].geometries == 8 && hits[0].base_first == 0);
    // the same build starting at another first index (the IB start not at component 0): still found
    geoms.erase(geoms.begin());
    hits = MatchSignatures(geoms, comps);
    CHECK(hits.size() == 1 && hits[0].mod == 0 && hits[0].base_first == 24111);
    // one count off: refused
    geoms[2].index_count += 3;
    CHECK(MatchSignatures(geoms, comps).empty());
    // a lone geometry with a unique count
    hits = MatchSignatures({ Geom(0x1d20e00000, 0xc188100, 20496, 5495) }, comps);
    CHECK(hits.size() == 1 && hits[0].mod == 1);
    // a lone geometry whose count two components share: refused
    comps.push_back({ 2, 0, 500, 20496 });
    CHECK(MatchSignatures({ Geom(0x1d20e00000, 0xc188100, 20496, 5495) }, comps).empty());
    // the registry remembers vertex bases
    Registry r;
    r.NoteBlas({ Geom(0x9000000, 0x100, 30) }, 5);
    CHECK(r.HasGeomVb(0x9000000) && !r.HasGeomVb(0x9000004));
    r.Expire(5 + Registry::kKeepFrames + 1);
    CHECK(!r.HasGeomVb(0x9000000));
}

int main()
{
    signatures();
    blas_then_draw();
    draw_then_blas();
    mismatches_do_not_learn();
    stale_sides_expire();
    rings_expire_and_forget();
    weak_and_identified();
    wraparound_overlap();
    bones();
    judge();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("rt_registry_test: PASS\n");
    return 0;
}
