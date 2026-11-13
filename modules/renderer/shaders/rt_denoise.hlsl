// The RT shadows' and AO's denoiser (core/rt.cpp).
//
// ---------------------------------------------------------------------------
// The visibility denoiser. One stochastic ray per pixel is a coin toss inside
// a penumbra, and the colour TAA cannot be asked to settle it: its variance
// clip exists to REJECT history that disagrees with the neighbourhood, which is
// what a converging penumbra looks like, so patches of it never converge and
// flicker. So the visibility is filtered BEFORE the lighting sees it - a
// spatial blur of this frame's coin tosses, then a temporal blend with last
// frame's filtered result reprojected and validated against depth. The
// lighting then gets a stable term, and the colour TAA has nothing to fight.
// ---------------------------------------------------------------------------

Texture2D<float>   gRaw     : register(t0);   // this frame (EncodeRaw: < 0 = hit a moved object)
// Last frame: x filtered vis, y ndc depth, z 1 = a ray hit a moved object,
// w 1 = the pixel was a moved object's, else 0.45 x the history weight its
// rays allowed (Allow) where they hit one.
Texture2D<float4>  gHist    : register(t1);
Texture2D<float>   gDepth   : register(t2);
// Scene::MotionBuffer: w = 1 where an object moved, xy its NDC offset to last
// frame and z its depth there. Zero (a null view) when there is none.
Texture2D<float4>  gMotion  : register(t3);
RWTexture2D<float4> gOut    : register(u0);   // this frame's, the same layout

cbuffer K : register(b0)
{
    row_major float4x4 gReproj;   // current NDC+depth -> previous NDC (unjittered)
    uint2  gSize;
    float  gBlend;                // history weight
    float  gValid;
    float  gMotionOn;
    float  gBlendMoved;           // history weight where a moved object was hit
    float  gPeriod4;              // 1 = the window spans 4 pixels each way (the AO's)
    float  gBlendStill;           // history weight where the pixel stays put on the screen
};

// The depth one pixel along an axis, as the surface's PLANE predicts it.
// Reverse-Z NDC depth is linear in screen space across a plane, so the
// smaller of the two one-sided differences is the slope of the surface this
// pixel is on (the other may cross a silhouette). A flat 3% depth test on its
// own rejects most of a road seen at a grazing angle - its depth changes by
// that much from one row to the next some 30 m out - and the far road then
// had only its own row to average and its history to lose.
float Slope(float c, float m, float p)
{
    const float a = p - c, b = c - m;
    const float g = (abs(a) < abs(b)) ? a : b;
    return (abs(g) < 0.25 * c) ? g : 0.0;          // no plane: a lone pixel
}

// The group's 12x12 window of depth (negative: a moved object's pixel; 0: sky
// or off the screen) and raw result, loaded once: 25 taps of three textures
// per pixel were most of this pass's cost. And whether each moved pixel moves
// AGAINST THE WORLD there - over half a pixel from where the camera alone
// takes it (a car standing on the turntable, or parked as the camera passes,
// does not).
groupshared float sDepth[144];
groupshared float sRaw[144];
groupshared uint  sAgainst[144];

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int2 origin = int2(gid.xy) * 8 - 2;
    for (uint k = gi; k < 144u; k += 64u)
    {
        const int2 q = origin + int2(k % 12u, k / 12u);
        float d = 0.0, r = 1.0;
        uint against = 0u;
        if (all(q >= 0) && all(q < int2(gSize)))
        {
            d = max(gDepth.Load(int3(q, 0)), 0.0);
            r = gRaw.Load(int3(q, 0));
            const float4 m = (gMotionOn > 0.5) ? gMotion.Load(int3(q, 0)) : float4(0.0, 0.0, 0.0, 0.0);
            if (m.w > 0.5)
            {
                const float2 n = float2((q.x + 0.5) / gSize.x * 2.0 - 1.0, 1.0 - (q.y + 0.5) / gSize.y * 2.0);
                const float4 c = mul(float4(n, d, 1.0), gReproj);
                const float2 off = abs(c.w) > 1e-7 ? (n + m.xy - c.xy / c.w) * 0.5 * float2(gSize) : float2(1e3, 1e3);
                against = dot(off, off) > 0.25 ? 1u : 0u;
                d = -d;
            }
        }
        sDepth[k]   = d;
        sRaw[k]     = r;
        sAgainst[k] = against;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int2 px = int2(tid.xy);
    const int2 lp = int2(tid.xy) - origin;          // px in the window
    const float depth = abs(sDepth[lp.y * 12 + lp.x]);
    if (depth <= 0.0) { gOut[px] = float4(1.0, 0.0, 0.0, 0.0); return; }     // sky

    // SPATIAL: a depth-aware 5x5 mean of this frame's tosses. Nine-to-
    // twenty-five samples cut the per-frame variance by that much before the
    // temporal blend ever sees it, which is most of why this converges in a
    // few frames rather than a few dozen. Depth-aware so a shadow does not
    // bleed across a silhouette onto the thing behind it.
    //
    // And IDENTITY-aware: a moved object's pixels and static ones never mix,
    // even at the same depth. The road just under a bumper is within 3% of
    // it, so the back of the car - in its own shadow - was averaged into a
    // thin strip of road every frame, and the history carried each strip off
    // as the road went by: faint bands, one per frame, behind the car.
    //
    // For the AO, whose samples repeat every 4 pixels (AoSample), the outer
    // ring weighs a half, so each axis spans exactly one period: every cell
    // of the grid counts once, and the tile leaves no pattern of its own.
    const bool movedPixel = sDepth[lp.y * 12 + lp.x] < 0.0;
    // A moving object STANDING here - with the world: stopped on the turntable, parked as the camera passes: one of
    // its pixels in the 5x5 and none that moves against the world (sAgainst). Only there may its pixels and the
    // world's pass each other - at a standing car's outline the jitter swaps them, frame by frame. NOT where none is
    // here now: the object has GONE (as TAA's Gone), and what it uncovered must not take its history. Read as
    // "nothing here moves against the world", that let the road a car had covered keep the car's own lit visibility:
    // under a still TV camera (0.95 a frame) and far enough off that the roof is within the depth test of the road,
    // copies of the car on the shaded road behind it - several, a frame of travel apart (golden replay, --step 0.1).
    bool against = false, near = false;
    [unroll] for (int ay = -2; ay <= 2; ++ay)
    [unroll] for (int ax = -2; ax <= 2; ++ax)
    {
        const int k = (lp.y + ay) * 12 + lp.x + ax;
        near    = near    || sDepth[k] < 0.0;
        against = against || sAgainst[k] != 0u;
    }
    const bool standing = near && !against;
    float sum = 0.0, sum2 = 0.0, wsum = 0.0;
    bool  touched = false;      // a ray in the window hit a moved object
    float allow = 1.0;          // the least history weight those rays allow
    const int c0 = lp.y * 12 + lp.x;
    const float2 slope = float2(Slope(depth, abs(sDepth[c0 - 1]),  abs(sDepth[c0 + 1])),
                                Slope(depth, abs(sDepth[c0 - 12]), abs(sDepth[c0 + 12])));
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        const int k = (lp.y + y) * 12 + lp.x + x;
        const float ds = sDepth[k];
        float d = abs(ds);
        if (d <= 0.0) continue;
        // Linear depth ratio from reverse-Z NDC: 1/z is proportional to
        // distance, so this is a relative distance test - against the depth
        // the pixel's plane predicts there (Slope).
        const float e = max(depth + dot(slope, float2(x, y)), 1e-7);
        float rel = abs(1.0 / d - 1.0 / e) * e;
        const bool qMoved = ds < 0.0;
        float w = (rel < 0.03 && (qMoved == movedPixel || standing)) ? 1.0 : 0.0;
        if (gPeriod4 > 0.5) w *= (abs(x) == 2 ? 0.5 : 1.0) * (abs(y) == 2 ? 0.5 : 1.0);
        float r = sRaw[k];
        if (r < 0.0)
        {
            r = -r - 1.0;                       // v + 2 k (EncodeRaw)
            const float a = floor(r * 0.5);
            r -= 2.0 * a;
            if (w > 0.0) { touched = true; allow = min(allow, a / 7.0); }
        }
        sum  += r * w;
        sum2 += r * r * w;
        wsum += w;
    }
    float curr = sum / max(wsum, 1e-6);
    if (wsum <= 0.0)
    {
        const float r = gRaw.Load(int3(px, 0));
        const float a = floor((-r - 1.0) * 0.5);
        curr = r < 0.0 ? -r - 1.0 - 2.0 * a : r;
        touched = r < 0.0;
        if (touched) allow = a / 7.0;
    }

    // TEMPORAL: reproject into last frame, homogeneous so the sky/far case
    // needs no special handling (see post.cpp), and accept the history only
    // where it landed on the same surface - same linear depth within 3%.
    // A MOVED object's pixel takes the prepass's motion instead: by the
    // camera alone it lands where the object is not, and loses its history.
    float2 uv  = (float2(px) + 0.5) / float2(gSize);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 mo  = (gMotionOn > 0.5) ? gMotion.Load(int3(px, 0)) : float4(0, 0, 0, 0);
    float4 pc  = movedPixel ? float4(ndc + mo.xy, mo.z, 1.0)
                            : mul(float4(ndc, depth, 1.0), gReproj);
    float  blend = 0.0;
    float2 hist = float2(curr, depth);
    float  histDistrust = 0.0;    // the history's z where it was taken, else 0
    float  histAllow = 0.0;       // and the weight its rays allowed
    float  histYouth = 0.0;       // a moved pixel's history: 8 one frame old, down to 0 (w)
    if (gValid > 0.5 && abs(pc.w) > 1e-7)
    {
        float3 pn = pc.xyz / pc.w;
        float2 puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
        if (all(puv >= 0.0) && all(puv <= 1.0) && pn.z > 0.0)
        {
            int2 pp = int2(puv * float2(gSize));
            pp = clamp(pp, int2(0, 0), int2(gSize) - 1);
            float4 h = gHist.Load(int3(pp, 0));
            // STILL: a pixel that stays put on the screen (under a quarter
            // pixel of motion, as TAA's `still`) reads its history without
            // resampling it, so a longer one costs no blur: from 0.85 it goes
            // to gBlendStill. One ray a pixel, among three spotlights in the
            // Garage, averaged over 5x5 and a dozen frames left the floor's
            // shadows blotchy under a camera that never moves, the car
            // standing or turning; three dozen halve that. The neighbourhood
            // clamp below still pulls a stale value in at once where every
            // sample disagrees with it. A moving object's pixel too, by its own
            // motion: the Garage's car stopped is still, as it was before it
            // counted as moving at all (scene.cpp movedOf).
            const float still = saturate(1.0 - length(puv * float2(gSize) - (float2(px) + 0.5)) * 4.0);
            const float top = lerp(gBlend, gBlendStill, still);
            // Ground the car uncovered: last frame this spot was the moved
            // object, so its history is cut below - and that, too, is "a moved
            // object was involved". Without it the distrust broke on every
            // such strip: they started over as merely fresh, and TAA, which
            // reads the distrust (post.cpp), kept the shadow each strip was
            // first seen in - a band behind the car per frame.
            if (h.y > 0.0 && h.w > 0.5 && !movedPixel && !standing) { touched = true; allow = 0.0; }
            const bool histTouched = h.z > 0.5;
            // IDENTITY first, as TAA's (post.cpp): a moved object's history
            // never passes to a static pixel, nor the reverse. Depth cannot
            // tell them apart where they meet - the road just behind a
            // bumper is within 3% of the bumper - and the back of a car, in
            // its own shadow, then printed a dark band of itself on the sunlit
            // road behind it every frame.
            if (h.y > 0.0 && ((h.w > 0.5) == movedPixel || standing))
            {
                float rel = abs(1.0 / h.y - 1.0 / pn.z) * pn.z;
                // The history is read at the nearest pixel, up to half a pixel
                // from where this one landed: on a slope that alone is half a
                // pixel's depth change, which the 3% is widened by.
                const float tol = 0.03 + 0.5 * (abs(slope.x) + abs(slope.y)) / depth;
                // MOVED OCCLUDERS. Where a ray hit something that moved -
                // this frame, or last - the history holds where its shadow
                // or its occlusion WAS, not where it is: kept at 0.85 it lays
                // a fading copy on the ground every frame, which is the trail
                // behind a moving car. The neighbourhood clamp below cannot
                // catch it under the car, where the ground stays partly in
                // shadow either way. So there, the history counts for little.
                // Not on the moved object itself: its pixel was reprojected
                // by its own motion, and the occluder its rays hit is mostly
                // itself, moving with it - the car's self-shadow is as valid
                // a history as a wall's, and dropping it only makes it boil.
                //
                // How little is the rays' to say (Allow): the occluder's
                // travel this frame against the softness of what it casts.
                // Where none hit it now, the shadow has just left: the weight
                // its rays allowed then, kept in the history's w.
                const bool distrust = !movedPixel && (touched || histTouched);
                if (rel < tol)
                {
                    hist.x = h.x;
                    histAllow = histTouched ? h.w / 0.45 : 0.0;
                    blend = distrust ? clamp(touched ? allow : histAllow, gBlendMoved, top) : top;
                    histDistrust = histTouched ? h.z : 0.0;
                    // A YOUNG moved pixel's history - the car just out from
                    // behind a panel, which cut it - holds only the frames
                    // since: a running mean of them, not 0.85 of the first
                    // one's raw 5x5, which kept the car blotchy for a dozen.
                    // (A grown one is left to `top`: a standing car's still.)
                    if (movedPixel)
                    {
                        histYouth = round((1.0 - h.w) * 32.0);
                        const float frames = 9.0 - histYouth;
                        if (histYouth > 0.0) blend = min(blend, frames / (frames + 1.0));
                    }
                }
            }
        }
    }
    // NEIGHBOURHOOD CLAMP. The depth test above proves the history is the
    // same SURFACE, not that its lighting is the same: when a shadow moves -
    // a car's, over a road that did not - the history still holds where it
    // was, and at 0.85 a dark patch takes a dozen frames to fade, trailing the
    // car. So the history may not leave the range this frame's own samples
    // allow: their mean +- their spread, plus a margin for what 25 samples
    // cannot resolve. Inside a penumbra the spread is wide and nothing
    // changes; where every sample now says lit (or shadow), a stale opposite
    // is pulled to within the margin at once.
    if (blend > 0.0 && wsum > 0.0)
    {
        const float sigma  = sqrt(max(0.0, sum2 / wsum - curr * curr));
        const float spread = sigma + 0.5 / sqrt(wsum);
        hist.x = clamp(hist.x, curr - spread, curr + spread);
    }
    float vis = lerp(curr, hist.x, blend);
    // The depth for next frame's test, and the two flags it reads: whether a
    // moved object was involved (so next frame still distrusts this answer)
    // and whether this pixel is one. z is 0.5 where no history was taken at
    // all - next frame reads only z > 0.5; kAoWideSrc reads both.
    //
    // The distrust FADES: 1 where a ray hit a moved object this frame, then
    // 0.125 less each frame after, so the four frames after a shadow has left
    // still take only gBlendMoved of the past. One frame of it was not
    // enough: the few percent of old shadow it kept then faded at gBlend, and
    // stayed over 1% for half a dozen frames - a trail too faint to see on a
    // textured road, but there.
    const float fresh = blend <= 0.0 ? 0.5 : 0.0;
    const float fading = histDistrust - 0.125;
    const float level = touched ? 1.0 : (fading > 0.5 ? fading : fresh);
    const float allowed = touched ? allow : (fading > 0.5 ? histAllow : 0.0);
    // A moved pixel's w counts its history's youth down in 32nds, 8 where it
    // started over: 0.75 to 1, still > 0.5 for the identity tests.
    const float youth = blend <= 0.0 ? 8.0 : max(histYouth - 1.0, 0.0);
    gOut[px] = float4(vis, depth, level, movedPixel ? 1.0 - youth / 32.0 : 0.45 * allowed);
}
