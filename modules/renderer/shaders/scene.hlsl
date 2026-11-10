// The scene pass (core/scene.cpp): the prepass, the main pass (opaque, alpha-tested, blended, deferred G-buffer and the
//
// Declared row_major and multiplied as mul(vector, matrix) so the matrices RBR
// hands us work unchanged. D3D9 uses row-vector convention; HLSL defaults to
// column-major storage and mul(matrix, vector). Getting this wrong does not
// produce an error - it produces a plausible-looking but subtly wrong camera,
// which is far worse, so it is stated explicitly rather than transposed
// somewhere on the C++ side where it would be easy to lose.
// reflection focus's ray-query variant), the shadow maps' and the local lights' maps, the deferred local lights.
// Variants by define: ALPHA_TEST, BLEND, GBUF, NGG_REFL_RAY (shader model 6.5). Compiled with DXC (-HV 2018), or FXC
// (5.1) as the fallback in a runtime compile (core/shaders.h).
#include "atmosphere.hlsli"
#define NGG_LIGHTS_T0 t12
#define NGG_LIGHTS_T1 t13
#define NGG_LIGHTS_T2 t14
#include "lights.hlsli"
Texture2D<float>        gRtLights : register(t15);
Texture2DArray<float>   gLocalShadow : register(t21);
Texture2DArray<float>   gLocalShadowFlag : register(t22);
Texture2D<float4>       gLocalShadowMaskPrev : register(t23);
SamplerComparisonState  gShadowSamp : register(s3);
SamplerState            gSampVolume : register(s4);

cbuffer Camera : register(b0)
{
    row_major float4x4 gView;
    row_major float4x4 gProj;
    float3             gLightDir;
    float              gPhysical;     // 1 = physical lighting balance (5.0i)
    float3             gWorldOffset;
    float              gBakedScale;
    float              gDebugMode;
    float3             gCamPos;
    float3             gSunTowards;     // direction TOWARDS the sun
    float              gSunIntensity;
    float              gTurbidity;
    float              gExposure;
    float              gAerial;         // 0 = off, 1 = on
    float              gLightMode;      // 0 = baked reference, 1 = dynamic
    // FROM HERE ON, EVERY GROUP IS EXACTLY ONE float4.
    //
    // HLSL packs constants into float4 registers and will not let a float3
    // straddle a register boundary - it inserts padding. A tightly packed C++
    // struct does not. When gGroundColour was added after two floats, HLSL
    // pushed it to the next register and every field after it read the wrong
    // two floats: gProbeOn read padding (always 0, so the probe never fired),
    // and the bounce colour, bounce strength and bake mean read each other.
    //
    // Grouping in whole float4s makes the two layouts agree by construction
    // rather than by careful counting.
    float              gSunDiffuse;
    float              gSkyAmbient;
    float              gAerialMaxDist; // radial metres the froxel table reaches
    float              gAerialLut;     // 1 = read the table, 0 = march per pixel
    float3             gGroundColour;
    float              gGroundBounce;
    float2             gProbePixel;    // which pixel reports its terms
    float              gProbeOn;
    float              gEnvOn;         // ambient SPECULAR: 0 = off, 1 = on
    float              gEnvMips;       // roughness levels in the environment map
    float              gEnvReady;      // the map exists; separate from gEnvOn so
                                       // toggling specular does not also change
                                       // the diffuse path and muddle the A/B
    float              gRtReady;       // this frame's RT visibility is bound
    float              gShadowSource;  // 0 = cascaded shadow map, 1 = ray traced
    float              gAoReady;       // this frame's RT ambient occlusion is bound
    float              gAoOn;          // and the user wants it applied
    float2             gInvScreen;     // 1/width, 1/height: SV_Position -> table uv
    float3             gCamFwd;        // for cascade selection by view depth
    float              gLegacyMode;    // the legacy profile (Scene::SetLegacyProfile): 0 none, 1 A, 2 B
    float4             gCascadeSplit;  // view-space far distance per cascade
    row_major float4x4 gLightViewProj[4];
    float4             gShadowParams;  // x = on, y = texel size, z = normal bias
    float              gCloud;         // weather (5.0q): cloud cover
    float              gFogBeta;       //                 ground fog extinction, 1/m
    float              gFogH;          //                 fog scale height, m
    float              gWetness;       // wet surfaces (5.0v): 0 dry .. 1 wet
    // Motion (the prepass's second target). Both WITHOUT the TAA jitter, for
    // the reason Scene::LastViewProj gives: a motion vector carrying the
    // jitter would drag the history along the Halton pattern.
    row_major float4x4 gViewProjNoJitter;
    row_major float4x4 gPrevViewProj;  // last frame's gViewProjNoJitter
    // Local lights (lights.h): the clusters' tile (px), near plane and slice scale; lights on; their RT fraction on.
    float4             gClusterParams;
    float              gLightsOn;
    float              gLightsVisOn;
    float              gReflProbeOn;   // the cube probe (probe.h) is whole: the focus's objects read it
    float              gReflProbeMips;
    // The deferred local lights' pass (PSDeferredLights): the jittered view-projection's inverse - the matrix the
    // depth was written with.
    row_major float4x4 gInvViewProj;
    // The cube probe's faces only (probe.h): the car they leave out, as a box - its centre (w 1: on) and its three
    // axes, each with its half extent in w - so the ground still gets its shadow and the sky it hides (CarBoxVis).
    float4             gCarBoxC;
    float4             gCarBoxX, gCarBoxY, gCarBoxZ;
    // The local lights' shadow maps (Lights::ShadowCount, Scene::RenderLocalShadows) - the cascades' path only: each
    // slot's world -> clip, its tan across / up, near and far; x 1: drawn this frame, y 1 / the map's size, z the
    // screen-space AO's share of the local lights' diffuse (0 by day and with the ray tracer).
    row_major float4x4 gLocalShadowVP[4];
    float4             gLocalShadowInfo[4];
    float4             gLocalShadowParams;
};

// Cascaded shadow maps. A comparison sampler so the hardware does the depth
// test and the 2x2 PCF in one fetch.
// Per-object data, pushed as root constants because it changes every draw.
//
// The transform is a WORLD-SPACE delta on top of geometry that is already baked
// into world space: .ngg bakes instance placements into vertices, so there is no
// local space to place an object in. Identity for everything the file loaded,
// non-identity once the editor moves something. Rotating about an object's own
// centre is the caller's job to compose (translate(-c) * R * translate(c)) --
// see ngg_object_bounds, which exists so it can.
//
// 20 DWORDs: a uint, three of padding to reach the float4 boundary, then the
// matrix. Declared explicitly rather than left to packing rules, because the
// C++ side writes at these offsets.
// Mirrors kMaterialSingleStage in ngg_format.h. A static_assert in
// Scene::Load ties the two together, because a shader constant that silently
// drifts from the format's flag would turn every object mesh single-stage or
// none of them, with no error either way.
#define NGG_MAT_SINGLE_STAGE 256u
#define NGG_MAT_NORMAL_MAP   512u
// An instanced model's material (Scene::kDrawModel), and its alpha edge dithered (kDrawHashedAlpha: foliage).
#define NGG_DRAW_MODEL       1024u
#define NGG_DRAW_HASHED      2048u

// --- microfacet specular, metallic-roughness ------------------------------
// Value noise on the world's xz, for where puddles lie (design 5.0v): fixed to the ground, so a puddle stays put as the
// camera moves. The hash takes the cell's integer coordinates mod 4096 (puddle cells are metres: kilometres of stage
// repeat nowhere a driver can see), so it stays exact in float far from the origin.
float WetHash(float2 c)
{
    c = fmod(c, 4096.0) + 0.5;
    float3 p = frac(float3(c.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}
float WetNoise(float2 p)
{
    float2 c = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(WetHash(c), WetHash(c + float2(1, 0)), f.x),
                lerp(WetHash(c + float2(0, 1)), WetHash(c + float2(1, 1)), f.x), f.y);
}

// Declared ahead of its use; defined after the BRDF helpers below.
float3 LocalLights(float3 p, float3 N, float3 V, float3 diffuseCol, float3 f0, float a, float specScale, float2 px,
                   float wrap, float trans, bool withSpec, float2 coat);

#include "brdf.hlsli"
// The sun's and the moon's discs, ~0.27 degrees of radius: their highlight is widened by it as a lamp's is by its
// emitter (ShadeLocalLight - Karis 2013), sin(r) / 2.
static const float kSunDiscWiden = 0.0024;

// THE LOCAL LIGHTS at this pixel (lights.h, design 5.4 N2): every light of its cluster, unshadowed - Lambert and GGX
// as the sun's - then the shadowed ones times the ray tracer's denoised shadowed fraction (the ratio estimator: one
// ray per pixel to one light chosen by what it gives here, lights.h). `wrap` softens the cosine (a foliage card's),
// `withSpec` adds the highlight. Glass takes the fraction as 1: the traced pixel is what lies behind.
//
// The highlight is an EMITTER's, not a point's: the lobe widened by the disc's angular size (Karis 2013's widened
// roughness for sphere lights, without the representative point - the lobe blurred, its energy kept). A point's
// highlight on a wet road (roughness at its 0.002 floor) would peak near 1e5 x the irradiance, past half floats.
//
// HOW A WAVE WALKS ITS LIGHTS. A wave's pixels mostly share one cluster, so under shader model 6 the wave takes its
// clusters in turn - the first remaining lane's (WaveReadLaneFirst), shaded by every lane that has it - and within
// one the cluster, the indices and the records are WAVE-UNIFORM: scalar loads through the scalar cache, no per-lane
// waits and no vector registers for them. Bounded at 4 clusters a wave; what is left - a wave across more depth
// slices or tiles, and helper lanes, which wave intrinsics may not count as active - takes the per-pixel loop. FXC's
// fallback (5.1) has only that.
//
// WHAT A LIGHT COSTS here is its arithmetic, per FRAGMENT: ~130 vector instructions in one dependent chain, which at
// this shader's occupancy (4 waves per SIMD on the development GPU, a Radeon 780M - 117 VGPRs) stalls more than it
// computes. PIX's per-draw timings put it where the fragments are: three lights reaching most of the screen add ~3 ms
// to the main pass at 1080p, in proportion to each draw's pixel shader invocations - foliage's overdraw included.
// Scalar loads (above), the cone culling of the clusters (lights.cpp) and the early exit below each took little
// off it - fewer fragments did: the opaque world's lights are shaded once per PIXEL by the deferred pass
// (PSDeferredLights), only glass here (design 5.4 N2c).
// `trans`: a foliage card's transmission, as for the sun - a leaf with the lamp behind it is backlit, not black
// (design 5.4 N3: grass tufts lit by the lamps had black lower halves, their volume normals turned away).
// THE LOCAL LIGHTS' SHADOW MAPS (Lights::ShadowCount, Scene::RenderLocalShadows) - the cascades' path, where no ray
// tracer shadows them: a few spots and beams (the Garage's three spots, the player's headlamps) each have a
// perspective depth map from the lamp. Their emitters are big (the Garage's spots are 0.6 m discs), so a hard map's
// edge would be the wrong shadow: PERCENTAGE-CLOSER SOFT SHADOWS (Fernando 2005) - a blocker search over the region
// the emitter could be hidden from (kLocalSearch gathers of 4 texels), the penumbra from the blockers' mean distance
// (the emitter's radius x (receiver - blocker) / blocker), then kLocalPcf comparison taps over it (bilinear 2x2 each). The taps are turned per pixel
// (interleaved gradient noise) so a wide penumbra is a fine grain, not bands. The receiver is pushed off its surface
// along the normal by a texel and a half of the map at its distance, and a little towards the lamp: no acne.
static float2 gShadePx = float2(0.0, 0.0);   // the pixel being shaded (LocalLights): the taps' turn
static float  gLocalDiffAo = 1.0;              // the screen-space AO's share of the local lights' diffuse (callers)
// The last map read at this pixel and what it gave: the lights of one mounting share a map (Lights::Build) and come
// one after another in a cluster's list, so a ramp's four pods read it once. Two scalars, not an array by slot: a
// static array indexed by a variable would be scratch memory.
static uint   gShadowSlotLast = 0u;
static float  gShadowVisLast = 1.0;
static float  gShadowMovedLast = 0.0;
// Whether a MOVING object (DrawItem::dynamic: the car turning on the turntable) shadows this pixel from any lamp
// (LocalLights -> PSDeferredLights' moved-shadow mask, which TAA reads as it reads the ray tracer's distrust).
static float  gLocalMoved = 0.0;
static const float2 kLocalTaps[16] = {
    float2(-0.613392, 0.617481), float2(0.170019, -0.040254), float2(-0.299417, 0.791925), float2(0.645680, 0.493210),
    float2(-0.651784, 0.717887), float2(0.421003, 0.027070), float2(-0.817194, -0.271096), float2(-0.705374, -0.668203),
    float2(0.977050, -0.108615), float2(0.063326, 0.142369), float2(0.203528, 0.214331), float2(-0.667531, 0.326090),
    float2(-0.098422, -0.295755), float2(-0.885922, 0.215369), float2(0.566637, 0.605213), float2(0.039766, -0.396100) };
static const uint kLocalSearch = 4u, kLocalPcf = 12u;   // gathers (4 texels each) and comparison taps
float LocalShadowLinear(float d, float n, float f) { return n / max(1.0 - d * (f - n) / f, 1e-6); }
float LocalShadow(uint s, float3 p, float3 N, float3 l, float dist, float radius, out float moved)
{
    moved = 0.0;
    const float4 info = gLocalShadowInfo[s];   // tan across, tan up, near, far
    const float texelUv = gLocalShadowParams.y;
    const float texelM = 2.0 * max(info.x, info.y) * dist * texelUv;   // a texel here, metres
    const float3 Nl = dot(N, l) < 0.0 ? -N : N;
    const float3 q = p + Nl * (1.5 * texelM) + l * (0.5 * texelM + 0.005);
    const float4 c = mul(float4(q, 1.0), gLocalShadowVP[s]);
    if (c.w <= info.z || c.w >= info.w) return 1.0;
    const float2 uv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
    if (any(abs(uv - 0.5) > 0.5)) return 1.0;
    const float z = c.z / c.w, zR = c.w;
    const float2 uvPerTan = 0.5 / info.xy;
    const float ign = gLocalShadowParams.w * frac(52.9829189 * frac(dot(gShadePx, float2(0.06711056, 0.00583715))));
    float2 cs;
    sincos(ign * 6.28318530, cs.y, cs.x);
    const float2x2 rot = float2x2(cs.x, -cs.y, cs.y, cs.x);
    // The blocker search: where the emitter's disc could be hidden from here, for blockers half-way or nearer.
    const float2 search = clamp(radius / zR * uvPerTan, 2.0 * texelUv, 32.0 * texelUv);
    float zSum = 0.0, nB = 0.0;
    [unroll] for (uint k = 0; k < kLocalSearch; ++k)
    {
        const float3 at = float3(uv + mul(rot, kLocalTaps[k * (16u / kLocalSearch)]) * search, (float)s);
        const float4 g = gLocalShadow.GatherRed(gSampVolume, at);
        [unroll] for (uint j = 0; j < 4u; ++j)
            if (g[j] < z) { zSum += LocalShadowLinear(g[j], info.z, info.w); nB += 1.0; }
    }
    if (nB < 0.5) return 1.0;
    // Blocked: by a moving object? The casters' kind (RenderLocalShadows) round the pixel's own spot and one search tap
    // - a moving caster there lies between the lamp and this pixel (two reads, not one per tap: the mask it feeds
    // lasts four frames and TAA relights a neighbourhood, so it need not be exact).
    {
        const float4 f0 = gLocalShadowFlag.GatherRed(gSampVolume, float3(uv, (float)s));
        const float4 f1 = gLocalShadowFlag.GatherRed(gSampVolume, float3(uv + mul(rot, kLocalTaps[0]) * search, (float)s));
        const float4 f = max(f0, f1);
        moved = max(max(f.x, f.y), max(f.z, f.w));
    }
    const float zB = zSum / nB;
    const float2 pen = clamp(radius * max(zR - zB, 0.0) / (zB * zR) * uvPerTan, 1.0 * texelUv, 32.0 * texelUv);
    // A penumbra under a texel and a half: the comparison's own 2x2 is the whole filter.
    [branch] if (max(pen.x, pen.y) < 1.5 * texelUv)
        return gLocalShadow.SampleCmpLevelZero(gShadowSamp, float3(uv, (float)s), z);
    float v = 0.0;
    [unroll] for (uint t = 0; t < kLocalPcf; ++t)
        v += gLocalShadow.SampleCmpLevelZero(gShadowSamp, float3(uv + mul(rot, kLocalTaps[t]) * pen, (float)s), z);
    return v * (1.0 / (float)kLocalPcf);
}

void ShadeLocalLight(uint li, LightRec L, float3 p, float3 N, float3 V, float ndv, float3 diffuseCol, float3 f0,
                     float a, float specScale, float wrap, float trans, bool withSpec, float2 coat, inout float3 shadowed,
                     inout float3 open)
{
    float3 l;
    float dist;
    const float3 E = LightIrradiance(li, L, p, l, dist);
    const float ndl = saturate((dot(N, l) + wrap) / (1.0 + wrap)) + saturate(-dot(N, l)) * trans;
    // Nothing from it here - outside a spot's cone or past its range, or facing away: the rest is skipped (a cone's
    // edge on screen is a coherent boundary, so whole waves skip together).
    if (ndl <= 0.0 || max(E.r, max(E.g, E.b)) <= 0.0) return;
    float3 c3 = diffuseCol * E * (ndl * gLocalDiffAo);
    if (withSpec)
    {
        // GGX with Schlick by multiplies and the joint Smith term's sqrt-free approximation (Hammon 2017), D and V
        // under one reciprocal.
        const float3 H = normalize(V + l);
        const float aw = saturate(a + L.radius / (2.0 * max(dist, 1e-3)));
        const float aw2 = aw * aw;
        const float ndlTrue = saturate(dot(N, l));
        const float ndh = saturate(dot(N, H));
        const float x = 1.0 - saturate(dot(V, H)), x2 = x * x;
        const float3 F = f0 + (1.0 - f0) * (x2 * x2 * x);
        const float d = ndh * ndh * (aw2 - 1.0) + 1.0;
        const float vden = lerp(2.0 * ndlTrue * ndv, ndlTrue + ndv, aw);
        const float dv = aw2 / max(1e-6, 3.14159265 * d * d * 2.0 * vden);
        c3 += F * (dv * specScale * ndlTrue) * E;
        // The clear coat (kCoatRoughness): its own lobe over the base, widened by the emitter as the base's is - from
        // coat.y, the coat's alpha after the pixel's spread of normals (SpecAAKernel).
        if (coat.x > 0.0)
        {
            const float cw = saturate(coat.y + L.radius / (2.0 * max(dist, 1e-3)));
            const float cw2 = cw * cw;
            const float dc = ndh * ndh * (cw2 - 1.0) + 1.0;
            const float vdc = lerp(2.0 * ndlTrue * ndv, ndlTrue + ndv, cw);
            const float Fc = (kCoatF0 + (1.0 - kCoatF0) * (x2 * x2 * x)) * coat.x;
            c3 = c3 * (1.0 - Fc) + E * (Fc * cw2 / max(1e-6, 3.14159265 * dc * dc * 2.0 * vdc) * specScale * ndlTrue);
        }
    }
    // A shadow-mapped light (the cascades' path; its slot in flags bits 8-11): its own shadow, here.
    const uint slot = (L.flags >> 8) & 15u;
    [branch] if (slot != 0u && gLocalShadowParams.x > 0.5)
    {
        [branch] if (slot != gShadowSlotLast)
        {
            gShadowVisLast = LocalShadow(slot - 1u, p, N, l, dist, L.radius, gShadowMovedLast);
            gShadowSlotLast = slot;
        }
        c3 *= gShadowVisLast;
        gLocalMoved = max(gLocalMoved, gShadowMovedLast);
    }
    if (L.flags & kLightShadows) shadowed += c3;
    else open += c3;
}

void ShadeLightCluster(uint ci, float3 p, float3 N, float3 V, float ndv, float3 diffuseCol, float3 f0, float a,
                       float specScale, float wrap, float trans, bool withSpec, float2 coat, inout float3 shadowed,
                       inout float3 open)
{
    const uint2 c = gClusters[ci];
    for (uint k = 0; k < c.y; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        ShadeLocalLight(li, LightLoad(li), p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, coat,
                        shadowed, open);
    }
}

float3 LocalLights(float3 p, float3 N, float3 V, float3 diffuseCol, float3 f0, float a, float specScale, float2 px,
                   float wrap, float trans, bool withSpec, float2 coat)
{
    if (gLightsOn < 0.5) return float3(0.0, 0.0, 0.0);
    gShadePx = px;
    gShadowSlotLast = 0u;
    gLocalMoved = 0.0;
    const float ndv = saturate(dot(N, V)) + 1e-5;
    float3 shadowed = float3(0.0, 0.0, 0.0), open = float3(0.0, 0.0, 0.0);
    if (gLightsOn > 1.5)
    {
        // The cube probe's faces (probe.h): no clusters of their own, so every light (gClusterParams.x of them), and
        // no ray-traced fraction - unshadowed.
        for (uint li = 0; li < (uint)gClusterParams.x; ++li)
            ShadeLocalLight(li, LightLoad(li), p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, coat,
                            shadowed, open);
        return shadowed + open;
    }
    const uint ci = LightCluster(px, dot(p - gCamPos, gCamFwd), gClusterParams);
    bool done = false;
#if __SHADER_TARGET_MAJOR >= 6
    [loop] for (uint turn = 0; turn < 4u && !done; ++turn)
    {
        const uint cu = WaveReadLaneFirst(ci);   // wave-uniform
        [branch] if (cu == ci)
        {
            ShadeLightCluster(cu, p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, coat, shadowed,
                              open);
            done = true;
        }
    }
#endif
    [branch] if (!done)
        ShadeLightCluster(ci, p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, coat, shadowed, open);
#ifdef BLEND
    const float vis = 1.0;
#else
    const float vis = gLightsVisOn > 0.5 ? gRtLights.Load(int3(px, 0)) : 1.0;
#endif
    return shadowed * vis + open;
}

// Karis' analytic fit to the split-sum BRDF term. The alternative is a 2D LUT
// texture; this is two instructions and indistinguishable at the roughnesses
// anything outdoors actually uses.
float3 EnvBRDFApprox(float3 f0, float roughness, float ndv)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.040, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

cbuffer ObjectCB : register(b1)
{
    uint  gObjectLayer;
    uint  gMatFlags;
    uint  gObjectMoved;   // 1 = a moving object (RecordPass's movedOf): gPrevObjectXform is bound
    uint  gObjectProbe;   // 1: this object reflects the cube probe, not the sky (Scene::SetReflectionFocus)
    row_major float4x4 gObjectXform;
    // The material, as root constants rather than a buffer. It changes every
    // draw exactly as the transform does, and the root signature has the room
    // (60 of 64 dwords used) - a material buffer would buy nothing until there
    // is a reason to stop having one draw per object.
    float4 gBaseColourFactor;
    float4 gPbrA;    // roughness, metallic, normalScale, occlusionStrength
    float4 gPbrB;    // emissiveStrength, alphaCutoff, clearCoat, -
    // Last frame's transform, bound only for a moving object (gObjectMoved):
    // where this object's surface WAS, for the motion target.
    row_major float4x4 gPrevObjectXform;
};

Texture2DArray          gShadow     : register(t5);
// The prefiltered sky (env.h). Mip 0 is the mirror, the last mip is very nearly
// the hemisphere average, and roughness picks a level.
Texture2D               gEnv        : register(t6);
// The world around the car (probe.h): the same octahedral mapping and roughness axis, for the reflection focus.
Texture2D               gReflProbe  : register(t19);
// THIS frame's ray-traced sun visibility (rt.h), from the depth+normal prepass.
Texture2D<float>        gRtVis      : register(t7);
// And this frame's ray-traced ambient occlusion, same source, denoised.
Texture2D<float>        gRtAo       : register(t8);
// Sky irradiance per normal, cosine-convolved (env.h, design 5.0i).
Texture2D               gIrr        : register(t11);
// Aerial perspective, tabulated per froxel (aerial.h): screen x, screen y, and
// sqrt(distance / gAerialMaxDist). In-scatter and transmittance, both rgb.
Texture3D               gAerialIn   : register(t9);
Texture3D               gAerialTr   : register(t10);
// gSampVolume (s4, trilinear, CLAMP) and gShadowSamp (s3, the shadow comparison) are declared ahead of this source,
// with the local lights' shadow maps (Scene's shader assembly), which read them first.

// How much sun reaches this point. 0 = fully shadowed, 1 = fully lit.
//
// Cascade chosen by VIEW DEPTH, which is why the splits are carried in the
// constant buffer rather than recomputed - the shader and the fitting code must
// agree exactly or geometry lands in a cascade it was never rendered into.
static float gDbgCascade = -1.0, gDbgLightZ = 0.0, gDbgStored = 0.0;
static float2 gDbgUv = float2(0, 0);

float SunVisibility(float3 wpos, float viewDepth, float3 N, float3 L)
{
    if (gShadowParams.x < 0.5) return 1.0;

    // Far scenery is a distant backdrop, not part of the lit world. Shadowing
    // it means projecting a cascade fitted to the near field onto geometry
    // kilometres away, which can only be wrong.
    if (gObjectLayer == 2) return 1.0;

    int cascade = 3;
    if      (viewDepth < gCascadeSplit.x) cascade = 0;
    else if (viewDepth < gCascadeSplit.y) cascade = 1;
    else if (viewDepth < gCascadeSplit.z) cascade = 2;
    else if (viewDepth > gCascadeSplit.w) return 1.0;   // beyond the last cascade

    // NORMAL-OFFSET bias: step along the surface normal before projecting,
    // instead of biasing depth. Depth bias trades acne for peter-panning and
    // has to be retuned per cascade because texel size changes; offsetting by
    // the texel's own world size is scale-correct by construction. Scaled by
    // the grazing angle, where the error is largest.
    float slope = saturate(1.0 - dot(N, L));
    float biasScale = 1.0;
#ifdef ALPHA_TEST
    // See the shadow PSO note: foliage cards self-shadow because they have no
    // thickness. Most of that is handled by biasing the caster, but the
    // receiver side wants headroom too.
    biasScale = 3.0;
#endif
    float3 p = wpos + N * (gShadowParams.y * gShadowParams.z * biasScale * (1.0 + 2.0 * slope));

    float4 lp = mul(float4(p, 1.0), gLightViewProj[cascade]);
    lp.xyz /= lp.w;
    if (any(abs(lp.xy) > 1.0) || lp.z < 0.0 || lp.z > 1.0) return 1.0;

    float2 uv = float2(lp.x * 0.5 + 0.5, -lp.y * 0.5 + 0.5);

    // Diagnostics: the raw stored depth alongside the receiver depth is what
    // distinguishes "the shadow map is empty" from "everything is behind an
    // occluder" from "the two passes disagree about the matrix".
    gDbgCascade = (float)cascade;
    gDbgLightZ  = lp.z;
    gDbgUv      = uv;
    gDbgStored  = gShadow.Load(int4((int)(uv.x * gShadowParams.w),
                                    (int)(uv.y * gShadowParams.w), cascade, 0)).r;

    // 3x3 of hardware-PCF taps: 36 samples' worth of filtering for 9 fetches.
    float sum = 0.0;
    float texel = 1.0 / max(gShadowParams.w, 1.0);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += gShadow.SampleCmpLevelZero(gShadowSamp,
                       float3(uv + float2(x, y) * texel, cascade), lp.z);
    return sum / 9.0;
}

// The material's five slots, in NGGM order. An empty slot is bound to the 1x1
// white fallback rather than left unwritten, so a missing map is a neutral
// value and never an unwritten descriptor reading zero - which is a failure
// mode this renderer has already paid for once (design 5.0b).
Texture2D    gTex0      : register(t0);   // baseColour
Texture2D    gTex1      : register(t1);   // detail
Texture2D    gNormalMap : register(t2);
Texture2D    gOrm       : register(t3);   // R occlusion, G roughness, B metallic
Texture2D    gEmissive  : register(t4);
SamplerState gSamp      : register(s0);  // aniso 8, full chain - what we ship
SamplerState gSampLinear: register(s1);  // no anisotropy
SamplerState gSampClamp : register(s2);  // aniso 8, MaxLOD 1

// Pixel probe. One pixel per frame writes its intermediate lighting terms here
// and the host prints them by NAME. Reading numbers beats reading colours:
// three rounds were lost to inferring magnitudes from screenshots.
RWStructuredBuffer<float4> gProbe : register(u0);


// INST0..3: the INSTANCE STREAM (input slot 1, one record per instance - Scene::InstanceRec): rows of p' = A p + t,
// then tint (RGBA8), the crossfade (float bits) and flags (1 billboard, 2 a model). An object that is not a model's
// draws the stream's identity record: its vertex goes through as it always did.
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0;
               float4 ic0 : INST0; float4 ic1 : INST1; float4 ic2 : INST2; uint4 ix : INST3; };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; float3 wpos : TEXCOORD2; float3 prevWpos : TEXCOORD3;
              // A plant's card: its plant + 1, from uv1.x (SphericalFoliageNormals), else 0.
              nointerpolation uint plant : TEXCOORD4;
              // Its size: the plant's radius across, metres (uv1.y), else 0.
              nointerpolation float plantSize : TEXCOORD5;
              // What it casts (Plant::shadow): 0 all, 1 nothing (grass), 2 a lighter, softer shadow (a bush).
              nointerpolation float plantShadow : TEXCOORD6;
              // An instance's crossfade (FadeClip; 0 drawn whole) and its tint (1 none).
              nointerpolation float fade : TEXCOORD7;
              nointerpolation float3 tint : TEXCOORD8; };

// THE CROSSFADE between a model's LODs (Scene::CullModels): a placement in the band is drawn by both, one with
// fade = t (> 0: drops the pixels whose dither is under t), the next with -t (keeps exactly those) - so every pixel
// is drawn once, by one of them, and the share moves from one to the other across the band. A fixed per-pixel
// pattern (interleaved gradient noise), the same in the prepass and the main pass, whose depths must agree; the
// TAA jitter moves it across the pixel frame to frame.
float NggDither(float2 px)
{
    return frac(52.9829189 * frac(dot(floor(px), float2(0.06711056, 0.00583715))));
}
void FadeClip(float fade, float2 px)
{
    if (fade == 0.0) return;
    const float h = NggDither(px);
    if (fade > 0.0 ? (h < fade) : (h >= -fade)) discard;
}
// The alpha test's threshold. Foliage (NGG_DRAW_HASHED): dithered about the material's cutoff, a different pattern
// from the crossfade's, so a leaf's soft alpha edge comes out as a stipple the temporal filter resolves.
static const float kHashedSpread = 0.45;
float AlphaCut(float cutoff, float2 px)
{
    if ((gMatFlags & NGG_DRAW_HASHED) == 0u) return cutoff;
    const float h = NggDither(px.yx + float2(37.0, 11.0));
    return cutoff + (h - 0.5) * kHashedSpread;
}


// Leaves are not opaque lambertian sheets. Two cheap terms stand in for that:
// WRAP softens the terminator (light reaching a leaf from directions other
// than straight on), and TRANSMISSION is sunlight passing THROUGH the leaf,
// which is why real backlit foliage glows instead of silhouetting black.
static const float kFoliageWrap = 0.4;
static const float kFoliageTransmission = 0.5;
static const float kPlantShadowPack = 1000.0;   // uv1.y = size + this * Plant::shadow (scene.cpp)

// A plant's place-in-the-plant (kVolumeScale in scene.cpp): the same seen from
// either side of a card, so never flipped by facing.
static const float kVolumeScale = 0.25;
bool VolumeNormal(float3 n) { return dot(n, n) < 0.3025; }

// SELF-SHADOW of a plant, in place of the shadow its own cards cast. Rays from
// a plant skip their own plant (rt.cpp Occluded): traced, its 2-3 cards shadow
// each other card by card, which is the lit/dark split. This is the same
// question asked of the VOLUME the cards stand for: how far the sunlight
// travels through the canopy - the unit sphere around the plant - to reach the
// point, and Beer-Lambert over that. q is the point in the plant's unit space
// (|q| = 1 its surface); the sun direction is taken as is, so a tall tree is
// treated as round, which a shadow this soft cannot show.
//
// The density is per METRE, so the optical depth grows with the plant: a 6 m
// tree has a dark far side, a 30 cm grass tuft barely any. One density per
// radius made sunlit grass 0.85x the sunlit ground beside it (RBR's bake:
// 0.95x). Capped at kFoliageDensityCap radii, the depth trees were tuned at.
static const float kFoliageDensity = 0.33;     // per metre
static const float kFoliageDensityCap = 1.0;   // per plant radius
static const float kFoliageInnerAo = 0.7;    // the ambient a plant's centre keeps
static const float kFoliageGroundShade = 0.75;   // of the sunlit-ground bounce a shaded plant loses
float FoliageSelfShadow(float3 q, float3 L, float radius)
{
    const float b = dot(q, L);
    const float c = dot(q, q) - 1.0;
    const float d = b * b - c;
    // Distance to the far side of the sphere along L; 0 if the point is
    // outside it and the ray misses.
    const float t = d > 0.0 ? max(0.0, -b + sqrt(d)) : 0.0;
    return exp(-min(kFoliageDensity * radius, kFoliageDensityCap) * t);
}

VSOut VSMain(VSIn i)
{
    VSOut o;
    // An INSTANCED MODEL's vertex is in its model space: the instance places it (Scene::PlacementTransform). Every
    // other draw has the identity record and skips this, so its vertices are exactly as they were.
    float3 pos = i.pos, nrm = i.nrm;
    const bool model = (i.ix.z & 2u) != 0u;
    if (model)
    {
        const float3 t = float3(i.ic0.w, i.ic1.w, i.ic2.w);
        if ((i.ix.z & 1u) != 0u)
        {
            // THE FAR BILLBOARD: its quad (x across, y up) turned about the up axis to face the camera - or, in a
            // shadow map, the sun (RenderShadows puts the "camera" far towards it).
            float3 f = gCamPos - t;
            f.y = 0.0;
            f = dot(f, f) > 1e-8 ? normalize(f) : float3(0.0, 0.0, 1.0);
            const float sh = length(float3(i.ic0.x, i.ic1.x, i.ic2.x));
            const float sv = length(float3(i.ic0.y, i.ic1.y, i.ic2.y));
            pos = t + float3(f.z, 0.0, -f.x) * (i.pos.x * sh) + float3(0.0, i.pos.y * sv, 0.0) + f * (i.pos.z * sh);
            // Facing the viewer, leaning up: a canopy seen from afar is lit from above more than from the side.
            nrm = normalize(f + float3(0.0, 0.6, 0.0));
        }
        else
        {
            pos = float3(dot(i.ic0, float4(i.pos, 1.0)), dot(i.ic1, float4(i.pos, 1.0)), dot(i.ic2, float4(i.pos, 1.0)));
            // The normal by the cofactors of A (its inverse transpose up to scale): right under a squash as well.
            const float3 a = i.ic0.xyz, b = i.ic1.xyz, c = i.ic2.xyz;
            const float3 n = float3(dot(cross(b, c), i.nrm), dot(cross(c, a), i.nrm), dot(cross(a, b), i.nrm));
            nrm = dot(n, n) > 1e-20 ? normalize(n) : float3(0.0, 1.0, 0.0);
        }
    }
    o.fade = asfloat(i.ix.y);
    o.tint = float3(i.ix.x & 255u, (i.ix.x >> 8) & 255u, (i.ix.x >> 16) & 255u) / 255.0;
    // The object transform first: the vertex is in baked world space, this moves
    // it from where the file put it to where the editor wants it.
    float3 placed = mul(float4(pos, 1.0), gObjectXform).xyz;
    float4 world = float4(placed + gWorldOffset, 1.0);
    o.pos = mul(mul(world, gView), gProj);
    o.wpos = world.xyz;
    // The same vertex under last frame's transform. Only the motion target
    // reads it; for an object that did not move it is this frame's position.
    o.prevWpos = (gObjectMoved != 0)
               ? mul(float4(pos, 1.0), gPrevObjectXform).xyz + gWorldOffset
               : world.xyz;
    // Rotation only. Editor placements are rigid; a non-uniform scale would want
    // the inverse transpose, and that is the point at which to add one.
    o.nrm = mul(nrm, (float3x3)gObjectXform);
    o.uv0 = i.uv0;
    // RBR's OBJECT meshes carry a second UV set that is not UV data - on Joux
    // Verte every one of the 88,964 stage-1 uses of ObjectTexturePlateOpaque is
    // non-finite, up to 3.1e36. The game never notices because object meshes
    // run RLSingleTexture.pso, which is three instructions and samples t0 only;
    // stage 1 is simply never read (doc/rbr-shaders.txt:428).
    //
    // We DO read it, because materials point both stages at the same texture,
    // so without this the sample coordinate is undefined. Falling back to uv0
    // keeps it defined. The real fix is a single-stage material flag from the
    // converter so stage 1 is not evaluated at all on object meshes - this is
    // the guard, not the cure.
    o.uv1 = all(isfinite(i.uv1)) ? i.uv1 : i.uv0;
    // Meaningful only on a plant's vertex (VolumeNormal): its uv1 is free -
    // alpha-tested geometry never blends a second layer - and carries the
    // plant's index as a float, exact to 2^24.
    o.plant = (!model && VolumeNormal(i.nrm) && i.uv1.x >= 0.0 && i.uv1.x < 16777215.0) ? (uint)i.uv1.x + 1u : 0u;
    const float packed = max(i.uv1.y, 0.0);
    o.plantShadow = o.plant != 0u ? floor(packed / kPlantShadowPack) : 0.0;
    o.plantSize = o.plant != 0u ? packed - o.plantShadow * kPlantShadowPack : 0.0;
    o.col = i.col;
    return o;
}


// Shadow pass pixel shader. Only exists to honour alpha test: without it a
// tree casts the shadow of its bounding cards rather than of its leaves, which
// is worse than no shadow at all. Opaque geometry uses no PS.
void PSShadow(VSOut i)
{
#ifdef ALPHA_TEST
    // What a plant casts (Plant::shadow): grass nothing - its shadow is the cards' outlines - and a bush every other
    // texel, which the 2x2 comparison filter reads as a lighter shadow.
    if (i.plantShadow > 0.5 && i.plantShadow < 1.5) discard;
    if (i.plantShadow > 1.5 && ((uint(i.pos.x) + uint(i.pos.y)) & 1u) != 0u) discard;
    float4 t0 = gTex0.Sample(gSamp, i.uv0);
    clip(t0.a * i.col.a - (170.0 / 255.0));
#endif
}

// The local lights' shadow pass (RenderLocalShadows): beside the depth, what the nearest caster is - 1 a moving
// object's (gObjectMoved, set per draw there), 0 the world's - so their lighting knows a moving object's shadow.
float PSShadowFlag(VSOut i) : SV_Target
{
#ifdef ALPHA_TEST
    if (i.plantShadow > 0.5 && i.plantShadow < 1.5) discard;
    if (i.plantShadow > 1.5 && ((uint(i.pos.x) + uint(i.pos.y)) & 1u) != 0u) discard;
    float4 t0 = gTex0.Sample(gSamp, i.uv0);
    clip(t0.a * i.col.a - (170.0 / 255.0));
#endif
    return gObjectMoved != 0u ? 1.0 : 0.0;
}

// Which answer the lighting uses, and both answers kept for the comparison
// view. Sampling the shadow map when it is not the source is skipped except
// in the debug views that compare the two - the map's cost is in RENDERING
// its cascades, which the caller skips entirely in RT mode.
static float gDbgCsmVis = 1.0;
// THE CAR in the cube probe's faces (gCarBoxC): left out of them, it would leave the ground round it sunlit and open to
// the sky - and the paint's grazing edges, a mirror there, reflected the bright road where the car's shadow lies. A
// ray from `p` along `dir` against the box: 0 where it meets it (or starts inside), 1 where it misses or no box.
float CarBoxVis(float3 p, float3 dir)
{
    if (gCarBoxC.w < 0.5) return 1.0;
    const float3 o = p - gCarBoxC.xyz;
    float tNear = -1e9, tFar = 1e9;
    const float4 axes[3] = { gCarBoxX, gCarBoxY, gCarBoxZ };
    [unroll] for (int k = 0; k < 3; ++k)
    {
        const float po = dot(o, axes[k].xyz), d = dot(dir, axes[k].xyz);
        if (abs(d) < 1e-6)
        {
            if (abs(po) > axes[k].w) return 1.0;
            continue;
        }
        const float t0 = (-axes[k].w - po) / d, t1 = (axes[k].w - po) / d;
        tNear = max(tNear, min(t0, t1));
        tFar = min(tFar, max(t0, t1));
    }
    return (tFar >= max(tNear, 0.0)) ? 0.0 : 1.0;
}

float SunVisInner(float3 wpos, float2 pixel, float3 N);
float SunVis(float3 wpos, float2 pixel, float3 N)
{
    return SunVisInner(wpos, pixel, N) * CarBoxVis(wpos + N * 0.02, gSunTowards);
}

float SunVisInner(float3 wpos, float2 pixel, float3 N)
{
    if (gShadowParams.x < 0.5) return 1.0;      // shadows off: off for BOTH sources
    const bool useRt = gShadowSource > 0.5 && gRtReady > 0.5;
    float csm = 1.0;
    if (!useRt || gDebugMode > 10.5)
        csm = SunVisibility(wpos, dot(wpos - gCamPos, gCamFwd), N, gSunTowards);
    gDbgCsmVis = csm;
    if (!useRt) return csm;
#ifdef BLEND
    // Glass has no pixel of its own in the ray tracer's visibility - that is the opaque surface's behind it - and the
    // cascades are not drawn under ray tracing: it takes the sun whole.
    return 1.0;
#else
    return gRtVis.Load(int3(pixel, 0));
#endif
}

// PREPASS. Depth is the rasteriser's; this writes the GEOMETRIC normal -
// vertex normal, facing-flipped exactly as PSMain flips it, no normal map -
// which is what a ray-origin offset wants. The alpha test runs here too, with
// the same expression as PSMain, so the depth the main pass tests EQUAL
// against was produced by the same coverage decision.
//
// And the MOTION of every pixel of a moving object (RecordPass's movedOf):
// xy = last frame's NDC minus this frame's (both unjittered), z = last frame's
// reverse-Z depth, w = 1. Everything else writes 0, and w = 0 tells TAA and
// the RT denoiser to reproject by the camera alone, which for a static surface
// is exact (one matrix, no 16-bit rounding) - so only what moves takes the new
// path. Without this a moved object reprojects as if it were static: its
// pixels find the wrong history, and the ground it uncovers inherits it - the
// trails behind a driven car.
struct PrepassOut
{
    float4 normal : SV_Target0;   // R8G8B8A8_SNORM: -1..1 stored as is
    float4 motion : SV_Target1;   // R16G16B16A16_FLOAT
    uint   plant  : SV_Target2;   // R32_UINT: plant + 1, 0 = not a plant's card
};
PrepassOut PSPrepass(VSOut i, bool frontFace : SV_IsFrontFace)
{
#ifdef ALPHA_TEST
    FadeClip(i.fade, i.pos.xy);
    float4 t0 = gTex0.Sample(gSamp, i.uv0);
    clip(t0.a * i.col.a * gBaseColourFactor.a - AlphaCut(gPbrB.y, i.pos.xy));
#endif
    float3 N = normalize(i.nrm);
    if (!all(isfinite(N)) || dot(N, N) < 0.5) N = float3(0.0, 1.0, 0.0);
    if (!frontFace) N = -N;

    PrepassOut o;
    o.normal = float4(N, 1.0);
    o.plant = 0u;
#ifdef ALPHA_TEST
    // A card of a plant: its volume normal is for the LIGHTING, and can lie
    // nearly in the card's own plane, where a ray-origin offset along it would
    // leave the ray on the card. The rays want the card's plane, towards the
    // camera, and w = 0.5 tells them the pixel is a plant's (rt.cpp SurfaceAt).
    // A model's leaves are real geometry: their own normal for the rays, and they shadow each other (no card skip).
    if ((gMatFlags & NGG_DRAW_MODEL) != 0u) {}
    else if (VolumeNormal(i.nrm))
    {
        float3 G = cross(ddy(i.wpos), ddx(i.wpos));
        G = dot(G, G) > 1e-20 ? normalize(G) : float3(0.0, 1.0, 0.0);
        if (dot(G, gCamPos - i.wpos) < 0.0) G = -G;
        o.normal = float4(G, 0.5);
        o.plant = i.plant;
    }
    else
    {
        // Any other card - an automatic legacy import's (Scene::SetLegacyProfile: no plants), a forest wall (RBR's
        // ribbons of 13 m cards round a hillside, too wide for a plant), a single card: its rays skip the cards
        // within kLegacyCardSkip of their start - ~0 says so (rt.cpp Occluded) - so it never shadows ITSELF, while
        // cards further off and everything opaque still do. With the vertex normal it had (turned to the camera),
        // a card the sun lit from behind sent its ray back through its own plane: that stretch of a forest wall
        // went black to the joint with the next, sun-facing one - a hard lit/dark split down a "tree" (track-42's
        // first hairpin). The card's plane for the ray origin, as a plant's.
        float3 G = cross(ddy(i.wpos), ddx(i.wpos));
        G = dot(G, G) > 1e-20 ? normalize(G) : float3(0.0, 1.0, 0.0);
        if (dot(G, gCamPos - i.wpos) < 0.0) G = -G;
        o.normal = float4(G, 0.5);
        o.plant = 0xFFFFFFFFu;
    }
#endif
    o.motion = float4(0.0, 0.0, 0.0, 0.0);
    if (gObjectMoved != 0)
    {
        const float4 c = mul(float4(i.wpos, 1.0), gViewProjNoJitter);
        const float4 p = mul(float4(i.prevWpos, 1.0), gPrevViewProj);
        // Behind last frame's camera: no previous position on screen. An
        // offset that lands outside the image makes every consumer drop the
        // history, which is the right answer.
        o.motion = (p.w > 1e-6 && c.w > 1e-6)
                 ? float4(p.xy / p.w - c.xy / c.w, p.z / p.w, 1.0)
                 : float4(4.0, 4.0, 0.0, 1.0);
    }
    return o;
}

// A MOVING OBJECT'S GLASS: not in the depth, so the motion target holds what is behind it - over a still surface a
// still pixel to TAA, which keeps its history unclipped (a turning car's lamp lens left a smear on the dark beside
// it), over the car's interior the interior's motion, which is not the pane's: the letters on a side window and the
// banner on the windscreen came out twice, a frame's turn apart, when the car was spun fast in the Garage. So the
// glass writes its own motion into a target of its own (m_psoPrepassGlass: depth tested, not written; the nearest
// pane drawn last wins), which TAA and the upscalers' inputs read - the ray tracer keeps the surface behind it.
float2 PSGlassMotion(VSOut i) : SV_Target
{
    const float4 c = mul(float4(i.wpos, 1.0), gViewProjNoJitter);
    const float4 p = mul(float4(i.prevWpos, 1.0), gPrevViewProj);
    return (p.w > 1e-6 && c.w > 1e-6) ? p.xy / p.w - c.xy / c.w : float2(4.0, 4.0);   // (4: off the screen)
}

// Aerial perspective - the SAME scattering integral the sky uses, over the
// camera-to-surface segment instead of the whole ray. Distant geometry is
// both dimmed by transmittance and tinted by in-scatter; applying only the
// tint is what makes cheap distance fog look like paint over the world
// rather than air in front of it.
//
// Read from the froxel table (aerial.h) - one trilinear fetch per volume
// where this used to be an 8x3 march on every geometry pixel, the largest
// per-pixel cost left once the ambient march went (design 9b item 21).
// The march stays as the reference path (gAerialLut = 0): same integral,
// so a difference between the two is a table defect by definition.
// The scene pass's and the deferred local lights' (their light reaches the camera through the same air).
void AerialAt(float3 wpos, float2 px, out float3 tr, out float3 inscatter)
{
    float3 toSurf = wpos - gCamPos;
    float  dist   = length(toSurf);
    tr = float3(1.0, 1.0, 1.0);
    inscatter = float3(0.0, 0.0, 0.0);
    if (gAerialLut > 0.5)
    {
        float3 dims;
        gAerialIn.GetDimensions(dims.x, dims.y, dims.z);
        // The first slice CENTRE sits a few metres out, and clamping to it
        // would put that much air between the camera and its own bonnet.
        // Below it, scale towards (no in-scatter, full transmittance):
        // both are linear in distance that close, so this is exact to
        // first order rather than a fade chosen by eye.
        float w  = sqrt(saturate(dist / gAerialMaxDist));
        float w0 = 0.5 / dims.z;
        float f  = 1.0;
        if (w < w0) { f = (w * w) / (w0 * w0); w = w0; }
        float3 uvw = float3(px * gInvScreen, w);
        inscatter = gAerialIn.SampleLevel(gSampVolume, uvw, 0.0).rgb * f;
        tr = lerp(float3(1.0, 1.0, 1.0),
                  gAerialTr.SampleLevel(gSampVolume, uvw, 0.0).rgb, f);
    }
    else if (dist > 1.0)
    {
        inscatter = AtmScatter(toSurf / dist, gSunTowards, dist,
                               gTurbidity, gSunIntensity, 8, 3, tr);
    }
}

// What the deferred local lights' pass needs of a surface (GBUF; PSDeferredLights): its shading normal, its kind
// (0 no surface, 1 opaque, 2 a foliage card, 3 opaque under a clear coat) and roughness, albedo and metallic. Packed into two 32-bit targets
// (kGbufFormats): every byte is written per FRAGMENT, foliage's overdraw included - a third target with the aerial
// transmittance (and the normal in half floats) cost the opaque world +3.8 ms in PIX's replay; the transmittance is
// computed again where it is used (AerialAt).
struct GBufOut
{
    float3 n;
    float  kind;
    float  rough;
    float4 albedoMetal;
};

// What a surface (normal N) reflects towards the reflection vector R at `roughness`: the sky, or for the car the
// world around it - the cube probe (gObjectProbe), which has the sky behind its world already.
//
// THE PROBE'S ROUGH END. Its chain is a 2x2 box all the way down (probe.h), and from level ~6 an octahedral texel
// spans a quarter of the sphere and more - both hemispheres of a quadrant, folded at the map's edges. A tyre (0.85:
// level 7.6) read the average of the world on every side of it, the stage behind the tyre too: at night the lamp-lit
// road and barrier made the tyres glow grey. So a lobe much wider than a texel of the level it would read is read at
// the level whose texels are half its half-width (`tapLevel`; the GGX lobe's half-width ~ atan(alpha)), in taps over
// the lobe, each only where it is above the surface - what the box chain mixed in from behind it does not reach it.
// The sky (gEnv) is smooth: its chain as before.
static const float kProbeTexel0 = 3.5449 / 512.0;   // radians a texel at the probe's level 0 (ReflectionProbe::kOct)
float3 EnvRadiance(float3 R, float3 N, float roughness)
{
    const float2 euv = AtmOctEncode(R);
    if (gObjectProbe == 0 || gReflProbeOn < 0.5)
        return gEnv.SampleLevel(gSampLinear, euv, roughness * (gEnvMips - 1.0)).rgb;
    const float level = roughness * (gReflProbeMips - 1.0);
    const float alpha = roughness * roughness;
    const float cone = atan(alpha);                                   // the lobe's half-width
    const float tapLevel = clamp(log2(max(0.5 * cone, 1e-6) / kProbeTexel0), 0.0, gReflProbeMips - 1.0);
    if (level <= tapLevel + 0.5)
        return gReflProbe.SampleLevel(gSampLinear, euv, level).rgb;
    // Six around the reflection at the lobe's half-width, and the reflection itself (twice the weight).
    float3 T = abs(R.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), R) : cross(float3(1.0, 0.0, 0.0), R);
    T = normalize(T);
    const float3 B = cross(R, T);
    const float sc = sin(cone), cc = cos(cone);
    float3 sum = 2.0 * gReflProbe.SampleLevel(gSampLinear, euv, tapLevel).rgb;
    float w = 2.0;
    [unroll] for (int k = 0; k < 6; ++k)
    {
        const float ang = k * 1.0471976;
        const float3 d = R * cc + (T * cos(ang) + B * sin(ang)) * sc;
        if (dot(d, N) <= 0.0) continue;                               // below the surface: not in its reflection
        sum += gReflProbe.SampleLevel(gSampLinear, AtmOctEncode(d), tapLevel).rgb;
        w += 1.0;
    }
    return sum / w;
}

// THE CAR'S OWN REFLECTIONS (gObjectProbe 2). The probe sees the world from the car's middle with the car left out, so
// a part of the car read it as if nothing of the car stood between: the roll cage in the cabin mirrored the stage
// straight through the roof, a sill or a wheel's inside through the arch. At night that is the lamp-lit road and
// barrier beside the car - the brightest thing in the frame - and every tube of the cage, every arch's lip and every
// crease drew a white line of it (the night glints). One ray along the reflection against the car's own instances
// (kRtMaskFocus: nothing else - the world beyond is the probe's own), up to the car's size: blocked, the reflection
// is the car's inside, not the stage. Glass is not in the ray tracer's scene: through a window the stage is seen.
// The ray leaves from above the TRIANGLE (its plane from the position's derivatives, faced towards the viewer) and
// is kept above it: a smooth normal at a silhouette reflects into its own triangle, which is not an occluder.
#if defined(NGG_REFL_RAY)
RaytracingAccelerationStructure gReflTlas : register(t20);
#endif
static const float kReflReach = 5.0;   // metres: past the car's length from anywhere on it
float ReflOpen(float3 p, float3 V, float3 R, float3 Ng)
{
#if defined(NGG_REFL_RAY)
    if (gObjectProbe != 2u) return 1.0;
    if (dot(Ng, V) < 0.0) Ng = -Ng;
    const float lift = dot(R, Ng);
    const float3 dir = normalize(R + Ng * max(0.0, 0.02 - lift));
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc ray;
    ray.Origin = p + Ng * (0.01 + length(p - gCamPos) * 0.0005);
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = kReflReach;
    q.TraceRayInline(gReflTlas, RAY_FLAG_NONE, 0x02, ray);   // Scene::kRtMaskFocus
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
#else
    return 1.0;
#endif
}

float4 Shade(VSOut i, bool frontFace, out GBufOut g)
{
    g = (GBufOut)0;
    AtmSetWeather(gCloud, gFogBeta, gFogH, dot(gGroundColour, float3(0.3333, 0.3333, 0.3333)));
    // Deliberately plain: a single directional term plus ambient. Anything
    // fancier would make it harder to tell a lighting bug from a camera bug,
    // and camera correctness is the entire point of this scene.
    // RBR's ground blends TWO textures using the VERTEX ALPHA as the weight,
    // then modulates by the vertex colour (which carries the baked lighting).
    //
    // Measured, after two wrong guesses: the ground textures are effectively
    // opaque DXT1, so the ModulateInvAlphaAddColor op copied from Wallaby's
    // fixed-function path collapsed to "second texture replaces first" and
    // painted the road with the wrong texture. Wallaby only uses that path as
    // an editor fallback - it skips those stage states when shaders are on,
    // and RBR renders ground with shaders. Meanwhile the vertex alpha channel
    // takes all 256 values (68% at zero), which is a blend weight and nothing
    // else.
    //
    // No longer inferred: this is RBR's own PixelShaders/fog/RLDoubleTexture.pso,
    // disassembled with fxc /dumpbin (full listing in doc/rbr-shaders.txt):
    //
    //     mul_sat r0.xyz, v0, t0        r0 = baseColour * tex0
    //     mul_sat r1.xyz, v0, t1        r1 = baseColour * tex1
    //     mul_sat r1.w,   t1.w, v0.w    weight = tex1.a * vertexAlpha
    //     lrp_sat r0, r1.w, r1, r0      lerp(r0, r1, weight)
    //     add_sat r0.xyz, r0, v1        + in-scatter
    //
    // The weight is tex1.a * vertexAlpha, NOT vertexAlpha alone. Joux Verte's
    // ground textures are opaque so the two agree there, but they will not
    // agree on a track whose blend layer carries alpha.
    // Uniform branch (gMipClamp is a constant for the whole draw), so this
    // costs nothing measurable and keeps one PSO instead of two.
    // Sample(), not SampleLevel(): the clamp lives in the sampler's MaxLOD, so
    // anisotropy still works on both paths. SampleLevel would force a fixed LOD
    // and disable aniso, which would change two things at once and make the
    // comparison say nothing.
    float4 t0 = gTex0.Sample(gSamp, i.uv0);

    // Single-stage materials skip the second fetch entirely rather than
    // fetching and then discarding: gMatFlags is constant for the draw, so this
    // is a uniform branch and the sample really does not happen. That is the
    // cure the isfinite() guard in the VS is only a symptom-fix for.
    const bool singleStage = (gMatFlags & NGG_MAT_SINGLE_STAGE) != 0;
    float4 t1 = t0;
    if (!singleStage) t1 = gTex1.Sample(gSamp, i.uv1);

#ifdef ALPHA_TEST
    // Foliage. RBR uses AlphaFunc GreaterEqual with AlphaRef 170, and its
    // RLSingleTexture shader forms alpha as texture.a * vertex.a - so the
    // vertex alpha is coverage here, not the terrain blend weight it is on
    // ground meshes. The threshold is the MATERIAL's now: 170/255 is only
    // RBR's default, and an authored texture has no reason to share it.
    FadeClip(i.fade, i.pos.xy);
    clip(t0.a * i.col.a * gBaseColourFactor.a - AlphaCut(gPbrB.y, i.pos.xy));
#endif
#ifdef BLEND
    // Glass (kMaterialBlend): the same alpha is COVERAGE - how much of the tint lies over what is behind. The output
    // is premultiplied (below): the tint's light scales with it, the reflection does not, so a clear pane still
    // reflects the sky.
    const float coverage = saturate(t0.a * i.col.a * gBaseColourFactor.a);
#endif

    // Vertex alpha is the terrain BLEND WEIGHT on ground meshes and COVERAGE on
    // object meshes - the same channel meaning two different things, which is
    // why the weight has to be zero here rather than merely small.
    float  weight = singleStage ? 0.0 : (t1.a * i.col.a);
    float3 albedo = lerp(t0.rgb, t1.rgb, weight) * gBaseColourFactor.rgb * i.tint;

    // --- the rest of the material -----------------------------------------
    // Every map MULTIPLIES its factor rather than replacing it, which is glTF's
    // rule and the reason an untextured material is still a complete one: the
    // white fallback bound to an empty slot makes map*factor == factor. A
    // converter can therefore ship plausible PBR for all 6,515 RBR textures
    // before a single map has been authored.
    const float4 ormA = gOrm.Sample(gSamp, i.uv0);
    float3 orm = ormA.rgb;
    // The clear coat (kCoatRoughness): the material's, times the ORM map's alpha - where a car's maps say lacquer.
    const float coat = saturate(gPbrB.z * ormA.a);
    float  roughness = saturate(orm.g * gPbrA.x);
    float  metallic  = saturate(orm.b * gPbrA.y);
    float  matAo     = lerp(1.0, orm.r, saturate(gPbrA.w));
    // RAY-TRACED ambient occlusion, composed with the material's own map. It
    // scales the AMBIENT terms only - the sun has its own visibility - which
    // is what an occlusion term means: how much of the sky and ground bounce
    // this point can actually see. Until now that was "all of it", everywhere,
    // which is most of why props read as pasted onto the terrain.
    float rtAo = (gAoReady > 0.5 && gAoOn > 0.5) ? gRtAo.Load(int3(i.pos.xy, 0)) : 1.0;
#ifdef BLEND
    rtAo = 1.0;   // the AO buffer is the opaque surface's behind the glass, not the glass's
#endif
    matAo *= rtAo;
    // Under the car, in the probe's faces (CarBoxVis): the sky it hides, most of the ambient's light.
    matAo *= lerp(0.25, 1.0, CarBoxVis(i.wpos + float3(0.0, 0.02, 0.0), float3(0.0, 1.0, 0.0)));
#ifdef ALPHA_TEST
    // A plant's AO rays skip its own cards (rt.cpp Occluded) for the same
    // reason its sun rays do: per card, they made one card dark and the one
    // crossing it light. What the cards gave - a canopy darker inside than at
    // its edge - comes back from the depth in the plant instead (kVolumeScale:
    // |q| = 0 at the centre, 1 at the surface), the same on every card -
    // and as much as the plant is big: the inside of a grass tuft is a few
    // centimetres from open sky, a tree's is metres (kFoliageDensity).
    if (gAoReady > 0.5 && gAoOn > 0.5 && VolumeNormal(i.nrm))
    {
        const float inner = lerp(1.0, kFoliageInnerAo,
                                 min(kFoliageDensity * i.plantSize, kFoliageDensityCap) / kFoliageDensityCap);
        matAo *= lerp(inner, 1.0, saturate(length(i.nrm) / kVolumeScale));
    }
#endif
    float3 emissive  = gEmissive.Sample(gSamp, i.uv0).rgb * gPbrB.x;

    // gBakedScale stands in for RBR's own lighting constant. RLDoubleTexture.vso
    // computes oD0.xyz = v5.xyz * r3, where r3 is extinction * sun colour *
    // exposure (c56/c65.x/c55/c55.w) and ps_1_1 clamps the product to 1.0.
    //
    // It is MEASURED PER ASSET, not a constant, because the bake's scale comes
    // from whichever editor built the track: Kangaroo (the original tool) bakes
    // half-scale, peaking at 128; Wallaby bakes full-scale, peaking at 255. A
    // 2.0 tuned on a Kangaroo track drives a Wallaby one to solid white, which
    // is precisely what happened. Community tracks are a mix of both, so this
    // has to come from the file.
    // ---------------------------------------------------------------------
    // Lighting.
    //
    // Mode 0 reproduces RBR: the vertex colour IS the lighting, baked by the
    // track editor for one sun position, scaled by gBakedScale (see above).
    //
    // Mode 1 throws that away and lights the surface from the same atmosphere
    // that draws the sky - which is the point of the whole overhaul, because a
    // baked bounce cannot follow a sun that moves.
    //
    // Mode 2 keeps the bake as an AMBIENT OCCLUSION term only. That is not a
    // compromise for its own sake: RBR's bake contains real occlusion
    // information (valleys, undersides, the shadowed side of a cutting) that we
    // cannot yet reproduce because there are no dynamic shadows. Using its
    // luminance to darken the ambient - never the sun - keeps that information
    // while still letting the sun move. It is a stopgap with an expiry date:
    // once shadows land, mode 1 becomes the honest one.
    // Captured for the probe. Zero in baked mode, which is itself informative.
    float3 gProbeSun = float3(0, 0, 0), gProbeAmbient = float3(0, 0, 0);
    float3 gProbeAmbSpec = float3(0, 0, 0);
    float3 gProbeESky = float3(0, 0, 0), gProbeEGround = float3(0, 0, 0);
    float  gProbeNdl = 0.0, gProbeAo = 1.0;
    float3 gProbeTr = float3(1, 1, 1), gProbeInscatter = float3(0, 0, 0);
    float  gProbeDist = 0.0, gProbeVis = 1.0;


    float3 lit;
    if (gLightMode < 0.5)
    {
        lit = albedo * i.col.rgb * gBakedScale;
#ifdef BLEND
        lit *= coverage;   // premultiplied, as the lit path is
#endif
    }
    else
    {
        float3 N = normalize(i.nrm);

        // Normals are per-vertex from the LBS, with a face-accumulated fallback
        // where the vertex type carries none. A degenerate one would otherwise
        // produce a black or blown-out patch that looks like a texture bug.
        if (!all(isfinite(N)) || dot(N, N) < 0.5)
            N = float3(0.0, 1.0, 0.0);

        // EVERYTHING is rasterised with CullMode NONE, so we routinely see the
        // back of a surface - and a back face's normal points away from us. Left
        // alone it gives N.L <= 0 and the surface goes black even when the sun
        // is on the side we are looking at. Flipping by facing is the whole fix
        // for that half of the problem.
        if (!frontFace && !VolumeNormal(i.nrm)) N = -N;
#ifdef ALPHA_TEST
        // THE LEGACY PROFILE (Scene::SetLegacyProfile), on an automatic import's cards: their own plane normals are
        // what makes crossed cards read as cardboard - one catches the sun, the one crossing it goes dark. A: an
        // upward surface (the bake gives the pattern); B: halfway up. (The importer's vegetation is plants - its
        // connected pieces in a plant table - and has volume normals, so this is the rest: fences, spectators.)
        if (gLegacyMode > 0.5 && !VolumeNormal(i.nrm))
        {
            if (gLegacyMode < 1.5) N = float3(0.0, 1.0, 0.0);
            else N = normalize(N + float3(0.0, 1.0, 0.0));
        }
#endif

        // Normal mapping, with the tangent frame from SCREEN-SPACE DERIVATIVES
        // rather than a vertex attribute. Two reasons, and the second decided
        // it: the .ngg vertex is 44 bytes with no tangent, so adding one is a
        // format break AND a converter change - and RBR's geometry carries no
        // tangents either, so they would have to be derived somewhere no matter
        // what. Four derivative instructions here works on converted and
        // authored content alike.
        //
        // Gated on a FLAG, not on the sampled value: an empty slot is bound to
        // the white fallback, which decodes to (1,1,1) and would tilt every
        // normal 54 degrees off true. Neutral for a normal map is (0.5,0.5,1),
        // and no fallback texture can be neutral for every slot at once.
        if (gMatFlags & NGG_MAT_NORMAL_MAP)
        {
            float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
            float2 du1 = ddx(i.uv0),  du2 = ddy(i.uv0);
            float3 T = dp1 * du2.y - dp2 * du1.y;
            if (dot(T, T) > 1e-20)
            {
                T = normalize(T);
                float3 B = normalize(cross(N, T));
                T = cross(B, N);
                float3 nm = gNormalMap.Sample(gSamp, i.uv0).xyz * 2.0 - 1.0;
                // z from x and y: a two-channel map (BC5) stores none, and a three-channel one agrees.
                nm.z = sqrt(saturate(1.0 - dot(nm.xy, nm.xy)));
                nm.xy *= gPbrA.z;
                float3 Nn = T * nm.x + B * nm.y + N * nm.z;
                if (dot(Nn, Nn) > 1e-12) N = normalize(Nn);
            }
        }

#if !defined(ALPHA_TEST) && !defined(BLEND)
        // WET SURFACES (design 5.0v; after Lagarde, "Water drop 2b"). Water in a POROUS surface's pores traps light
        // - wet asphalt and gravel go darker - and a film of it on top is smooth: everything wet is glossier, and
        // then the sky's reflection does the rest. Rain reaches what faces up and is open; walls take half, and
        // what something stands over (the AO's dark: a canopy, the car) stays drier. Above half wet (damp -> wet),
        // the ground's flat spots fill: PUDDLES, dark, near-mirror water whose surface is level, wherever a
        // world-fixed noise lies low enough.
        if (gWetness > 0.001)
        {
            const float exposed = saturate(0.5 + 0.5 * N.y) * lerp(0.35, 1.0, rtAo);
            const float wet = gWetness * exposed;
            const float porosity = saturate((roughness - 0.3) / 0.5);    // the rough are porous: road, gravel
            // GREEN ground is grass: water soaks in and stands on the blades in drops, so it neither pools nor
            // turns to a mirror. RBR's materials carry no such tag, and the albedo's own colour tells it well:
            // grass is green-dominant, asphalt, gravel and dirt are not.
            const float green = saturate((albedo.g - max(albedo.r, albedo.b)) / max(albedo.g, 0.01) * 3.0);
            albedo *= lerp(1.0, lerp(0.85, 0.45, porosity), wet);
            roughness = lerp(roughness, min(roughness, lerp(0.25, 0.6, green)), wet);
            if (gObjectLayer == 0 && gWetness > 0.5)
            {
                const float level = (gWetness - 0.5) * 2.0;                // 0 at damp .. 1 wet
                const float n = WetNoise(i.wpos.xz * 0.30) * 0.65 + WetNoise(i.wpos.xz * 1.10) * 0.35;
                const float edge = lerp(0.74, 0.62, level);                // more of the road under water as it gets wetter
                const float puddle = smoothstep(edge, edge + 0.05, n)
                                   * saturate((N.y - 0.93) / 0.05) * exposed * (1.0 - green);
                albedo *= lerp(1.0, 0.35, puddle);
                roughness = lerp(roughness, 0.02, puddle);
                N = normalize(lerp(N, float3(0.0, 1.0, 0.0), puddle));
            }
        }
#endif

        // The pixel's spread of normals (SpecAAKernel) and what it makes of every specular lobe here: the base's alpha
        // (floored as before), the base's roughness for the environment's mip (no floor: a flat mirror stays one),
        // and the clear coat's two (coatA for the analytic lobes, coatRough for the environment).
        const float specK     = SpecAAKernel(N);
        const float specA     = max(0.002, SpecAAAlpha(roughness * roughness, specK));
        const float envRough  = sqrt(SpecAAAlpha(roughness * roughness, specK));
        const float coatA     = SpecAAAlpha(max(0.002, kCoatRoughness * kCoatRoughness), specK);
        const float coatRough = sqrt(SpecAAAlpha(kCoatRoughness * kCoatRoughness, specK));

        // PHYSICAL balance needs the irradiance map, which comes with the
        // environment; without one it falls back to the established balance
        // rather than to a guess.
        const bool physical = gPhysical > 0.5 && gEnvReady > 0.5;
        const float3 sunE = gSunIntensity * AtmSunTransmittance(gSunTowards, 8);
        float3 sunColour = physical ? sunE : sunE * gSunDiffuse;

        // THE LOCAL LIGHTS (lights.h, LocalLights): one block for the three variants, ahead of the sun-and-sky section
        // below and added to `lit` at its end - their material terms recomputed here as that section computes them.
        float3 localLight = float3(0.0, 0.0, 0.0);
#if defined(GBUF) && !defined(BLEND)
        // DEFERRED (PSDeferredLights): the lights come after the pass, once per pixel - here only what they will need.
        g.n = N;
#if defined(ALPHA_TEST)
        g.kind = 2.0;
#else
        g.kind = coat > 0.5 ? 3.0 : 1.0;   // 3: under a clear coat
#endif
        // The roughness the lights will see: widened by the pixel's spread of normals (specK), so the deferred lobes
        // are this pass's. Under a clear coat the coat needs the spread too, and the target has 10 bits for both:
        // GbufRoughPack - the base's widened roughness in the upper 5, the spread in the lower 5.
        g.rough = g.kind > 2.5 ? GbufRoughPack(saturate(envRough), specK) : saturate(envRough);
        g.albedoMetal = float4(saturate(albedo), saturate(metallic));
#else
        {
            const float3 lf0   = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
            const float3 ldiff = albedo * (1.0 - metallic) * (physical ? 1.0 / 3.14159265 : 1.0);
            const float  la    = specA;
            const float3 lV    = normalize(gCamPos - i.wpos);
            const float  lspec = physical ? 1.0 : 3.14159265;
#if !defined(BLEND)
            if (gLocalShadowParams.z > 0.0) gLocalDiffAo = lerp(1.0, rtAo, gLocalShadowParams.z);
#endif
#if defined(ALPHA_TEST)
            localLight = LocalLights(i.wpos, N, lV, ldiff, lf0, la, lspec, i.pos.xy, kFoliageWrap, kFoliageTransmission, false,
                                     float2(0.0, 0.0));
#elif defined(BLEND)
            localLight = LocalLights(i.wpos, N, lV, ldiff * coverage, lf0, la, lspec, i.pos.xy, 0.0, 0.0, true,
                                     float2(0.0, 0.0));
#else
            localLight = LocalLights(i.wpos, N, lV, ldiff, lf0, la, lspec, i.pos.xy, 0.0, 0.0, true, float2(coat, coatA));
#endif
        }
#endif

        // HEMISPHERE ambient: sky above, ground bounce below.
        //
        // A single sky sample was not enough, and the failure was specific: a
        // wall facing away from the sun received pure Rayleigh scatter and
        // nothing else, so it rendered NAVY BLUE. Real shadow is dominated by
        // light bounced off the ground in front of the surface, which is why it
        // reads desaturated and slightly warm rather than blue.
        //
        // The bounce colour is RBR's own Terrain_Reflectance_R/G/B (design
        // 3.4a), so this is measured rather than invented.
        //
        // The sky sample is reused as the light arriving at the ground instead
        // of marching a second time towards the zenith - a real approximation,
        // but the alternative doubles the per-pixel cost of the pass for a term
        // that is then multiplied by a ~0.5 albedo.
        // The sky this surface sees. A LOOKUP, not a march (design 9b item
        // 14): the environment map is built from the same AtmSkyRadiance in the
        // same direction, so this replaces a recomputation rather than
        // answering a different question. It was the last per-pixel evaluation
        // of the atmosphere that did not have to be one - N + up is in the
        // upper hemisphere for every possible normal, where AtmSkyRadiance and
        // AtmScatter agree exactly.
        //
        // The march stays as the fallback for a client that has not given us an
        // environment. It is the expensive path and it should be: a renderer
        // that silently drops ambient because a caller forgot a setter is worse
        // than one that is merely slow.
        float3 skyDir = normalize(N + float3(0.0, 1.0, 0.0));
        float3 skyRad;
        if (gEnvReady > 0.5)
        {
            skyRad = gEnv.SampleLevel(gSampLinear, AtmOctEncode(skyDir), 0.0).rgb;
        }
        else
        {
            float3 atr;
            skyRad = AtmScatter(skyDir, gSunTowards, 1e9, gTurbidity,
                                gSunIntensity, 6, 2, atr);
        }

        // UNITS. `ambient` is added to `sunColour * ndl` and the sum multiplied
        // by albedo, so ambient must be an IRRADIANCE - the same units as
        // sunColour. AtmScatter returns a RADIANCE. Over a hemisphere the two
        // differ by a factor of PI, and getting that wrong is what produced
        // both of this section's failures:
        //
        //   sky-only ambient, radiance used directly -> PI times too DARK,
        //     and blue, so shaded walls went navy;
        //   ground bounce without its 1/PI           -> PI times too BRIGHT,
        //     and warm, so shaded walls went red.
        //
        // They partly cancelled, which is why fixing either one alone appeared
        // to break the other. Written out properly:
        //
        //   E_sky    = PI * skyRadiance
        //   E_ground = groundAlbedo * (sunIrradiance * cos + E_sky)
        //
        // The ground term needs no 1/PI here because it is already expressed as
        // an irradiance: the 1/PI turning irradiance into bounced radiance and
        // the PI turning that back into irradiance on our surface cancel.
        const float kPi = 3.14159265;

        float3 E_sky    = skyRad * kPi;
        float3 E_ground = gGroundColour * (sunColour * saturate(gSunTowards.y) + E_sky)
                        * gGroundBounce;

        // How much of the lower hemisphere this normal sees: 0 straight up,
        // 0.5 for a vertical wall, 1 for a downward face.
        float down = saturate(0.5 - 0.5 * N.y);
        float3 ambient = lerp(E_sky, E_ground, down) * gSkyAmbient;
        // The SUN's share of what the ground bounces at this surface. The
        // ground plane is assumed sunlit everywhere; foliage takes that back
        // where it is in shade itself (below).
        float3 groundSun = gGroundColour * sunColour * saturate(gSunTowards.y) * gGroundBounce
                         * down * gSkyAmbient;

        if (physical)
        {
            // The sky this normal sees, integrated properly: the irradiance
            // map is the cosine-weighted upper-hemisphere integral, so it
            // already knows how much of the sky a tilted or vertical surface
            // gets - no lerp, and no single-direction sample.
            E_sky = gIrr.SampleLevel(gSampLinear, AtmOctEncode(N), 0.0).rgb;

            // The ground as a uniform Lambertian plane of albedo gGroundColour,
            // lit by the sun and the whole sky: its radiance is
            // albedo * E_h / PI, and a surface seeing a fraction `down` of it
            // receives PI * that * down. The PIs cancel.
            const float3 E_skyUp = gIrr.SampleLevel(gSampLinear, AtmOctEncode(float3(0.0, 1.0, 0.0)), 0.0).rgb;
            E_ground = gGroundColour * (sunE * saturate(gSunTowards.y) + E_skyUp) * gGroundBounce;
            ambient = E_sky + E_ground * down;
            groundSun = gGroundColour * sunE * saturate(gSunTowards.y) * gGroundBounce * down;
        }
        gProbeESky = E_sky; gProbeEGround = E_ground; gProbeSun = sunColour;



        // METALLIC-ROUGHNESS split. A metal has no diffuse lobe and tints its
        // reflection with its own colour; a dielectric reflects ~4% white and
        // keeps its albedo for the diffuse. One lerp expresses both.
        float3 f0        = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        float3 diffuseCol = albedo * (1.0 - metallic);
        float  a          = specA;   // Disney remap, widened by the pixel's spread of normals (SpecAAKernel)
        float3 V          = normalize(gCamPos - i.wpos);
        float  ndv        = saturate(dot(N, V)) + 1e-5;

        // UNITS, again, and stated because this is where the project has been
        // bitten twice. The diffuse term here is `albedo * E`, which is PI times
        // the physical `albedo/PI * E` - a convention inherited from the first
        // lighting pass and absorbed by gSunDiffuse. Specular must therefore
        // carry the SAME factor or the two are out of balance by PI, which
        // would read as a chrome sheen on everything. Both terms collapse to the
        // physical form together when exposure becomes physical (9b item 17).
        //
        // In the PHYSICAL balance the convention goes: diffuse is albedo/PI
        // and specular carries no factor - Lambert and GGX exactly as written
        // in every reference, so a glTF material means what it means elsewhere.
        const float kSpecScale = physical ? 1.0 : kPi;
        const float kDiffNorm  = physical ? 1.0 / kPi : 1.0;
        diffuseCol *= kDiffNorm;

#ifdef ALPHA_TEST
        // Foliage. The other half of the black-vegetation problem: a leaf with
        // the sun BEHIND it is not unlit, it is backlit. Without a transmission
        // term it falls back to ambient alone and reads as a black silhouette
        // against a bright sky - which is exactly how it looked.
        float ndl   = saturate((dot(N, gSunTowards) + kFoliageWrap) / (1.0 + kFoliageWrap));
        float trans = saturate(dot(-N, gSunTowards)) * kFoliageTransmission;
        float vis = SunVis(i.wpos, i.pos.xy, N);
        // A plant under the RAY-TRACED sun: its rays skipped its own cards, so
        // the volume stands in for them (FoliageSelfShadow). The cascades keep
        // their own answer - their foliage caster bias already removes most of
        // a tree's self-shadow, and what is left of it is theirs.
        // A plant stands ON the ground, and in a shadow so does the ground
        // around it: the sunlit-ground bounce in `ambient` is wrong there. A
        // card faces sideways, so it took half of that bounce - warm, and as
        // much as the sky - while flat ground in the same shade takes none:
        // at the stage 41 start, tufts in a tree's shade came out 1.05x the
        // ground beside them where their albedo is 0.59x (RBR's own bake:
        // 0.62x). The plant's own visibility stands in for the ground's -
        // most of it: a shadow on the ground is patchy, and the ground a few
        // metres off is often in sun (all of it took the tufts to 0.48x).
        ambient -= groundSun * (1.0 - vis) * kFoliageGroundShade;
        if (VolumeNormal(i.nrm) && gShadowParams.x > 0.5 && gShadowSource > 0.5 && gRtReady > 0.5)
            vis *= FoliageSelfShadow(i.nrm / kVolumeScale, gSunTowards, i.plantSize);
        gProbeNdl = ndl; gProbeAmbient = ambient; gProbeVis = vis;
        // No specular lobe on foliage. A leaf card is not a surface with a
        // meaningful microfacet normal - the card normal is a shading fiction
        // already (see SphericalFoliageNormals) - and a highlight computed from
        // a fiction lands in the wrong place and flickers as the camera moves.
        lit = diffuseCol * (sunColour * (ndl + trans) * vis + ambient * matAo);
        // The legacy profile's two ways (Scene::SetLegacyProfile); `vis` is already from opaque geometry only (the
        // card's rays skipped every card). RBR's bake is the vertex colour times its scale.
        if (gLegacyMode > 0.5 && !VolumeNormal(i.nrm))
        {
            const float3 bake = i.col.rgb * gBakedScale;
            if (gLegacyMode < 1.5)
                // A: the bake's pattern under today's light - an upward surface's sun and sky, so weather and
                // exposure still apply; the bake's own sun stays where RBR baked it.
                lit = diffuseCol * bake * (sunColour * ndl * vis + ambient * matAo);
            else
                // B: dynamic, the bake only as occlusion of the ambient (its darker canopy insides and undergrowth).
                lit = diffuseCol * (sunColour * (ndl + trans) * vis
                                    + ambient * matAo * saturate(dot(bake, float3(0.299, 0.587, 0.114))));
        }
        // The local lights on a card: wrapped as the sun is, no highlight (as for the sun, a card's normal is a fiction).
        lit += localLight;   // (computed above, before the sun and the sky)
#else
        float ndl = saturate(dot(N, gSunTowards));
        float vis = SunVis(i.wpos, i.pos.xy, N);
        gProbeNdl = ndl; gProbeAmbient = ambient; gProbeVis = vis;

        float3 H   = normalize(V + gSunTowards);
        float  ndh = saturate(dot(N, H));
        float  vdh = saturate(dot(V, H));
        // The sun's (or the moon's) disc widens its highlight, as an emitter's does (kSunDiscWiden).
        const float as = saturate(a + kSunDiscWiden);
        float3 spec = FresnelSchlick(f0, vdh) * DistributionGGX(ndh, as) * VisSmith(ndv, ndl, as);

        // THE CLEAR COAT (kCoatRoughness): the sun's sharp highlight in the lacquer, and the share of the light it
        // reflects (Fresnel at the view: it rises to a mirror at grazing angles) kept from the base below.
        float coatSun = 0.0, coatKeep = 1.0;
        if (coat > 0.0)
        {
            const float ca = saturate(coatA + kSunDiscWiden);
            coatSun = coat * FresnelSchlick(kCoatF0.xxx, vdh).x * DistributionGGX(ndh, ca) * VisSmith(ndv, ndl, ca);
            coatKeep = 1.0 - coat * FresnelSchlick(kCoatF0.xxx, ndv).x;
        }

        // AMBIENT SPECULAR - the split sum. What the surface reflects of
        // everything that is not the sun, which until now was nothing at all:
        // a polished surface and a rough one differed only in the size of one
        // highlight, and that reads as flat whatever the material data says.
        //
        // The mip chain IS the roughness axis (env.h), so this is one sample.
        // The reflection vector can point below the horizon, and the map has
        // the ground blend there for exactly that reason.
        float3 ambSpec = float3(0.0, 0.0, 0.0);
        // (derivatives before the branch, in the whole quad's flow)
        const float3 R = reflect(-V, N);
        const float3 Ng = cross(ddy(i.wpos), ddx(i.wpos));
        if (gEnvOn > 0.5)
        {
            // Where the car itself stands in the reflection's way (ReflOpen): its inside, dark against the stage -
            // for both layers (a rough lobe in the cabin sees the cabin too; and a thin tube's lobe is wide only from
            // SpecAAKernel, whose widening must not let the stage back in).
            const float open = ReflOpen(i.wpos, V, R, normalize(Ng + 1e-12));
            // (the roughnesses widened by the pixel's spread of normals, SpecAAKernel)
            float3 envRad = EnvRadiance(R, N, envRough);
            // Same PI as the direct lobe, and for the same reason: our diffuse
            // is albedo*E rather than albedo/PI*E, so every specular term has
            // to carry the factor or the balance is off by PI.
            ambSpec = envRad * EnvBRDFApprox(f0, envRough, ndv) * kSpecScale * matAo * open;
            if (coat > 0.0)
            {
                const float3 coatRad = EnvRadiance(R, N, coatRough);
                ambSpec = ambSpec * coatKeep
                        + coatRad * (coat * EnvBRDFApprox(kCoatF0.xxx, coatRough, ndv).x * kSpecScale * matAo * open);
            }
        }
        gProbeAmbSpec = ambSpec;

#ifdef BLEND
        // Premultiplied: the tint's light by its coverage, the reflection whole.
        lit = diffuseCol * (sunColour * ndl * vis + ambient * matAo) * coverage
            + spec * kSpecScale * sunColour * ndl * vis
            + ambSpec;
#else
        // (ambSpec has the coat's share already; the coat's highlight is uncoloured, as the lacquer is)
        lit = (diffuseCol * (sunColour * ndl * vis + ambient * matAo)
               + spec * kSpecScale * sunColour * ndl * vis) * coatKeep
            + coatSun * kSpecScale * sunColour * ndl * vis
            + ambSpec;
#endif
        lit += localLight;   // (computed above, before the sun and the sky)
#endif
        lit += emissive;
    }

    // Aerial perspective (AerialAt).
    if (gAerial > 0.5)
    {
        float3 tr, inscatter;
        AerialAt(i.wpos, i.pos.xy, tr, inscatter);
        gProbeDist = length(i.wpos - gCamPos); gProbeTr = tr; gProbeInscatter = inscatter;
#ifdef BLEND
        inscatter *= coverage;           // premultiplied: the air in front of the tint, as much as the tint covers
#endif
        lit = lit * tr + inscatter;      // linear; exposure is applied below
    }

    // NO TONEMAP AND NO sRGB ENCODE HERE. Both moved into PostFx, which is the
    // only place that can apply them once to the whole image - the sky pass
    // used to apply exposure and this one did not, which is precisely why the
    // exposure control appeared to work on the sky and do nothing to surfaces.
    //
    // What leaves this shader is LINEAR HDR radiance, unclamped. BAKED mode is
    // the exception in units but not in handling: RBR's vertex colour is
    // already display-referred 0..1, so PostFx renders mode 0 with the
    // sRGB-only curve and the A/B against RBR stays byte-identical.
    float3 litPre = lit;          // kept: the probe reports pre-exposure values


    // Probe: does this pixel want to report? Uniform-ish branch, one pixel.
    // SV_Position is the pixel CENTRE - 812.5 for pixel 812 - so comparing
    // against the integer with a 0.5 tolerance never matched and the probe
    // silently reported zeros. Truncate both sides instead.
    //
    // Not in the alpha-tested variant: a UAV write makes the depth test wait for
    // the shader (PSMain), and that variant cannot be told otherwise - it clips.
#if !defined(ALPHA_TEST)
    if (gProbeOn > 0.5 && all(uint2(i.pos.xy) == uint2(gProbePixel)))
    {
        gProbe[0] = float4(albedo, weight);
        gProbe[1] = float4(i.col.rgb * gBakedScale, i.col.a);
        gProbe[2] = float4(normalize(i.nrm), frontFace ? 1.0 : 0.0);
        gProbe[3] = float4(gProbeSun, gProbeNdl);
        gProbe[4] = float4(gProbeAmbient, gProbeAo);
        gProbe[5] = float4(gProbeESky, 0.0);
        gProbe[6] = float4(gProbeEGround, 0.0);
        gProbe[7] = float4(litPre, gExposure);
        gProbe[8] = float4(gProbeTr, gProbeDist);
        gProbe[9] = float4(gProbeInscatter, gAerial);
        gProbe[10] = float4(lit, gLightMode);          // linear HDR, pre-PostFx
        gProbe[11] = float4(gDebugMode, 0.0, gSkyAmbient, gGroundBounce);
        gProbe[12] = float4(gProbeVis, gDbgCascade, gDbgLightZ, gDbgStored);
        gProbe[13] = float4(gDbgUv, dot(i.wpos - gCamPos, gCamFwd), gShadowParams.x);
        gProbe[14] = float4(gProbeAmbSpec, gEnvOn);
        // The FIRED marker, against a buffer zeroed before the frame.
        //
        // Without it the probe cannot report a miss: if no geometry covers the
        // pixel the shader never runs, nothing is written, and the readback
        // still holds the PREVIOUS armed frame's numbers - which read as
        // "identical to the baseline" and are indistinguishable from a change
        // that correctly produced no change. That false pass has already been
        // paid for once, in the first version of the transform test.
        gProbe[15] = float4(1.0, 0.0, 0.0, 0.0);
    }
#endif

    // Debug views. Each isolates ONE input, so an artefact can be attributed
    // instead of guessed at: if the road seams survive in "tex0 only" they come
    // from stage 0, if they only appear in "blend weight" they come from the
    // vertex alpha, and if they track the hard colour edges of "mip level" they
    // are mip selection. Uniform branch, one PSO.
    if (gDebugMode >= 0.5)
    {
        if (gDebugMode < 1.5) return float4(t0.rgb, 1.0);
        if (gDebugMode < 2.5) return float4(t1.rgb, 1.0);
        if (gDebugMode < 3.5) return float4(weight.xxx, 1.0);
        if (gDebugMode < 4.5) return float4(i.col.rgb * gBakedScale, 1.0);
        // uv0 straight to colour: a discontinuity in the mapping shows up as a
        // hard colour break exactly where the geometry seam is.
        if (gDebugMode < 5.5) return float4(frac(i.uv0), 0.0, 1.0);
        if (gDebugMode < 6.5) return float4(0.5, 0.5, 0.5, 1.0);

        if (gDebugMode < 7.5) return float4(gProbeVis.xxx, 1.0);

        // SHADOW ACNE. A surface that shadows ITSELF fails the depth test by a
        // hair: the stored occluder depth is barely in front of the receiver.
        // Genuine occlusion fails it by a lot. Separating the two by the SIZE
        // of the depth difference turns "does that look like acne" into
        // something the shader answers.
        if (gDebugMode < 8.5)
        {
            if (gProbeVis > 0.99)              return float4(0.30, 0.30, 0.30, 1.0);  // lit
            float gap = gDbgLightZ - gDbgStored;
            if (gap > 0.0 && gap < 0.0015)     return float4(1.00, 0.10, 0.10, 1.0);  // ACNE
            return float4(0.10, 0.45, 1.00, 1.0);                                     // real shadow
        }

        // AMBIENT SPECULAR, isolated and multiplied by 8 so it is legible.
        //
        // The x8 is the honest part of this view. For a rough dielectric the
        // term is SMALL by construction - at the default roughness 0.75 and
        // f0 0.04 the split-sum weight is about 0.023, so the whole contribution
        // is a few percent of the diffuse. That is physically right and it is
        // also why the effect is easy to mistake for "nothing happened". This
        // view answers "is it there and how much", which is a different question
        // from "can I see it", and only the first one has a correct answer.
        if (gDebugMode < 9.5) return float4(gProbeAmbSpec * 8.0, 1.0);

        // ROUGHNESS and METALLIC as red and green. Nothing reads them today -
        // every material takes the factor and no asset ships an ORM map - so
        // this view is currently flat. It stops being flat the moment the
        // converter emits one, which is exactly when a wrong channel or a
        // missed sRGB decode needs to be visible rather than inferred.
        if (gDebugMode < 10.5) return float4(roughness, metallic, 0.0, 1.0);

        // RAY-TRACED sun visibility, last frame's, straight from the probe.
        float rtVis = gRtReady > 0.5 ? gRtVis.Load(int3(i.pos.xy, 0)) : 1.0;
        if (gDebugMode < 11.5) return float4(rtVis.xxx, 1.0);

        // RT AGAINST THE SHADOW MAP. Grey where they agree; RED where the rays
        // say shadow and the map says lit; BLUE the reverse. Two independent
        // answers to one question, and the colour says which of them is wrong
        // where - which is the whole reason the probe's first ray was a shadow
        // ray rather than ambient occlusion.
        // RT ambient occlusion, as computed: white = sees the whole sky.
        if (gDebugMode > 12.5 && gDebugMode < 13.5)
            return float4((gAoReady > 0.5 ? gRtAo.Load(int3(i.pos.xy, 0)) : 1.0).xxx, 1.0);

        if (gDebugMode < 12.5)
        {
            bool mapLit = gDbgCsmVis > 0.5;
            bool rtLit  = rtVis > 0.5;
            if (mapLit == rtLit) return float4((rtLit ? 0.55 : 0.20).xxx, 1.0);
            return rtLit ? float4(0.10, 0.45, 1.00, 1.0)     // map shadowed, RT lit
                         : float4(1.00, 0.10, 0.10, 1.0);    // RT shadowed, map lit
        }

        // PLANTS: which pixels FindPlants made a plant of - the ones that get a
        // volume normal and whose rays skip their own plant (rt.cpp Occluded).
        if (gDebugMode > 13.5 && gDebugMode < 14.5)
        {
#ifdef ALPHA_TEST
            return VolumeNormal(i.nrm) ? float4(0.1, 0.9, 0.1, 1.0) : float4(0.9, 0.1, 0.1, 1.0);
#else
            return float4(0.0, 0.0, 0.0, 1.0);
#endif
        }

        // The local lights' shadowed fraction (the ray tracer's, denoised; lights.h): white = every lamp reaches it.
        if (gDebugMode > 14.5 && gDebugMode < 15.5)
            return float4((gLightsVisOn > 0.5 ? gRtLights.Load(int3(i.pos.xy, 0)) : 1.0).xxx, 1.0);

        // FLAT GREY - no texture, no vertex colour, no blend. Every shading
        // input is gone, so anything still visible here is geometry, depth or
        // rasterisation. This is the view that separates "we are shading it
        // wrong" from "the surface itself is wrong", and it is the one worth
        // reaching for FIRST next time.
        return float4(0.5, 0.5, 0.5, 1.0);
    }

#ifdef BLEND
    return float4(lit, coverage);   // premultiplied: ONE, INV_SRC_ALPHA over what is behind
#else
    return float4(lit, 1.0);
#endif
}

// THE DEPTH TEST BEFORE THE SHADER. The probe's UAV write (Shade) makes the default a test AFTER it - every fragment
// the rasteriser makes ran the whole shader and only then failed: 6.4 per pixel on stage 41 at 1080p, with the
// prepass's depth to test against or without (PIX's PS invocations, design 5.4 N2d). [earlydepthstencil] restores it
// where it is exact - no clip, no SV_Depth, so testing and writing before the shader gives the same depth; the
// alpha-tested variant clips, so it leaves out the probe instead and the driver tests early on its own.
#if defined(ALPHA_TEST)
#define NGG_EARLY_DEPTH
#else
#define NGG_EARLY_DEPTH [earlydepthstencil]
#endif

#if defined(GBUF)
struct PSOutG
{
    float4 colour : SV_Target0;
    float4 n      : SV_Target1;   // R10G10B10A2: the normal octahedral, roughness, kind / 3
    float4 albedo : SV_Target2;   // R8G8B8A8 sRGB: albedo, metallic
};
NGG_EARLY_DEPTH
PSOutG PSMain(VSOut i, bool frontFace : SV_IsFrontFace)
{
    GBufOut g;
    PSOutG o;
    o.colour = Shade(i, frontFace, g);
    o.n = float4(AtmOctEncode(normalize(g.n)), g.rough, g.kind / 3.0);
    o.albedo = g.albedoMetal;
    return o;
}
#else
NGG_EARLY_DEPTH
float4 PSMain(VSOut i, bool frontFace : SV_IsFrontFace) : SV_Target
{
    GBufOut g;
    return Shade(i, frontFace, g);
}
#endif

// THE DEFERRED LOCAL LIGHTS (design 5.4 N2c): one full-screen pass after the opaque world, each PIXEL's lights once -
// in the main pass they were shaded per FRAGMENT, foliage's overdraw and every 2x2 quad's helper lanes included, at
// the big shader's occupancy. The same LocalLights, from the surface the GBUF variants left (Shade), its position
// from the depth; added (ONE, ONE) times the aerial transmittance, as the main pass added them before its aerial
// perspective (AerialAt). Glass keeps its lights forward: drawn after this pass, over the lit world.
Texture2D<float4> gGbufN      : register(t16);
Texture2D<float4> gGbufAlbedo : register(t17);
Texture2D<float>  gDepthTex   : register(t18);

float4 VSFullscreen(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// The second target: the MOVED-SHADOW MASK (z), for TAA (post.cpp, as the ray tracer's distrust): 1 where a moving
// object shadows the pixel from a shadow-mapped lamp this frame, and for three frames after (last frame's, at where
// the pixel was, less 1/8 a frame) - where the car's shadow has just left the floor, the floor's history is the
// shadow. Without the maps: 0.
struct DeferredOut { float4 c : SV_Target0; float4 m : SV_Target1; };
DeferredOut PSDeferredLights(float4 pos : SV_Position)
{
    DeferredOut o;
    o.c = 0.0;
    o.m = 0.0;
    const int3 px = int3(pos.xy, 0);
    const float4 n = gGbufN.Load(px);
    const float kind = round(n.w * 3.0);
    if (kind < 0.5) return o;   // no surface: the sky, glass, a debug view
    // The roughness Shade left: widened by its pixel's spread of normals already; under a coat, packed with that
    // spread for the coat's own lobe (GbufRoughPack).
    float roughness = n.z, specK = 0.0;
    if (kind > 2.5) GbufRoughUnpack(n.z, roughness, specK);
    const float coatA = SpecAAAlpha(max(0.002, kCoatRoughness * kCoatRoughness), specK);
    const float4 am = gGbufAlbedo.Load(px);
    const float2 ndc = float2(pos.x * gInvScreen.x * 2.0 - 1.0, 1.0 - pos.y * gInvScreen.y * 2.0);
    const float4 hp = mul(float4(ndc, gDepthTex.Load(px), 1.0), gInvViewProj);
    const float3 p = hp.xyz / hp.w;
    const bool physical = gPhysical > 0.5 && gEnvReady > 0.5;
    const float3 N = AtmOctDecode(n.xy);
    const float3 V = normalize(gCamPos - p);
    const float3 lf0 = lerp(float3(0.04, 0.04, 0.04), am.rgb, am.a);
    const float3 ldiff = am.rgb * (1.0 - am.a) * (physical ? 1.0 / 3.14159265 : 1.0);
    const float la = max(0.002, roughness * roughness);
    const float lspec = physical ? 1.0 : 3.14159265;
    const bool foliage = kind > 1.5 && kind < 2.5;
    // The screen-space AO's share of the lights' diffuse (gLocalShadowParams.z: at night, without the ray tracer).
    if (gLocalShadowParams.z > 0.0 && gAoReady > 0.5 && gAoOn > 0.5)
        gLocalDiffAo = lerp(1.0, gRtAo.Load(px), gLocalShadowParams.z);
    const float3 L = LocalLights(p, N, V, ldiff, lf0, la, lspec, pos.xy, foliage ? kFoliageWrap : 0.0,
                                 foliage ? kFoliageTransmission : 0.0, !foliage, float2(kind > 2.5 ? 1.0 : 0.0, coatA));
    float3 tr = float3(1.0, 1.0, 1.0), inscatter;
    if (gAerial > 0.5)
    {
        AtmSetWeather(gCloud, gFogBeta, gFogH, dot(gGroundColour, float3(0.3333, 0.3333, 0.3333)));
        AerialAt(p, pos.xy, tr, inscatter);
    }
    o.c = float4(L * tr, 0.0);
    if (gLocalShadowParams.x > 0.5)
    {
        float was = 0.0;
        const float4 hp2 = mul(float4(p, 1.0), gPrevViewProj);
        if (hp2.w > 1e-4)
        {
            const float2 puv = hp2.xy / hp2.w * float2(0.5, -0.5) + 0.5;
            if (all(puv > 0.0) && all(puv < 1.0)) was = gLocalShadowMaskPrev.Load(int3(puv / gInvScreen, 0)).z;
        }
        o.m = float4(0.0, 0.0, max(gLocalMoved, was - 0.125), 0.0);
    }
    return o;
}
