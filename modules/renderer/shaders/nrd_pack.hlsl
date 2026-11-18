// The NRD path's own passes (core/nrd_visibility.cpp), before and after NRD's denoisers. Ours, not NVIDIA's: the encodings NRD
// documents in its NRD.hlsli are written out here (that file is the SDK's and may not be in this repository).
//
// CSPack: our prepass and the raw AO in NRD's guide inputs, at the render size -
//   u0 IN_VIEWZ             R32F: the view matrix's z of the surface (as NRD reconstructs it with the same view and
//                           projection); the sky far beyond NRD's denoising range
//   u1 IN_NORMAL_ROUGHNESS  R10G10B10A2: NRD's signed-octahedron normal (NRD_NORMAL_ENCODING 2), linear roughness 1
//                           (every signal here is occlusion, diffuse), and in the 2-bit alpha a material id: 1 on a
//                           moved object's pixel, 0 elsewhere - REBLUR compares them (nrd_visibility.cpp minMaterialForDiffuse
//                           0), so a car's pixels and the road under its bumper are never averaged, our denoiser's
//                           identity rule
//   u2 IN_MV                RGBA16F: xy previous uv - current uv of the pixel's unjittered centre, the upscalers'
//                           motion (post.hlsl PSUpscaleIn): the camera's reprojection, a moved object's own motion;
//                           z its view Z last frame - this frame's (NRD's "2D + Z delta"): without it a car moving
//                           towards or away from the camera failed NRD's depth test every frame and kept no history
//   u3 IN_DIFF_CONFIDENCE   R8: how much of the AO's history this pixel may keep - 1, or the least that the rays of the
//                           5x5 around it allow where they hit a moved object (rt_common.hlsli Allow, carried in the
//                           raw AO's sign): NRD's way to the "no ghost behind the car" rule. Over the window, as our
//                           denoiser judges it: per pixel, the AO's stratified rays touched the car in every other
//                           pixel, and REBLUR kept history in a checkerboard (its validation layer) - a dither on the
//                           road behind the car. And IN LAST FRAME'S LAYOUT, which is where REBLUR reads it (at each
//                           pixel's previous position): each pixel's confidence goes to where it was (u6, R32_UINT,
//                           the least of those landing there, in 255ths), and CSConf turns that into u3 - read at
//                           the current position, the road scrolling under the chase camera found the car's own
//                           pixels' 1 there, and the car's occlusion lagged behind it
//   u5 IN_DIFF_HITDIST      R16F: what REBLUR_DIFFUSE_OCCLUSION denoises (gAoMode) - 0 the rays' mean normalized hit
//                           distance (rt_ao.hlsl, NRD's own meaning: a near hit occludes more than a far one, so its
//                           AO is lighter than ours), 1 the open fraction of our raw AO: the occluded fraction of the
//                           ambient, our denoiser's quantity, every hit at distance 0 as REBLUR reads it
//
// CSUnpack: one NRD output back into the layout our denoisers' have (Rt::Filtered): RGBA16F, x the visibility, y the
// NDC depth, z the moved-object distrust TAA reads (1 where a ray hit a moved object this frame, fading by 1/8 a
// frame - the last four frames), w 0. Its mode: 0 SIGMA's shadow (NRD stores its square root), 1 REBLUR's occlusion.
// The sky: visibility 1, no distrust.

Texture2D<float>  gIn0 : register(t0);   // pack: depth (reverse-Z, jittered)   unpack: NRD's output
Texture2D<float4> gIn1 : register(t1);   // pack: normal (SNORM8, w plant)      unpack: the raw trace (R16F, EncodeRaw)
Texture2D<float4> gIn2 : register(t2);   // pack: motion (Scene::MotionBuffer)  unpack: last frame's output
Texture2D<float>  gIn3 : register(t3);   // pack: the raw AO (R16F)             unpack: depth
Texture2D<float4> gIn4 : register(t4);   //                                     unpack: IN_MV (pack's u2)
Texture2D<float>  gIn5 : register(t5);   // pack: the AO's hit distance (Rt::NrdIn)

RWTexture2D<float>  gViewZ  : register(u0);   // pack
RWTexture2D<float4> gNormal : register(u1);
RWTexture2D<float4> gMv     : register(u2);
RWTexture2D<float>  gConf   : register(u3);
RWTexture2D<float4> gOut    : register(u4);   // unpack
RWTexture2D<float>  gAoIn   : register(u5);
RWTexture2D<uint>   gConfAt : register(u6);
RWTexture2D<float>  gCut    : register(u7);   // pack: IN_DISOCCLUSION_THRESHOLD_MIX, 1 - the confidence, here   // pack: the confidences by last frame's pixel; CSConf: read, reset

cbuffer K : register(b0)
{
    row_major float4x4 gInvViewProj;   // jittered: the surface as the prepass drew it
    row_major float4x4 gView;
    row_major float4x4 gReproj;        // unjittered NDC -> last frame's clip (post.cpp's)
    row_major float4x4 gPrevView;      // last frame's view
    row_major float4x4 gPrevInvViewProj;   // last frame's unjittered view-projection, inverted
    uint2  gSize;
    float  gMotionOn;
    float  gAoOn;
    uint   gMode;                      // unpack: 0 SIGMA, 1 REBLUR occlusion
    float  gHistoryOn;                 // unpack: last frame's output is this signal's
    uint   gAoMode;                    // pack: what REBLUR denoises (u5)
    float  gScale;                     // unpack: the output's scale (1; NGG_NRD_HISTORY's history length)
};

static const float kSkyViewZ = 1.0e7;   // beyond nrd_visibility.cpp's denoising range

// NRD's signed octahedron (NRD.hlsli _NRD_EncodeNormalRoughness101010, after White 2017): xy the normal folded onto
// the octahedron and rotated 45 degrees, z the roughness signed by the normal's z - never 0, which would lose the sign.
float3 EncodeNormalRoughness(float3 n, float roughness)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;
    roughness = max(roughness, 1.5 / 512.0);
    r.z = (n.z < 0.0 ? -roughness : roughness) * 0.5 + 0.5;
    return r;
}

float3 WorldAt(float2 ndc, float depth)
{
    const float4 wp = mul(float4(ndc, depth, 1.0), gInvViewProj);
    return wp.xyz / wp.w;
}

[numthreads(8, 8, 1)]
void CSPack(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int3 px = int3(tid.xy, 0);
    const float2 uv  = (float2(tid.xy) + 0.5) / float2(gSize);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float depth = gIn0.Load(px);
    const float4 motion = gMotionOn > 0.5 ? gIn2.Load(px) : float4(0, 0, 0, 0);
    const bool moved = motion.w > 0.5;

    // The sky: far beyond the denoising range, on the side of the view the scene is (reverse-Z: a depth near 0 is
    // far), so NRD leaves it alone.
    if (depth <= 0.0)
    {
        const float z = mul(float4(WorldAt(ndc, 1e-7), 1.0), gView).z;
        gViewZ[tid.xy]  = z < 0.0 ? -kSkyViewZ : kSkyViewZ;
        gNormal[tid.xy] = float4(EncodeNormalRoughness(float3(0, 1, 0), 1.0), 0.0);
        gMv[tid.xy]     = float4(0, 0, 0, 0);
        gCut[tid.xy]    = 0.0;
        gAoIn[tid.xy]   = 1.0;
        return;
    }
    const float3 world = WorldAt(ndc, depth);
    const float viewZ = mul(float4(world, 1.0), gView).z;
    gViewZ[tid.xy] = viewZ;

    float3 N = gIn1.Load(px).xyz;
    N = dot(N, N) < 0.25 ? float3(0, 1, 0) : normalize(N);
    gNormal[tid.xy] = float4(EncodeNormalRoughness(N, 1.0), moved ? 1.0 / 3.0 : 0.0);

    float2 prevNdc = ndc;
    float prevViewZ = mul(float4(world, 1.0), gPrevView).z;    // a still surface: where it is, seen from last frame
    if (moved)
    {
        // A moved object: its own motion, and where it was last frame (the prepass's depth there, motion.z).
        prevNdc = ndc + motion.xy;
        const float4 was = mul(float4(prevNdc, motion.z, 1.0), gPrevInvViewProj);
        if (abs(was.w) > 1e-7 && motion.z > 0.0) prevViewZ = mul(float4(was.xyz / was.w, 1.0), gPrevView).z;
    }
    else
    {
        const float4 prevClip = mul(float4(ndc, depth, 1.0), gReproj);
        if (abs(prevClip.w) > 1e-7) prevNdc = prevClip.xy / prevClip.w;
    }
    // uv: y down
    const float4 mv = float4(float2(prevNdc.x - ndc.x, ndc.y - prevNdc.y) * 0.5, prevViewZ - viewZ, 0.0);
    gMv[tid.xy] = mv;

    // EncodeRaw: negative where a ray hit a moved object, -(1 + v + 2 k), k the history it allows in sevenths.
    float conf = 1.0;
    if (gAoOn > 0.5)
    {
        float r = gIn3.Load(px);
        if (r < 0.0)
        {
            r = -r - 1.0;                       // v + 2 k
            r -= 2.0 * floor(r * 0.5);
        }
        gAoIn[tid.xy] = gAoMode == 1u ? r : gIn5.Load(px);
        // A moved object's own pixels keep their history, as in our denoiser (NGG design 4.5): their rays hit the
        // object itself, which moves with them, and REBLUR reprojects them by their own motion.
        if (!moved)
            for (int y = -2; y <= 2; ++y)
                for (int x = -2; x <= 2; ++x)
                {
                    const int2 q = clamp(int2(tid.xy) + int2(x, y), int2(0, 0), int2(gSize) - 1);
                    const float rq = gIn3.Load(int3(q, 0));
                    if (rq < 0.0) conf = min(conf, floor((-rq - 1.0) * 0.5) / 7.0);
                }
    }
    gCut[tid.xy] = 1.0 - conf;
    if (conf < 1.0)
    {
        const int2 at = int2(floor(((float2(tid.xy) + 0.5) / float2(gSize) + mv.xy) * float2(gSize)));
        if (all(at >= 0) && all(at < int2(gSize))) InterlockedMin(gConfAt[at], uint(conf * 255.0 + 0.5));
    }
}

// The confidences in last frame's layout into IN_DIFF_CONFIDENCE - the least over the 3x3: a scatter leaves holes
// where no pixel of this frame landed (a grid of them as the road scrolls towards the camera), and a hole is no news,
// not full confidence - then their scatter target back to "none landed" (255) for the next frame's CSPack, once every
// pixel has read its neighbours (the next dispatch).
[numthreads(8, 8, 1)]
void CSConf(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    uint c = 255u;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            c = min(c, gConfAt[clamp(int2(tid.xy) + int2(x, y), int2(0, 0), int2(gSize) - 1)]);
    gConf[tid.xy] = c / 255.0;
}
[numthreads(8, 8, 1)]
void CSConfReset(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    gConfAt[tid.xy] = 255u;
}

[numthreads(8, 8, 1)]
void CSUnpack(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    const int3 px = int3(tid.xy, 0);
    const float depth = gIn3.Load(px);
    if (depth <= 0.0) { gOut[tid.xy] = float4(1.0, 0.0, 0.0, 0.0); return; }

    const float o = gIn0.Load(px);
    const float vis = gMode == 0u ? o * o : gMode == 2u ? gIn5.Load(px) : o * gScale;   // 2: IN_DIFF_CONFIDENCE (t5)    // SIGMA_BackEnd_UnpackShadow: its output squared

    // The distrust: a ray of this frame hit a moved object (the raw trace's sign), or did in the last four frames -
    // last frame's, where this pixel was then.
    float distrust = gIn1.Load(px).x < 0.0 ? 1.0 : 0.0;
    if (gHistoryOn > 0.5)
    {
        const float2 uv = (float2(tid.xy) + 0.5) / float2(gSize) + gIn4.Load(px).xy;
        if (all(uv > 0.0) && all(uv < 1.0))
        {
            const int2 prev = int2(uv * float2(gSize));
            distrust = max(distrust, gIn2.Load(int3(prev, 0)).z - 0.125);
        }
    }
    gOut[tid.xy] = float4(saturate(vis), depth, saturate(distrust), 0.0);
}
