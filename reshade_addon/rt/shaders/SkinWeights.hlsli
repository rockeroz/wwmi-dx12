// Shared with the CPU regression test. The accumulated transform already uses
// weights divided by 255; normalise that sum without blending in the bind pose.
float RtSkinWeightScale(uint sum)
{
    return sum ? 255.0f / float(sum) : 0.0f;
}
