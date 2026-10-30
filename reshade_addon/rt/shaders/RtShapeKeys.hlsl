// RtShapeKeys -- WWMI shape keys for the RT module's mod poses (M2).
//
// With ray tracing on, Wuthering Waves never runs its shape key compute (the dispatches WWMI hooks: ShapeKeyLoaderCS
// 9bf4420c82102011 and ShapeKeyMultiplierCS 7a8396180d416117), so WWMI's chain never writes a mod's per-vertex deltas.
// This kernel computes them from the mod's own shape key data, the way ShapeKeyLoader.hlsl + ShapeKeyMultiplier.hlsl
// accumulate them, with the values WWMI's ShapeKeySetter wrote into the mod's CustomShapeKeyValuesRW (each frame, from
// the mod's [Present]):
//   delta(v) = sum over the shape keys k touching v of weight(k) * offset(k, v)
// weight(k) = CustomShapeKeyValuesRW[k] - 1000000 where the mod overrides it (non-zero), else 0: the game's own weights
// (expressions) come from the dispatch that does not run, so a shape key the mod does not set stays at rest.
//
// Three passes over one root signature, picked by Pass:
//   0  clear the fixed-point accumulator (6 ints a vertex)
//   1  one thread per shape key entry: InterlockedAdd of weight * offset into its vertex
//   2  the accumulator as floats (position delta xyz, normal delta xyz: 24 bytes a vertex, RtSkinMod's t6)
//
//   t0 Offsets        128 uints: shape key k's entries are [Offsets[k], Offsets[k + 1]) (ShapeKeyOffset.buf, 1 batch)
//   t1 VertexIds      one uint per entry: the mod vertex it moves (ShapeKeyVertexId.buf)
//   t2 VertexOffsets  six R16_FLOAT per entry: position xyz, normal xyz (ShapeKeyVertexOffset.buf)
//   t3 Values         128 floats (CustomShapeKeyValuesRW, 32 float4)
//   u0 Accum          6 ints per vertex
//   u1 Morphs         6 floats per vertex
#define RtShapeKeysRS \
    "RootConstants(num32BitConstants=4, b0)," \
    "SRV(t0), SRV(t1), SRV(t2), SRV(t3)," \
    "UAV(u0), UAV(u1)"

cbuffer Parameters : register(b0)
{
    uint Pass;
    uint EntryCount;
    uint VertexCount;
    uint Pad0;
};

ByteAddressBuffer   Offsets       : register(t0);
ByteAddressBuffer   VertexIds     : register(t1);
ByteAddressBuffer   VertexOffsets : register(t2);
ByteAddressBuffer   Values        : register(t3);
RWByteAddressBuffer Accum         : register(u0);
RWByteAddressBuffer Morphs        : register(u1);

static const float kScale = 65536.0; // fixed point: +-32768 units of offset * weight, 1/65536 resolution

uint Offset(uint k)
{
    return Offsets.Load(min(k, 127u) * 4u);
}

float Half(uint index)
{
    const uint word = VertexOffsets.Load((index * 2u) & ~3u);
    return f16tof32((index & 1u) ? (word >> 16) : (word & 0xffffu));
}

[RootSignature(RtShapeKeysRS)]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (Pass == 0u) {
        if (i < VertexCount * 6u) Accum.Store(i * 4u, 0u);
        return;
    }
    if (Pass == 2u) {
        if (i < VertexCount * 6u) {
            const float f = float(asint(Accum.Load(i * 4u))) / kScale;
            Morphs.Store(i * 4u, asuint(isfinite(f) ? f : 0.0));
        }
        return;
    }
    if (i >= EntryCount) return;
    // the shape key this entry belongs to: the last k whose first entry is at or before it
    uint k = 0;
    [unroll]
    for (uint step = 64u; step; step >>= 1)
        if (Offset(k + step) <= i) k += step;
    if (k >= 127u || i >= Offset(k + 1u)) return;
    const float c = asfloat(Values.Load(k * 4u));
    if (c == 0.0) return; // not overridden by the mod: the game's weight, unknown here (0)
    const float w = c - 1000000.0;
    if (w == 0.0 || !isfinite(w)) return;
    const uint v = VertexIds.Load(i * 4u);
    if (v >= VertexCount) return;
    [unroll]
    for (uint j = 0; j < 6u; ++j) {
        const float d = Half(i * 6u + j) * w * kScale;
        if (isfinite(d) && d != 0.0) {
            int ignored;
            Accum.InterlockedAdd((v * 6u + j) * 4u, int(round(clamp(d, -2147483000.0, 2147483000.0))), ignored);
        }
    }
}
