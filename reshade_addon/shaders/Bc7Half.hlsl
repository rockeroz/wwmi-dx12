// Bc7Half -- a smaller BC7 copy of a BC7 texture, 2^shift times smaller on each side. One thread per output block:
// its texels averaged 2^shift x 2^shift, encoded in mode 6; each group adds the squared error against that average
// to its slot.
cbuffer C : register(b0)
{
    uint blocks_x;
    uint blocks_y;
    uint row_pitch;
    uint slot;
    uint srgb;
    uint shift;
    uint pad0;
    uint pad1;
}
Texture2D<float4>   src : register(t0);
RWByteAddressBuffer dst : register(u0);
RWByteAddressBuffer err : register(u1);

groupshared uint g_rgb[64];
groupshared uint g_a[64];

static const uint kWeight[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

float SrgbOf(float c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

void Quantize(float4 e, out uint4 q, out uint p)
{
    q = uint4(0, 0, 0, 0);
    p = 0;
    float best = 1e30;
    [unroll] for (uint pb = 0; pb < 2; ++pb) {
        const uint4  qq = (uint4)clamp(round((e - pb) * 0.5), 0.0, 127.0);
        const float4 d = float4(qq * 2 + pb) - e;
        const float  s = dot(d, d);
        if (s < best) {
            best = s;
            q = qq;
            p = pb;
        }
    }
}

uint4 Interpolated(uint4 e0, uint4 e1, uint k)
{
    return ((64 - kWeight[k]) * e0 + kWeight[k] * e1 + 32) >> 6;
}

void Put(inout uint b[4], uint pos, uint bits, uint v)
{
    const uint w = pos >> 5;
    const uint o = pos & 31;
    b[w] |= v << o;
    if (o + bits > 32) b[w + 1] |= v >> (32 - o);
}

void Add64(uint off, uint v)
{
    uint prev;
    err.InterlockedAdd(off, v, prev);
    if (prev + v < prev) err.InterlockedAdd(off + 4, 1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint e_rgb = 0;
    uint e_a = 0;
    if (id.x < blocks_x && id.y < blocks_y) {
        float4      px[16];
        float4      mean = 0;
        float4      lo = 255;
        float4      hi = 0;
        const uint  f = 1u << shift;
        const float inv = 1.0 / (float)(f * f);
        const int2  base = int2(id.xy) * (int)(4 * f);
        [unroll] for (uint i = 0; i < 16; ++i) {
            const int2 p = base + int2(i & 3, i >> 2) * (int)f;
            float4     s = 0;
            for (uint y = 0; y < f; ++y)
                for (uint x = 0; x < f; ++x) s += src.Load(int3(p + int2(x, y), 0));
            s *= inv;
            if (srgb) s.rgb = float3(SrgbOf(s.r), SrgbOf(s.g), SrgbOf(s.b));
            px[i] = round(saturate(s) * 255.0);
            mean += px[i];
            lo = min(lo, px[i]);
            hi = max(hi, px[i]);
        }
        mean *= 1.0 / 16.0;

        float4 c0 = 0;
        float4 c1 = 0;
        float4 c2 = 0;
        float4 c3 = 0;
        [unroll] for (uint i = 0; i < 16; ++i) {
            const float4 d = px[i] - mean;
            c0 += d.x * d;
            c1 += d.y * d;
            c2 += d.z * d;
            c3 += d.w * d;
        }
        float4 axis = hi - lo;
        [unroll] for (uint k = 0; k < 4; ++k) {
            const float4 n = float4(dot(c0, axis), dot(c1, axis), dot(c2, axis), dot(c3, axis));
            const float  l = dot(n, n);
            if (l > 1e-12) axis = n * rsqrt(l);
        }
        float tmin = 1e30;
        float tmax = -1e30;
        [unroll] for (uint i = 0; i < 16; ++i) {
            const float t = dot(px[i] - mean, axis);
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
        float4 ep0 = clamp(mean + axis * tmin, 0.0, 255.0);
        float4 ep1 = clamp(mean + axis * tmax, 0.0, 255.0);

        uint4 q0;
        uint4 q1;
        uint  p0;
        uint  p1;
        uint4 e0;
        uint4 e1;
        uint  idx[16];
        [unroll] for (uint r = 0; r < 2; ++r) {
            Quantize(ep0, q0, p0);
            Quantize(ep1, q1, p1);
            e0 = q0 * 2 + p0;
            e1 = q1 * 2 + p1;
            const float4 dir = float4(e1) - float4(e0);
            const float  dd = dot(dir, dir);
            float        a = 0;
            float        b = 0;
            float        c = 0;
            float4       x0 = 0;
            float4       x1 = 0;
            [unroll] for (uint i = 0; i < 16; ++i) {
                const float t = dd > 0 ? dot(px[i] - float4(e0), dir) / dd : 0;
                const int   j = clamp((int)round(t * 15.0), 0, 15);
                uint        best_k = (uint)j;
                float       best_e = 1e30;
                for (int k = max(j - 1, 0); k <= min(j + 1, 15); ++k) {
                    const float4 d = float4(Interpolated(e0, e1, (uint)k)) - px[i];
                    const float  e = dot(d, d);
                    if (e < best_e) {
                        best_e = e;
                        best_k = (uint)k;
                    }
                }
                idx[i] = best_k;
                const float w = kWeight[best_k] / 64.0;
                a += (1 - w) * (1 - w);
                b += w * (1 - w);
                c += w * w;
                x0 += (1 - w) * px[i];
                x1 += w * px[i];
            }
            const float det = a * c - b * b;
            if (r == 0 && abs(det) > 1e-6) {
                ep0 = clamp((c * x0 - b * x1) / det, 0.0, 255.0);
                ep1 = clamp((a * x1 - b * x0) / det, 0.0, 255.0);
            }
        }

        if (idx[0] >= 8) {
            const uint4 tq = q0;
            const uint  tp = p0;
            const uint4 te = e0;
            q0 = q1;
            p0 = p1;
            e0 = e1;
            q1 = tq;
            p1 = tp;
            e1 = te;
            [unroll] for (uint i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
        }

        float s_rgb = 0;
        float s_a = 0;
        [unroll] for (uint i = 0; i < 16; ++i) {
            const float4 d = float4(Interpolated(e0, e1, idx[i])) - px[i];
            s_rgb += dot(d.rgb, d.rgb);
            s_a += d.a * d.a;
        }
        e_rgb = (uint)s_rgb;
        e_a = (uint)s_a;

        uint bits[4] = { 0, 0, 0, 0 };
        Put(bits, 0, 7, 0x40);
        Put(bits, 7, 7, q0.r);
        Put(bits, 14, 7, q1.r);
        Put(bits, 21, 7, q0.g);
        Put(bits, 28, 7, q1.g);
        Put(bits, 35, 7, q0.b);
        Put(bits, 42, 7, q1.b);
        Put(bits, 49, 7, q0.a);
        Put(bits, 56, 7, q1.a);
        Put(bits, 63, 1, p0);
        Put(bits, 64, 1, p1);
        Put(bits, 65, 3, idx[0]);
        [unroll] for (uint i = 1; i < 16; ++i) Put(bits, 68 + (i - 1) * 4, 4, idx[i]);
        dst.Store4(id.y * row_pitch + id.x * 16, uint4(bits[0], bits[1], bits[2], bits[3]));
    }

    g_rgb[gi] = e_rgb;
    g_a[gi] = e_a;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1) {
        if (gi < s) {
            g_rgb[gi] += g_rgb[gi + s];
            g_a[gi] += g_a[gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) {
        Add64(slot * 16, g_rgb[0]);
        Add64(slot * 16 + 8, g_a[0]);
    }
}
