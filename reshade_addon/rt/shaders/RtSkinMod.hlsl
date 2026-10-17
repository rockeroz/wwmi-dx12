#include "SkinWeights.hlsli"
// RtSkinMod -- the RT module's mod skinning (M2). With ray tracing on, Wuthering Waves poses characters in UE's GPU
// skin cache (SkinCacheUpdateBatchCS, one dispatch per mesh section, each with its own bone palette) and draws them
// through a pass-through vertex factory that reads posed positions (IA slot 0) and fetches the rest by vertex id.
// This kernel poses a mod component the same way: every vertex referenced by one sub-draw's index range, skinned
// with the palette of the native section the component replaces (the mod's bone indices are local to it).
//
// One thread per index. A vertex referenced by several indices is written several times with the same value.
//
// Inputs are root descriptors only (no descriptor heap is touched on the game's list):
//   t0 Indices   R32 indices of the mod (its Index.buf)
//   t1 Positions FLOAT3 per vertex (Position.buf, bind pose in the mesh's local space)
//   t2 Blends    InfluenceCount bone indices (bytes) then InfluenceCount UNORM8 weights (Blend.buf, stride 8 or 16)
//   t3 Vectors   two R8G8B8A8_SNORM per vertex (Vector.buf): tangent, then normal with the bitangent sign in w
//                (VectorFlags bit 0 swaps the two)
//   t4 Palette   the native section's bone matrices: 3 float4 rows per bone, translation in w (a typed view in the
//                frame's upload buffer; the root SRV points at the view's first element)
//   u0 OutPositions FLOAT3 per vertex
//   u1 OutTangents  two R16G16B16A16_SNORM per vertex (TangentX, TangentZ.w = sign), the skin cache's own format
#define RtSkinModRS \
    "RootConstants(num32BitConstants=12, b0)," \
    "SRV(t0), SRV(t1), SRV(t2), SRV(t3), SRV(t4)," \
    "UAV(u0), UAV(u1)"

cbuffer Parameters : register(b0)
{
    uint IndexStart;     // first index of the sub-draw
    uint IndexCount;     // indices to process
    uint VertexCount;    // vertices in the mod buffers (bounds)
    uint BoneCount;      // bones in the palette view
    uint InfluenceCount; // 4 or 8
    uint BlendStride;    // InfluenceCount * 2
    uint VectorFlags;    // bit 0: normal first
    uint OutPosOffset;   // byte offset of this list's region in u0
    uint OutTanOffset;   // byte offset of this list's region in u1
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

ByteAddressBuffer   Indices      : register(t0);
ByteAddressBuffer   Positions    : register(t1);
ByteAddressBuffer   Blends       : register(t2);
ByteAddressBuffer   Vectors      : register(t3);
ByteAddressBuffer   Palette      : register(t4);
RWByteAddressBuffer OutPositions : register(u0);
RWByteAddressBuffer OutTangents  : register(u1);

uint BlendByte(uint offset)
{
    return (Blends.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 255u;
}

float4 Snorm8x4(uint packed)
{
    const int4 v = int4(int(packed << 24) >> 24, int(packed << 16) >> 24, int(packed << 8) >> 24, int(packed) >> 24);
    return max(float4(v) / 127.0, -1.0);
}

uint2 Snorm16x4(float4 v)
{
    const int4 q = int4(round(clamp(v, -1.0, 1.0) * 32767.0));
    return uint2((uint(q.x) & 0xffffu) | (uint(q.y) << 16), (uint(q.z) & 0xffffu) | (uint(q.w) << 16));
}

[RootSignature(RtSkinModRS)]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= IndexCount) return;
    const uint v = Indices.Load((IndexStart + tid.x) * 4u);
    if (v >= VertexCount) return;

    // the blended bone matrix: sum of weight * rows
    float4 r0 = 0, r1 = 0, r2 = 0;
    uint   sum = 0;
    [unroll(8)]
    for (uint i = 0; i < 8u; ++i) {
        if (i >= InfluenceCount) break;
        const uint w = BlendByte(v * BlendStride + InfluenceCount + i);
        if (!w) continue;
        const uint b = BlendByte(v * BlendStride + i);
        if (b >= BoneCount) continue; // a bone the palette does not have: skipped, not read out of bounds
        const float f = float(w) / 255.0;
        r0 += asfloat(Palette.Load4(b * 48u)) * f;
        r1 += asfloat(Palette.Load4(b * 48u + 16u)) * f;
        r2 += asfloat(Palette.Load4(b * 48u + 32u)) * f;
        sum += w;
    }
    const float3 p = asfloat(Positions.Load3(v * 12u));
    float3       q = p;
    if (sum) {
        const float4 h = float4(p, 1.0);
        q = float3(dot(r0, h), dot(r1, h), dot(r2, h));
        if (sum != 255u) q *= RtSkinWeightScale(sum); // normalise the accumulated transform
    }
    if (!all(isfinite(q))) q = p;
    OutPositions.Store3(OutPosOffset + v * 12u, asuint(q));

    const uint2  vec = Vectors.Load2(v * 8u);
    const float4 a = Snorm8x4((VectorFlags & 1u) ? vec.y : vec.x); // tangent
    const float4 n = Snorm8x4((VectorFlags & 1u) ? vec.x : vec.y); // normal, w = bitangent sign
    float3       t = a.xyz, z = n.xyz;
    if (sum) {
        t = float3(dot(r0.xyz, a.xyz), dot(r1.xyz, a.xyz), dot(r2.xyz, a.xyz));
        z = float3(dot(r0.xyz, n.xyz), dot(r1.xyz, n.xyz), dot(r2.xyz, n.xyz));
    }
    t = normalize(t + 1e-12);
    z = normalize(z + 1e-12);
    const uint2 tx = Snorm16x4(float4(t, 0.0));
    const uint2 tz = Snorm16x4(float4(z, n.w < 0.0 ? -1.0 : 1.0));
    OutTangents.Store4(OutTanOffset + v * 16u, uint4(tx, tz));
}
