#include "post.h"
#include "log.h"

#include <d3dcompiler.h>
#include <string>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// One fullscreen triangle from SV_VertexID - no vertex buffer, no input layout,
// and no seam down the diagonal that the two-triangle version has.
const char* kPostSrc = R"(
cbuffer Post : register(b0)
{
    row_major float4x4 gReproj;   // current NDC+depth -> previous NDC
    float2 gTexel;                // 1/width, 1/height
    float2 gJitter;               // this frame's sub-pixel offset, in NDC
    float  gBlend;                // history weight when nothing rejects it
    float  gValid;                // 0 when the history describes a different image
    float  gExposure;
    float  gCurve;
    float  gAutoExposure;         // 1 = read the exposure the meter wrote
    float  gSharpen;              // post-tonemap sharpen amount, 0 = off
    float  gHistoryCubic;         // 1 = Catmull-Rom history fetch, 0 = bilinear
    float  gMotionOn;             // 1 = gMotion holds this frame's prepass motion
};

// [0] adapted log2 luminance, [1] metered, [2] valid, [3] exposure to apply.
// Written by CSAdapt on the GPU this frame; read here without a round trip.
StructuredBuffer<float> gExposureState : register(t3);

float FrameExposure()
{
    return (gAutoExposure > 0.5) ? gExposureState[3] : gExposure;
}

Texture2D    gColour  : register(t0);   // TAA: this frame. Tonemap: the resolved image.
Texture2D    gHistory : register(t1);
Texture2D    gDepth   : register(t2);
// The scene prepass's motion (Scene::MotionBuffer): w = 1 where an object
// moved, with xy its NDC offset to last frame and z its depth there. t4, as
// t3 is the exposure state's root SRV.
Texture2D    gMotion  : register(t4);
// The ray-traced shadow's denoised result (Rt::Filtered): z > 0.5 where a ray
// hit a moved object this frame or in the last four (rt.cpp's distrust). A
// NULL descriptor without RT shadows - zero, nothing distrusted.
Texture2D    gShadow  : register(t5);
Texture2D    gAo      : register(t6);   // the RT AO's denoise (Rt::AoDenoised), likewise
Texture2D    gAoPrev  : register(t7);   // and last frame's (Rt::AoPrevDenoised)
SamplerState sPoint   : register(s0);
SamplerState sLinear  : register(s1);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The history at a fractional position, Catmull-Rom filtered.
//
// The bilinear fetch this replaces is a small blur every time the camera moves
// by a non-whole number of pixels - which is every frame of driving - and at a
// 0.9 history weight the blurs COMPOUND: the image converges to something
// softer than any single frame. Catmull-Rom is interpolating rather than
// smoothing (its kernel has negative lobes), so it holds detail through
// repeated resampling. At a static camera prevUv lands on texel centres and
// the two filters are identical; the difference is purely a motion property.
//
// Sixteen texels, fetched in five bilinear taps: the middle two rows/columns
// of the 4x4 kernel are merged into one tap each by placing the bilinear
// sample between them at the ratio of their weights, and the four corners -
// whose weights are tiny products of two negative lobes - are dropped and the
// rest renormalised. The standard real-time form of the filter.
float3 HistoryCatmullRom(float2 uv)
{
    const float2 size = 1.0 / gTexel;
    const float2 pos  = uv * size;
    const float2 t1   = floor(pos - 0.5) + 0.5;      // texel centre at or left of pos
    const float2 f    = pos - t1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 w12 = w1 + w2;
    const float2 c0  = (t1 - 1.0) * gTexel;
    const float2 c12 = (t1 + w2 / w12) * gTexel;
    const float2 c3  = (t1 + 2.0) * gTexel;

    float3 r = 0.0;
    float  w = 0.0;
    float  k;
    k = w12.x * w0.y;  r += gHistory.SampleLevel(sLinear, float2(c12.x, c0.y),  0).rgb * k; w += k;
    k = w0.x  * w12.y; r += gHistory.SampleLevel(sLinear, float2(c0.x,  c12.y), 0).rgb * k; w += k;
    k = w12.x * w12.y; r += gHistory.SampleLevel(sLinear, float2(c12.x, c12.y), 0).rgb * k; w += k;
    k = w3.x  * w12.y; r += gHistory.SampleLevel(sLinear, float2(c3.x,  c12.y), 0).rgb * k; w += k;
    k = w12.x * w3.y;  r += gHistory.SampleLevel(sLinear, float2(c12.x, c3.y),  0).rgb * k; w += k;

    // The negative lobes can undershoot below zero next to a bright edge; the
    // variance clip after this would catch most of it, but a negative HDR
    // value must never reach the 1/(1+luma) weight below.
    return max(r / max(w, 1e-5), 0.0);
}
)" R"(
// ---------------------------------------------------------------------------
// TAA
// ---------------------------------------------------------------------------
float4 PSTaa(VSOut i) : SV_Target
{
    const float2 uv = i.uv;
    float3 curr = gColour.SampleLevel(sPoint, uv, 0).rgb;

    // Reprojection, entirely in homogeneous coordinates and deliberately so.
    // Reconstructing a world position first would need a divide by w, and for a
    // SKY pixel - reverse-Z depth 0, i.e. the far plane - that point is at
    // infinity and the divide produces a NaN. Carried as a homogeneous point,
    // one matrix maps the finite geometry and the point at infinity correctly,
    // and the sky reprojects as pure camera rotation with no special case.
    const float depth = gDepth.SampleLevel(sPoint, uv, 0).r;
    const float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    // A MOVED object's pixel cannot be reprojected by the camera: the surface
    // was somewhere else last frame. The prepass says where.
    const float4 motion = (gMotionOn > 0.5) ? gMotion.SampleLevel(sPoint, uv, 0) : float4(0, 0, 0, 0);
    const bool   moved  = motion.w > 0.5;
    // MOVING GLASS over a still surface (Scene's PSPrepass, w = 0.25): not in the depth, so reprojected as what is
    // behind it, but its own light is moving - never "still" (below), and marked in the history as a moved object's,
    // so the pixels it leaves drop what it laid there. Without it a turning car's headlamp lens, the brightest thing
    // on the showroom's dark, smeared across it at 0.97 a frame.
    const bool   glass  = !moved && motion.w > 0.125;
    float2 prevUv = float2(0.0, 0.0);
    float  prevDepth = 0.0;               // reverse-Z depth this surface had then
    bool   usable;
    if (moved)
    {
        const float2 prevNdc = ndc + motion.xy;
        prevUv    = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
        prevDepth = motion.z;
        usable    = all(prevUv >= 0.0) && all(prevUv <= 1.0);
    }
    else
    {
        const float4 prevClip = mul(float4(ndc, depth, 1.0), gReproj);
        usable = abs(prevClip.w) > 1e-7;
        if (usable)
        {
            const float2 prevNdc = prevClip.xy / prevClip.w;
            prevUv    = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
            prevDepth = prevClip.z / prevClip.w;
            usable    = all(prevUv >= 0.0) && all(prevUv <= 1.0);
        }
    }

    // DISOCCLUSION. The history's alpha is the depth each pixel had, negated
    // where it belonged to a moved object (see the return below). The road a
    // car has just uncovered reprojects correctly by the camera, but last
    // frame that spot was the car - and the colour clip below cannot tell a
    // dark tyre from a dark patch of road, so it lets part of it through,
    // frame after frame: copies of the car's back laid on the road behind it.
    //
    // Decided by IDENTITY, not by depth. A depth test cannot see this case:
    // where a tyre meets the road the two ARE at the same depth, and the road
    // just behind a bumper is within a few percent of it. So a static pixel
    // whose history fetch touches any texel of a moved object drops its
    // history outright - the cost is one unaccumulated frame along the car's
    // outline, which the next frame accumulates again. A moved pixel keeps it
    // if one texel of the 2x2 it straddles is of a moved object at the depth
    // it had, so the car's own edges still accumulate. Static against static
    // is left exactly as it was - the camera reprojection is exact there.
    bool fresh = false;     // the history there had none itself: one raw frame (alpha + 2, below)
    bool masked = false;    // a moved pixel behind a still occluder: its own texels only (below)
    float4 own = 0.0;       // and their bilinear weights, in GatherAlpha's order
    if (usable && gMotionOn > 0.5)
    {
        const float4 hd = gHistory.GatherAlpha(sPoint, prevUv);
        // The whole 4x4 the Catmull-Rom fetch reads, not just the 2x2:
        // its outer taps reach two texels out, with negative weights, and
        // that alone lays a ringing copy of the car's edge on the road.
        const float4 a = gHistory.GatherAlpha(sPoint, prevUv + float2(-gTexel.x, -gTexel.y));
        const float4 b = gHistory.GatherAlpha(sPoint, prevUv + float2( gTexel.x, -gTexel.y));
        const float4 c = gHistory.GatherAlpha(sPoint, prevUv + float2(-gTexel.x,  gTexel.y));
        const float4 d = gHistory.GatherAlpha(sPoint, prevUv + float2( gTexel.x,  gTexel.y));
        if (!moved)
        {
            // (The glass's own pixels keep theirs: it was there last frame too, and the clip judges it.)
            usable = glass || !any(min(min(a, b), min(c, d)) < 0.0);
            fresh  = any(hd > 1.5);
        }
        else
        {
            float4 same = 0.0;
            [unroll] for (int k = 0; k < 4; ++k)
            {
                const float h = -hd[k];             // > 0 only for a moved texel
                same[k] = (h > 0.0 && abs(prevDepth / h - 1.0) < 0.05) ? 1.0 : 0.0;
            }
            usable = any(same > 0.0);
            // A STILL OCCLUDER where the car was. A car driving behind a fence
            // reprojects onto last frame's fence bars: one texel of the car in
            // the 2x2 passed it, and the Catmull-Rom's 4x4 read the bars along
            // with it. The thin light bars sit inside the clip of a grainy
            // white door, so each frame laid their edges on the car, and the
            // car carried them off: copies of the bars trailing on its side.
            // So where any texel of the 4x4 is a still surface IN FRONT of
            // the car (reverse-Z: a greater depth; the + 2 of a fresh one
            // taken off), the history is the car's own texels of the 2x2
            // alone, bilinear. Still surfaces BEHIND it - the background at
            // its outline - are left as they were, so its edges keep their
            // filtering.
            const float near = prevDepth * 1.05;
            [unroll] for (int g = 0; g < 4; ++g)
            {
                const float4 t  = (g == 0) ? a : (g == 1) ? b : (g == 2) ? c : d;
                const float4 td = (t > 1.5) ? t - 2.0 : t;
                masked = masked || any(t >= 0.0 && td > near);
            }
            masked = masked && usable;
            const float2 f = frac(prevUv / gTexel - 0.5);
            own = same * float4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
        }
    }

    // Neighbourhood constraint. Variance clipping rather than a min/max box:
    // the box is set by the two most extreme samples in the 3x3, so a single
    // specular pixel widens it enough to let a genuinely stale history through,
    // which is what ghosting is.
    float3 m1 = 0.0, m2 = 0.0, lo = curr, hi = curr;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float3 c = gColour.SampleLevel(sPoint, uv + float2(x, y) * gTexel, 0).rgb;
            m1 += c; m2 += c * c;
            lo = min(lo, c); hi = max(hi, c);
        }
    }
    const float3 mean  = m1 / 9.0;
    const float3 sigma = sqrt(max(0.0, m2 / 9.0 - mean * mean));
    // STANDING STILL. A pixel that does not move on screen is reprojected
    // exactly, so its history is stale only if its lighting changed - and
    // there the clip and the 0.9 were what made THIN geometry flicker on a
    // stationary car's view: a fence bar a pixel or two wide drops out of this
    // frame's 3x3 whenever the jitter samples past it, the box shrinks to the
    // road behind and the accumulated bar is clipped away; and a tenth of each
    // jittered frame is a tenth of the bar's aliasing, every frame. So from a
    // quarter pixel of motion down to none, the clip fades out and the history
    // weight goes to 0.97 (`still`): frame-to-frame flicker on the start's
    // fence -71%, on the car's edges -65%. A view that moves is untouched;
    // a lighting change on a still pixel is a few frames slower to arrive.
    const float  travel = length((uv - prevUv) / gTexel);
    // A MOVED OCCLUDER (see below) is a lighting change: never still.
    //
    // Where the RT shadow's rays hit a moved object - this frame or in the
    // last four, the denoiser's distrust - the road is the same surface it
    // was, so nothing above rejects its history, but its light is not: the
    // car's shadow has just left it, or just arrived. The clip cannot fix
    // that on a textured road - a stale shadow a sigma darker passes it - and
    // at 0.9 each frame's leftover laid a faint copy of the shadow behind the
    // car: the trails seen only with TAA on (the debug views turn it off).
    // The AO likewise, around the wheels.
    const int3   px = int3(i.pos.xy, 0);
    const bool   shadowMoved = !moved && usable && gShadow.Load(px).z > 0.5;
    const bool   aoMoved     = !moved && usable && gAo.Load(px).z > 0.5;
    const float  still  = (usable && !shadowMoved && !aoMoved && !glass) ? saturate(1.0 - travel * 4.0) : 0.0;
    const float3 clipLo = max(lo, mean - 1.25 * sigma);
    const float3 clipHi = min(hi, mean + 1.25 * sigma);

    float3 hist = (gHistoryCubic > 0.5) ? HistoryCatmullRom(prevUv)
                                        : gHistory.SampleLevel(sLinear, prevUv, 0).rgb;
    if (masked)
    {
        // The 2x2 GatherAlpha read, in its order: (0,1) (1,1) (1,0) (0,0).
        const int2 t0 = int2(floor(prevUv / gTexel - 0.5));
        hist = (gHistory.Load(int3(t0 + int2(0, 1), 0)).rgb * own.x + gHistory.Load(int3(t0 + int2(1, 1), 0)).rgb * own.y
              + gHistory.Load(int3(t0 + int2(1, 0), 0)).rgb * own.z + gHistory.Load(int3(t0, 0)).rgb * own.w)
             / max(dot(own, 1.0), 1e-5);
    }
    // So there the history is RELIT: scaled so its local mean (four bilinear
    // taps, a 3x3 tent) is this frame's 3x3 mean. The light changes slowly
    // across the screen and the texture does not, so the ratio moves the
    // shadow and keeps the accumulated detail. Dropping the history there
    // instead also ended the trail, but at 2.5x the grain of the road behind
    // the car - TAA off, following it around.
    //
    // For the AO alone the ratio is bounded by the AO's own change, now / was
    // (both over the same tent), less 6% for the AO's noise: its rays reach
    // 3 m, so its distrust covers a wide area where the car barely changes
    // the occlusion, and there an unbounded ratio follows the grass's
    // jittered texture instead - 60% more grain on the grass beside the car;
    // bounded, 3%, and the dark streak off the wheels is gone all the same.
    if (shadowMoved || aoMoved)
    {
        float3 hm = 0.0;
        float  now = 0.0, was = 0.0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            const float2 o = float2((k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5) * gTexel;
            hm += gHistory.SampleLevel(sLinear, prevUv + o, 0).rgb;
            if (!shadowMoved)
            {
                now += gAo.SampleLevel(sLinear, uv + o, 0).x;
                was += gAoPrev.SampleLevel(sLinear, prevUv + o, 0).x;
            }
        }
        float q = now / max(was, 1e-3);
        q = 1.0 + sign(q - 1.0) * max(0.0, abs(q - 1.0) - 0.06);
        const float2 r = shadowMoved ? float2(0.25, 4.0) : float2(max(min(1.0, q), 0.25), min(max(1.0, q), 4.0));
        hist *= clamp(mean / max(hm * 0.25, 1e-4), r.x, r.y);
    }
    hist = lerp(clamp(hist, clipLo, clipHi), hist, still);

    float blend = (gValid < 0.5 || !usable) ? 0.0 : lerp(gBlend, 0.97, still);
    // ONE RAW FRAME. Where the history is a pixel that had none itself - the
    // road the car uncovered last frame - it is a single jittered frame, and
    // the one right under the car carries the contact shadow's dark edge, a
    // row or two thin: too thin for the relight above, inside the clip on
    // textured asphalt. At 0.9 it printed a dark line on the road one frame
    // of travel behind the car, the ghost left after the trails were gone.
    // A quarter of it: the line goes (the user's eye at tick 3121; measured
    // -0.7% -> -0.2%), and the road behind the car keeps most of its
    // accumulation - none of it took grain there from 0.0043 to 0.0057.
    if (fresh) blend = min(blend, 0.25);

    // Blend with a 1/(1+luma) weight and undo it afterwards. Averaging HDR
    // radiance directly lets one very bright sample dominate the accumulation
    // and survive as a flickering dot for many frames - the firefly. Weighting
    // by inverse luminance makes the average behave like a display-referred one
    // without actually tonemapping here, which would break the clip above.
    //
    // On EXPOSED luminance. The weight is meant to behave like a display-
    // referred average, which it only does if 1.0 means "bright on screen";
    // with a fixed exposure near 1 the difference was invisible, with auto
    // exposure swinging several EV it is not. Last frame's exposure - the
    // meter runs after this pass - which for a weight is exact enough.
    const float e  = FrameExposure();
    const float wc = (1.0 - blend) / (1.0 + Luma(curr) * e);
    const float wh = blend         / (1.0 + Luma(hist) * e);
    const float3 outc = (curr * wc + hist * wh) / max(1e-5, wc + wh);
    // Alpha: this pixel's depth, negative if it belongs to a moved object (or moving glass) -
    // what next frame's disocclusion test reads - and plus 2 for a static one
    // that took no history (`fresh` above); static depth is read for its sign
    // only. Nothing displays it: the tonemap and the meter read rgb.
    return float4(outc, (moved || glass) ? -max(depth, 1e-7) : (usable ? depth : depth + 2.0));
}

// ---------------------------------------------------------------------------
// Tonemap
// ---------------------------------------------------------------------------
float3 LinearToSrgbEnc(float3 c)
{
    c = saturate(c);
    return (c <= 0.0031308) ? c * 12.92
                            : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Narkowicz 2015, the cheap RRT+ODT fit. Linear in, linear out.
float3 Aces(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 AgxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
         - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// AgX. Compresses in a perceptual log space and rotates towards the achromatic
// axis on the way, so a channel that clips loses saturation instead of hue -
// which is the failure ACES has on a sun disc, where red saturates first and
// the highlight turns yellow before it turns white.
float3 Agx(float3 x)
{
    const float3x3 inset = float3x3(
        0.8424790622530940, 0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.8784686364697720, 0.0791661274605434,
        0.0423756549057051, 0.0784336000000000, 0.8791429737931040);
    const float3x3 outset = float3x3(
         1.19687900512017,  -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562, 1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438,-0.0980434501171241,  1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;

    float3 v = mul(inset, max(x, 0.0));
    v = clamp(log2(max(v, 1e-10)), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    v = AgxContrast(v);
    v = mul(outset, v);
    return pow(max(v, 0.0), 2.2);      // back to linear; the OETF follows
}

// NIGHT VISION (design 5.4, N1c): in dim light the rods take over from the cones - colour drains away and what is
// left shifts to blue-green (the Purkinje shift), which is most of why a moonlit scene reads as night. Per pixel, on
// the scene's own luminance BEFORE exposure, against the day's reference grey: full colour down to ~8 EV under it
// (late twilight), rods alone from ~17 EV under (moonlight), mesopic between. A surface in a headlight's beam stays
// in colour while the moonlit verge beside it does not - as it is to the eye.
float3 NightVision(float3 c)
{
    const float kRefLog2 = -2.025;          // kReferenceLog2: the day's metered grey
    const float under = kRefLog2 - log2(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-30));
    const float rods = smoothstep(8.0, 17.0, under);
    if (rods <= 0.0) return c;
    const float scotopic = dot(c, float3(0.046, 0.454, 0.5));   // the rods' response: they peak near 507 nm
    return lerp(c, scotopic * float3(0.72, 0.86, 1.08), rods);
}

// Exposure and curve: HDR radiance -> display-linear 0..1, before the encode.
float3 ToneDisplay(float3 c)
{
    c = NightVision(c);
    c *= FrameExposure();
    if (gCurve < 2.5) return 1.0 - exp(-c);
    if (gCurve < 3.5) return Aces(c);
    return Agx(c);
}

float4 PSTonemap(VSOut i) : SV_Target
{
    float3 c = gColour.SampleLevel(sPoint, i.uv, 0).rgb;

    if (gCurve < 0.5)  return float4(c, 1.0);                      // raw
    if (gCurve < 1.5)  return float4(LinearToSrgbEnc(c), 1.0);     // sRGB only

    float3 t = ToneDisplay(c);

    // Sharpen AFTER the tonemap, on display values: in HDR a sharpen is
    // dominated by the brightest edge in the frame - the sun against the sky -
    // and does nearly nothing to the road texture next to it, which is where
    // the softness is seen.
    //
    // Unsharp mask on the 4-neighbour cross, then CLAMPED to that cross's
    // min/max. The clamp is the whole safety argument: a sharpen may steepen
    // an edge but never create a value that was not already among the pixel
    // and its neighbours, so there is no halo and no ringing to tune away.
    // What it does to noise is the one cost, and TAA has already removed most
    // of it by the time this runs.
    if (gSharpen > 0.0)
    {
        const float3 n = ToneDisplay(gColour.SampleLevel(sPoint, i.uv + float2(0.0, -gTexel.y), 0).rgb);
        const float3 s = ToneDisplay(gColour.SampleLevel(sPoint, i.uv + float2(0.0,  gTexel.y), 0).rgb);
        const float3 e = ToneDisplay(gColour.SampleLevel(sPoint, i.uv + float2( gTexel.x, 0.0), 0).rgb);
        const float3 w = ToneDisplay(gColour.SampleLevel(sPoint, i.uv + float2(-gTexel.x, 0.0), 0).rgb);
        const float3 lo = min(t, min(min(n, s), min(e, w)));
        const float3 hi = max(t, max(max(n, s), max(e, w)));
        t = clamp(t + (t - 0.25 * (n + s + e + w)) * gSharpen, lo, hi);
    }

    return float4(LinearToSrgbEnc(t), 1.0);
}
)";

// ---------------------------------------------------------------------------
// Auto exposure: a log-luminance histogram, then a percentile mean adapted over
// time. Two compute passes, one command list, no CPU in the loop.
//
// A HISTOGRAM rather than a log-average of the whole image, because the mean
// is dominated by exactly the pixels metering should ignore: the sun disc and
// the glare around it pull a plain log-average up, and a strip of deep shadow
// pulls it down. The histogram lets both tails be dropped by percentile.
// ---------------------------------------------------------------------------
const char* kMeterSrc = R"(
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
    const float metered = (sumW > 0.0) ? sumL / sumW : (valid ? gState[1] : gRefLog2);

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
}
)";

// The metering constants. Kept together because they are one decision.
//
// kReferenceLog2 is the CALIBRATION: the adapted log2 luminance at which auto
// exposure applies the dial unchanged. It was measured, not chosen - see 5.0g:
// the track-108 start camera, the scene the current dial (0.8) was tuned on,
// meters at this value, so turning auto exposure on leaves that view as it was.
// The meter reaches down to 2^-32 for the night (design 5.4, N1c): a moonlit stage meters near 2^-21, a moonless
// one near 2^-28 - both below the day's floor of 2^-12, where they counted as black and the meter froze.
const float kMinLog2       = -32.0f;
const float kLog2Range     = 40.0f;    // to 2^8: above that is the sun disc
const float kReferenceLog2 = -2.025f;  // MEASURED, 5.0g: track-108 start camera
const float kSpeedBright   = 3.0f;     // 1/e in ~0.33 s - closing down to glare
const float kSpeedDark     = 1.2f;     // 1/e in ~0.8 s - opening up to shade
const float kEvDown        = 3.0f;     // dial / 8 at most
// The day's adaptation is +-3 EV round the dial; the NIGHT gets more, but not all of it: beyond kNightKnee EV darker
// than the reference only kNightSlope of the rest is made up. A full moon (~19 EV under the day) then sits ~4 EV
// under the day's grey, a moonless night (~26 EV) ~6 EV under: dark, readable - and headlights, when they come,
// will meter the lit road back up.
const float kEvUp          = 24.0f;
const float kNightKnee     = 3.0f;
const float kNightSlope    = 0.75f;
const float kLowPct        = 0.40f;
const float kHighPct       = 0.90f;

struct MeterCB
{
    uint32_t size[2];
    float minLog2, log2Range, dt, speedBright, speedDark, exposure;
    float refLog2, evDown, evUp, lowPct, highPct, snap, nightKnee, nightSlope;
};
static_assert(sizeof(MeterCB) == 64, "MeterCB is 16 root constants");

// Halton, the standard low-discrepancy choice for sub-pixel offsets: any prefix
// of it covers the pixel evenly, so a history only a few frames deep is still
// unbiased. A regular grid would bias every partial accumulation towards
// whichever corner it had reached.
float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0)
    {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

void MatMul(const float* a, const float* b, float* out)   // row-vector: v*a*b
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

// General 4x4 inverse. The view-projection is not affine - the projection makes
// w depend on z - so the cheap "transpose the rotation, negate the translation"
// shortcut does not apply here.
bool MatInverse(const float* m, float* out)
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    return b;
}

// Descriptor groups, eight wide so one root signature serves both passes.
// t0 is "the colour input", which differs per pass; t1/t2 are only read by TAA.
enum { kGroupTaa0 = 0, kGroupTaa1 = 1, kGroupTone0 = 2, kGroupTone1 = 3, kGroupToneHdr = 4 };
enum { kGroupSize = 8, kGroupCount = 5 };   // colour, history, depth, motion, RT shadow, RT AO, its last

} // namespace

const char* PostFx::CurveName(int c)
{
    switch (c)
    {
    case kCurveRaw:  return "raw (debug)";
    case kCurveSrgb: return "sRGB only (RBR reference)";
    case kCurveExp:  return "1-exp (previous default)";
    case kCurveAces: return "ACES";
    case kCurveAgx:  return "AgX";
    default:         return "?";
    }
}

bool PostFx::Create(ID3D12Device* dev, DXGI_FORMAT outputFormat)
{
    m_outputFormat = outputFormat;
    return CreatePasses(dev, outputFormat);
}

bool PostFx::CreatePasses(ID3D12Device* dev, DXGI_FORMAT outputFormat)
{
    // t0..t2 colour, history, depth; then the motion, the RT shadow, the RT AO
    // and last frame's at t4..t7 - t3 is the exposure state's root SRV - from
    // the group's fourth to eighth slots.
    D3D12_DESCRIPTOR_RANGE range[2] = {};
    range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range[0].NumDescriptors = 3;
    range[0].BaseShaderRegister = 0;
    range[0].OffsetInDescriptorsFromTableStart = 0;
    range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range[1].NumDescriptors = 4;
    range[1].BaseShaderRegister = 4;
    range[1].OffsetInDescriptorsFromTableStart = 3;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 28;   // 4x4 matrix + 12 scalars
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // The exposure the meter wrote this frame, as a root SRV: one buffer, and
    // no descriptor to keep in step with anything.
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 3;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("post: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const std::string src = kPostSrc;
    ComPtr<ID3DBlob> vs, psTaa, psTone;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_post", nullptr, nullptr,
                          "VSMain", "vs_5_1", flags, 0, &vs, &err)))
    {
        Log("post: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_post", nullptr, nullptr,
                          "PSTaa", "ps_5_1", flags, 0, &psTaa, &err)))
    {
        Log("post: TAA compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_post", nullptr, nullptr,
                          "PSTonemap", "ps_5_1", flags, 0, &psTone, &err)))
    {
        Log("post: tonemap compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;

    pd.PS = { psTaa->GetBufferPointer(), psTaa->GetBufferSize() };
    pd.RTVFormats[0] = kHdrFormat;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoTaa))))
    {
        Log("post: TAA pipeline creation failed");
        return false;
    }

    pd.PS = { psTone->GetBufferPointer(), psTone->GetBufferSize() };
    pd.RTVFormats[0] = outputFormat;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoTonemap))))
    {
        Log("post: tonemap pipeline creation failed");
        return false;
    }

    if (!CreateMetering(dev))
    {
        // Not fatal: the dial still works, it just stops being automatic.
        Log("post: auto exposure unavailable - exposure stays manual");
        m_autoExposure = false;
    }

    Log("post: ready - HDR RGBA16F, TAA on, tonemap %s, auto exposure %s",
        CurveName(m_curve), m_autoExposure ? "on" : "off");
    return true;
}

bool PostFx::CreateMetering(ID3D12Device* dev)
{
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;                // t0: the image being tonemapped
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;   // u0 histogram
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;   // u1 state
    params[2].Descriptor.ShaderRegister = 1;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 0;
    params[3].Constants.Num32BitValues = sizeof(MeterCB) / 4;
    for (D3D12_ROOT_PARAMETER& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 4;
    rs.pParameters = params;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("post: meter root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_meterRootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const std::string src = kMeterSrc;
    ComPtr<ID3DBlob> csHist, csAdapt;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_meter", nullptr, nullptr,
                          "CSHistogram", "cs_5_1", flags, 0, &csHist, &err)) ||
        FAILED(D3DCompile(src.c_str(), src.size(), "ngg_meter", nullptr, nullptr,
                          "CSAdapt", "cs_5_1", flags, 0, &csAdapt, &err)))
    {
        Log("post: meter compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = m_meterRootSig.Get();
    cd.CS = { csHist->GetBufferPointer(), csHist->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_psoHistogram)))) return false;
    cd.CS = { csAdapt->GetBufferPointer(), csAdapt->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&m_psoAdapt)))) return false;

    // Both buffers start zeroed - committed resources are - which is exactly
    // the state each needs: an empty histogram, and a state whose valid flag
    // is 0 so the first frame snaps instead of adapting up from nothing.
    auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS f,
                      D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = type;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = f;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                      nullptr, IID_PPV_ARGS(&out)));
    };
    if (!buffer(128 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, m_histogram) ||
        !buffer(4 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, m_exposureState) ||
        !buffer(4 * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST, m_exposureReadback))
        return false;

    return true;
}

void PostFx::ExposureStateTo(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES s)
{
    if (!m_exposureState || m_exposureStateNow == s) return;
    D3D12_RESOURCE_BARRIER b = Transition(m_exposureState.Get(), m_exposureStateNow, s);
    list->ResourceBarrier(1, &b);
    m_exposureStateNow = s;
}

// Histogram of the image about to be tonemapped, then the adapted exposure,
// into m_exposureState - which the tonemap reads in the same command list.
void PostFx::Meter(ID3D12GraphicsCommandList* list, ID3D12Resource* image,
                   D3D12_GPU_DESCRIPTOR_HANDLE imageTable, UINT width, UINT height)
{
    // Wall-clock frame time, measured here so every client gets adaptation
    // without having to pass a dt it may not track. Clamped: a hitch or a
    // breakpoint must not count as seconds of adaptation.
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    float dt = 0.0f;
    if (m_lastTick.QuadPart)
        dt = (float)(double(now.QuadPart - m_lastTick.QuadPart) / double(hz.QuadPart));
    m_lastTick = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    MeterCB cb = {};
    cb.size[0] = width;
    cb.size[1] = height;
    cb.minLog2 = kMinLog2;
    cb.log2Range = kLog2Range;
    cb.dt = dt;
    cb.speedBright = kSpeedBright;
    cb.speedDark = kSpeedDark;
    cb.exposure = m_exposure;
    cb.refLog2 = kReferenceLog2;
    cb.evDown = kEvDown;
    cb.evUp = kEvUp;
    cb.nightKnee = kNightKnee;
    cb.nightSlope = kNightSlope;
    cb.lowPct = kLowPct;
    cb.highPct = kHighPct;
    cb.snap = m_snapExposure ? 1.0f : 0.0f;
    m_snapExposure = false;

    // The image is in PIXEL_SHADER_RESOURCE, which a COMPUTE read may not use.
    D3D12_RESOURCE_BARRIER pre[2];
    pre[0] = Transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The histogram decayed to COMMON when last frame's list executed.
    pre[1] = Transition(m_histogram.Get(), D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(2, pre);
    ExposureStateTo(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->SetComputeRootSignature(m_meterRootSig.Get());
    list->SetComputeRootDescriptorTable(0, imageTable);
    list->SetComputeRootUnorderedAccessView(1, m_histogram->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, m_exposureState->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstants(3, sizeof(cb) / 4, &cb, 0);

    list->SetPipelineState(m_psoHistogram.Get());
    list->Dispatch((width / 2 + 15) / 16, (height / 2 + 15) / 16, 1);

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_histogram.Get();
    list->ResourceBarrier(1, &uav);

    list->SetPipelineState(m_psoAdapt.Get());
    list->Dispatch(1, 1, 1);

    // The histogram's clear must land before next frame's accumulation, and
    // the state goes to the tonemap via a copy for the HUD.
    D3D12_RESOURCE_BARRIER post[2];
    post[0] = uav;
    post[1] = Transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(2, post);
    ExposureStateTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(m_exposureReadback.Get(), 0, m_exposureState.Get(), 0, 16);
    ExposureStateTo(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_readbackPending = true;
}

bool PostFx::OnResize(ID3D12Device* dev, UINT width, UINT height)
{
    if (!width || !height) return false;

    m_width = width;
    m_height = height;
    m_historyValid = false;      // a resized history is a different image
    m_hdrIsTarget = false;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kHdrFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kHdrFormat;
    for (int k = 0; k < 4; ++k) clear.Color[k] = kHdrClear[k];

    m_hdr.Reset();
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            &clear, IID_PPV_ARGS(&m_hdr))))
    {
        Log("post: HDR target creation failed");
        return false;
    }
    for (int i = 0; i < 2; ++i)
    {
        m_history[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                &clear, IID_PPV_ARGS(&m_history[i]))))
        {
            Log("post: history buffer %d creation failed", i);
            return false;
        }
    }

    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
        m_rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (!m_srvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kGroupCount * kGroupSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        m_srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(m_hdr.Get(), nullptr, rtv);
    rtv.ptr += m_rtvStride;
    dev->CreateRenderTargetView(m_history[0].Get(), nullptr, rtv);
    rtv.ptr += m_rtvStride;
    dev->CreateRenderTargetView(m_history[1].Get(), nullptr, rtv);

    // Colour SRVs. The depth slot of each group is written in Resolve, once the
    // depth resource is known - it belongs to the Scene, not to us.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = kHdrFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;

    auto slot = [&](int group, int index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(group * kGroupSize + index) * m_srvStride;
        return h;
    };
    // TAA reading history 0 / history 1.
    dev->CreateShaderResourceView(m_hdr.Get(),        &sd, slot(kGroupTaa0, 0));
    dev->CreateShaderResourceView(m_history[0].Get(), &sd, slot(kGroupTaa0, 1));
    dev->CreateShaderResourceView(m_hdr.Get(),        &sd, slot(kGroupTaa1, 0));
    dev->CreateShaderResourceView(m_history[1].Get(), &sd, slot(kGroupTaa1, 1));
    // Tonemap of whichever history was just written, and of the raw HDR when
    // TAA is off.
    dev->CreateShaderResourceView(m_history[0].Get(), &sd, slot(kGroupTone0, 0));
    dev->CreateShaderResourceView(m_history[0].Get(), &sd, slot(kGroupTone0, 1));
    dev->CreateShaderResourceView(m_history[1].Get(), &sd, slot(kGroupTone1, 0));
    dev->CreateShaderResourceView(m_history[1].Get(), &sd, slot(kGroupTone1, 1));
    dev->CreateShaderResourceView(m_hdr.Get(),        &sd, slot(kGroupToneHdr, 0));
    dev->CreateShaderResourceView(m_hdr.Get(),        &sd, slot(kGroupToneHdr, 1));

    Log("post: %u x %u HDR + 2 history buffers", width, height);
    return true;
}

void PostFx::Jitter(UINT width, UINT height, float* outX, float* outY) const
{
    if (!m_taa || !width || !height) { *outX = *outY = 0.0f; return; }

    // 8 samples: long enough to look converged on an edge, short enough that a
    // 16-frame history is not required to complete the pattern.
    const uint32_t index = (m_frame % 8) + 1;
    const float hx = Halton(index, 2) - 0.5f;
    const float hy = Halton(index, 3) - 0.5f;
    *outX =  2.0f * hx / (float)width;
    *outY = -2.0f * hy / (float)height;
}

D3D12_CPU_DESCRIPTOR_HANDLE PostFx::Begin(ID3D12GraphicsCommandList* list)
{
    if (!m_hdrIsTarget)
    {
        D3D12_RESOURCE_BARRIER b = Transition(m_hdr.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
        m_hdrIsTarget = true;
    }
    return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
}

void PostFx::Resolve(ID3D12GraphicsCommandList* list,
                     D3D12_CPU_DESCRIPTOR_HANDLE outRtv,
                     ID3D12Resource* depth,
                     ID3D12Resource* motion,
                     ID3D12Resource* rtShadow,
                     ID3D12Resource* rtAo,
                     ID3D12Resource* rtAoPrev,
                     const float* sceneViewProj,
                     UINT width, UINT height)
{
    if (!Ready() || !m_hdr) return;

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;

    const int writeIndex = (int)(m_frame & 1);   // history written this frame
    const int readIndex  = writeIndex ^ 1;       // history written last frame

    // Depth SRV into every group that reads it. R32_FLOAT over the scene's
    // R32_TYPELESS - see Scene::CreateDepth, which had to stop using D32_FLOAT
    // for exactly this.
    bool haveDepth = false;
    if (depth)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dd.Texture2D.MipLevels = 1;
        for (int g = 0; g < kGroupCount; ++g)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)(g * kGroupSize + 2) * m_srvStride;
            dev->CreateShaderResourceView(depth, &dd, h);
        }
        haveDepth = true;
    }
    // The motion SRV likewise, or a NULL descriptor - which reads zero, i.e.
    // "nothing moved" - so the table is never left unwritten.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC md = {};
        md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        md.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        md.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        md.Texture2D.MipLevels = 1;
        for (int g = 0; g < kGroupCount; ++g)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)(g * kGroupSize + 3) * m_srvStride;
            dev->CreateShaderResourceView(motion, &md, h);
            h.ptr += m_srvStride;
            dev->CreateShaderResourceView(rtShadow, &md, h);   // RGBA16F as well
            h.ptr += m_srvStride;
            dev->CreateShaderResourceView(rtAo, &md, h);
            h.ptr += m_srvStride;
            dev->CreateShaderResourceView(rtAoPrev, &md, h);
        }
    }
    dev->Release();

    const bool taa = m_taa && haveDepth;

    // current NDC+depth -> previous NDC, as one matrix.
    float reproj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool  reprojOk = false;
    if (sceneViewProj)
    {
        float inv[16];
        if (MatInverse(sceneViewProj, inv))
        {
            MatMul(inv, m_prevViewProj, reproj);
            reprojOk = true;
        }
    }

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(m_rootSig.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);

    auto gpuGroup = [&](int group) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)(group * kGroupSize) * m_srvStride;
        return h;
    };

    // Last frame's exposure, for the HUD. The frame is synchronous, so the copy
    // recorded last frame has landed.
    if (m_readbackPending && m_exposureReadback)
    {
        float* st = nullptr;
        D3D12_RANGE r = { 0, 16 };
        if (SUCCEEDED(m_exposureReadback->Map(0, &r, reinterpret_cast<void**>(&st))))
        {
            m_lastAdaptedLog2 = st[0];
            m_lastMeteredLog2 = st[1];
            m_lastExposure    = st[3];
            D3D12_RANGE none = { 0, 0 };
            m_exposureReadback->Unmap(0, &none);
        }
        m_readbackPending = false;
    }
    m_exposureStateNow = D3D12_RESOURCE_STATE_COMMON;   // decayed with last frame's list
    const bool autoExposure = m_autoExposure && m_psoAdapt && m_exposureState;
    if (!autoExposure) m_lastExposure = m_exposure;

    float consts[28] = {};
    memcpy(consts, reproj, sizeof(reproj));
    consts[16] = 1.0f / (float)width;
    consts[17] = 1.0f / (float)height;
    Jitter(width, height, &consts[18], &consts[19]);
    consts[20] = 0.9f;                                          // history weight
    consts[21] = (m_historyValid && reprojOk) ? 1.0f : 0.0f;
    consts[22] = m_exposure;
    consts[23] = (float)m_curve;
    consts[24] = autoExposure ? 1.0f : 0.0f;
    consts[25] = taa ? m_sharpen : 0.0f;       // nothing to give back without TAA
    consts[26] = m_historyCubic ? 1.0f : 0.0f;
    consts[27] = motion ? 1.0f : 0.0f;

    // The HDR target becomes readable either way.
    {
        D3D12_RESOURCE_BARRIER b = Transition(m_hdr.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
        m_hdrIsTarget = false;
    }

    int toneGroup = kGroupToneHdr;
    if (taa)
    {
        D3D12_RESOURCE_BARRIER pre[3];
        pre[0] = Transition(m_history[writeIndex].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pre[1] = Transition(depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (motion)
            pre[2] = Transition(motion,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(motion ? 3 : 2, pre);

        D3D12_CPU_DESCRIPTOR_HANDLE hrtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        hrtv.ptr += (SIZE_T)(1 + writeIndex) * m_rtvStride;
        list->OMSetRenderTargets(1, &hrtv, FALSE, nullptr);
        list->SetPipelineState(m_psoTaa.Get());
        list->SetGraphicsRootDescriptorTable(0, gpuGroup(readIndex == 0 ? kGroupTaa0 : kGroupTaa1));
        list->SetGraphicsRoot32BitConstants(1, 28, consts, 0);
        if (m_exposureState)
        {
            ExposureStateTo(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->SetGraphicsRootShaderResourceView(2, m_exposureState->GetGPUVirtualAddress());
        }
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER post[3];
        post[0] = Transition(m_history[writeIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        post[1] = Transition(depth,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (motion)
            post[2] = Transition(motion,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(motion ? 3 : 2, post);

        toneGroup = (writeIndex == 0) ? kGroupTone0 : kGroupTone1;
        m_historyValid = true;
    }
    else
    {
        m_historyValid = false;
    }

    // Meter exactly what is about to be tonemapped - the resolved history, or
    // the raw HDR without TAA - and only for the curves that apply exposure.
    if (autoExposure && m_curve >= kCurveExp)
        Meter(list, (toneGroup == kGroupToneHdr) ? m_hdr.Get()
                    : m_history[(toneGroup == kGroupTone0) ? 0 : 1].Get(),
              gpuGroup(toneGroup), width, height);
    else
        m_snapExposure = true;     // whatever it held is stale when it resumes

    list->OMSetRenderTargets(1, &outRtv, FALSE, nullptr);
    list->SetPipelineState(m_psoTonemap.Get());
    list->SetGraphicsRootDescriptorTable(0, gpuGroup(toneGroup));
    list->SetGraphicsRoot32BitConstants(1, 28, consts, 0);
    if (m_exposureState)
    {
        ExposureStateTo(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->SetGraphicsRootShaderResourceView(2, m_exposureState->GetGPUVirtualAddress());
    }
    list->DrawInstanced(3, 1, 0, 0);

    if (sceneViewProj) memcpy(m_prevViewProj, sceneViewProj, sizeof(m_prevViewProj));
    ++m_frame;
}

} // namespace core
} // namespace ngg
