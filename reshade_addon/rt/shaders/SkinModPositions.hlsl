// SPDX-License-Identifier: GPL-3.0-only
// Standalone preparation kernel, NOT connected to the game/BLAS hooks yet.
// Input positions must already include the current frame's local-space shape keys.
// BoneRows is a CANONICAL palette: three float4 affine rows per bone. The native
// RT palette must be decoded/verified before it is adapted to this representation.
cbuffer Parameters : register(b0)
{
    uint VertexCount;
    uint BoneCount;
    uint InfluenceCount; // 4 or 8; indices first, then UNORM8 weights
    uint BlendStride;    // 8 or 16 bytes respectively
};
ByteAddressBuffer Positions : register(t0); // tightly packed FLOAT3, 12 bytes
ByteAddressBuffer Blends : register(t1);
StructuredBuffer<float4> BoneRows : register(t2);
RWByteAddressBuffer SkinnedPositions : register(u0); // tightly packed FLOAT3 for a future BLAS
RWByteAddressBuffer VertexStatus : register(u1); // one uint per vertex; every status must be zero before publication

uint LoadBlendByte(uint offset)
{
    return (Blends.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 255u;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint v = tid.x;
    if (v >= VertexCount) return;
    uint positionBytes, blendBytes, outputBytes, statusBytes, rows, rowStride;
    Positions.GetDimensions(positionBytes);
    Blends.GetDimensions(blendBytes);
    SkinnedPositions.GetDimensions(outputBytes);
    VertexStatus.GetDimensions(statusBytes);
    BoneRows.GetDimensions(rows, rowStride);
    if (v >= statusBytes / 4u) return; // host must verify output capacities before dispatch
    if ((InfluenceCount != 4u && InfluenceCount != 8u) || BlendStride != InfluenceCount * 2u ||
        !BoneCount || BoneCount > rows / 3u || v >= positionBytes / 12u ||
        v >= blendBytes / max(BlendStride, 1u) || v >= outputBytes / 12u)
    {
        VertexStatus.Store(v * 4u, 1u); // invalid layout/capacity; output is not publishable
        return;
    }
    const float3 p = asfloat(Positions.Load3(v * 12u));
    uint status = all(isfinite(p)) ? 0u : 2u;
    uint sum = 0;
    float3 skinned = 0;
    [unroll(8)]
    for (uint i = 0; i < 8u; ++i)
    {
        if (i >= InfluenceCount) break;
        const uint weight = LoadBlendByte(v * BlendStride + InfluenceCount + i);
        sum += weight;
        if (!weight) continue; // unused bone indices are not dereferenced
        const uint bone = LoadBlendByte(v * BlendStride + i);
        if (bone >= BoneCount) { status |= 4u; continue; }
        const float4 homogeneous = float4(p, 1.0);
        const float3 transformed = float3(dot(BoneRows[bone * 3u], homogeneous),
            dot(BoneRows[bone * 3u + 1u], homogeneous), dot(BoneRows[bone * 3u + 2u], homogeneous));
        skinned += transformed * (float(weight) / 255.0);
    }
    if (sum != 255u) status |= 8u;
    if (!all(isfinite(skinned))) status |= 16u;
    SkinnedPositions.Store3(v * 12u, asuint(status ? float3(0, 0, 0) : skinned));
    VertexStatus.Store(v * 4u, status);
}
