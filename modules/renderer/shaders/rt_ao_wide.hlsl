// The RT AO's wide pass, after the denoiser (core/rt.cpp).
//
// ---------------------------------------------------------------------------
// The AO's WIDE pass, after its denoise. Where the history was cut - a ray hit
// a moved object (0.25 of it, so the car leaves no trail) or none was found
// (disoccluded, behind the car) - the answer is little more than one frame's
// 5x5, and the ground around a moving car boiled. So there, and only there,
// the denoised result is averaged again over a 13x13 footprint: 5x5 taps 3
// pixels apart, the same depth and identity rules, static pixels only. It is
// spatial, so it cannot trail; it blurs only what had no history to lose.
// Not fed back: the history stays the 5x5's, so the next frame's clamp and
// blend see what they always did.
// ---------------------------------------------------------------------------

Texture2D<float4>  gIn   : register(t1);   // this frame's denoised AO (kDenoiseSrc's layout)
RWTexture2D<float> gWide : register(u0);

float Slope(float c, float m, float p)       // as kDenoiseSrc's
{
    const float a = p - c, b = c - m;
    const float g = (abs(a) < abs(b)) ? a : b;
    return (abs(g) < 0.25 * c) ? g : 0.0;
}

cbuffer K : register(b0)
{
    row_major float4x4 gReproj;
    uint2  gSize;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int2 px = int2(tid.xy);
    const float4 c = gIn.Load(int3(px, 0));
    // Static, with a cut or no history: the rest keep their own answer. A
    // slow occluder's pixels too are widened unless their rays allowed all of
    // the history (w 0.45, Allow): under the Garage's turning car they keep
    // 0.7-0.85 of it, a third of what a still pixel keeps (gBlendStill), and
    // were the grainiest floor there - widened, smoother than the car standing.
    //
    // And a moved object's pixels while their history is young (w < 1, its
    // youth, kDenoiseSrc): the car come out from behind a panel had a single
    // frame's 5x5 of the sky's occlusion, then a few frames' - the whole side
    // of it blotchy. Widened over its own pixels only, less as it ages.
    const bool moved = c.w > 0.5;
    const float youth = moved ? (1.0 - c.w) * 4.0 : 1.0;     // 1 fresh .. 0 grown up
    if (c.y <= 0.0 || (moved ? youth <= 0.0 : (c.z < 0.25 || c.w > 0.44))) { gWide[px] = c.x; return; }
    const float2 slope = float2(Slope(c.y, gIn.Load(int3(px - int2(1, 0), 0)).y, gIn.Load(int3(px + int2(1, 0), 0)).y),
                                Slope(c.y, gIn.Load(int3(px - int2(0, 1), 0)).y, gIn.Load(int3(px + int2(0, 1), 0)).y));
    float sum = c.x, wsum = 1.0;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        if (x == 0 && y == 0) continue;
        const int2 q = px + int2(x, y) * 3;
        if (any(q < 0) || any(q >= int2(gSize))) continue;
        const float4 h = gIn.Load(int3(q, 0));
        if (h.y <= 0.0 || (h.w > 0.5) != moved) continue;
        const float e = max(c.y + dot(slope, float2(x, y) * 3.0), 1e-7);
        if (abs(1.0 / h.y - 1.0 / e) * e >= 0.03) continue;
        sum += h.x; wsum += 1.0;
    }
    gWide[px] = lerp(c.x, sum / wsum, youth);
}
