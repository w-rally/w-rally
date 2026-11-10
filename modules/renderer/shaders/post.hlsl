// Post: TAA, the tonemap and the upscalers' inputs (core/post.cpp).
//
// One fullscreen triangle from SV_VertexID - no vertex buffer, no input layout,
// and no seam down the diagonal that the two-triangle version has.

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
// The scene prepass's motion (Scene::MotionBuffer): w = 1 on a moving object,
// with xy its NDC offset to last frame and z its depth there. t4, as t3 is
// the exposure state's root SRV.
Texture2D    gMotion  : register(t4);
// The ray-traced shadow's denoised result (Rt::Filtered): z > 0.5 where a ray
// hit a moved object this frame or in the last four (rt.cpp's distrust). A
// NULL descriptor without RT shadows - zero, nothing distrusted.
Texture2D    gShadow  : register(t5);
Texture2D    gAo      : register(t6);   // the RT AO's denoise (Rt::AoDenoised), likewise
Texture2D    gAoPrev  : register(t7);   // and last frame's (Rt::AoPrevDenoised)
// A moving object's glass, its own motion (Scene::GlassMotionBuffer): x < 6 where it is in front. Read only with
// gMotionOn (a NULL descriptor reads 0, which would be glass everywhere).
Texture2D    gGlass   : register(t8);
Texture2D    gLights  : register(t9);   // the RT local lights' denoise (Rt::LightsDenoised): z as gShadow's
Texture2D    gSettled : register(t10);  // the history's companion: how long each pixel has stood still (PSTaa)
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

// ---------------------------------------------------------------------------
// TAA
// ---------------------------------------------------------------------------
// Where the camera alone takes a pixel (NDC and its reverse-Z depth) to last frame, in NDC; far off the screen where
// it was behind the camera.
float2 CameraPrev(float2 ndc, float depth)
{
    const float4 c = mul(float4(ndc, depth, 1.0), gReproj);
    return abs(c.w) > 1e-7 ? c.xy / c.w : float2(4.0, 4.0);
}

// How far the MOVING OBJECTS about a static pixel went this frame AGAINST THE WORLD there, in pixels: the most any of
// this frame's pixels of one (or of its glass) in the 4x4 from `q` moves other than the camera alone would take it -
// nothing for a car standing on the turntable, or parked on a stage under a camera passing it; a frame's turn for one
// spun past - or 1e3 where there is none there now: it has gone. Its pixels, not the history's texels one by one: at
// a standing car's outline the jitter takes a pixel from the car one frame and gives it back the next.
float Gone(int2 q)
{
    float most = -1.0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const int3 at = int3(q + int2(x, y), 0);
        const float2 g = gGlass.Load(at).xy;
        const float4 m = gMotion.Load(at);
        if (g.x < 6.0 || m.w > 0.5)
        {
            const float2 n = float2((at.x + 0.5) * gTexel.x * 2.0 - 1.0, 1.0 - (at.y + 0.5) * gTexel.y * 2.0);
            const float2 off = n + ((g.x < 6.0) ? g : m.xy) - CameraPrev(n, gDepth.Load(at).r);
            most = max(most, length(off * 0.5 / gTexel));
        }
    }
    return most < 0.0 ? 1e3 : most;
}

struct TaaOut { float4 colour : SV_Target0; float settled : SV_Target1; };

TaaOut PSTaa(VSOut i)
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
    // A MOVING OBJECT'S GLASS (Scene's PSGlassMotion): not in the depth, so the motion target holds what is behind
    // it - the dark beside a turning car's headlamp lens (a still pixel: the lens smeared across it at 0.97 a frame),
    // or the car's interior behind a side window (its letters came out twice at a fast spin). It reprojects by its
    // own motion onto the glass it was - so it is "still" (below) by that motion, as anything else, standing - and
    // is marked in the history as glass, so the pixels it leaves drop what it laid there.
    const float2 glassMv = (gMotionOn > 0.5) ? gGlass.SampleLevel(sPoint, uv, 0).xy : float2(8.0, 8.0);
    bool         glass  = glassMv.x < 6.0;
    // A moving object STANDING with the world here: within a tenth of a pixel of where the camera alone takes this
    // pixel - a car stopped on the turntable, or parked as the camera passes. Nothing it covers or uncovers then, and
    // it is reprojected and judged as the world is (`moved` and `glass` are for an object on the move): its outline,
    // which the jitter gives to the car one frame and to what is behind it the next, keeps its history as the world's
    // does. It stays marked a moving object's in the history (below), for the frame it moves again.
    const bool   moving = glass || motion.w > 0.5;
    const float2 motionNdc = ndc + (glass ? glassMv : motion.xy);
    const bool   standing = moving && length((motionNdc - CameraPrev(ndc, depth)) * 0.5 / gTexel) < 0.1;
    const bool   pane   = glass;
    const bool   moved  = !glass && moving && !standing;
    glass = glass && !standing;
    float2 prevUv = float2(0.0, 0.0);
    float  prevDepth = 0.0;               // reverse-Z depth this surface had then
    bool   usable;
    if (moved || glass)
    {
        prevUv    = float2(motionNdc.x * 0.5 + 0.5, 0.5 - motionNdc.y * 0.5);
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
    // where it belonged to a moving object (see the return below). The road a
    // car has just uncovered reprojects correctly by the camera, but last
    // frame that spot was the car - and the colour clip below cannot tell a
    // dark tyre from a dark patch of road, so it lets part of it through,
    // frame after frame: copies of the car's back laid on the road behind it.
    //
    // Decided by IDENTITY, not by depth. A depth test cannot see this case:
    // where a tyre meets the road the two ARE at the same depth, and the road
    // just behind a bumper is within a few percent of it. So a static pixel
    // whose history fetch touches a texel of a moving object that has gone
    // from there (Gone) drops its history outright - the cost is one
    // unaccumulated frame along the car's outline, which the next frame
    // accumulates again. A moving object standing still has not gone: the
    // world beside it keeps its history, as beside anything else. A moved
    // pixel keeps it if one texel of the 2x2 it straddles is of a moving
    // object at the depth it had, so the car's own edges still accumulate; a
    // glass pixel if one is glass. Static against static is left exactly as
    // it was - the camera reprojection is exact there.
    bool fresh = false;     // the history there had none itself: one raw frame (alpha + 2, below)
    bool masked = false;    // a moved pixel behind a still occluder: its own texels only (below)
    float4 own = 0.0;       // and their bilinear weights, in GatherAlpha's order
    float gone = 0.0;       // the furthest a moving object beside a static pixel went (Gone), in pixels
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
        if (glass)
            usable = any(hd < -3.0);
        else if (!moved)
        {
            // The 4x4 the history is read from, where it is on the screen now (the camera's shift).
            if (any(min(min(a, b), min(c, d)) < 0.0))
                gone = Gone(int2(floor(prevUv / gTexel - 0.5)) - 1 + int2(round((uv - prevUv) / gTexel)));
            usable = gone < 1.0;
            fresh  = any(hd > 1.5);
        }
        else
        {
            float4 same = 0.0;
            [unroll] for (int k = 0; k < 4; ++k)
            {
                const float h = -hd[k];             // > 0 only for a moving object's texel
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
    // The RT local lights' shadows (the Garage's three spots) as the sun's.
    //
    // And beside a MOVING OBJECT that moved, however little (`gone`): what
    // the static pixel keeps is that object's edge where it was, and kept
    // unclipped at 0.97 a car turning a fraction of a pixel a frame laid a
    // soft copy of its outline behind it. Still only as far as it stood.
    const int3   px = int3(i.pos.xy, 0);
    // (The world's: on a moving object, standing or not, its rays hit mostly itself.)
    const bool   shadowMoved = !moving && usable && (gShadow.Load(px).z > 0.5 || gLights.Load(px).z > 0.5);
    const bool   aoMoved     = !moving && usable && gAo.Load(px).z > 0.5;
    // And only as far as the pixel has STOOD (`settled`, kept beside the
    // history): from none to all over 16 frames still, back down by a
    // quarter of what it moved over the quarter pixel. A history that moved holds the softening
    // of every frame it was resampled at, and kept at once at 0.97 when it
    // stopped, that took a second to go - the car stopped by the mouse on the
    // Garage's turntable came sharp slowly, a settle seen; before, its whole
    // history was dropped there, and the edges jumped.
    const float  settledWas = (gValid > 0.5 && usable) ? gSettled.SampleLevel(sPoint, prevUv, 0).r : 0.0;
    const float  settled = (travel < 0.25) ? min(settledWas + 1.0 / 16.0, 1.0) : max(settledWas - (travel - 0.25) * 0.25, 0.0);
    const float  still  = (usable && !shadowMoved && !aoMoved)
                        ? saturate(1.0 - travel * 4.0) * saturate(1.0 - gone * 4.0) * settled : 0.0;
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
    // A MOVING OBJECT AT SPEED keeps less: its history is resampled every
    // frame, and of a turning surface seen anew each frame, and at 0.9 the
    // softening compounded - the car spun fast in the Garage came out blurred
    // (and stopped, took that blur into the long still history). From 0.9 at
    // eight pixels a frame to 0.6 at forty: under that the car the camera
    // follows on a stage, whose RT grain a shorter history shows (at two
    // pixels, +20% behind the panel on track 42). Spun at 10 degrees a frame,
    // the car's difference from its standing picture 23.1 -> 19.8 (x1e-3).
    if ((moved || glass) && travel > 8.0) blend = min(blend, gBlend - 0.3 * saturate((travel - 8.0) / 32.0));
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
    // Alpha: this pixel's depth, negative if it belongs to a moving object, less
    // 4 more for its glass - what next frame's disocclusion test reads - and
    // plus 2 for a static one that took no history (`fresh` above); static
    // depth is read for its sign only. Nothing displays it: the tonemap and
    // the meter read rgb.
    TaaOut o;
    o.colour  = float4(outc, pane ? -depth - 4.0 : moving ? -max(depth, 1e-7) : (usable ? depth : depth + 2.0));
    o.settled = settled;
    return o;
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

// THE UPSCALER'S INPUTS (upscaler.h), at render size: every pixel's motion, which our TAA works out per pixel from
// the depth and the prepass and an upscaler needs as a texture - the same reprojection PSTaa makes, as (previous uv -
// current uv) of the pixel's unjittered centre, a moving object's glass by its own (PSTaa) - and the reactive mask:
// that glass is a blend over what is behind it, which moves otherwise, so it keeps a little less history.
struct UpscaleIn { float2 motion : SV_Target0; float reactive : SV_Target1; };

UpscaleIn PSUpscaleIn(VSOut i)
{
    const float2 uv = i.uv;
    const float depth = gDepth.SampleLevel(sPoint, uv, 0).r;
    const float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 motion = (gMotionOn > 0.5) ? gMotion.SampleLevel(sPoint, uv, 0) : float4(0, 0, 0, 0);
    const float2 glassMv = (gMotionOn > 0.5) ? gGlass.SampleLevel(sPoint, uv, 0).xy : float2(8.0, 8.0);
    const bool   glass = glassMv.x < 6.0;
    float2 prevNdc = ndc;
    if (glass)
        prevNdc = ndc + glassMv;
    else if (motion.w > 0.5)
        prevNdc = ndc + motion.xy;
    else
    {
        const float4 prevClip = mul(float4(ndc, depth, 1.0), gReproj);
        if (abs(prevClip.w) > 1e-7) prevNdc = prevClip.xy / prevClip.w;
    }
    UpscaleIn o;
    o.motion   = float2(prevNdc.x - ndc.x, ndc.y - prevNdc.y) * 0.5;    // uv: y down
    o.reactive = glass ? 0.25 : 0.0;
    return o;
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
