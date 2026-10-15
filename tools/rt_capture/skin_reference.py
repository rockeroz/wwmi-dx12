"""CPU oracle for the canonical palette accepted by SkinModPositions.hlsl.

This does not infer a game's native bone layout or perform shape-key evaluation.
"""
import math


def skin_position(position, blend, bones):
    if len(position) != 3 or not all(math.isfinite(v) for v in position):
        raise ValueError("Position must be a finite FLOAT3")
    if len(blend) not in (8, 16):
        raise ValueError("Expected 4 or 8 packed byte influences")
    count = len(blend) // 2
    weights = blend[count:]
    if sum(weights) != 255:
        raise ValueError("UNORM8 weights must sum to 255")
    output = [0.0, 0.0, 0.0]
    p = (*position, 1.0)
    for index, weight in zip(blend[:count], weights):
        if not weight:
            continue
        if index >= len(bones):
            raise ValueError("Active bone index exceeds the canonical palette")
        rows = bones[index]
        if len(rows) != 3 or any(len(row) != 4 for row in rows):
            raise ValueError("Expected three affine float4 rows per bone")
        for axis in range(3):
            output[axis] += sum(a * b for a, b in zip(rows[axis], p)) * weight / 255.0
    if not all(math.isfinite(v) for v in output):
        raise ValueError("Skinned position is not finite")
    return tuple(output)
