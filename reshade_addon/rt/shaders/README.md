# Canonical mod skinning preparation

`SkinModPositions.hlsl` is a standalone compute kernel. It is **not wired into
the add-on**, does not enable `RtModulePresent`, and is not evidence of working
native RT mesh replacement.

It accepts FLOAT3 positions after local shape-key evaluation, WWMI byte-packed
indices/weights (four or eight influences), and a canonical row-major affine
palette with three float4 rows per bone. That palette format matches the
existing raster skeleton module's representation. A game's native RT palette
still requires a verified adapter; no native RT offsets are assumed here.

The kernel writes tightly packed FLOAT3 positions and one status uint per
vertex. It checks dimensions, finite values, bone bounds and weights summing
to 255. The caller must validate every status, manage states/barriers and retain
input/output resources until the submission fence completes before publishing
the positions to a BLAS. Output allocation uses the position buffer's byte
length divided by twelve, not an unchecked INI vertex-count declaration.

Compile independently, without installing it into the game:

```sh
dxc -T cs_6_0 -E main -O3 -WX -Fo SkinModPositions.cso SkinModPositions.hlsl
```

`tools/rt_capture/skin_reference.py` supplies a CPU oracle. Tests cover four/eight
influences, weighted affine rotation/translation, unused indices, invalid active
bones and invalid weights. CPU tests and shader compilation do not validate GPU
execution, the native palette adapter, morphs, materials, BLAS/TLAS redirection
or synchronization in the game.
