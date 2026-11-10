// Auto exposure: the luminance histogram and the adaptation (core/post.cpp).
//
// ---------------------------------------------------------------------------
// Auto exposure: a log-luminance histogram, then a percentile mean adapted over
// time. Two compute passes, one command list, no CPU in the loop.
//
// A HISTOGRAM rather than a log-average of the whole image, because the mean
// is dominated by exactly the pixels metering should ignore: the sun disc and
// the glare around it pull a plain log-average up, and a strip of deep shadow
// pulls it down. The histogram lets both tails be dropped by percentile.
// ---------------------------------------------------------------------------

cbuffer Meter : register(b0)
{
    uint2  gSize;           // full image size
    float  gMinLog2;        // lower edge of bin 1; anything darker is "black"
    float  gLog2Range;      // bins 1..N-1 span [gMinLog2, gMinLog2 + range]
    float  gDt;             // seconds since the last frame
    float  gSpeedBright;    // adaptation rate when the scene got BRIGHTER
    float  gSpeedDark;      // ... and DARKER
    float  gExposure;       // the dial: exposure at reference brightness
    float  gRefLog2;        // reference brightness
    float  gEvDown;         // most the meter may take OFF the dial, in EV
    float  gEvUp;           // most it may ADD
    float  gLowPct;         // percentile window of the mean
    float  gHighPct;
    float  gSnap;           // 1 = no adaptation this frame
    float  gNightKnee;      // EV darker than the reference made up in full (design 5.4, N1c)
    float  gNightSlope;     // the share of the rest made up
};

#define BINS 128

Texture2D                 gImage : register(t0);
RWByteAddressBuffer       gHist  : register(u0);
RWStructuredBuffer<float> gState : register(u1);

groupshared uint gsHist[BINS];

float LumaOf(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// One thread per 2x2 block, sampling one pixel of it. Metering is a statistic
// over a million pixels; a quarter of them gives the same answer for a quarter
// of the bandwidth.
[numthreads(16, 16, 1)]
void CSHistogram(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < BINS) gsHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint2 px = id.xy * 2;
    if (all(px < gSize))
    {
        const float l = LumaOf(gImage.Load(int3(px, 0)).rgb);
        if (l > exp2(gMinLog2))
        {
            const float t = saturate((log2(l) - gMinLog2) / gLog2Range);
            const uint bin = 1 + min((uint)(t * (BINS - 1)), BINS - 2);

            // Centre-weighted, 4:1. The driver looks down the road, and a
            // rally stage puts the most sky at the top edge and the most
            // bonnet-adjacent verge at the bottom corners - neither is what the
            // exposure should be chosen for. Aspect-corrected so the falloff is
            // round on screen.
            float2 d = (float2(px) + 0.5) / float2(gSize) - 0.5;
            d.x *= float(gSize.x) / float(gSize.y);
            const uint w = 1 + (uint)(3.0 * saturate(1.0 - length(d) / 0.6));
            InterlockedAdd(gsHist[bin], w);
        }
        else
        {
            InterlockedAdd(gsHist[0], 1);     // black: counted, never metered
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < BINS && gsHist[gi] != 0)
        gHist.InterlockedAdd(gi * 4, gsHist[gi]);
}

groupshared float gsWeight[BINS];

// The night's highlight protection (CSAdapt): the percentile, and how far above the metered level it may sit.
static const float kHighlightPct = 0.97;
static const float kHighlightEv = 3.0;

[numthreads(BINS, 1, 1)]
void CSAdapt(uint gi : SV_GroupIndex)
{
    const uint c = gHist.Load(gi * 4);
    gHist.Store(gi * 4, 0);                 // consumed: zero for the next frame
    gsWeight[gi] = (gi == 0) ? 0.0 : float(c);
    GroupMemoryBarrierWithGroupSync();
    if (gi != 0) return;

    float total = 0.0;
    for (uint i = 1; i < BINS; ++i) total += gsWeight[i];

    // The mean of log2 luminance over the [low, high] percentile window, with
    // the bins straddling either edge counted fractionally so the result moves
    // smoothly rather than in bin-sized steps.
    const float lo = total * gLowPct, hi = total * gHighPct;
    float acc = 0.0, sumW = 0.0, sumL = 0.0;
    for (uint j = 1; j < BINS; ++j)
    {
        const float w = gsWeight[j];
        const float inside = max(0.0, min(acc + w, hi) - max(acc, lo));
        const float binLog2 = gMinLog2 + (float(j - 1) + 0.5) / float(BINS - 1) * gLog2Range;
        sumL += inside * binLog2;
        sumW += inside;
        acc += w;
    }

    const bool valid = gState[2] > 0.5;
    // An all-black frame (a load, a fade) has nothing to meter: hold.
    float metered = (sumW > 0.0) ? sumL / sumW : (valid ? gState[1] : gRefLog2);

    // THE NIGHT'S HIGHLIGHTS. At night the window above lands on the black of the sky and the verge, and the
    // exposure opened up as far as the night lets it (gNightKnee, gNightSlope) - while the headlamps' patch of road
    // beside the car, a tenth of the screen, clipped flat white, and everything that reflected a few percent of it
    // with it: the car's paint, its tyres (the night glints). An eye adapts to the light in view, not to the dark
    // around it. So at night the meter reads no darker than kHighlightEv under its kHighlightPct percentile: the
    // brightest few percent of the screen at most that far above the grey. Blended in over 2 EV past the knee, so
    // the day's metering is as before.
    if (sumW > 0.0)
    {
        const float at = total * kHighlightPct;
        float run = 0.0, pHigh = metered;
        for (uint k = 1; k < BINS; ++k)
        {
            run += gsWeight[k];
            if (run >= at)
            {
                pHigh = gMinLog2 + (float(k - 1) + 0.5) / float(BINS - 1) * gLog2Range;
                break;
            }
        }
        const float night = saturate((gRefLog2 - metered - gNightKnee) / 2.0);
        metered = max(metered, lerp(metered, pHigh - kHighlightEv, night));
    }

    // Adaptation in LOG space, exponential in time, so a step of N EV settles
    // in the same time whatever N is - which is how adaptation is perceived.
    // Asymmetric: the eye closes down to glare much faster than it opens up
    // to shade, and a stage exit from forest into sun is exactly that case.
    float adapted = valid ? gState[0] : metered;
    if (gSnap > 0.5 || !valid)
        adapted = metered;
    else
    {
        const float speed = (metered > adapted) ? gSpeedBright : gSpeedDark;
        adapted += (metered - adapted) * (1.0 - exp(-gDt * speed));
    }

    // How much darker than the reference this is, and how much of that the exposure makes up: all of it within
    // gNightKnee EV, then gNightSlope of the rest - the night stays darker than the day, as it does to adapted
    // eyes, instead of being exposed to the day's grey (design 5.4, N1c).
    const float darker = gRefLog2 - adapted;
    const float madeUp = darker > gNightKnee ? gNightKnee + (darker - gNightKnee) * gNightSlope : darker;
    const float ev = clamp(madeUp, -gEvDown, gEvUp);
    gState[0] = adapted;
    gState[1] = metered;
    gState[2] = 1.0;
    gState[3] = gExposure * exp2(ev);
    // Again at byte 512, where a buffer-to-texture copy may start: the upscaler's 1x1 exposure (PostFx::Resolve).
    gState[128] = gState[3];
}
