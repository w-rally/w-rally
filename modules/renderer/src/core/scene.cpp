#include "scene.h"
#include "log.h"
#include "atmosphere_hlsl.h"
#include "mipgen.h"
#include "shadow.h"
#include "../format/ngg_format.h"

#include <dxgiformat.h>

#include <d3dcompiler.h>
#include <vector>
#include <cstdio>
#include <string>
#include <cmath>
#include <utility>   // std::swap, for the selection counter's barrier flips
#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <array>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

// The prepass normal buffer. SNORM8 holds a unit vector to ~1/127, which is
// more than a ray-origin offset needs and a quarter of the bytes of float16.
static const DXGI_FORMAT kNormalFormat = DXGI_FORMAT_R8G8B8A8_SNORM;
// A plant's vertices (SphericalFoliageNormals) carry not a normal but their
// PLACE IN THE PLANT: q = (p - centre) / extent, the unit ellipsoid's
// coordinates, times this scale. q is affine in the position, so it
// interpolates exactly across a triangle, and two crossing cards have the same
// q where they cross - the shading is a function of the point in the plant,
// never of the card. The direction of q is the volume normal; its length is
// how deep in the canopy the point is, and with the sun direction it gives the
// path through the canopy (FoliageSelfShadow in the HLSL).
//
// The scale is also the MARKER: |q| stays under 2.2, so a stored length under
// 0.55 says "plant" (VolumeNormal in the HLSL), where a unit normal
// interpolated across a triangle stays above ~0.7 unless its corners point
// more than 90 degrees apart. The shaders must not flip it for a back face.
static const float kVolumeScale = 0.25f;
// The prepass's motion target (see PSPrepass). 16-bit is enough because xy is
// a DIFFERENCE - small, where half floats are dense - and only moved objects
// use it at all. w: 1 a moved object's pixel, 0.25 moving glass over a still one.
static const DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// The prepass's third target: which PLANT a pixel is a card of (FindPlants),
// + 1, or 0. The ray tracer compares it with the plant of what a ray hits.
static const DXGI_FORMAT kPlantFormat = DXGI_FORMAT_R32_UINT;
// The surface the deferred local lights read (Shade's GBufOut): the shading normal (octahedral), roughness and kind;
// albedo and metallic. Only while there are local lights (the GBUF pipelines).
static const DXGI_FORMAT kGbufFormats[2] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
// A plant vertex's uv1.y is its size plus this times what it casts (Plant::shadow) - the same in the shaders.
static const float kPlantShadowPack = 1000.0f;

namespace {

// Declared row_major and multiplied as mul(vector, matrix) so the matrices RBR
// hands us work unchanged. D3D9 uses row-vector convention; HLSL defaults to
// column-major storage and mul(matrix, vector). Getting this wrong does not
// produce an error - it produces a plausible-looking but subtly wrong camera,
// which is far worse, so it is stated explicitly rather than transposed
// somewhere on the C++ side where it would be easy to lose.
const char* kShaderSrc = R"(
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
    float2             gLightPad;
    // The deferred local lights' pass (PSDeferredLights): the jittered view-projection's inverse - the matrix the
    // depth was written with.
    row_major float4x4 gInvViewProj;
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
                   float wrap, float trans, bool withSpec);

// Standard GGX: Trowbridge-Reitz distribution, height-correlated Smith
// visibility, Schlick Fresnel. Nothing bespoke here on purpose - the value of a
// standard BRDF is that an authored glTF material looks the same in Blender.
float3 FresnelSchlick(float3 f0, float vdh)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - vdh), 5.0);
}
float DistributionGGX(float ndh, float a)
{
    float a2 = a * a;
    float d  = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(1e-6, 3.14159265 * d * d);
}
float VisSmith(float ndv, float ndl, float a)
{
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(1e-6, lv + ll);
}

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
void ShadeLocalLight(uint li, LightRec L, float3 p, float3 N, float3 V, float ndv, float3 diffuseCol, float3 f0,
                     float a, float specScale, float wrap, float trans, bool withSpec, inout float3 shadowed,
                     inout float3 open)
{
    float3 l;
    float dist;
    const float3 E = LightIrradiance(li, L, p, l, dist);
    const float ndl = saturate((dot(N, l) + wrap) / (1.0 + wrap)) + saturate(-dot(N, l)) * trans;
    // Nothing from it here - outside a spot's cone or past its range, or facing away: the rest is skipped (a cone's
    // edge on screen is a coherent boundary, so whole waves skip together).
    if (ndl <= 0.0 || max(E.r, max(E.g, E.b)) <= 0.0) return;
    float3 c3 = diffuseCol * E * ndl;
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
    }
    if (L.flags & kLightShadows) shadowed += c3;
    else open += c3;
}

void ShadeLightCluster(uint ci, float3 p, float3 N, float3 V, float ndv, float3 diffuseCol, float3 f0, float a,
                       float specScale, float wrap, float trans, bool withSpec, inout float3 shadowed, inout float3 open)
{
    const uint2 c = gClusters[ci];
    for (uint k = 0; k < c.y; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        ShadeLocalLight(li, LightLoad(li), p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, shadowed,
                        open);
    }
}

float3 LocalLights(float3 p, float3 N, float3 V, float3 diffuseCol, float3 f0, float a, float specScale, float2 px,
                   float wrap, float trans, bool withSpec)
{
    if (gLightsOn < 0.5) return float3(0.0, 0.0, 0.0);
    const uint ci = LightCluster(px, dot(p - gCamPos, gCamFwd), gClusterParams);
    const float ndv = saturate(dot(N, V)) + 1e-5;
    float3 shadowed = float3(0.0, 0.0, 0.0), open = float3(0.0, 0.0, 0.0);
    bool done = false;
#if __SHADER_TARGET_MAJOR >= 6
    [loop] for (uint turn = 0; turn < 4u && !done; ++turn)
    {
        const uint cu = WaveReadLaneFirst(ci);   // wave-uniform
        [branch] if (cu == ci)
        {
            ShadeLightCluster(cu, p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, shadowed, open);
            done = true;
        }
    }
#endif
    [branch] if (!done)
        ShadeLightCluster(ci, p, N, V, ndv, diffuseCol, f0, a, specScale, wrap, trans, withSpec, shadowed, open);
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
    uint  gObjectMoved;   // 1 = the transform differs from last frame's; 2 = and it is glass (the prepass's mark)
    uint  gObjectPad;
    row_major float4x4 gObjectXform;
    // The material, as root constants rather than a buffer. It changes every
    // draw exactly as the transform does, and the root signature has the room
    // (60 of 64 dwords used) - a material buffer would buy nothing until there
    // is a reason to stop having one draw per object.
    float4 gBaseColourFactor;
    float4 gPbrA;    // roughness, metallic, normalScale, occlusionStrength
    float4 gPbrB;    // emissiveStrength, alphaCutoff, -, -
    // Last frame's transform, bound only when gObjectMoved says it differs:
    // where this object's surface WAS, for the motion target.
    row_major float4x4 gPrevObjectXform;
};

Texture2DArray          gShadow     : register(t5);
// The prefiltered sky (env.h). Mip 0 is the mirror, the last mip is very nearly
// the hemisphere average, and roughness picks a level.
Texture2D               gEnv        : register(t6);
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
SamplerState            gSampVolume : register(s4);   // trilinear, CLAMP


SamplerComparisonState  gShadowSamp : register(s3);

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


struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 col : COLOR0; float3 wpos : TEXCOORD2; float3 prevWpos : TEXCOORD3;
              // A plant's card: its plant + 1, from uv1.x (SphericalFoliageNormals), else 0.
              nointerpolation uint plant : TEXCOORD4;
              // Its size: the plant's radius across, metres (uv1.y), else 0.
              nointerpolation float plantSize : TEXCOORD5;
              // What it casts (Plant::shadow): 0 all, 1 nothing (grass), 2 a lighter, softer shadow (a bush).
              nointerpolation float plantShadow : TEXCOORD6; };

)" /* fifth split: MSVC 16380-byte literal limit, adjacent literals concatenate */ R"(
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
    // The object transform first: the vertex is in baked world space, this moves
    // it from where the file put it to where the editor wants it.
    float3 placed = mul(float4(i.pos, 1.0), gObjectXform).xyz;
    float4 world = float4(placed + gWorldOffset, 1.0);
    o.pos = mul(mul(world, gView), gProj);
    o.wpos = world.xyz;
    // The same vertex under last frame's transform. Only the motion target
    // reads it; for an object that did not move it is this frame's position.
    o.prevWpos = (gObjectMoved != 0)
               ? mul(float4(i.pos, 1.0), gPrevObjectXform).xyz + gWorldOffset
               : world.xyz;
    // Rotation only. Editor placements are rigid; a non-uniform scale would want
    // the inverse transpose, and that is the point at which to add one.
    o.nrm = mul(i.nrm, (float3x3)gObjectXform);
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
    o.plant = (VolumeNormal(i.nrm) && i.uv1.x >= 0.0 && i.uv1.x < 16777215.0) ? (uint)i.uv1.x + 1u : 0u;
    const float packed = max(i.uv1.y, 0.0);
    o.plantShadow = o.plant != 0u ? floor(packed / kPlantShadowPack) : 0.0;
    o.plantSize = o.plant != 0u ? packed - o.plantShadow * kPlantShadowPack : 0.0;
    o.col = i.col;
    return o;
}

)" /* MSVC: 16380-byte literal limit, adjacent literals concatenate */ R"(
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

// Which answer the lighting uses, and both answers kept for the comparison
// view. Sampling the shadow map when it is not the source is skipped except
// in the debug views that compare the two - the map's cost is in RENDERING
// its cascades, which the caller skips entirely in RT mode.
static float gDbgCsmVis = 1.0;
float SunVis(float3 wpos, float2 pixel, float3 N)
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
// And the MOTION of every pixel whose object moved since last frame:
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
    float4 t0 = gTex0.Sample(gSamp, i.uv0);
    clip(t0.a * i.col.a * gBaseColourFactor.a - gPbrB.y);
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
    if (VolumeNormal(i.nrm))
    {
        float3 G = cross(ddy(i.wpos), ddx(i.wpos));
        G = dot(G, G) > 1e-20 ? normalize(G) : float3(0.0, 1.0, 0.0);
        if (dot(G, gCamPos - i.wpos) < 0.0) G = -G;
        o.normal = float4(G, 0.5);
        o.plant = i.plant;
    }
    else if (gLegacyMode > 0.5)
    {
        // A card of an automatic legacy import (Scene::SetLegacyProfile): no plants, and its rays skip EVERY
        // card - ~0 says so (rt.cpp Occluded) - so only opaque geometry shadows it. The card's plane for the
        // ray origin, as a plant's.
        float3 G = cross(ddy(i.wpos), ddx(i.wpos));
        G = dot(G, G) > 1e-20 ? normalize(G) : float3(0.0, 1.0, 0.0);
        if (dot(G, gCamPos - i.wpos) < 0.0) G = -G;
        o.normal = float4(G, 0.5);
        o.plant = 0xFFFFFFFFu;
    }
#endif
    o.motion = float4(0.0, 0.0, 0.0, 0.0);
    // MOVING GLASS: not in the depth, so where it lies over a still surface that surface's pixel would look still to
    // TAA, which keeps a still pixel's history unclipped - a turning car's lamp lens left a smear on the dark beside it.
    // w = 0.25 marks it, MAX-blended into alpha alone (m_psoPrepassGlass): over a moved object its 1 stays, and under
    // 0.5 it is a still pixel to everything but TAA (rt.cpp reads w > 0.5).
    if (gObjectMoved == 2)
        o.motion = float4(0.0, 0.0, 0.0, 0.25);
    else if (gObjectMoved != 0)
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
// (0 no surface, 1 opaque, 2 a foliage card) and roughness, albedo and metallic. Packed into two 32-bit targets
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
    clip(t0.a * i.col.a * gBaseColourFactor.a - gPbrB.y);
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
    float3 albedo = lerp(t0.rgb, t1.rgb, weight) * gBaseColourFactor.rgb;

    // --- the rest of the material -----------------------------------------
    // Every map MULTIPLIES its factor rather than replacing it, which is glTF's
    // rule and the reason an untextured material is still a complete one: the
    // white fallback bound to an empty slot makes map*factor == factor. A
    // converter can therefore ship plausible PBR for all 6,515 RBR textures
    // before a single map has been authored.
    float3 orm = gOrm.Sample(gSamp, i.uv0).rgb;
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

)" /* third split: MSVC 16380-byte literal limit, adjacent literals concatenate */ R"(
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
        g.kind = 1.0;
#endif
        g.rough = saturate(roughness);
        g.albedoMetal = float4(saturate(albedo), saturate(metallic));
#else
        {
            const float3 lf0   = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
            const float3 ldiff = albedo * (1.0 - metallic) * (physical ? 1.0 / 3.14159265 : 1.0);
            const float  la    = max(0.002, roughness * roughness);
            const float3 lV    = normalize(gCamPos - i.wpos);
            const float  lspec = physical ? 1.0 : 3.14159265;
#if defined(ALPHA_TEST)
            localLight = LocalLights(i.wpos, N, lV, ldiff, lf0, la, lspec, i.pos.xy, kFoliageWrap, kFoliageTransmission, false);
#elif defined(BLEND)
            localLight = LocalLights(i.wpos, N, lV, ldiff * coverage, lf0, la, lspec, i.pos.xy, 0.0, 0.0, true);
#else
            localLight = LocalLights(i.wpos, N, lV, ldiff, lf0, la, lspec, i.pos.xy, 0.0, 0.0, true);
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


)" /* fourth split: MSVC 16380-byte literal limit, adjacent literals concatenate */ R"(
        // METALLIC-ROUGHNESS split. A metal has no diffuse lobe and tints its
        // reflection with its own colour; a dielectric reflects ~4% white and
        // keeps its albedo for the diffuse. One lerp expresses both.
        float3 f0        = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        float3 diffuseCol = albedo * (1.0 - metallic);
        float  a          = max(0.002, roughness * roughness);   // Disney remap
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
        float3 spec = FresnelSchlick(f0, vdh) * DistributionGGX(ndh, a) * VisSmith(ndv, ndl, a);

        // AMBIENT SPECULAR - the split sum. What the surface reflects of
        // everything that is not the sun, which until now was nothing at all:
        // a polished surface and a rough one differed only in the size of one
        // highlight, and that reads as flat whatever the material data says.
        //
        // The mip chain IS the roughness axis (env.h), so this is one sample.
        // The reflection vector can point below the horizon, and the map has
        // the ground blend there for exactly that reason.
        float3 ambSpec = float3(0.0, 0.0, 0.0);
        if (gEnvOn > 0.5)
        {
            float3 R = reflect(-V, N);
            float2 euv = AtmOctEncode(R);
            float  lod = roughness * (gEnvMips - 1.0);
            float3 envRad = gEnv.SampleLevel(gSampLinear, euv, lod).rgb;
            // Same PI as the direct lobe, and for the same reason: our diffuse
            // is albedo*E rather than albedo/PI*E, so every specular term has
            // to carry the factor or the balance is off by PI.
            ambSpec = envRad * EnvBRDFApprox(f0, roughness, ndv) * kSpecScale * matAo;
        }
        gProbeAmbSpec = ambSpec;

#ifdef BLEND
        // Premultiplied: the tint's light by its coverage, the reflection whole.
        lit = diffuseCol * (sunColour * ndl * vis + ambient * matAo) * coverage
            + spec * kSpecScale * sunColour * ndl * vis
            + ambSpec;
#else
        lit = diffuseCol * (sunColour * ndl * vis + ambient * matAo)
            + spec * kSpecScale * sunColour * ndl * vis
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

)" /* second split: MSVC 16380-byte literal limit */ R"(
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

float4 PSDeferredLights(float4 pos : SV_Position) : SV_Target
{
    const int3 px = int3(pos.xy, 0);
    const float4 n = gGbufN.Load(px);
    const float kind = round(n.w * 3.0);
    if (kind < 0.5) return float4(0.0, 0.0, 0.0, 0.0);   // no surface: the sky, glass, a debug view
    const float roughness = n.z;
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
    const bool foliage = kind > 1.5;
    const float3 L = LocalLights(p, N, V, ldiff, lf0, la, lspec, pos.xy, foliage ? kFoliageWrap : 0.0,
                                 foliage ? kFoliageTransmission : 0.0, !foliage);
    float3 tr = float3(1.0, 1.0, 1.0), inscatter;
    if (gAerial > 0.5)
    {
        AtmSetWeather(gCloud, gFogBeta, gFogH, dot(gGroundColour, float3(0.3333, 0.3333, 0.3333)));
        AerialAt(p, pos.xy, tr, inscatter);
    }
    return float4(L * tr, 0.0);
}
)";

// The shader above carries its own copy of the flag, because a raw string
// literal cannot include a header. This is the only thing stopping the two from
// drifting - and a drift here would turn every object mesh single-stage or none
// of them, silently either way.
static_assert(Scene::kDrawSingleStage == 256u,
              "NGG_MAT_SINGLE_STAGE in the scene shader disagrees with kDrawSingleStage");
static_assert(Scene::kDrawNormalMap == 512u,
              "NGG_MAT_NORMAL_MAP in the scene shader disagrees with kDrawNormalMap");

struct CameraCB
{
    float view[16];
    float proj[16];
    float lightDir[3];
    float physical;
    float worldOffset[3];
    float bakedScale;      // was padding; mirrors gBakedScale
    float debugMode;
    float camPos[3];
    float sunTowards[3];
    float sunIntensity;
    float turbidity;
    float exposure;
    float aerial;
    float lightMode;
    // Mirrors the HLSL layout above IN WHOLE float4 GROUPS. Keep it that way:
    // HLSL inserts padding that a packed C++ struct does not, and the resulting
    // shift is silent - every field simply reads its neighbour's value.
    float sunDiffuse;
    float skyAmbient;
    float aerialMaxDist;
    float aerialLut;
    float groundColour[3];
    float groundBounce;
    float probePixel[2];
    float probeOn;
    float envOn;
    float envMips;
    float envReady;
    float rtReady;
    float shadowSource;
    float aoReady;
    float aoOn;
    float invScreen[2];
    float camFwd[3];
    float legacyMode;      // gLegacyMode: the legacy profile of this scene, 0 none
    float cascadeSplit[4];
    float lightViewProj[4][16];
    float shadowParams[4];
    float cloudCover, fogBeta, fogHeight, wetness;   // wetness: the former pad (Scene::SetWetness)
    float viewProjNoJitter[16];
    float prevViewProj[16];
    float clusterParams[4];
    float lightsOn, lightsVisOn, lightPad[2];
    float invViewProj[16];
};
static_assert(sizeof(CameraCB) % 16 == 0,
              "CameraCB must be a whole number of float4 registers - see the note above");

// Camera world position from a rigid view matrix, row-vector convention.
//
// A D3D9 view matrix stores the camera basis in its COLUMNS:
//     | xaxis.x  yaxis.x  zaxis.x  0 |
//     | xaxis.y  yaxis.y  zaxis.y  0 |
//     | xaxis.z  yaxis.z  zaxis.z  0 |
//     | -dot(x,eye) -dot(y,eye) -dot(z,eye) 1 |
// so with t = row 3 and axis_k = column k:
//     t_k = -dot(axis_k, eye)   =>   eye = -sum_k t_k * axis_k
//     eye[j] = -(t0*m[j][0] + t1*m[j][1] + t2*m[j][2])     <- row j
//
// The first version of this indexed COLUMN j instead of row j, i.e. computed
// -t*R rather than -t*R^T. That is wrong unless the rotation is symmetric, and
// its symptom is instructive: the position stays plausible in magnitude but
// points somewhere else entirely, so the test grid appeared "above us and far
// in front" rather than obviously broken. Transposed-matrix bugs rarely look
// like bugs.
void CameraPosFromView(const float* v, float* out)
{
    const float tx = v[12], ty = v[13], tz = v[14];
    for (int j = 0; j < 3; ++j)
        out[j] = -(tx * v[j * 4 + 0] + ty * v[j * 4 + 1] + tz * v[j * 4 + 2]);
}

float SnapTo(float value, float step)
{
    return floorf(value / step) * step;
}

bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    out.resize((size_t)size.QuadPart);

    DWORD read = 0;
    bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
    CloseHandle(h);
    return ok;
}

// Upload heap for simplicity: this is a one-off static scene of a few MB, and a
// DEFAULT-heap copy would add a staging path for no measurable benefit at this
// size. Real streaming (design 1.8.2) does not look like this.
ComPtr<ID3D12Resource> MakeUploadBuffer(ID3D12Device* dev, const void* data, size_t bytes)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    void* mapped = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(res->Map(0, &none, &mapped)))
        return nullptr;
    memcpy(mapped, data, bytes);
    res->Unmap(0, nullptr);
    return res;
}

// Frustum planes from a combined view-projection, row-vector convention:
// clip.x = dot(v, col0) ... clip.w = dot(v, col3), so the planes come from the
// matrix COLUMNS. (The usual write-up assumes column vectors and uses rows -
// copying that version gives six planes that are silently wrong.)
void ExtractPlanes(const float* m, float p[6][4])
{
    auto col = [&](int c, int r) { return m[r * 4 + c]; };
    for (int i = 0; i < 4; ++i)
    {
        p[0][i] = col(3, i) + col(0, i);   // left
        p[1][i] = col(3, i) - col(0, i);   // right
        p[2][i] = col(3, i) + col(1, i);   // bottom
        p[3][i] = col(3, i) - col(1, i);   // top
        p[4][i] = col(2, i);               // near  (z > 0)
        p[5][i] = col(3, i) - col(2, i);   // far
    }
    for (int i = 0; i < 6; ++i)
    {
        const float len = sqrtf(p[i][0]*p[i][0] + p[i][1]*p[i][1] + p[i][2]*p[i][2]);
        if (len > 1e-9f) for (int k = 0; k < 4; ++k) p[i][k] /= len;
    }
}

// A transform that mirrors (its 3x3 part's determinant negative) turns the
// winding of what it places round.
bool Mirrors(const float* m)
{
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8])
         + m[2] * (m[4] * m[9] - m[5] * m[8]) < 0.0f;
}

// Conservative: tests the AABB's positive vertex against each plane. Rejects
// only what is definitely outside, which is what culling must do - a false
// reject is a hole in the world, a false accept is merely wasted work.
bool AabbOutside(const float p[6][4], const float* bmin, const float* bmax)
{
    for (int i = 0; i < 6; ++i)
    {
        const float x = (p[i][0] >= 0.0f) ? bmax[0] : bmin[0];
        const float y = (p[i][1] >= 0.0f) ? bmax[1] : bmin[1];
        const float z = (p[i][2] >= 0.0f) ? bmax[2] : bmin[2];
        if (p[i][0]*x + p[i][1]*y + p[i][2]*z + p[i][3] < 0.0f) return true;
    }
    return false;
}

// The reverse-Z flip, in ONE place because two passes have to agree on it to
// the bit. The selection overlay depth-tests against what Record rasterised;
// a copy of these four lines that drifted by a term would present as a
// highlight that flickers at grazing angles, which reads as a bias problem and
// is not one.
//
// Row-vector convention: clip.z = dot(v, col2), clip.w = dot(v, col3), so
// near->1/far->0 is exactly col2 = col3 - col2.
void ReverseZ(const float* proj, float* out)
{
    for (int i = 0; i < 16; ++i) out[i] = proj[i];
    for (int r = 0; r < 4; ++r)
        out[r * 4 + 2] = proj[r * 4 + 3] - proj[r * 4 + 2];
}

void Mul4x4(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[r*4+k] * b[k*4+c];
            out[r*4+c] = sum;
        }
}

// General 4x4 inverse (the view-projection is not affine), by cofactors - kept local as aerial.cpp, rt.cpp and
// post.cpp keep theirs. False if singular.
bool Invert4x4(const float* m, float* out)
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
    const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
    if (det == 0.0) return false;
    const float r = (float)(1.0 / det);
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * r;
    return true;
}

#include "textures.inl"

} // namespace

// A point on the ground, roughly mid-stage, for a viewer to start at.
//
// This is a STAND-IN. The right answer is the car's start position, which RBR
// keeps in the track data and the converter does not currently carry into .ngg
// (design 9b item 11). Until it does, the bounding box alone is a poor guide:
// a stage is kilometres wide and ~150 m tall with an arbitrary origin, so its
// box centre is usually in mid-air over a valley, and its corner - which is
// what the first version of this used - is nowhere at all.
//
// Centroid for XZ, then the highest surface near that XZ for Y, so the camera
// starts above the terrain rather than inside it.
void Scene::ComputeFocusPoint(const Vertex* v, uint32_t count)
{
    if (!v || count == 0) return;

    double cx = 0.0, cz = 0.0;
    for (uint32_t i = 0; i < count; ++i) { cx += v[i].pos[0]; cz += v[i].pos[2]; }
    cx /= count; cz /= count;

    const float radius = 60.0f;
    float highest = -1e30f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const float dx = v[i].pos[0] - (float)cx;
        const float dz = v[i].pos[2] - (float)cz;
        if (dx * dx + dz * dz < radius * radius && v[i].pos[1] > highest)
            highest = v[i].pos[1];
    }
    if (highest < -1e29f) highest = (m_bboxMin[1] + m_bboxMax[1]) * 0.5f;

    m_focus[0] = (float)cx;
    m_focus[1] = highest;
    m_focus[2] = (float)cz;
    m_haveFocus = true;
    Log("scene: focus point (%.1f %.1f %.1f)", m_focus[0], m_focus[1], m_focus[2]);
}

bool Scene::FocusPoint(float* out3) const
{
    if (!m_haveFocus) return false;
    for (int i = 0; i < 3; ++i) out3[i] = m_focus[i];
    return true;
}

// Average normals across co-located vertices, preserving genuine hard edges.
//
// WHY THIS EXISTS: RBR's shaders never declared a normal input
// (doc/rbr-shaders.txt:1825 - no dcl_normal), so the normals in the track data
// were never used for shading by anything, and are split per face.
//
// THIS IS A FALLBACK FOR OLD ASSETS, AND IT DEFERS TO THE CONVERTER.
// wallaby-cli now welds at conversion, and does it better in ways that are
// measured rather than asserted: exact positions instead of buckets, area
// weighting, layer isolation (a fence post is not welded into the terrain it
// stands on), and complete-linkage clustering so a "within threshold" chain
// cannot smear across a hard edge - "within 60 degrees" is NOT transitive.
//
// Running this pass on an already-welded asset would therefore make it WORSE,
// not merely waste time: it would re-merge the pairs the converter deliberately
// forced apart. So it MEASURES FIRST and does nothing when the asset is already
// welded. Measured on a converted track-42: 302,496 co-located in-threshold
// pairs already identical against 24,378 differing - detection is unambiguous.
//
// Exact position keys, not quantised: the converter measured that RBR's
// duplicated vertices are bit-identical, and that 1 cm buckets merge across
// layers (1,572 cross-layer groups against 914 at exact equality).
void Scene::WeldNormals(std::vector<Vertex>& verts)
{
    if (verts.empty()) return;

    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

    const float kCosThreshold = 0.5f;     // 60 degrees

    const size_t buckets = verts.size() * 2 + 1;
    std::vector<uint32_t> head(buckets, UINT32_MAX);
    std::vector<uint32_t> next(verts.size(), UINT32_MAX);

    auto bits = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
    auto hashOf = [&](const Vertex& v) -> size_t {
        uint64_t h = (uint64_t)bits(v.pos[0]) * 73856093ULL
                   ^ (uint64_t)bits(v.pos[1]) * 19349663ULL
                   ^ (uint64_t)bits(v.pos[2]) * 83492791ULL;
        return (size_t)(h % buckets);
    };
    auto samePos = [](const Vertex& a, const Vertex& b) {
        return a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2];
    };

    for (uint32_t i = 0; i < (uint32_t)verts.size(); ++i)
    {
        const size_t h = hashOf(verts[i]);
        next[i] = head[h];
        head[h] = i;
    }

    // --- detect: are in-threshold co-located pairs already identical? -------
    size_t identical = 0, differing = 0;
    for (uint32_t i = 0; i < (uint32_t)verts.size(); ++i)
    {
        const Vertex& vi = verts[i];
        for (uint32_t j = head[hashOf(vi)]; j != UINT32_MAX; j = next[j])
        {
            if (j <= i) continue;
            const Vertex& vj = verts[j];
            if (!samePos(vi, vj)) continue;
            const float d = vi.normal[0] * vj.normal[0] + vi.normal[1] * vj.normal[1] +
                            vi.normal[2] * vj.normal[2];
            if (d < kCosThreshold) continue;               // hard edge, not our business
            if (d > 0.9999f) ++identical; else ++differing;
        }
    }

    if (identical > differing * 5)
    {
        Log("scene: normals already welded (%zu in-threshold pairs agree, %zu differ) "
            "- skipping the renderer fallback, the converter did it better",
            identical, differing);
        return;
    }

    std::vector<float> acc(verts.size() * 3, 0.0f);
    size_t changed = 0;

    for (uint32_t i = 0; i < (uint32_t)verts.size(); ++i)
    {
        const Vertex& vi = verts[i];
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;

        for (uint32_t j = head[hashOf(vi)]; j != UINT32_MAX; j = next[j])
        {
            const Vertex& vj = verts[j];
            if (j != i && !samePos(vi, vj)) continue;
            const float d = vi.normal[0] * vj.normal[0] + vi.normal[1] * vj.normal[1] +
                            vi.normal[2] * vj.normal[2];
            if (d < kCosThreshold) continue;
            nx += vj.normal[0]; ny += vj.normal[1]; nz += vj.normal[2];
        }

        const float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
        else { nx = vi.normal[0]; ny = vi.normal[1]; nz = vi.normal[2]; }

        // Count what actually MOVED. The first version counted vertices that
        // merely had a neighbour, and reported 268,706 "smoothed" on an asset
        // that was already welded - a number that described the input, not the
        // work.
        if (nx * vi.normal[0] + ny * vi.normal[1] + nz * vi.normal[2] < 0.99999f)
            ++changed;

        acc[i * 3 + 0] = nx; acc[i * 3 + 1] = ny; acc[i * 3 + 2] = nz;
    }

    for (size_t i = 0; i < verts.size(); ++i)
    {
        verts[i].normal[0] = acc[i * 3 + 0];
        verts[i].normal[1] = acc[i * 3 + 1];
        verts[i].normal[2] = acc[i * 3 + 2];
    }

    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    Log("scene: welded normals (fallback) - %zu of %zu vertices CHANGED, %.0f ms",
        changed, verts.size(), 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
}

// Replace foliage card normals with VOLUME normals: out of the plant's own
// centre, the same on every card of it.
//
// MEASURED on track-42: 1167 alpha-tested objects, median 20 triangles carrying
// 15 DISTINCT normals. RBR's trees are a handful of crossed cards, and each card
// carries its own plane normal - so two cards meeting at 90 degrees have normals
// 90 degrees apart. WeldNormals deliberately refuses to merge those, because
// geometrically they ARE a hard edge. The result is one card catching the sun
// while the card behind it goes dark, which reads as a tree cut out of cardboard.
//
// The standard fix is to stop pretending a billboard has a meaningful surface
// normal: shade the canopy as a rounded volume.
//
// PER PLANT, not per object. The first version took the OBJECT's centre, and
// on stage 41 an alpha-tested object is a whole stand of trees (448 objects,
// ~25,000 plants): every tree's normals pointed away from the middle of its
// stand, so a tree was lit or dark as a whole by where it stood. And it kept
// a quarter of the card normal, which the pixel shader then flipped for a
// back face - the card seen from behind got an INWARD normal, which is the
// very split this pass was meant to remove. Now: the plant (FindPlants), an
// ellipsoid around it, nothing of the card, and a marker so the shaders do not
// flip it (kVolumeScale).
//
// FALLBACK, like WeldNormals - the converter can do this better because it still
// knows the object hierarchy, whereas here an "object" is only an index range.
// ---------------------------------------------------------------------------
// Load-time fallback for kMaterialSingleStage.
//
// RBR's object meshes carry a second UV set that is not UV data, because their
// shader (RLSingleTexture.pso) never reads it. We do read it, so the fetch
// lands somewhere arbitrary - and the result is then BLENDED IN, because
// `weight = t1.a * vertexAlpha` and vertex alpha on an object mesh is coverage
// rather than a blend weight. On track-42 stage 1 of those materials points at
// the ORIGINAL atlas page while stage 0 points at the de-atlased cell, so what
// gets mixed into every leaf is a sample of the whole page.
//
// WHY THIS PASS EXISTS AT ALL, rather than trusting the vertex shader's
// isfinite() guard: THE GUARD NEVER FIRES. The garbage is finite. The largest
// values measured are around 3.1e36, comfortably inside float's 3.4e38 range,
// so `all(isfinite(uv1))` is true for every one of them and the fallback to uv0
// has never once been taken. A guard that cannot fail is worse than no guard,
// because it is read as protection.
//
// The magnitude test is what actually separates them, and it is not arbitrary:
// past about 1e6 the UV derivative across a triangle is thousands of texels per
// pixel, so the sampler collapses to the top mip and returns the texture's
// average colour whatever the coordinate was. At that point stage 1 carries no
// information and can only dilute stage 0.
namespace {
const uint64_t kRtWorld = 0ull << 56, kRtMoved = 1ull << 56, kRtDynamic = 2ull << 56;

// MOVED, for the ray tracing's denoise (RtInstanceDesc::moved) and for the
// motion target TAA and the denoise reproject by (RecordPass), is having gone
// further than this since last frame, in metres - not any change of the
// transform. A car standing with its engine running is set to a new transform
// every tick, a hundredth of a millimetre from the last, and as "moved" it cut
// the AO history around itself to a quarter: the ground about a stationary car
// boiled. 2 mm a frame lags the history by about a centimetre at most.
const float kRtMovedMin = 0.002f;

// How far an object went since last frame, in metres: the largest distance any
// corner of its (untransformed) box moved between the two transforms.
float Travel(const float* bmin0, const float* bmax0, const float* xform, const float* prev)
{
    float most = 0.0f;
    for (int corner = 0; corner < 8; ++corner)
    {
        const float c[3] = {
            (corner & 1) ? bmax0[0] : bmin0[0],
            (corner & 2) ? bmax0[1] : bmin0[1],
            (corner & 4) ? bmax0[2] : bmin0[2],
        };
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k)       // row vectors, as Scene::SetObjectTransform
        {
            const float d = c[0] * (xform[k] - prev[k]) + c[1] * (xform[4 + k] - prev[4 + k]) +
                            c[2] * (xform[8 + k] - prev[8 + k]) + (xform[12 + k] - prev[12 + k]);
            d2 += d * d;
        }
        if (d2 > most) most = d2;
    }
    return sqrtf(most);
}
}

void Scene::DescribeRt(std::vector<uint64_t>& meshes, std::vector<RtInstanceDesc>& instances) const
{
    meshes.clear();
    instances.clear();

    // The world's key: FNV over the load and the static objects it LEAVES OUT.
    // Same exclusions, same key - so an edit that only moves an already-moved
    // object does not touch the world BLAS. A keyed object (NGGK: the physics
    // moves it) is left out from the load, moved or not: its first move is
    // then a TLAS update, not a rebuild of the whole world's BLAS - which a
    // car going through a row of barriers paid once per barrier.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(m_rtGeneration);
    bool worldHasGeometry = false;
    for (size_t i = 0; i < m_objects.size(); ++i)
    {
        const DrawItem& o = m_objects[i];
        if (o.meshId != 0 || o.layerId == kLayerFarScenery) continue;
        if (!InRtWorld(i)) mix(i + 1);
        else worldHasGeometry = true;
    }
    if (worldHasGeometry)
    {
        meshes.push_back(kRtWorld | (h & 0x00FFFFFFFFFFFFFFull));
        instances.push_back(RtInstanceDesc{ meshes.back() });
    }

    for (size_t i = 0; i < m_objects.size(); ++i)
    {
        const DrawItem& o = m_objects[i];
        // Far scenery is a backdrop, not part of the lit world (kLayerFarScenery). Glass (kMaterialBlend) is not
        // in the ray-traced scene: it neither shadows nor occludes - the sun reaches a car's seats through its
        // windows.
        if (o.removed || !o.visible || o.layerId == kLayerFarScenery || (o.matFlags & kMaterialBlend)) continue;
        if (o.meshId == 0)
        {
            if (InRtWorld(i)) continue;                     // in the world
            RtInstanceDesc d;
            d.mesh = kRtMoved | (uint64_t)i;
            memcpy(d.xform, o.xform, sizeof(d.xform));
            d.moved = Travel(o.bmin0, o.bmax0, o.xform, o.prevXform) > kRtMovedMin;
            memcpy(d.prevXform, o.prevXform, sizeof(d.prevXform));
            meshes.push_back(d.mesh);
            instances.push_back(d);
        }
        else
        {
            RtInstanceDesc d;
            d.mesh = kRtDynamic | ((uint64_t)m_rtGeneration << 32) | o.meshId;
            memcpy(d.xform, o.xform, sizeof(d.xform));
            d.material = o.materialId;
            d.forceOpaque = (o.matFlags & kMaterialAlphaTest) == 0;
            d.moved = Travel(o.bmin0, o.bmax0, o.xform, o.prevXform) > kRtMovedMin;
            memcpy(d.prevXform, o.prevXform, sizeof(d.prevXform));
            if (std::find(meshes.begin(), meshes.end(), d.mesh) == meshes.end())
                meshes.push_back(d.mesh);
            instances.push_back(d);
        }
    }
}

bool Scene::BuildRtMesh(uint64_t key, RtMeshData& out) const
{
    out = RtMeshData();
    out.key = key;
    const uint64_t kind = key & 0xFF00000000000000ull;
    const uint32_t stride = m_vbv.StrideInBytes ? m_vbv.StrideInBytes : (uint32_t)sizeof(Vertex);

    auto addStatic = [&](const DrawItem& o) {
        if (o.firstIndex + o.indexCount > m_staticIndices.size()) return;
        if (o.matFlags & kMaterialBlend) return;   // glass: not in the ray-traced scene (DescribeRt)
        const uint32_t* src = m_staticIndices.data() + o.firstIndex;
        const bool alphaTested = (o.matFlags & kMaterialAlphaTest) != 0;
        std::vector<uint32_t>& dst = alphaTested ? out.alpha : out.opaque;
        dst.insert(dst.end(), src, src + o.indexCount);
        if (alphaTested)
        {
            out.alphaMat.insert(out.alphaMat.end(), o.indexCount / 3, o.materialId);
            const size_t t0 = o.firstIndex / 3;
            for (size_t t = t0; t < t0 + o.indexCount / 3; ++t)
                out.alphaPlant.push_back(t < m_triPlant.size() ? m_triPlant[t] : 0xFFFFFFFFu);
        }
    };

    if (kind == kRtWorld || kind == kRtMoved)
    {
        out.vb = m_vb.Get();
        out.vertexCount = StaticVertexCount();
        out.vbSelect = 0;
        if (kind == kRtWorld)
        {
            for (size_t i = 0; i < m_objects.size(); ++i)
                if (m_objects[i].meshId == 0 && m_objects[i].layerId != kLayerFarScenery && InRtWorld(i))
                    addStatic(m_objects[i]);
        }
        else
        {
            const size_t i = (size_t)(key & 0xFFFFFFFFull);
            if (i >= m_objects.size()) return false;
            addStatic(m_objects[i]);        // at its AUTHORED position: the instance moves it
        }
    }
    else if (kind == kRtDynamic)
    {
        const uint32_t meshId = (uint32_t)(key & 0xFFFFFFFFull);
        if (meshId == 0 || meshId > m_dynMeshes.size() || !m_dynVb) return false;
        const DynMesh& m = m_dynMeshes[meshId - 1];
        out.vb = m_dynVb.Get();
        out.vbOffset = (UINT64)m.firstVertex * stride;
        out.vertexCount = m.vertexCount;
        out.vbSelect = 1;
        out.vertexBase = m.firstVertex;
        // All NON-opaque: the material is the instance's, and so is whether it
        // alpha-tests - instances drawn opaque say so with FORCE_OPAQUE.
        out.alpha.assign(m_dynIndices.begin() + m.firstIndex,
                         m_dynIndices.begin() + m.firstIndex + m.indexCount);
        out.alphaMat.assign(m.indexCount / 3, 0xFFFFFFFFu);
        out.alphaPlant.assign(m.indexCount / 3, 0xFFFFFFFFu);
    }
    else
        return false;

    return !out.opaque.empty() || !out.alpha.empty();
}

bool Scene::BuildRtShared(ID3D12Device* dev, RtShared& out) const
{
    if (!dev || !m_vb) return false;
    out = RtShared();
    out.staticVb = m_vb.Get();
    out.staticVertexCount = StaticVertexCount();
    out.dynVb = m_dynVb.Get();
    out.dynVertexCount = (uint32_t)m_dynVerts.size();
    out.stride = m_vbv.StrideInBytes ? m_vbv.StrideInBytes : (uint32_t)sizeof(Vertex);

    struct MatEntry { uint32_t tex; float cutoff; };
    std::vector<MatEntry> mats(m_materials.size());
    out.textures.reserve(m_textures.size());
    for (const auto& t : m_textures)
        out.textures.push_back(t ? t.Get() : m_textures[m_whiteSlot].Get());
    for (size_t i = 0; i < m_materials.size(); ++i)
    {
        const uint32_t t0 = m_materials[i].tex[0];
        mats[i].tex = (t0 < out.textures.size()) ? t0 : m_whiteSlot;
        mats[i].cutoff = m_materials[i].alphaCutoff;
    }
    out.materialCount = (uint32_t)mats.size();
    out.materials = mats.empty() ? nullptr : MakeUploadBuffer(dev, mats.data(), mats.size() * sizeof(MatEntry));
    return true;
}

const WeatherPreset* Scene::FindWeather(const char* name) const
{
    if (!name || !*name) return m_weather.empty() ? nullptr : &m_weather[0];
    for (const WeatherPreset& w : m_weather)
        if (_stricmp(w.name.c_str(), name) == 0) return &w;
    return nullptr;
}

bool Scene::GetMaterial(uint32_t index, MaterialSlot* out) const
{
    if (!out || index >= m_materials.size()) return false;
    *out = m_materials[index];
    return true;
}

const char* Scene::MaterialName(uint32_t index) const
{
    if (index >= m_materials.size()) return nullptr;
    const uint32_t t = m_materials[index].tex[0];
    if (t >= m_textureNames.size()) return "";
    return m_textureNames[t].c_str();
}

bool Scene::ObjectMaterial(uint32_t index, uint32_t* out) const
{
    if (!out || index >= m_objects.size()) return false;
    *out = m_objects[index].materialId;
    return true;
}

// Live material edit. The scalars need nothing but a write - the draw loop
// reads m_materials every frame and sends them as root constants - so a slider
// in the editor costs one memcpy and the next frame shows it.
//
// Two things do need work, and both are only done when they actually changed:
//
//   - A TEXTURE slot rewrites the SRV heap. That is a descriptor write per
//     material, not an allocation, but it is not free and it must not happen on
//     every drag of a roughness slider.
//   - The DRAW flags are derived from the slots (design 3.3g), so they have to
//     be recomposed on every object using this material. The MEASURED
//     single-stage decision from MeasureSingleStage is kept separately and
//     re-applied here, because it came from the geometry and not from the
//     material - losing it on an unrelated edit would silently bring back the
//     atlas-average blend that 3.3f exists to remove.
bool Scene::SetMaterial(ID3D12Device* dev, uint32_t index, const MaterialSlot& in)
{
    if (index >= m_materials.size()) return false;

    MaterialSlot& m = m_materials[index];
    bool texturesChanged = false;
    for (int k = 0; k < kMaterialSrvCount; ++k)
        if (m.tex[k] != in.tex[k]) texturesChanged = true;

    m = in;
    // Derived, never taken from the caller: the same rule the loader follows.
    m.flags &= ~(uint32_t)(kDrawSingleStage | kDrawNormalMap);
    if (m.tex[1] == kNoTexture) m.flags |= kDrawSingleStage;
    if (m.tex[2] != kNoTexture) m.flags |= kDrawNormalMap;

    for (DrawItem& o : m_objects)
    {
        if (o.materialId != index) continue;
        o.matFlags = m.flags | (o.measuredSingleStage ? (uint32_t)kDrawSingleStage : 0u);
    }

    if (texturesChanged && m_materialBatch)
    {
        m_srvStale = true;                     // (EndMaterialBatch rebuilds the heap, once)
        return true;
    }
    if (texturesChanged && dev && !RebuildSrvHeap(dev))
    {
        Log("scene: material %u textures changed but the SRV heap rebuild failed", index);
        return false;
    }
    return true;
}

void Scene::MeasureSingleStage(const Vertex* verts, uint32_t vertexCount,
                               const uint8_t* indexData, uint32_t indexCount)
{
    if (!verts || !indexData || !m_singleStageFix) return;

    const uint32_t* index = reinterpret_cast<const uint32_t*>(indexData);
    const float kAbsurd = 1.0e6f;

    size_t flagged = 0, alreadyFlagged = 0, partial = 0, alphaTested = 0;
    for (DrawItem& o : m_objects)
    {
        if (o.matFlags & kDrawSingleStage) { ++alreadyFlagged; continue; }
        if (!o.indexCount) continue;

        // ALPHA-TESTED geometry never blends a second layer. Its vertex alpha
        // is COVERAGE (see PSMain), so it cannot also be the blend weight, and
        // RBR draws it with RLSingleTexture, which never reads stage 1. The
        // uv1 test below cannot see this: on stage 41 the foliage atlas
        // (material 106, 204 objects) has 132 objects whose uv1 is finite
        // and sane-looking - and points at the sign atlas, whose rows came
        // through as horizontal stripes across the roadside grass.
        if (o.materialId < m_materials.size() &&
            (m_materials[o.materialId].flags & kMaterialAlphaTest))
        {
            o.matFlags |= kDrawSingleStage;
            o.measuredSingleStage = true;
            ++alphaTested;
            continue;
        }

        size_t bad = 0, total = 0;
        for (uint32_t k = o.firstIndex; k < o.firstIndex + o.indexCount; ++k)
        {
            if (k >= indexCount) break;
            const uint32_t vi = index[k];
            if (vi >= vertexCount) continue;
            const float u = verts[vi].uv1[0], v = verts[vi].uv1[1];
            ++total;
            if (!isfinite(u) || !isfinite(v) || fabsf(u) > kAbsurd || fabsf(v) > kAbsurd)
                ++bad;
        }
        if (!total) continue;

        // ALL of them, not most. A partial object is geometry whose uv1 is
        // usable somewhere, and silently dropping a real blend layer is the
        // failure this pass must not cause - it would look exactly like a
        // missing detail texture.
        if (bad == total)      { o.matFlags |= kDrawSingleStage;
                                 o.measuredSingleStage = true; ++flagged; }
        else if (bad)          { ++partial; }
    }

    if (flagged || partial || alreadyFlagged || alphaTested)
        Log("scene: single-stage - %zu object(s) flagged by the file, %zu alpha-tested, %zu measured "
            "(uv1 unusable on every vertex), %zu mixed and left alone. The converter "
            "should set kMaterialSingleStage; this is the fallback.",
            alreadyFlagged, alphaTested, flagged, partial);
}

void Scene::SphericalFoliageNormals(std::vector<Vertex>& verts,
                                    const uint8_t* indexData, uint32_t indexCount)
{
    if (verts.empty() || !indexData || !indexCount) return;

    const uint32_t* idx = reinterpret_cast<const uint32_t*>(indexData);

    // Same deference as WeldNormals: the converter has a `spherical` /
    // `sphericalBlend` override that does this properly, per material, with a
    // human deciding which materials deserve it. Applying it twice would
    // compound the blend and over-round the canopy, so measure first.
    //
    // The measure is the mean angle between the stored normal and the outward
    // direction, which is exactly what the pass changes: on a converted
    // track-42 it is 89.1 degrees (card normals, not applied); with the
    // converter's override at blend 0.75 it drops to ~10 degrees.
    {
        double sumDeg = 0.0;
        size_t n = 0;
        for (const DrawItem& o : m_objects)
        {
            if (o.materialId >= m_materials.size()) continue;
            if (!(m_materials[o.materialId].flags & kMaterialAlphaTest)) continue;
            if (o.firstIndex + o.indexCount > indexCount) continue;

            double cx = 0.0, cy = 0.0, cz = 0.0;
            uint32_t m = 0;
            for (uint32_t k = o.firstIndex; k < o.firstIndex + o.indexCount; ++k)
            {
                const uint32_t v = idx[k];
                if (v >= verts.size()) continue;
                cx += verts[v].pos[0]; cy += verts[v].pos[1]; cz += verts[v].pos[2];
                ++m;
            }
            if (!m) continue;
            cx /= m; cy /= m; cz /= m;

            for (uint32_t k = o.firstIndex; k < o.firstIndex + o.indexCount; k += 7)
            {
                const uint32_t v = idx[k];
                if (v >= verts.size()) continue;
                float dx = verts[v].pos[0] - (float)cx;
                float dy = verts[v].pos[1] - (float)cy;
                float dz = verts[v].pos[2] - (float)cz;
                const float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len < 0.01f) continue;
                float dp = (verts[v].normal[0]*dx + verts[v].normal[1]*dy + verts[v].normal[2]*dz) / len;
                dp = dp > 1.0f ? 1.0f : (dp < -1.0f ? -1.0f : dp);
                sumDeg += acosf(dp) * (180.0f / 3.14159265f);
                ++n;
            }
        }
        if (n && (sumDeg / n) < 30.0)
        {
            Log("scene: foliage normals already spherical (mean %.1f deg off outward) "
                "- skipping the renderer fallback", sumDeg / n);
            return;
        }
        if (n)
            Log("scene: foliage normals are card normals (mean %.1f deg off outward)", sumDeg / n);
    }

    // The ellipsoid around the plant's box, its centre a quarter of the way
    // up rather than halfway, so the lower canopy leans out more than down: a
    // tree's underside facing the ground reads as a dark band, and on a grass
    // tuft - nearly all of it "underside" by height - as grass darker than the
    // ground it stands on.
    std::vector<uint8_t> done(verts.size(), 0);
    size_t touched = 0;
    std::vector<uint8_t> plantUsed(m_plants.size(), 0);
    for (size_t t = 0; t < m_triPlant.size() && t * 3 + 2 < indexCount; ++t)
    {
        const uint32_t pi = m_triPlant[t];
        if (pi >= m_plants.size() || !m_plants[pi].crossed) continue;
        const Plant& pl = m_plants[pi];
        plantUsed[pi] = 1;
        float c[3], e[3];
        for (int k = 0; k < 3; ++k)
        {
            c[k] = 0.5f * (pl.bmin[k] + pl.bmax[k]);
            e[k] = (std::max)(0.5f * (pl.bmax[k] - pl.bmin[k]), 0.05f);
        }
        const float h = pl.bmax[1] - pl.bmin[1];
        c[1] = pl.bmin[1] + 0.25f * h;
        e[1] = (std::max)(0.75f * h, 0.05f);
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t v = idx[t * 3 + k];
            if (v >= verts.size() || done[v]) continue;
            done[v] = 1;
            // The point in the plant's unit space (kVolumeScale). At the very
            // centre there is no direction: nudged up, so it still reads as a
            // plant and its normal is the sky's.
            float q[3];
            for (int j = 0; j < 3; ++j) q[j] = (verts[v].pos[j] - c[j]) / e[j];
            float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            if (l < 1e-3f) { q[0] = 0.0f; q[1] = 1e-3f; q[2] = 0.0f; l = 1e-3f; }
            if (l > 2.2f) for (float& x : q) x *= 2.2f / l;
            for (int j = 0; j < 3; ++j) verts[v].normal[j] = q[j] * kVolumeScale;
            // The plant's index in uv1, which nothing alpha-tested reads
            // (MeasureSingleStage): the prepass writes it per pixel, and a
            // ray from the pixel skips only that plant's cards (rt.cpp).
            // And its size: the self-shadow and the inner AO grow with it
            // (FoliageSelfShadow). The THINNER of its half-width and
            // half-height, which is how far light goes through it - roadside
            // grass is a strip metres long and 40 cm tall, and its length as
            // the size gave sunlit tufts a tree's dark side (sun 0.70).
            verts[v].uv1[0] = (float)pi;
            // And what it casts, in the thousands (Plant::shadow: the shadow pass and the rays read it there).
            verts[v].uv1[1] = (std::min)((std::max)(e[0], e[2]), 0.5f * h) + kPlantShadowPack * (float)pl.shadow;
            ++touched;
        }
    }
    size_t plants = 0;
    for (uint8_t u : plantUsed) plants += u;
    Log("scene: volume foliage normals - %zu vertices across %zu plants of crossed cards",
        touched, plants);
}

// The PLANTS in the alpha-tested objects. An object is an index range, and on
// stage 41 it is usually a whole stand of trees or a strip of grass tufts, so
// shading or shadowing "per object" means per stand. A plant is found in two
// steps: ISLANDS (triangles joined by a vertex, or by a vertex at the same
// position), then islands standing on the same spot joined - crossed cards
// share their centre seen from above and overlap in height, two neighbouring
// trees do not. Measured on stage 41: 27,270 islands make 25,042 plants; most
// islands already hold 3 card planes, the tree's cards sharing their crossing
// edge in the mesh.
//
// Feeds the volume normals (SphericalFoliageNormals) and the ray tracer, whose
// foliage rays ignore the plant they start in (rt.cpp, Occluded).
static const float kPlantMaxWidth = 50.0f;    // metres; see FindPlants
// An untagged import's plant casts by its height (Plant::shadow) as Wallaby's tags would have it - measured on stage
// 41's tagged .ngg: grass-card 99% under 0.60 m, bush-card 10-90% 0.69-2.25 m, tree-card from 2.89 m (p10). Under
// kGrassHeight nothing, under kBushHeight a bush's soft shadow, above all of it.
static const float kGrassHeight = 0.65f;      // metres
static const float kBushHeight = 2.5f;        // metres

bool Scene::ReadPlants(const uint8_t* tags, size_t tagBytes,
                       const uint8_t* table, size_t tableBytes, uint32_t indexCount, bool untaggedCards)
{
    m_plants.clear();
    m_triPlant.clear();
    m_tagNames.clear();
    m_plantTagRefs.clear();
    if (!table || tableBytes < sizeof(PlantTableHeader)) return false;

    // Tag names.
    if (tags && tagBytes >= 4)
    {
        uint32_t n = 0;
        memcpy(&n, tags, 4);
        size_t at = 4;
        for (uint32_t i = 0; i < n && at + 4 <= tagBytes; ++i)
        {
            uint32_t len = 0;
            memcpy(&len, tags + at, 4);
            at += 4;
            if (at + len > tagBytes) break;
            m_tagNames.emplace_back(reinterpret_cast<const char*>(tags + at), len);
            at += len + (4 - len % 4) % 4;
        }
    }

    PlantTableHeader h;
    memcpy(&h, table, sizeof(h));
    const size_t need = sizeof(h) + (size_t)h.plantCount * sizeof(PlantRecord)
                      + (size_t)h.partCount * sizeof(PlantPart) + (size_t)h.tagRefCount * 4;
    if (tableBytes < need)
    {
        Log("scene: plant table is %zu bytes, its header says %zu - ignored, plants guessed", tableBytes, need);
        return false;
    }
    const PlantRecord* rec = reinterpret_cast<const PlantRecord*>(table + sizeof(h));
    const PlantPart* parts = reinterpret_cast<const PlantPart*>(rec + h.plantCount);
    const uint32_t* refs = reinterpret_cast<const uint32_t*>(parts + h.partCount);
    m_plantTagRefs.assign(refs, refs + h.tagRefCount);

    // The tags this renderer acts on: old crossed-card vegetation, one volume per plant.
    std::vector<uint8_t> isCard(m_tagNames.size(), 0);
    for (size_t i = 0; i < m_tagNames.size(); ++i)
        isCard[i] = m_tagNames[i] == "tree-card" || m_tagNames[i] == "bush-card" ||
                    m_tagNames[i] == "grass-card";

    m_plants.resize(h.plantCount);
    size_t cards = 0, shadowless = 0, soft = 0;
    for (uint32_t p = 0; p < h.plantCount; ++p)
    {
        Plant& pl = m_plants[p];
        memcpy(pl.bmin, rec[p].bboxMin, sizeof(pl.bmin));
        memcpy(pl.bmax, rec[p].bboxMax, sizeof(pl.bmax));
        pl.firstTag = rec[p].firstTagRef;
        pl.tagCount = rec[p].tagCount;
        pl.crossed = untaggedCards;
        // What it casts: an untagged import's plant by its height (a tuft or a strip of grass is short, its shadow
        // the cards' outlines and nothing else), a tagged one by its tag.
        const float height = pl.bmax[1] - pl.bmin[1];
        pl.shadow = !untaggedCards ? kPlantShadowCast : height < kGrassHeight ? kPlantShadowNone
                  : height < kBushHeight ? kPlantShadowSoft : kPlantShadowCast;
        for (uint32_t k = 0; k < pl.tagCount; ++k)
        {
            const uint32_t r = pl.firstTag + k;
            if (r < m_plantTagRefs.size() && m_plantTagRefs[r] < isCard.size() && isCard[m_plantTagRefs[r]])
                pl.crossed = true;
            if (r < m_plantTagRefs.size() && m_plantTagRefs[r] < m_tagNames.size())
            {
                const std::string& t = m_tagNames[m_plantTagRefs[r]];
                if (t == "grass-card") pl.shadow = kPlantShadowNone;
                else if (t == "bush-card") pl.shadow = kPlantShadowSoft;
            }
        }
        cards += pl.crossed;
        shadowless += pl.shadow == kPlantShadowNone;
        soft += pl.shadow == kPlantShadowSoft;
    }

    m_triPlant.assign(indexCount / 3, 0xFFFFFFFFu);
    size_t tris = 0, bad = 0;
    for (uint32_t i = 0; i < h.partCount; ++i)
    {
        const PlantPart& q = parts[i];
        if (q.plant >= h.plantCount || q.firstIndex % 3 || q.indexCount % 3 ||
            (uint64_t)q.firstIndex + q.indexCount > indexCount) { ++bad; continue; }
        for (uint32_t t = q.firstIndex / 3; t < (q.firstIndex + q.indexCount) / 3; ++t)
            m_triPlant[t] = q.plant;
        tris += q.indexCount / 3;
    }
    Log("scene: plants from the file - %u plants in %u parts (%zu triangles), %zu of crossed cards, "
        "%zu tag names; %zu cast no shadow, %zu a soft one%s", h.plantCount, h.partCount, tris, cards, m_tagNames.size(),
        shadowless, soft,
        bad ? " - some parts out of range, skipped" : "");
    return true;
}
void Scene::FindPlants(const Vertex* verts, uint32_t vertexCount,
                       const uint8_t* indexData, uint32_t indexCount)
{
    m_plants.clear();
    m_triPlant.assign(indexCount / 3, 0xFFFFFFFFu);
    if (!verts || !vertexCount || !indexData || !indexCount) return;
    const uint32_t* idx = reinterpret_cast<const uint32_t*>(indexData);

    struct Uf
    {
        std::vector<uint32_t> p;
        uint32_t Find(uint32_t x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
        void Join(uint32_t a, uint32_t b) { a = Find(a); b = Find(b); if (a != b) p[b] = a; }
    };
    struct Box { float mn[3], mx[3]; };
    auto grow = [](Box& b, const float* q) {
        for (int c = 0; c < 3; ++c) { b.mn[c] = (std::min)(b.mn[c], q[c]); b.mx[c] = (std::max)(b.mx[c], q[c]); }
    };
    const Box kEmpty = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };

    size_t islandsTotal = 0, crossedPlants = 0, crossedTris = 0, singleTris = 0;
    std::vector<uint32_t> hist(8, 0);   // islands per plant: 1..7, 8+

    for (const DrawItem& o : m_objects)
    {
        if (o.meshId != 0 || o.materialId >= m_materials.size()) continue;
        if (!(m_materials[o.materialId].flags & kMaterialAlphaTest)) continue;
        if (o.firstIndex + o.indexCount > indexCount || o.indexCount < 3) continue;
        const uint32_t tris = o.indexCount / 3;
        const uint32_t t0 = o.firstIndex / 3;

        // ISLANDS.
        Uf uf; uf.p.resize(tris);
        for (uint32_t t = 0; t < tris; ++t) uf.p[t] = t;
        {
            std::unordered_map<uint64_t, uint32_t> at;   // quantised position -> a triangle
            at.reserve(tris * 3);
            for (uint32_t t = 0; t < tris; ++t)
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t v = idx[o.firstIndex + t * 3 + k];
                    if (v >= vertexCount) continue;
                    const float* q = verts[v].pos;
                    // Exact, not hashed: 21 bits a coordinate at 1 cm is
                    // +-10 km, and a collision here would join two trees.
                    auto cm = [](float x) { return (uint64_t)((int64_t)floorf(x * 100.0f + 0.5f) + (1 << 20)) & 0x1FFFFFull; };
                    const uint64_t key = cm(q[0]) | (cm(q[1]) << 21) | (cm(q[2]) << 42);
                    auto it = at.emplace(key, t);
                    if (!it.second) uf.Join(it.first->second, t);
                }
        }
        std::vector<uint32_t> islandOf(tris), root2island(tris, 0xFFFFFFFFu);
        std::vector<Box> islands;
        for (uint32_t t = 0; t < tris; ++t)
        {
            const uint32_t r = uf.Find(t);
            if (root2island[r] == 0xFFFFFFFFu) { root2island[r] = (uint32_t)islands.size(); islands.push_back(kEmpty); }
            islandOf[t] = root2island[r];
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t v = idx[o.firstIndex + t * 3 + k];
                if (v < vertexCount) grow(islands[islandOf[t]], verts[v].pos);
            }
        }
        islandsTotal += islands.size();

        // PLANTS: a hash on the islands' centres in XZ finds the candidates.
        const uint32_t n = (uint32_t)islands.size();
        Uf pl; pl.p.resize(n);
        for (uint32_t i = 0; i < n; ++i) pl.p[i] = i;
        const float kCell = 1.0f;
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        auto cellKey = [](int x, int z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; };
        auto cx = [](const Box& b) { return 0.5f * (b.mn[0] + b.mx[0]); };
        auto cz = [](const Box& b) { return 0.5f * (b.mn[2] + b.mx[2]); };
        auto reach = [](const Box& b) {      // how far apart two centres may be
            return 0.25f * (std::max)(b.mx[0] - b.mn[0], b.mx[2] - b.mn[2]);
        };
        for (uint32_t i = 0; i < n; ++i)
            grid[cellKey((int)floorf(cx(islands[i]) / kCell), (int)floorf(cz(islands[i]) / kCell))].push_back(i);
        for (uint32_t i = 0; i < n; ++i)
        {
            const Box& a = islands[i];
            const float r = reach(a);
            const int gx = (int)floorf(cx(a) / kCell), gz = (int)floorf(cz(a) / kCell);
            const int span = (int)ceilf(r / kCell);
            for (int x = gx - span; x <= gx + span; ++x)
                for (int z = gz - span; z <= gz + span; ++z)
                {
                    auto it = grid.find(cellKey(x, z));
                    if (it == grid.end()) continue;
                    for (uint32_t j : it->second)
                    {
                        if (j <= i) continue;
                        const Box& b = islands[j];
                        const float dx = cx(a) - cx(b), dz = cz(a) - cz(b);
                        const float rr = (std::max)(r, reach(b));
                        if (dx * dx + dz * dz > rr * rr) continue;
                        // Overlapping in height, by half the shorter one.
                        const float lo = (std::max)(a.mn[1], b.mn[1]), hi = (std::min)(a.mx[1], b.mx[1]);
                        const float shorter = (std::min)(a.mx[1] - a.mn[1], b.mx[1] - b.mn[1]);
                        if (hi - lo < 0.5f * shorter) continue;
                        pl.Join(i, j);
                    }
                }
        }

        std::vector<uint32_t> root2plant(n, 0xFFFFFFFFu), plantIslands;
        const uint32_t firstPlant = (uint32_t)m_plants.size();
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t r = pl.Find(i);
            if (root2plant[r] == 0xFFFFFFFFu)
            {
                root2plant[r] = (uint32_t)m_plants.size();
                Plant p;
                p.crossed = false;
                memcpy(p.bmin, kEmpty.mn, sizeof(p.bmin));
                memcpy(p.bmax, kEmpty.mx, sizeof(p.bmax));
                m_plants.push_back(p);
                plantIslands.push_back(0);
            }
            Plant& p = m_plants[root2plant[r]];
            for (int c = 0; c < 3; ++c)
            {
                p.bmin[c] = (std::min)(p.bmin[c], islands[i].mn[c]);
                p.bmax[c] = (std::max)(p.bmax[c], islands[i].mx[c]);
            }
            ++plantIslands[root2plant[r] - firstPlant];
        }

        // CROSSED or not: the distinct card planes in the plant (face normals
        // more than 20 degrees apart, either side). A plant of one plane is a
        // single card - a fence, a spectator, a lone strip of grass - and keeps
        // its card normal.
        std::vector<std::vector<std::array<float, 3>>> planes(m_plants.size() - firstPlant);
        for (uint32_t t = 0; t < tris; ++t)
        {
            const uint32_t p = root2plant[pl.Find(islandOf[t])];
            m_triPlant[t0 + t] = p;
            const uint32_t* tv = idx + o.firstIndex + t * 3;
            if (tv[0] >= vertexCount || tv[1] >= vertexCount || tv[2] >= vertexCount) continue;
            const float* a = verts[tv[0]].pos;
            const float* b = verts[tv[1]].pos;
            const float* c = verts[tv[2]].pos;
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float l = sqrtf(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
            if (l < 1e-9f) continue;
            for (float& x : fn) x /= l;
            auto& v = planes[p - firstPlant];
            bool known = false;
            for (const auto& q : v)
                if (fabsf(q[0] * fn[0] + q[1] * fn[1] + q[2] * fn[2]) > 0.94f) { known = true; break; }
            if (!known && v.size() < 8) v.push_back({ fn[0], fn[1], fn[2] });
        }
        for (size_t i = 0; i < planes.size(); ++i)
        {
            Plant& p = m_plants[firstPlant + i];
            // And not a hillside. Stage 41's big trees are 20-40 m across and
            // 20-33 m tall, 3-5 card planes (a 15 m cap left them split card
            // by card); its forest walls run 80 m. Since the rays skip only
            // their own plant's cards, by id, a large plant costs no one else
            // a shadow - the cap only keeps a volume normal off a wall.
            const float width = (std::max)(p.bmax[0] - p.bmin[0], p.bmax[2] - p.bmin[2]);
            p.crossed = planes[i].size() >= 2 && width <= kPlantMaxWidth;
            if (p.crossed) ++crossedPlants;
            ++hist[(std::min<uint32_t>)(plantIslands[i], 8) - 1];
        }
        for (uint32_t t = 0; t < tris; ++t)
            if (m_plants[m_triPlant[t0 + t]].crossed) ++crossedTris; else ++singleTris;
    }
    // Size, as a check on the grouping: a "plant" wider than a tree means
    // islands of different trees were joined.
    float widest = 0.0f, tallest = 0.0f;
    size_t wide = 0;
    for (const Plant& p : m_plants)
    {
        const float w = (std::max)(p.bmax[0] - p.bmin[0], p.bmax[2] - p.bmin[2]);
        if (w > kPlantMaxWidth) { ++wide; continue; }
        if (!p.crossed) continue;
        widest = (std::max)(widest, w);
        tallest = (std::max)(tallest, p.bmax[1] - p.bmin[1]);
    }
    Log("scene: plants - %zu islands in %zu plants (%zu of crossed cards, %zu triangles; %zu triangles in "
        "single cards); islands per plant 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8+:%u; widest %.1f m, "
        "tallest %.1f m; %zu wider than that, left as cards",
        islandsTotal, m_plants.size(), crossedPlants, crossedTris, singleTris,
        hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], widest, tallest, wide);
}

bool Scene::CreateProbe(ID3D12Device* dev)
{
    m_probe.Reset();
    m_probeReadback.Reset();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kProbeSlots * 16;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_probe))))
        return false;

    // A block of zeros to reset the probe with. The FIRED marker only means
    // anything against a buffer that was cleared first.
    std::vector<uint8_t> zeros(kProbeSlots * 16, 0);
    m_probeZero = MakeUploadBuffer(dev, zeros.data(), zeros.size());
    if (!m_probeZero) return false;

    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    return SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&m_probeReadback)));
}

// Called AFTER the caller has waited for the GPU. The frame loop is synchronous,
// so by then the copy has landed and no fence of our own is needed.
void Scene::LogProbe()
{
    if (!m_probeOn || !m_probeReadback) return;
    m_probeOn = false;                 // one shot per click

    float* p = nullptr;
    D3D12_RANGE all = { 0, kProbeSlots * 16 };
    if (FAILED(m_probeReadback->Map(0, &all, reinterpret_cast<void**>(&p)))) return;

    static const char* kNames[kProbeSlots] = {
        "albedo            (rgb) / blendWeight",
        "bakedColour*scale (rgb) / vertexAlpha",
        "normal            (xyz) / frontFace",
        "sunColour         (rgb) / N.L",
        "ambient           (rgb) / aoFactor",
        "E_sky             (rgb)",
        "E_ground          (rgb)",
        "lit (pre-tonemap) (rgb) / exposure",
        "aerial transmit  (rgb) / DISTANCE m",
        "aerial inscatter (rgb) / aerialOn",
        "shader out, LINEAR HDR (rgb) / lightMode",
        "debugMode/samplerMode/skyAmbient/groundBounce",
        "sunVis / cascade / lightZ / STORED depth",
        "shadowUV.x / .y / viewDepth / shadowsOn",
        "ambient SPECULAR (rgb) / envOn",
        "FIRED",
    };
    if (p[(kProbeSlots - 1) * 4] < 0.5f)
    {
        // Not an error, and not nothing: "no geometry covered that pixel" is a
        // real answer about the frame, and it is the answer that used to be
        // silently replaced by the previous frame's numbers.
        Log("--- pixel probe (%d,%d): DID NOT FIRE - no scene geometry at that pixel ---",
            m_probeX, m_probeY);
        D3D12_RANGE nothing = { 0, 0 };
        m_probeReadback->Unmap(0, &nothing);
        return;
    }

    Log("--- pixel probe (%d,%d) ---", m_probeX, m_probeY);
    for (int i = 0; i < kProbeSlots - 1; ++i)
        Log("  %-38s %8.4f %8.4f %8.4f | %8.4f",
            kNames[i], p[i*4+0], p[i*4+1], p[i*4+2], p[i*4+3]);

    // Slot 10 is the value actually written to the render target, so there is
    // nothing left to derive. The previous version re-applied the tonemap to a
    // number it could not be sure was pre- or post-, and reported a figure that
    // matched neither.

    D3D12_RANGE none = { 0, 0 };
    m_probeReadback->Unmap(0, &none);
}

// Binds an object's transform, skipping the write when identity is already
// bound. Most of a track never moves, so the common case costs one comparison
// rather than 16 root-constant DWORDs per draw.
//
// `state` is -1 unknown, 1 identity bound, 0 a real matrix bound.
static void BindObjectXform(ID3D12GraphicsCommandList* list, const float xform[16],
                            bool identity, int& state)
{
    if (identity && state == 1) return;
    list->SetGraphicsRoot32BitConstants(4, 16, xform, 4);
    state = identity ? 1 : 0;
}

// Binds whichever buffers an object's geometry lives in, skipping the call when
// they are already bound. Static content is one bind for the whole frame, so
// adding a dynamic pool costs the existing path nothing.
//
// `state` is -1 unknown, 0 static bound, 1 dynamic bound.
static void BindGeometry(ID3D12GraphicsCommandList* list, bool dynamic, int& state,
                         const D3D12_VERTEX_BUFFER_VIEW& vbv,
                         const D3D12_INDEX_BUFFER_VIEW& ibv)
{
    const int want = dynamic ? 1 : 0;
    if (state == want) return;
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetIndexBuffer(&ibv);
    state = want;
}

void Scene::RenderShadows(ID3D12GraphicsCommandList* list,
                         const float* view, const float* proj, const float* sunDirTravel)
{
    if (!m_shadows || !m_shadows->Ready() || !m_psoShadow || !Ready() || !m_shadowsOn) return;

    // Params carry the direction light TRAVELS; fitting wants the direction
    // TOWARDS the sun. Same negation as everywhere else, done once.
    float toward[3] = { -sunDirTravel[0], -sunDirTravel[1], -sunDirTravel[2] };
    const float len = sqrtf(toward[0]*toward[0] + toward[1]*toward[1] + toward[2]*toward[2]);
    if (len < 1e-6f) return;
    for (int k = 0; k < 3; ++k) toward[k] /= len;

    // Sun below the horizon casts nothing; fitting a cascade to it produces a
    // degenerate projection.
    if (toward[1] <= 0.02f) return;

    m_shadows->Update(view, proj, toward, 1.0f, 1500.0f);
    m_shadows->ToDepthWrite(list);

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;

    const UINT res = m_shadows->Resolution();
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)res, (float)res, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)res, (LONG)res };

    // Last call's time (the frame is synchronous), then open this one.
    const bool timed = m_shadowTimer && m_shadowTimerReadback && m_timerHz;
    if (timed && m_shadowTimerPending)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r = { 0, 2 * sizeof(UINT64) };
        if (SUCCEEDED(m_shadowTimerReadback->Map(0, &r, reinterpret_cast<void**>(&t))))
        {
            if (t[1] >= t[0]) m_shadowMapMs = 1000.0 * double(t[1] - t[0]) / double(m_timerHz);
            D3D12_RANGE none = { 0, 0 };
            m_shadowTimerReadback->Unmap(0, &none);
        }
        m_shadowTimerPending = false;
    }
    if (timed) list->EndQuery(m_shadowTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    for (int c = 0; c < ShadowMaps::kCascades; ++c)
    {
        // One constant buffer per cascade: the vertex shader is the scene's own
        // VSMain, so "render from the light" is just a different view/proj.
        CameraCB cb = {};
        memcpy(cb.view, m_shadows->LightViewProj(c), sizeof(cb.view));
        // Identity projection - the light view-proj is already combined above.
        memset(cb.proj, 0, sizeof(cb.proj));
        cb.proj[0] = cb.proj[5] = cb.proj[10] = cb.proj[15] = 1.0f;
        cb.bakedScale = m_bakedScale;

        ComPtr<ID3D12Resource> cbRes = MakeUploadBuffer(dev, &cb, sizeof(cb));
        if (!cbRes) continue;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_shadows->Dsv(c);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);

        list->SetGraphicsRootSignature(m_rootSig.Get());
        list->SetGraphicsRootConstantBufferView(0, cbRes->GetGPUVirtualAddress());
        if (m_probe) list->SetGraphicsRootUnorderedAccessView(2, m_probe->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &m_vbv);
        list->IASetIndexBuffer(&m_ibv);

        if (m_srvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
            list->SetDescriptorHeaps(1, heaps);
        }

        float cplanes[6][4];
        ExtractPlanes(m_shadows->LightViewProj(c), cplanes);

        bool alphaBound = false;
        // Unknown, not identity: the shadow pass shares VSMain, so leaving the
        // constants unset would have it transform by whatever the last pass
        // wrote -- a moved object casting from where it used to be, or worse.
        int  xformState = -1;
        int  geomState = 0;          // the static buffers are bound just above
        // Same reason for the moved flag: VSMain reads it, and the motion
        // output it guards is meaningless here.
        const uint32_t notMoved = 0;
        list->SetGraphicsRoot32BitConstants(4, 1, &notMoved, 2);
        list->SetPipelineState(m_psoShadow.Get());
        for (const DrawItem& o : m_objects)
        {
            if (!o.visible) continue;                      // hidden casts nothing
            if (o.materialId >= m_materials.size()) continue;
            if (o.layerId == kLayerFarScenery) continue;   // never a caster
            // Cull against THIS cascade's volume. Cascade 0 covers a few tens
            // of metres, so it should touch a tiny fraction of the stage - this
            // is where most of the shadow cost disappears.
            if (m_cull && AabbOutside(cplanes, o.bmin, o.bmax)) continue;
            if (m_materials[o.materialId].flags & kMaterialBlend) continue;   // glass casts no shadow
            const bool wantAlpha = (m_materials[o.materialId].flags & kMaterialAlphaTest) != 0;
            if (wantAlpha && !m_psoShadowAlpha) continue;
            if (wantAlpha != alphaBound)
            {
                list->SetPipelineState(wantAlpha ? m_psoShadowAlpha.Get() : m_psoShadow.Get());
                alphaBound = wantAlpha;
            }
            if (wantAlpha && m_srvHeap)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
                h.ptr += (UINT64)o.materialId * kMaterialSrvCount * m_srvStride;
                list->SetGraphicsRootDescriptorTable(1, h);
            }
            BindObjectXform(list, o.xform, o.identity, xformState);
            // A dynamically added prop has to cast too; leaving it out gives a
            // solid object with no shadow, which reads as a lighting bug.
            BindGeometry(list, o.meshId != 0, geomState,
                         o.meshId ? m_dynVbv : m_vbv, o.meshId ? m_dynIbv : m_ibv);
            list->DrawIndexedInstanced(o.indexCount, 1, o.firstIndex, o.baseVertex, 0);
        }
        m_shadowCB[c] = cbRes;      // must outlive GPU execution
    }
    dev->Release();

    m_shadows->ToShaderResource(list);

    if (timed)
    {
        list->EndQuery(m_shadowTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(m_shadowTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               m_shadowTimerReadback.Get(), 0);
        m_shadowTimerPending = true;
    }
}

// The mode names, as ARRAYS rather than switches, for two reasons that both bit.
//
// A switch needs a `default`, and a default absorbs everything: it made an
// out-of-range index return the last name instead of null, and it would make a
// mode added to the enum without a name here return that same last name. Neither
// shows up as an error. With an array the count is a fact the compiler can check,
// which is what the static_asserts below do.
//
// And the modulo does NOT belong here. Cycling is the setters' job -- SetDebugMode
// and SetLightMode already wrap -- while these answer "what is mode N called", for
// which the honest answer to an out-of-range N is null. Wrapping made
// ngg_debug_view_name(9) return mode 0's name, so a caller enumerating until null
// never terminated and the editor's Rendering dialog listed the same views over and
// over.
static const char* const kLightModeNames[] = {
    "BAKED - RBR's vertex colour, kept as the A/B reference",
    "DYNAMIC - sun + sky from the atmosphere, with real shadows",
};

static const char* const kDebugModeNames[] = {
    "normal",
    "tex0 only",
    "tex1 only",
    "blend weight (tex1.a * vertex.a)",
    "baked vertex colour only",
    "uv0 as colour",
    "FLAT GREY - no shading at all; anything visible is geometry/depth",
    "sun visibility - white = lit, black = shadowed",
    "SHADOW ACNE - RED = self-shadowing, blue = real shadow, grey = lit",
    "ambient specular only (x8 - the term is a few % by construction)",
    "roughness (red) / metallic (green)",
    "RT sun visibility (last frame) - white = lit",
    "RT vs SHADOW MAP - RED = RT shadow/map lit, BLUE = reverse, grey = agree",
    "RT ambient occlusion - white = sees the whole sky",
    "PLANTS - green = a card of a plant (volume normal), red = other alpha-tested",
    "RT local lights - the lamps' shadowed fraction, white = every lamp reaches it",
};

static_assert(sizeof(kLightModeNames) / sizeof(kLightModeNames[0])
                  == Scene::kLightModeCount,
              "kLightModeNames and kLightModeCount disagree");
static_assert(sizeof(kDebugModeNames) / sizeof(kDebugModeNames[0])
                  == Scene::kDebugCount,
              "kDebugModeNames and kDebugCount disagree");

const char* Scene::LightModeName(int mode)
{
    if (mode < 0 || mode >= kLightModeCount) return nullptr;
    return kLightModeNames[mode];
}

const char* Scene::DebugModeName(int mode)
{
    if (mode < 0 || mode >= kDebugCount) return nullptr;
    return kDebugModeNames[mode];
}

void Scene::Bounds(float* outMin3, float* outMax3) const
{
    for (int i = 0; i < 3; ++i) { outMin3[i] = m_bboxMin[i]; outMax3[i] = m_bboxMax[i]; }
}

bool Scene::CreateDepth(ID3D12Device* dev, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    // TYPELESS, not D32_FLOAT: TAA reprojects from depth (post.h), and a
    // depth-format resource cannot carry a shader resource view. The DSV below
    // supplies the D32_FLOAT interpretation that the format no longer implies.
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 0.0f;   // reverse-Z: far plane is 0

    m_depth.Reset();
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                            &clear, IID_PPV_ARGS(&m_depth))))
    {
        Log("scene: depth buffer creation failed");
        return false;
    }

    if (!m_dsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 2;                   // the depth, and its read-only view (the deferred lights)
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap))))
            return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(m_depth.Get(), &dsv,
                                m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // The normal buffer, same size, written by the prepass and read by the ray
    // tracer. Lives in RENDER_TARGET between frames; Rt::Trace borrows it.
    D3D12_RESOURCE_DESC nd = desc;
    nd.Format = kNormalFormat;
    nd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE nclear = {};
    nclear.Format = kNormalFormat;
    m_normal.Reset();
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &nd,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &nclear, IID_PPV_ARGS(&m_normal))))
    {
        Log("scene: normal buffer creation failed");
        return false;
    }
    // And the motion target beside it, same lifetime and state. Its RTV is the
    // heap's second slot, so the prepass binds both as one contiguous range.
    D3D12_RESOURCE_DESC md = nd;
    md.Format = kMotionFormat;
    D3D12_CLEAR_VALUE mclear = {};
    mclear.Format = kMotionFormat;
    m_motion.Reset();
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &md,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &mclear, IID_PPV_ARGS(&m_motion))))
    {
        Log("scene: motion buffer creation failed");
        return false;
    }
    D3D12_RESOURCE_DESC pd = nd;
    pd.Format = kPlantFormat;
    D3D12_CLEAR_VALUE pclear = {};
    pclear.Format = kPlantFormat;
    m_plantTarget.Reset();
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &pd,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            &pclear, IID_PPV_ARGS(&m_plantTarget))))
    {
        Log("scene: plant id buffer creation failed");
        return false;
    }
    if (!m_normalRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_normalRtvHeap))))
            return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs = m_normalRtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(m_normal.Get(), nullptr, rtvs);
    m_prepassRtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtvs.ptr += m_prepassRtvStride;
    dev->CreateRenderTargetView(m_motion.Get(), nullptr, rtvs);
    rtvs.ptr += m_prepassRtvStride;
    dev->CreateRenderTargetView(m_plantTarget.Get(), nullptr, rtvs);

    // The deferred local lights' surface (kGbufFormats), and the depth's READ-ONLY view: in that pass and the glass
    // after it the depth is read (and tested) while bound, so no writes.
    if (!m_gbufRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gbufRtvHeap))))
            return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE grtv = m_gbufRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int k = 0; k < 2; ++k)
    {
        D3D12_RESOURCE_DESC gd = nd;
        gd.Format = kGbufFormats[k];
        D3D12_CLEAR_VALUE gclear = {};
        gclear.Format = kGbufFormats[k];
        m_gbuf[k].Reset();
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gd,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                &gclear, IID_PPV_ARGS(&m_gbuf[k]))))
        {
            Log("scene: deferred lights' target creation failed");
            return false;
        }
        dev->CreateRenderTargetView(m_gbuf[k].Get(), nullptr, grtv);
        grtv.ptr += m_prepassRtvStride;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC rdsv = dsv;
    rdsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    D3D12_CPU_DESCRIPTOR_HANDLE ro = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_dsvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ro.ptr += m_dsvStride;
    dev->CreateDepthStencilView(m_depth.Get(), &rdsv, ro);
    return true;
}

void Scene::OnResize(ID3D12Device* dev, UINT width, UINT height)
{
    CreateDepth(dev, width, height);
}

// Everything that belongs to one loaded stage. Anything added to Scene that is
// per-track MUST be cleared here too.
void Scene::ResetScene()
{
    m_objects.clear();
    m_materials.clear();
    m_textures.clear();
    m_textureNames.clear();
    m_linearTextures.clear();
    m_srvHeap.Reset();
    m_shadowSrvReady = false;      // the heap is gone, so the descriptor is too
    m_vb.Reset();
    m_ib.Reset();
    m_vbv = {};
    m_ibv = {};
    m_indexCount = 0;
    m_bakedScale = 1.0f;
    m_anchorToCamera = false;
    m_legacyImport = false;
    m_staticIndices.clear();
    ++m_rtGeneration;              // every mesh key from the previous stage is dead
}

bool Scene::Load(ID3D12Device* dev, ID3D12CommandQueue* queue, const wchar_t* path,
                 DXGI_FORMAT rtvFormat, UINT width, UINT height)
{
    std::vector<uint8_t> blob;
    if (!ReadWholeFile(path, blob))
    {
        Log("scene: cannot open %ls", path);
        return false;
    }
    Log("scene: loading %ls (%zu bytes)", path, blob.size());
    return LoadFromMemory(dev, queue, blob.data(), blob.size(), rtvFormat, width, height);
}

bool Scene::LoadFromMemory(ID3D12Device* dev, ID3D12CommandQueue* queue,
                           const void* data, size_t bytes,
                           DXGI_FORMAT rtvFormat, UINT width, UINT height)
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    // Const in, const through: the caller may hand us a mapped file or a buffer
    // owned by the editor, and nothing here writes to it.
    const uint8_t* base = static_cast<const uint8_t*>(data);
    if (!base) { Log("scene: null buffer"); return false; }

    if (bytes < sizeof(FileHeader))
    {
        Log("scene: file too small");
        return false;
    }

    auto* fh = reinterpret_cast<const FileHeader*>(base);
    if (fh->magic != kMagic)
    {
        Log("scene: bad magic 0x%08X (not a .ngg file)", fh->magic);
        return false;
    }
    // v2 is accepted alongside v3 for one reason only: the converter lives in
    // another repository, and a hard cut would mean the two had to land in the
    // same hour or every asset stops loading. Only the MATERIAL table differs -
    // see the upgrade branch below - and this goes when Wallaby writes v3.
    if (fh->version != kFormatCurrent && fh->version != kFormatV2)
    {
        Log("scene: format version %u, this build reads %u (and %u in transition) "
            "- reconvert the asset", fh->version, kFormatCurrent, kFormatV2);
        return false;
    }

    const SceneHeader* sh = nullptr;
    const uint8_t* verts = nullptr; size_t vertBytes = 0;
    const uint8_t* idx = nullptr;   size_t idxBytes = 0;
    const uint8_t* mats = nullptr;  size_t matBytes = 0;
    const uint8_t* objs = nullptr;  size_t objBytes = 0;
    const uint8_t* texs = nullptr;  size_t texBytes = 0;
    const uint8_t* weather = nullptr; size_t weatherBytes = 0;
    const uint8_t* keys = nullptr;    size_t keyBytes = 0;
    const uint8_t* tagNames = nullptr; size_t tagNameBytes = 0;
    const uint8_t* plants = nullptr;  size_t plantBytes = 0;

    size_t off = sizeof(FileHeader);
    for (uint32_t i = 0; i < fh->chunkCount; ++i)
    {
        if (off + sizeof(ChunkHeader) > bytes) { Log("scene: truncated chunk table"); return false; }
        auto* ch = reinterpret_cast<const ChunkHeader*>(base + off);
        off += sizeof(ChunkHeader);
        if (off + ch->byteSize > bytes) { Log("scene: truncated chunk payload"); return false; }

        const uint8_t* payload = base + off;
        if (ch->fourcc == ChunkSceneHeader() && ch->byteSize >= sizeof(SceneHeader))
            sh = reinterpret_cast<const SceneHeader*>(payload);
        else if (ch->fourcc == ChunkVertices())
        { verts = payload; vertBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkIndices())
        { idx = payload; idxBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkMaterials())
        { mats = payload; matBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkObjects())
        { objs = payload; objBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkTextures())
        { texs = payload; texBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkWeather())
        { weather = payload; weatherBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkObjectKeys())
        { keys = payload; keyBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkTagNames())
        { tagNames = payload; tagNameBytes = ch->byteSize; }
        else if (ch->fourcc == ChunkPlants())
        { plants = payload; plantBytes = ch->byteSize; }
        // Unknown chunks are skipped on purpose - that is what makes the format
        // forward-compatible.

        off += ch->byteSize;
        off += (4 - (ch->byteSize % 4)) % 4;
    }

    if (!sh || !verts || !idx) { Log("scene: missing NGGH/NGGV/NGGI"); return false; }

    if (vertBytes != (size_t)sh->vertexCount * sizeof(Vertex))
    {
        Log("scene: vertex chunk is %zu bytes, header says %u verts x %zu = %zu"
               " - writer and loader disagree on the layout",
               vertBytes, sh->vertexCount, sizeof(Vertex),
               (size_t)sh->vertexCount * sizeof(Vertex));
        return false;
    }
    if (idxBytes != (size_t)sh->indexCount * sizeof(uint32_t))
    {
        Log("scene: index chunk size mismatch");
        return false;
    }

    // Drop the previous stage before taking on this one.
    //
    // These are vectors that used to be push_back'd into without ever being
    // cleared, so loading a second track APPENDED to the first: every
    // materialId in the new scene then pointed at the old scene's material,
    // and every texture id was off by however many the previous track had.
    // The symptom is a second stage wearing the first stage's textures.
    //
    // Safe to release here: the frame loop is synchronous and waits for the GPU
    // at the end of every frame, so nothing is in flight at this point.
    ResetScene();

    // The objects' source keys (NGGK): a count, then one int32 per object.
    m_objectKeys.clear();
    if (keys && keyBytes >= 4)
    {
        uint32_t n = 0;
        memcpy(&n, keys, 4);
        if (keyBytes >= 4 + (size_t)n * 4)
        {
            m_objectKeys.resize(n);
            memcpy(m_objectKeys.data(), keys + 4, (size_t)n * 4);
            size_t linked = 0;
            for (int32_t k : m_objectKeys) linked += k >= 0;
            Log("scene: %zu of %u object(s) keyed to a source instance", linked, n);
        }
        else Log("scene: NGGK truncated - ignored");
    }

    // Weather presets carried by the file (NGGW): binary records, design 5.0t.
    m_weather.clear();
    if (weather)
    {
        ParseWeatherChunk(weather, weatherBytes, m_weather);
        Log("scene: %zu weather preset(s) in the file, default [%s]", m_weather.size(),
            m_weather.empty() ? "-" : m_weather[0].name.c_str());
    }

    if (mats)
    {
        auto take = [this](const Material& m) {
            MaterialSlot s = {};
            s.tex[0] = m.baseColour; s.tex[1] = m.detail; s.tex[2] = m.normal;
            s.tex[3] = m.orm;        s.tex[4] = m.emissive;
            s.flags  = m.flags;
            // DERIVED, never stored: a material with no detail map has no
            // second layer, and saying it twice is how a file starts
            // contradicting itself.
            if (m.detail == kNoTexture) s.flags |= kDrawSingleStage;
            if (m.normal != kNoTexture) s.flags |= kDrawNormalMap;
            for (int k = 0; k < 4; ++k) s.baseColourFactor[k] = m.baseColourFactor[k];
            s.roughness = m.roughness;   s.metallic = m.metallic;
            s.normalScale = m.normalScale;
            s.occlusionStrength = m.occlusionStrength;
            s.emissiveStrength = m.emissiveStrength;
            s.alphaCutoff = m.alphaCutoff;
            m_materials.push_back(s);
        };

        if (fh->version >= kFormatV3)
        {
            const Material* m = reinterpret_cast<const Material*>(mats);
            for (size_t i = 0; i < matBytes / sizeof(Material); ++i) take(m[i]);
        }
        else
        {
            // TRANSITION, not compatibility: v2 knew two texture stages and
            // nothing else, so everything PBR gets the default. Delete this
            // branch, MaterialV2 and this log line once Wallaby writes v3.
            const MaterialV2* m = reinterpret_cast<const MaterialV2*>(mats);
            const size_t n = matBytes / sizeof(MaterialV2);
            for (size_t i = 0; i < n; ++i)
            {
                Material up = DefaultMaterial();
                up.baseColour = m[i].texture0;
                // v2 wrote texture1 == texture0 on single-stage materials
                // rather than saying "none", so the pairing alone cannot tell
                // a real detail layer from a duplicate. MeasureSingleStage
                // settles it from the geometry, as it always has.
                up.detail = m[i].texture1;
                up.flags  = m[i].flags & (kMaterialAlphaTest | kMaterialTwoSided);
                take(up);
            }
            Log("scene: v%u materials upgraded to v3 defaults (%zu) - the converter "
                "is still writing the old struct", fh->version, n);
        }
        {
            size_t twoSided = 0;
            for (const MaterialSlot& s : m_materials) twoSided += (s.flags & kMaterialTwoSided) != 0;
            Log("scene: %zu material(s), %zu two-sided", m_materials.size(), twoSided);
        }

        // Which textures hold DATA rather than colour. Built here because
        // UploadTextures runs later and cannot know: the same DDS could be an
        // albedo on one material and a normal map on another, and if it ever is,
        // the first binding wins and that is worth knowing about.
        for (const MaterialSlot& s : m_materials)
        {
            for (int k = 2; k <= 3; ++k)
            {
                if (s.tex[k] == kNoTexture) continue;
                if (s.tex[k] >= m_linearTextures.size())
                    m_linearTextures.resize(s.tex[k] + 1, 0);
                m_linearTextures[s.tex[k]] = 1;
            }
        }
    }
    if (objs)
    {
        const Object* o = reinterpret_cast<const Object*>(objs);
        for (size_t i = 0; i < objBytes / sizeof(Object); ++i)
        {
            DrawItem d = {};
            d.layerId = o[i].layerId;
            d.materialId = o[i].materialId;
            d.firstIndex = o[i].firstIndex;
            d.indexCount = o[i].indexCount;
            d.matFlags = (d.materialId < m_materials.size())
                       ? m_materials[d.materialId].flags : 0;
            m_objects.push_back(d);
        }
    }

    // Per-object AABBs, for frustum and cascade culling. One pass over the
    // index buffer at load; without it every cascade redraws the entire stage.
    if (idx && sh->indexCount)
    {
        const Vertex* vv = reinterpret_cast<const Vertex*>(verts);
        const uint32_t* ii = reinterpret_cast<const uint32_t*>(idx);
        for (DrawItem& o : m_objects)
        {
            for (int k = 0; k < 3; ++k) { o.bmin[k] = 1e30f; o.bmax[k] = -1e30f; }
            if (o.firstIndex + o.indexCount > sh->indexCount) continue;
            for (uint32_t j = o.firstIndex; j < o.firstIndex + o.indexCount; ++j)
            {
                const uint32_t v = ii[j];
                if (v >= sh->vertexCount) continue;
                for (int k = 0; k < 3; ++k)
                {
                    if (vv[v].pos[k] < o.bmin[k]) o.bmin[k] = vv[v].pos[k];
                    if (vv[v].pos[k] > o.bmax[k]) o.bmax[k] = vv[v].pos[k];
                }
            }
            // The untransformed pair, so a later move recomputes from the
            // geometry rather than from its own previous answer.
            for (int k = 0; k < 3; ++k) { o.bmin0[k] = o.bmin[k]; o.bmax0[k] = o.bmax[k]; }
        }
    }

    if (idx && sh->indexCount && sh->vertexCount)
        MeasureSingleStage(reinterpret_cast<const Vertex*>(verts), sh->vertexCount,
                           idx, sh->indexCount);

    // Normal repair happens BEFORE the upload, which is why the material and
    // object tables are read first: foliage is identified by its material's
    // alpha-test flag, and the object table is what says which vertices belong
    // to which tree.
    // The plants as the file says (NGGP, written by the converter, corrected by a human
    // where one did); only a file without them gets the renderer's own guess - and not an
    // automatic legacy import (kSceneFlagLegacyImport): its cards are the legacy profile's,
    // and the guess is wrong there (RBR's vegetation block also holds people, cars, fences).
    if (idx && sh->indexCount && sh->vertexCount &&
        !ReadPlants(tagNames, tagNameBytes, plants, plantBytes, sh->indexCount, (sh->flags & kSceneFlagLegacyImport) != 0) &&
        !(sh->flags & kSceneFlagLegacyImport))
        FindPlants(reinterpret_cast<const Vertex*>(verts), sh->vertexCount, idx, sh->indexCount);

    std::vector<Vertex> fixed;
    const uint8_t* vertexSource = verts;
    if ((m_weldNormals || m_foliageNormals) && sh->vertexCount)
    {
        fixed.assign(reinterpret_cast<const Vertex*>(verts),
                     reinterpret_cast<const Vertex*>(verts) + sh->vertexCount);
        if (m_weldNormals)   WeldNormals(fixed);
        if (m_foliageNormals) SphericalFoliageNormals(fixed, idx, sh->indexCount);
        vertexSource = reinterpret_cast<const uint8_t*>(fixed.data());
    }

    m_vb = MakeUploadBuffer(dev, vertexSource, vertBytes);
    m_ib = MakeUploadBuffer(dev, idx, idxBytes);
    // And kept on the CPU: the ray tracer rebuilds BLASes from object index
    // ranges, and the GPU copy is write-combined memory that must not be read.
    m_staticIndices.assign(reinterpret_cast<const uint32_t*>(idx),
                           reinterpret_cast<const uint32_t*>(idx) + idxBytes / 4);
    if (!m_vb || !m_ib) { Log("scene: buffer upload failed"); return false; }

    m_vbv.BufferLocation = m_vb->GetGPUVirtualAddress();
    m_vbv.SizeInBytes = (UINT)vertBytes;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = (UINT)idxBytes;
    m_ibv.Format = DXGI_FORMAT_R32_UINT;

    m_indexCount = sh->indexCount;
    m_anchorToCamera = (sh->flags & kSceneFlagAnchorToCamera) != 0;
    m_legacyImport = (sh->flags & kSceneFlagLegacyImport) != 0;
    if (m_legacyImport) Log("scene: an automatic legacy import - its cards lit by the legacy profile %d", m_legacyProfile);
    for (int i = 0; i < 3; ++i) { m_bboxMin[i] = sh->bboxMin[i]; m_bboxMax[i] = sh->bboxMax[i]; }
    ComputeFocusPoint(reinterpret_cast<const Vertex*>(verts), sh->vertexCount);


    // Guard against a nonsense or absent value rather than rendering black.
    m_bakedScale = (sh->bakedScale > 0.01f && sh->bakedScale < 64.0f)
                 ? sh->bakedScale : 1.0f;
    Log("scene: baked colour scale %.3f", m_bakedScale);
    CreateProbe(dev);

    // Main-pass GPU timer: [0] start, [1] after env, [2] after aerial, [3] end.
    if (!m_timer)
    {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 4;
        if (SUCCEEDED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timer))))
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 4 * sizeof(UINT64);
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&m_timerReadback))) ||
                !queue || FAILED(queue->GetTimestampFrequency(&m_timerHz)))
                m_timer.Reset();
        }
    }
    // And the cascades, timed on their own: RT vs shadow maps on the same view
    // is the comparison design 9b item 23 is decided by.
    if (!m_shadowTimer && m_timer)
    {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 2 * sizeof(UINT64);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_shadowTimer))) ||
            FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&m_shadowTimerReadback))))
            m_shadowTimer.Reset();
    }

    if (!UploadTextures(dev, queue, texs, texBytes))
        return false;

    // --- root signature: one inline constant buffer, nothing else ----------
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kMaterialSrvCount;   // t0..t4, contiguous per material
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[12] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // One descriptor per draw rather than a bindless array: 3 ground materials
    // do not justify the machinery, and this keeps the first textured build
    // simple enough to debug.
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root UAV, not a descriptor table: the probe is one buffer and a root
    // descriptor needs no heap slot, so it cannot disturb the SRV heap the
    // material bindings depend on.
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // The shadow array lives in the SAME descriptor heap as the material SRVs -
    // only one CBV/SRV/UAV heap can be bound at a time - so it gets the slot
    // after the last material pair.
    static D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 5;          // t5, after the five material slots
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &shadowRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Per-object data as root constants: it changes every draw, so a buffer would
    // mean either a ring allocator or one upload per object.
    //
    // VISIBILITY_ALL, not PIXEL: the layer id is read by the pixel shader but the
    // transform is read by the vertex shader, and getting that wrong costs a
    // silently untransformed scene rather than a compile error.
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 1;      // b1
    params[4].Constants.Num32BitValues = 48;     // 3 uint + 1 pad + 4x4 + 3 float4 + 4x4
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // The environment map shares the heap for the same reason the shadow array
    // does: one CBV/SRV/UAV heap at a time.
    static D3D12_DESCRIPTOR_RANGE envRange = {};
    envRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    envRange.NumDescriptors = 1;
    envRange.BaseShaderRegister = 6;             // t6
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5].DescriptorTable.NumDescriptorRanges = 1;
    params[5].DescriptorTable.pDescriptorRanges = &envRange;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE rtRange = {};
    rtRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rtRange.NumDescriptors = 1;
    rtRange.BaseShaderRegister = 7;              // t7
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges = &rtRange;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE aoRange = {};
    aoRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aoRange.NumDescriptors = 1;
    aoRange.BaseShaderRegister = 8;              // t8
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges = &aoRange;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE aerialRange = {};
    aerialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aerialRange.NumDescriptors = 2;
    aerialRange.BaseShaderRegister = 9;          // t9 in-scatter, t10 transmittance
    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].DescriptorTable.NumDescriptorRanges = 1;
    params[8].DescriptorTable.pDescriptorRanges = &aerialRange;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE irrRange = {};
    irrRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    irrRange.NumDescriptors = 1;
    irrRange.BaseShaderRegister = 11;            // t11 sky irradiance
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &irrRange;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // The atmosphere's multiple-scattering table, t0 space1 - read by
    // AtmScatter in the fallback paths of this shader.
    static D3D12_DESCRIPTOR_RANGE msRange = {};
    msRange = AtmosphereMs::MsSrvRange();
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[10].DescriptorTable.NumDescriptorRanges = 1;
    params[10].DescriptorTable.pDescriptorRanges = &msRange;
    params[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // The local lights (lights.h): their buffers and the ray tracer's shadowed fraction, four SRVs in the shared
    // heap - one table, because this root signature is near its 64-DWORD limit and three root SRVs would pass it.
    static D3D12_DESCRIPTOR_RANGE lightRange = {};
    lightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightRange.NumDescriptors = 7;
    lightRange.BaseShaderRegister = 12;          // t12 lights, t13 clusters, t14 indices, t15 the RT fraction,
                                                 // t16-t18 the deferred pass's surface and depth (PSDeferredLights)
    params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[11].DescriptorTable.NumDescriptorRanges = 1;
    params[11].DescriptorTable.pDescriptorRanges = &lightRange;
    params[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // THREE samplers so the atlas-bleed diagnosis can be A/B'd live in the
    // viewer instead of argued about.
    //
    // RBR's ground textures are ATLAS PAGES ("plate" = page):
    // TrackTexturePlateOpaque is 2048x2048 holding 18 cells of 256x256, and
    // every road chunk uses one cell - and the road's cell is MEASURABLY
    // bordered by darker content:
    //
    //     cell interior        luma 163
    //     just outside left    luma 100
    //     just outside bottom  luma 112
    //
    // so any filter tap crossing the border darkens the result, and the
    // authored gutter is only ~2.25 texels.
    //
    // Clamping MaxLOD was tried first and changed nothing. That negative result
    // is the informative one: it clamps the mip LEVEL, but anisotropy spreads
    // its taps along the major axis independently of the level. At the grazing
    // angle a road is viewed from that is a long line of taps running off the
    // cell edge - which is exactly why the artefact is distance-dependent (the
    // footprint grows) yet NOT mip-dependent. s1 tests that directly.
    D3D12_STATIC_SAMPLER_DESC samp[6] = {};
    for (int i = 0; i < 3; ++i)
    {
        samp[i].Filter = D3D12_FILTER_ANISOTROPIC;
        samp[i].MaxAnisotropy = 8;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp[i].MipLODBias = m_mipBias;
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp[i].ShaderRegister = i;
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // s0 = what we ship today.
    samp[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;   // s1 = no anisotropy
    samp[1].MaxAnisotropy = 1;
    samp[2].MaxLOD = 1.0f;                              // s2 = mip clamp only

    // s3: shadow comparison. LESS_EQUAL against conventional depth (the shadow
    // pass does NOT use reverse-Z), with border white so anything sampling
    // outside a cascade reads as lit rather than as a black slab.
    samp[3].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samp[3].AddressU = samp[3].AddressV = samp[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samp[3].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samp[3].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samp[3].MaxLOD = D3D12_FLOAT32_MAX;
    samp[3].ShaderRegister = 3;
    samp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s4: the aerial volume. CLAMP is not optional - wrapping in w would blend
    // the farthest slice into the nearest, i.e. put kilometres of haze on the
    // pixels right in front of the camera.
    samp[4].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp[4].AddressU = samp[4].AddressV = samp[4].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp[4].MaxLOD = D3D12_FLOAT32_MAX;
    samp[4].ShaderRegister = 4;
    samp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp[5] = AtmosphereMs::MsSampler(D3D12_SHADER_VISIBILITY_PIXEL);   // s0 space1

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 12;
    rs.pParameters = params;
    rs.NumStaticSamplers = 6;
    rs.pStaticSamplers = samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("scene: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSig))))
        return false;

    // --- shaders -----------------------------------------------------------
    // DXC, shader model 6.0 (dxc.h): the local lights' loop walks a wave's lights with scalar loads, which takes wave
    // intrinsics (LocalLights, design 5.4 N2). FXC (5.1) stays as the fallback - a machine without dxcompiler.dll, or
    // a source DXC refuses - so the source compiles under both, the wave path behind __SHADER_TARGET_MAJOR. A
    // pipeline cannot mix DXIL and DXBC, so the whole set goes through one compiler. -HV 2018: the language FXC
    // speaks (2021 changed what && || ?: do on vectors).
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    // The local lights' HLSL (lights.h) and their buffers, ahead of the shader that shades them.
    std::string src = std::string(kAtmosphereHlsl) +
        "#define NGG_LIGHTS_T0 t12\n#define NGG_LIGHTS_T1 t13\n#define NGG_LIGHTS_T2 t14\n" + kLightsHlsl +
        "Texture2D<float>        gRtLights : register(t15);\n" + kShaderSrc;

    enum Variant { kOpaque, kAlphaTest, kBlend };
    bool dxil = m_dxc.Load();
    // `gbuf`: the variant that leaves the surface for the deferred local lights (Shade, PSDeferredLights).
    auto compile = [&](const char* entry, bool vertex, Variant v, bool gbuf = false) -> ComPtr<ID3DBlob> {
        if (dxil)
        {
            const std::wstring we(entry, entry + strlen(entry));
            std::vector<const wchar_t*> args = { L"-HV", L"2018" };
            if (v == kAlphaTest) args.push_back(L"-DALPHA_TEST=1");
            if (v == kBlend) args.push_back(L"-DBLEND=1");
            if (gbuf) args.push_back(L"-DGBUF=1");
            return m_dxc.Compile(src, we.c_str(), vertex ? L"vs_6_0" : L"ps_6_0", "ngg_scene", args);
        }
        std::vector<D3D_SHADER_MACRO> defs;
        if (v == kAlphaTest) defs.push_back({ "ALPHA_TEST", "1" });
        if (v == kBlend) defs.push_back({ "BLEND", "1" });
        if (gbuf) defs.push_back({ "GBUF", "1" });
        defs.push_back({ nullptr, nullptr });
        ComPtr<ID3DBlob> out, e;
        if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_scene", defs.data(), nullptr, entry,
                              vertex ? "vs_5_1" : "ps_5_1", flags, 0, &out, &e)))
        {
            Log("scene: %s (%s%s) compile: %s", entry, v == kAlphaTest ? "alpha-test" : v == kBlend ? "blend" : "opaque",
                gbuf ? ", deferred" : "", e ? (const char*)e->GetBufferPointer() : "?");
            return nullptr;
        }
        return out;
    };
    ComPtr<ID3DBlob> vs, ps, psAlpha, psBlend, psPre, psPreAlpha, psShadowAlpha;   // (the opaque shadow pass: no PS)
    ComPtr<ID3DBlob> psG, psAlphaG, vsFull, psDeferred;                              // the deferred local lights
    auto compileAll = [&]() {
        vs = compile("VSMain", true, kOpaque);
        ps = compile("PSMain", false, kOpaque);
        psAlpha = compile("PSMain", false, kAlphaTest);
        psBlend = compile("PSMain", false, kBlend);
        psPre = compile("PSPrepass", false, kOpaque);
        psPreAlpha = compile("PSPrepass", false, kAlphaTest);
        psShadowAlpha = compile("PSShadow", false, kAlphaTest);
        psG = compile("PSMain", false, kOpaque, true);
        psAlphaG = compile("PSMain", false, kAlphaTest, true);
        vsFull = compile("VSFullscreen", true, kOpaque);
        psDeferred = compile("PSDeferredLights", false, kOpaque);
        return vs && ps && psAlpha && psBlend && psPre && psPreAlpha && psShadowAlpha &&
               psG && psAlphaG && vsFull && psDeferred;
    };
    if (!compileAll() && dxil)
    {
        Log("scene: DXC failed on the scene's shaders - compiled with FXC (5.1) instead, the lights' loop per pixel");
        dxil = false;
        compileAll();
    }
    if (!vs || !ps || !psAlpha || !psBlend) return false;
    Log("scene: shaders compiled with %s", dxil ? "DXC (shader model 6.0)" : "FXC (shader model 5.1)");

    // NGG_DUMP_PS=<path>: the opaque pixel shader for offline analysis - what Radeon GPU Analyzer needs to compile the
    // pipeline for a given GPU (registers, occupancy, ISA): the PS and VS blobs (DXIL, or DXBC from the fallback), the
    // root signature and the source (<path>.ps.obj, .vs.obj, .rs.bin, .hlsl). How the local lights' cost was taken
    // apart (design 5.4 N2).
    if (const char* dump = std::getenv("NGG_DUMP_PS"))
    {
        auto put = [&](const char* ext, const void* p, size_t n) {
            if (FILE* f = fopen((std::string(dump) + ext).c_str(), "wb")) { fwrite(p, 1, n, f); fclose(f); }
        };
        put(".ps.obj", ps->GetBufferPointer(), ps->GetBufferSize());
        put(".vs.obj", vs->GetBufferPointer(), vs->GetBufferSize());
        put(".rs.bin", sig->GetBufferPointer(), sig->GetBufferSize());
        put(".hlsl", src.data(), src.size());
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // No culling in these: they draw the two-sided materials. The others draw
    // with the m_psoCulled variants made below.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    // GREATER_EQUAL, not GREATER: reverse-Z either way, but after a prepass
    // has already written this exact depth the main pass must PASS at equality
    // or it draws nothing. Same VS bytecode, same matrices, so equality is
    // exact rather than hopeful. Without a prepass it behaves as GREATER did.
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    for (UINT i = 0; i < 8; ++i)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.PS = { psAlpha->GetBufferPointer(), psAlpha->GetBufferSize() };
    if (FAILED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoAlphaTest))))
    {
        Log("scene: alpha-test PSO creation failed");
        return false;
    }
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    // --- prepass PSOs: depth + geometric normal, nothing else ---------------
    {
        if (psPre && psPreAlpha)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pp = pso;
            pp.NumRenderTargets = 3;
            pp.RTVFormats[0] = kNormalFormat;
            pp.RTVFormats[1] = kMotionFormat;
            pp.RTVFormats[2] = kPlantFormat;
            pp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pp.PS = { psPre->GetBufferPointer(), psPre->GetBufferSize() };
            if (FAILED(dev->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&m_psoPrepass))))
                Log("scene: prepass PSO creation failed");
            pp.PS = { psPreAlpha->GetBufferPointer(), psPreAlpha->GetBufferSize() };
            if (FAILED(dev->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&m_psoPrepassAlpha))))
                Log("scene: alpha-test prepass PSO creation failed");
            for (int alpha = 0; alpha < 2; ++alpha)
                for (int c = 0; c < 2; ++c)
                {
                    pp.PS = alpha ? D3D12_SHADER_BYTECODE{ psPreAlpha->GetBufferPointer(), psPreAlpha->GetBufferSize() }
                                  : D3D12_SHADER_BYTECODE{ psPre->GetBufferPointer(), psPre->GetBufferSize() };
                    pp.RasterizerState.CullMode = c ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
                    if (FAILED(dev->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&m_psoCulled[1][alpha][c]))))
                        Log("scene: culled prepass PSO creation failed - drawn two-sided");
                }
        }
        else
            Log("scene: prepass shader failed to compile");
    }

    // --- the moving glass's mark in the prepass (PSPrepass, gObjectMoved 2): the motion target's alpha alone, MAX -
    // depth tested, not written, the normal and the plant untouched. [cull] none (two-sided), back, front.
    if (psPre)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gp = pso;
        gp.NumRenderTargets = 3;
        gp.RTVFormats[0] = kNormalFormat;
        gp.RTVFormats[1] = kMotionFormat;
        gp.RTVFormats[2] = kPlantFormat;
        gp.PS = { psPre->GetBufferPointer(), psPre->GetBufferSize() };
        gp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gp.BlendState.IndependentBlendEnable = TRUE;
        gp.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        gp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
        D3D12_RENDER_TARGET_BLEND_DESC& b = gp.BlendState.RenderTarget[1];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_ZERO;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALPHA;
        const D3D12_CULL_MODE culls[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT };
        for (int c = 0; c < 3; ++c)
        {
            gp.RasterizerState.CullMode = culls[c];
            if (FAILED(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&m_psoPrepassGlass[c]))))
                Log("scene: the glass's prepass PSO creation failed - moving glass unmarked for TAA");
        }
    }

    // --- shadow PSOs: depth only, no render target -------------------------
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC sp = pso;
        sp.NumRenderTargets = 0;
        sp.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        sp.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        // Conventional depth here, NOT reverse-Z: the shadow projection is
        // orthographic, so reverse-Z buys nothing (its whole benefit is
        // cancelling perspective's 1/z crowding) and it would only invert the
        // comparison for no gain.
        sp.DepthStencilState.DepthEnable = TRUE;
        sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        sp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        // Front-face culling for the opaque pass: shadowing from back faces
        // moves acne to surfaces the camera cannot see. A modest slope-scaled
        // bias covers what remains on shallow slopes.
        sp.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        sp.RasterizerState.DepthBias = 40;
        sp.RasterizerState.SlopeScaledDepthBias = 2.0f;
        sp.PS = { nullptr, 0 };
        if (FAILED(dev->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&m_psoShadow))))
            Log("scene: shadow PSO creation failed - shadows disabled");

        // Alpha-tested casters must be two-sided: a foliage card culled by
        // winding casts nothing at all. But that removes the protection
        // front-face culling gives everything else, and a card has NO
        // THICKNESS - it casts its own depth and then receives it straight
        // back, so both faces self-shadow. Normal-offset bias cannot help:
        // on a flat card the normal lies in the card's own plane, so offsetting
        // slides the sample sideways rather than off the surface.
        //
        // Push the CASTER away from the light instead. The bias is much larger
        // than the opaque one because it has to exceed a leaf's own depth
        // spread, and foliage can afford it - a shadow that starts a few
        // centimetres inside a canopy is invisible, where the same error on a
        // road surface would be obvious peter-panning.
        sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        sp.RasterizerState.DepthBias = 400;
        sp.RasterizerState.SlopeScaledDepthBias = 4.0f;
        if (psShadowAlpha)
            sp.PS = { psShadowAlpha->GetBufferPointer(), psShadowAlpha->GetBufferSize() };
        if (FAILED(dev->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&m_psoShadowAlpha))))
            Log("scene: alpha-test shadow PSO creation failed");
    }

    if (FAILED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso))))
    {
        Log("scene: pipeline state creation failed");
        return false;
    }
    // The one-sided materials (all but kMaterialTwoSided): their back faces
    // culled: the meshes' front faces are clockwise on screen, D3D's own
    // convention (CULL_BACK), and a mirroring transform turns that round
    // (CULL_FRONT). Drawn unculled, a closed shell's inner faces fought
    // its outer ones for the depth wherever the two lie close - the car's
    // body, whose inner faces sample the texture's black, showed black
    // triangles through its paint, and distant scenery did the same.
    for (int alpha = 0; alpha < 2; ++alpha)
        for (int c = 0; c < 2; ++c)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cp = pso;
            if (alpha) cp.PS = { psAlpha->GetBufferPointer(), psAlpha->GetBufferSize() };
            cp.RasterizerState.CullMode = c ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
            if (FAILED(dev->CreateGraphicsPipelineState(&cp, IID_PPV_ARGS(&m_psoCulled[0][alpha][c]))))
                Log("scene: culled PSO creation failed - drawn two-sided");
        }

    // THE DEFERRED LOCAL LIGHTS (PSDeferredLights): the opaque world's pipelines again with the surface's three targets
    // (the GBUF variants, used while there are local lights), and the full-screen pass that lights it - added (ONE,
    // ONE) into the HDR target, the depth read through its read-only view, not tested.
    m_deferredReady = false;
    if (psG && psAlphaG && vsFull && psDeferred)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gp = pso;
        gp.NumRenderTargets = 3;
        for (int k = 0; k < 2; ++k) gp.RTVFormats[1 + k] = kGbufFormats[k];
        bool ok = true;
        gp.PS = { psG->GetBufferPointer(), psG->GetBufferSize() };
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&m_psoG)));
        gp.PS = { psAlphaG->GetBufferPointer(), psAlphaG->GetBufferSize() };
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&m_psoAlphaTestG)));
        for (int alpha = 0; alpha < 2; ++alpha)
            for (int c = 0; c < 2; ++c)
            {
                gp.PS = alpha ? D3D12_SHADER_BYTECODE{ psAlphaG->GetBufferPointer(), psAlphaG->GetBufferSize() }
                              : D3D12_SHADER_BYTECODE{ psG->GetBufferPointer(), psG->GetBufferSize() };
                gp.RasterizerState.CullMode = c ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
                ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&m_psoCulledG[alpha][c])));
            }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC dp = pso;
        dp.InputLayout = { nullptr, 0 };
        dp.VS = { vsFull->GetBufferPointer(), vsFull->GetBufferSize() };
        dp.PS = { psDeferred->GetBufferPointer(), psDeferred->GetBufferSize() };
        dp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        dp.DepthStencilState.DepthEnable = FALSE;
        dp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        D3D12_RENDER_TARGET_BLEND_DESC& b = dp.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_ONE;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        ok = ok && SUCCEEDED(dev->CreateGraphicsPipelineState(&dp, IID_PPV_ARGS(&m_psoDeferred)));
        m_deferredReady = ok;
    }

    // Foliage after a prepass: the depth is final, so the main pass tests it and does NOT write it - which lets the
    // GPU test BEFORE the shader in spite of the clip(); a shader that may discard AND writes depth is tested after
    // it, every card of a canopy running the whole shader first. [kind] forward, deferred (the GBUF targets);
    // [cull] none (two-sided), back, front (a mirroring transform).
    {
        const D3D12_CULL_MODE culls[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT };
        for (int kind = 0; kind < 2; ++kind)
        {
            if (kind == 1 && !m_deferredReady) break;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC kp = pso;
            kp.PS = kind ? D3D12_SHADER_BYTECODE{ psAlphaG->GetBufferPointer(), psAlphaG->GetBufferSize() }
                         : D3D12_SHADER_BYTECODE{ psAlpha->GetBufferPointer(), psAlpha->GetBufferSize() };
            if (kind)
            {
                kp.NumRenderTargets = 3;
                for (int k = 0; k < 2; ++k) kp.RTVFormats[1 + k] = kGbufFormats[k];
            }
            kp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            for (int c = 0; c < 3; ++c)
            {
                kp.RasterizerState.CullMode = culls[c];
                if (FAILED(dev->CreateGraphicsPipelineState(&kp, IID_PPV_ARGS(&m_psoAlphaKept[kind][c]))))
                    Log("scene: alpha-test PSO over the prepass's depth failed - the depth-writing one instead");
            }
        }
    }
    if (!m_deferredReady)
        Log("scene: the deferred local lights' pipelines failed - the lights shaded in the main pass");

    // The blended materials (glass): over everything opaque, PREMULTIPLIED (the shader scales the tint's light by its
    // coverage and leaves the reflection whole), tested against the opaque depth but not writing it - what is behind
    // a pane must stay drawn and other panes behind it must still pass.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC bp = pso;
        bp.PS = { psBlend->GetBufferPointer(), psBlend->GetBufferSize() };
        bp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        D3D12_RENDER_TARGET_BLEND_DESC& b = bp.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        const D3D12_CULL_MODE culls[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT };
        for (int c = 0; c < 3; ++c)
        {
            bp.RasterizerState.CullMode = culls[c];
            if (FAILED(dev->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&m_psoBlend[c]))))
                Log("scene: blend PSO creation failed - glass not drawn");
        }
    }

    if (!CreateDepth(dev, width, height))
        return false;

    // Not fatal: without the table the scene pass marches per pixel, exactly
    // as it did before the table existed.
    if (!m_aerialLut.Ready() && !m_aerialLut.Create(dev))
        Log("scene: aerial table unavailable - aerial perspective will march per pixel");
    if (!m_lightsReady && !(m_lightsReady = m_lights.Create(dev)))
        Log("scene: local light buffers unavailable - no local lights");
    if (!m_ms.Ready() && !m_ms.Create(dev))
        Log("scene: multiple-scattering table unavailable - single scattering");
    if (!m_ownEnv.Ready() && !m_ownEnv.Create(dev))
        Log("scene: environment unavailable - no ambient specular, sky ambient marches per pixel");

    // No path here on purpose: this loads from bytes, and the caller may have no
    // file at all. Load() logs the name before calling in.
    Log("scene: loaded %u verts, %u indices (%u tris), %u objects",
           sh->vertexCount, sh->indexCount, sh->indexCount / 3,
           (unsigned)m_objects.size());
    return true;
}

// ---------------------------------------------------------------------------
// The dynamic pool (design 8.2, tier 2).
//
// Added content lives in its own buffers beside the static scene. The point is
// what does NOT happen: placing one prop does not touch the 50 MB the track
// occupies, so the cost of an edit is proportional to the edit rather than to
// the project.
//
// The CPU-side vectors are the source of truth. An UPLOAD heap is
// write-combined memory -- writes are fine, reads are catastrophically slow --
// so anything that needs to look at the geometry again (growth, compaction,
// bounds) reads the vector, never the buffer.
// ---------------------------------------------------------------------------

uint32_t Scene::AddMesh(ID3D12Device* dev, const ::ngg::Vertex* verts, uint32_t vertexCount,
                        const uint32_t* indices, uint32_t indexCount)
{
    if (!dev || !verts || !indices || !vertexCount || !indexCount) return 0;
    if (indexCount % 3) return 0;              // triangles, and a partial one is a caller bug

    // Validate BEFORE anything is appended. An index past the end of the mesh
    // reads whatever the neighbouring mesh put there, which draws a plausible
    // wrong shape rather than failing -- the worst kind of bug to chase.
    for (uint32_t i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return 0;

    DynMesh m;
    m.firstVertex = (uint32_t)m_dynVerts.size();
    m.vertexCount = vertexCount;
    m.firstIndex = (uint32_t)m_dynIndices.size();
    m.indexCount = indexCount;

    for (int k = 0; k < 3; ++k) { m.bmin[k] = 1e30f; m.bmax[k] = -1e30f; }
    for (uint32_t i = 0; i < vertexCount; ++i)
        for (int k = 0; k < 3; ++k)
        {
            const float v = verts[i].pos[k];
            if (v < m.bmin[k]) m.bmin[k] = v;
            if (v > m.bmax[k]) m.bmax[k] = v;
        }

    m_dynVerts.insert(m_dynVerts.end(), verts, verts + vertexCount);
    // Stored mesh-relative and drawn with BaseVertexLocation, so compaction can
    // move a mesh without rewriting a single index.
    m_dynIndices.insert(m_dynIndices.end(), indices, indices + indexCount);

    m_dynMeshes.push_back(m);
    m_dynDirty = true;
    return (uint32_t)m_dynMeshes.size();       // 1-based
}

uint32_t Scene::AddTexture(ID3D12Device* dev, ID3D12CommandQueue* queue,
                           const void* dds, size_t bytes)
{
    if (!dev || !queue || !dds || !bytes) return 0;

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                      nullptr, IID_PPV_ARGS(&list))))
        return 0;

    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> tex = CreateTextureFromDDS(
        dev, list.Get(), static_cast<const uint8_t*>(dds), (uint32_t)bytes, upload, "added", false);
    if (!tex) return 0;

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    // Block: the upload buffer dies with this scope, and the texture has to be
    // resident before anything can sample it.
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 0;
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 30000);
        CloseHandle(evt);
    }

    m_textures.push_back(tex);
    return (uint32_t)m_textures.size();        // 1-based
}

uint32_t Scene::AddMaterial(ID3D12Device* dev, uint32_t tex0, uint32_t tex1, uint32_t flags)
{
    if (!dev) return 0;

    // 0 means "no texture given" and resolves to the white fallback, which is
    // what an untextured added prop should look like -- lit, not black, and not
    // borrowing whatever texture happened to be bound last.
    auto resolve = [&](uint32_t h) -> uint32_t {
        if (h == 0 || h > m_textures.size()) return m_whiteSlot;
        return h - 1;
    };

    // The editor's simple path: two texture handles and the RBR-shaped flags.
    // Everything PBR takes DefaultMaterial's values, so an added prop is a
    // complete dielectric surface rather than a black or mirrored one. A richer
    // entry point belongs with the editor's material UI, not ahead of it.
    const Material d = DefaultMaterial();
    MaterialSlot m = {};
    m.tex[0] = resolve(tex0);
    m.tex[1] = (tex1 == 0) ? kNoTexture : resolve(tex1);
    m.tex[2] = m.tex[3] = m.tex[4] = kNoTexture;
    m.flags = flags;
    if (m.tex[1] == kNoTexture) m.flags |= kDrawSingleStage;   // normal/orm stay empty here
    for (int k = 0; k < 4; ++k) m.baseColourFactor[k] = d.baseColourFactor[k];
    m.roughness = d.roughness;   m.metallic = d.metallic;
    m.normalScale = d.normalScale;
    m.occlusionStrength = d.occlusionStrength;
    m.emissiveStrength = d.emissiveStrength;
    m.alphaCutoff = d.alphaCutoff;
    m_materials.push_back(m);

    if (m_materialBatch)
    {
        m_srvStale = true;                     // (EndMaterialBatch rebuilds the heap, once)
        return (uint32_t)m_materials.size();
    }
    if (!RebuildSrvHeap(dev))
    {
        m_materials.pop_back();
        return 0;
    }
    return (uint32_t)m_materials.size();       // 1-based
}

bool Scene::EndMaterialBatch(ID3D12Device* dev)
{
    m_materialBatch = false;
    if (!m_srvStale) return true;
    m_srvStale = false;
    return RebuildSrvHeap(dev);
}

uint32_t Scene::AddInstance(uint32_t mesh, uint32_t material, uint32_t layer,
                            const float xform[16])
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (mesh == 0 || mesh > m_dynMeshes.size()) return kUnknownInstance;
    if (material == 0 || material > m_materials.size()) return kUnknownInstance;

    DynMesh& dm = m_dynMeshes[mesh - 1];
    if (!dm.alive) return kUnknownInstance;

    DrawItem o;
    o.layerId = layer;
    o.materialId = material - 1;
    o.matFlags = m_materials[o.materialId].flags;   // authored content is trusted as-is
    o.firstIndex = dm.firstIndex;
    o.indexCount = dm.indexCount;
    o.meshId = mesh;
    o.baseVertex = (int32_t)dm.firstVertex;
    for (int k = 0; k < 3; ++k) { o.bmin0[k] = dm.bmin[k]; o.bmax0[k] = dm.bmax[k]; }
    for (int k = 0; k < 3; ++k) { o.bmin[k] = dm.bmin[k]; o.bmax[k] = dm.bmax[k]; }

    m_objects.push_back(o);
    dm.refs++;

    const uint32_t index = (uint32_t)m_objects.size() - 1;

    // Route the placement through the same code the editor's drag uses, so a
    // freshly added object and a moved one can never disagree about what a
    // matrix means or how its culling box is derived.
    static const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    SetObjectTransform(index, xform ? xform : kIdentity);
    // Placed, not moved: its first frame has no previous position, and one
    // read as "it came from the identity" would be a jump across the stage.
    DrawItem& placed = m_objects[index];
    memcpy(placed.prevXform, placed.xform, sizeof(placed.xform));
    placed.prevIdentity = placed.identity;
    return index;
}

bool Scene::RemoveInstance(uint32_t index)
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (index >= m_objects.size()) return false;

    DrawItem& o = m_objects[index];
    if (o.removed) return true;                // idempotent: undo/redo hits this

    o.removed = true;
    o.visible = false;
    if (o.meshId && o.meshId <= m_dynMeshes.size() && m_dynMeshes[o.meshId - 1].refs)
        m_dynMeshes[o.meshId - 1].refs--;

    // The slot stays. Renumbering would invalidate every handle the editor is
    // holding -- including the ones in its undo stack, which is precisely what
    // must survive a delete.
    return true;
}

bool Scene::RemoveMesh(uint32_t mesh)
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (mesh == 0 || mesh > m_dynMeshes.size()) return false;
    DynMesh& m = m_dynMeshes[mesh - 1];
    if (m.refs) return false;                  // still drawn; the caller has a leak
    m.alive = false;
    return true;
}

bool Scene::Compact(ID3D12Device* dev)
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (!dev) return false;

    // Storage only. Instances are NEVER renumbered here -- see RemoveInstance --
    // so this is safe to run at any time, though save is where it belongs.
    std::vector<::ngg::Vertex> verts;
    std::vector<uint32_t>      indices;
    verts.reserve(m_dynVerts.size());
    indices.reserve(m_dynIndices.size());

    std::vector<uint32_t> newFirstVertex(m_dynMeshes.size(), 0);
    std::vector<uint32_t> newFirstIndex(m_dynMeshes.size(), 0);

    for (size_t i = 0; i < m_dynMeshes.size(); ++i)
    {
        DynMesh& m = m_dynMeshes[i];
        if (!m.alive || m.refs == 0)
        {
            m.alive = false;
            m.vertexCount = m.indexCount = 0;
            continue;
        }
        newFirstVertex[i] = (uint32_t)verts.size();
        newFirstIndex[i]  = (uint32_t)indices.size();
        verts.insert(verts.end(),
                     m_dynVerts.begin() + m.firstVertex,
                     m_dynVerts.begin() + m.firstVertex + m.vertexCount);
        // Mesh-relative indices, so moving a mesh needs no rewriting at all.
        indices.insert(indices.end(),
                       m_dynIndices.begin() + m.firstIndex,
                       m_dynIndices.begin() + m.firstIndex + m.indexCount);
    }

    const size_t freedVerts = m_dynVerts.size() - verts.size();
    for (size_t i = 0; i < m_dynMeshes.size(); ++i)
    {
        if (!m_dynMeshes[i].alive) continue;
        m_dynMeshes[i].firstVertex = newFirstVertex[i];
        m_dynMeshes[i].firstIndex  = newFirstIndex[i];
    }

    // Every live instance must follow its mesh to the new offsets. Missing this
    // would draw the right triangle count from the wrong place, which looks like
    // corrupted geometry rather than like a compaction bug.
    for (DrawItem& o : m_objects)
    {
        if (!o.meshId || o.removed) continue;
        const DynMesh& m = m_dynMeshes[o.meshId - 1];
        if (!m.alive) continue;
        o.firstIndex = m.firstIndex;
        o.indexCount = m.indexCount;
        o.baseVertex = (int32_t)m.firstVertex;
    }

    m_dynVerts.swap(verts);
    m_dynIndices.swap(indices);
    m_dynDirty = true;
    Log("scene: compacted the dynamic pool, %zu vertices reclaimed", freedVerts);
    return UploadDynamic(dev);
}

// Re-uploads the whole dynamic pool. Called lazily from the draw loop, so a
// hundred adds in one editor operation cost one upload rather than a hundred.
bool Scene::UploadDynamic(ID3D12Device* dev)
{
    m_dynDirty = false;
    if (m_dynVerts.empty() || m_dynIndices.empty())
    {
        m_dynVb.Reset(); m_dynIb.Reset();
        m_dynVbv = {}; m_dynIbv = {};
        return true;
    }

    const size_t vBytes = m_dynVerts.size() * sizeof(::ngg::Vertex);
    const size_t iBytes = m_dynIndices.size() * sizeof(uint32_t);

    m_dynVb = MakeUploadBuffer(dev, m_dynVerts.data(), vBytes);
    m_dynIb = MakeUploadBuffer(dev, m_dynIndices.data(), iBytes);
    if (!m_dynVb || !m_dynIb) { Log("scene: dynamic pool upload failed"); return false; }

    m_dynVbv.BufferLocation = m_dynVb->GetGPUVirtualAddress();
    m_dynVbv.SizeInBytes = (UINT)vBytes;
    m_dynVbv.StrideInBytes = sizeof(::ngg::Vertex);

    m_dynIbv.BufferLocation = m_dynIb->GetGPUVirtualAddress();
    m_dynIbv.SizeInBytes = (UINT)iBytes;
    m_dynIbv.Format = DXGI_FORMAT_R32_UINT;
    return true;
}

bool Scene::SetObjectVisible(uint32_t index, bool visible)
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (index >= m_objects.size()) return false;
    m_objects[index].visible = visible;
    return true;
}

bool Scene::SetObjectTransform(uint32_t index, const float xform[16])
{
    ++m_geomRevision;   // the RT structure follows this (Renderer::KeepRtCurrent)
    if (index >= m_objects.size() || !xform) return false;

    DrawItem& o = m_objects[index];
    for (int i = 0; i < 16; ++i) o.xform[i] = xform[i];

    // Worth detecting: the overwhelming majority of a track never moves, and an
    // identity matrix that is KNOWN to be identity costs no root-constant write.
    static const float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    o.identity = true;
    for (int i = 0; i < 16 && o.identity; ++i)
        if (o.xform[i] != kIdentity[i]) o.identity = false;

    // Re-bound the object, from the ORIGINAL box: culling tests this, and a box
    // that no longer contains the geometry culls it at the wrong moment -- which
    // reads as an object flickering out when the camera turns, not as a bounds
    // bug.
    if (o.identity)
    {
        for (int k = 0; k < 3; ++k) { o.bmin[k] = o.bmin0[k]; o.bmax[k] = o.bmax0[k]; }
        return true;
    }

    for (int k = 0; k < 3; ++k) { o.bmin[k] = 1e30f; o.bmax[k] = -1e30f; }
    for (int corner = 0; corner < 8; ++corner)
    {
        const float c[3] = {
            (corner & 1) ? o.bmax0[0] : o.bmin0[0],
            (corner & 2) ? o.bmax0[1] : o.bmin0[1],
            (corner & 4) ? o.bmax0[2] : o.bmin0[2],
        };

        // Row-vector convention, matching the shader's mul(v, M).
        for (int k = 0; k < 3; ++k)
        {
            const float v = c[0] * o.xform[k] + c[1] * o.xform[4 + k] +
                            c[2] * o.xform[8 + k] + o.xform[12 + k];
            if (v < o.bmin[k]) o.bmin[k] = v;
            if (v > o.bmax[k]) o.bmax[k] = v;
        }
    }
    return true;
}

bool Scene::ObjectBounds(uint32_t index, float* outMin3, float* outMax3) const
{
    if (index >= m_objects.size() || !outMin3 || !outMax3) return false;
    for (int k = 0; k < 3; ++k)
    {
        outMin3[k] = m_objects[index].bmin[k];
        outMax3[k] = m_objects[index].bmax[k];
    }
    return true;
}

bool Scene::ObjectVisible(uint32_t index) const
{
    return index < m_objects.size() && m_objects[index].visible;
}

uint32_t Scene::VisibleObjectCount() const
{
    uint32_t n = 0;
    for (const DrawItem& o : m_objects) if (o.visible) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Selection highlight (design 8.2, tier 1).
//
// The shader is deliberately tiny: position in, flat colour out. It reads no
// textures, no lighting, no atmosphere - an overlay routed through the shading
// path would change colour with the sun, which is the opposite of what a UI
// affordance is for.
//
// The pixel counters are not decoration. From outside the DLL, "the highlight
// is not drawing" and "the highlight is drawing and every pixel is being
// rejected by the depth test" look identical - both are an unchanged screen -
// and they have completely different causes. The renderer counts what it wrote.
// ---------------------------------------------------------------------------
namespace {

const char* kSelectShaderSrc = R"(
cbuffer SelCB : register(b0)
{
    row_major float4x4 gMvp;      // object transform * view * reverse-Z proj
    float4             gColour;
    uint               gPass;     // 0 = passed depth, 1 = occluded
    uint               gScreenSpace;  // labels: anchor.xy is PIXELS, not world
    float2             gTextOffset;   // labels: pixel shift, for the shadow pass
};

RWStructuredBuffer<uint> gCount : register(u0);

float4 VSSel(float3 pos : POSITION) : SV_Position
{
    return mul(float4(pos, 1.0), gMvp);
}

// [earlydepthstencil] IS LOAD-BEARING, not an optimisation.
//
// A pixel shader that writes a UAV has early depth-stencil turned OFF by
// default, because the hardware cannot know the shader will not discard or
// write oDepth. The depth test then runs AFTER the shader - which still
// suppresses the colour write, so the picture is correct either way, but the
// shader has already run and its InterlockedAdd has already landed. The
// counter would be measuring RASTERISED pixels and calling them VISIBLE ones.
//
// Measured, before the attribute went on: both counters reported exactly
// 25,932 - every wire pixel counted twice, once per pass, with the depth test
// contributing nothing. Two identical numbers out of two complementary depth
// functions is the shape of this bug.
//
// Safe here because this shader does neither of the things that make early-Z
// wrong: no discard, no SV_Depth. Depth WRITES are already masked off.
[earlydepthstencil]
float4 PSSel() : SV_Target
{
    InterlockedAdd(gCount[gPass], 1);
    return gColour;
}

// Overlay lines carry their own colour per vertex, so the editor picks colours
// by writing data rather than by calling a state setter. That is the whole
// difference between this and the immediate-mode D3D9 model it replaces.
struct OvOut
{
    float4 pos : SV_Position;
    float4 col : COLOR0;
};

OvOut VSOverlay(float3 pos : POSITION, float4 col : COLOR0)
{
    OvOut o;
    o.pos = mul(float4(pos, 1.0), gMvp);
    o.col = col;
    return o;
}

[earlydepthstencil]
float4 PSOverlay(OvOut i) : SV_Target
{
    InterlockedAdd(gCount[gPass], 1);
    // gColour.a is the pass fade: 1 where the line is in front, less where it
    // is behind something. The rgb stays the caller's.
    return float4(i.col.rgb, i.col.a * gColour.a);
}

// ------------------------------------------------------------------- labels
Texture2D    gFont     : register(t0);
SamplerState gFontSamp : register(s0);

struct LabOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

LabOut VSLabel(float3 anchor : POSITION, float2 off : TEXCOORD0,
               float2 uv : TEXCOORD1, float4 col : COLOR0)
{
    LabOut o;
    float4 clip;

    // gColour.xy carries 2/width and -2/height throughout; the sign flips y
    // because pixel space counts down and NDC counts up.
    if (gScreenSpace != 0)
    {
        // SCREEN SPACE: the anchor is already in pixels from the top-left, so
        // anchor and offset add in the same units and one quad builder serves
        // both modes. w = 1 means no perspective divide to cancel, and z = 0
        // puts it at the near plane - always drawn with the no-depth pipeline,
        // because a HUD that a hillside can occlude is not a HUD.
        float2 px = anchor.xy + off + gTextOffset;
        clip = float4(px.x * gColour.x - 1.0, px.y * gColour.y + 1.0, 0.0, 1.0);
    }
    else
    {
        clip = mul(float4(anchor, 1.0), gMvp);

        // The offset is in PIXELS. Multiplying by clip.w cancels the perspective
        // divide that follows, so a label keeps the same size on screen however
        // far away its anchor is -- which is the whole point of a label.
        clip.xy += (off + gTextOffset) * gColour.xy * clip.w;
    }

    o.pos = clip;
    o.uv = uv;
    o.col = col;
    return o;
}

[earlydepthstencil]
float4 PSLabel(LabOut i) : SV_Target
{
    float a = gFont.Sample(gFontSamp, i.uv).a;

    // Count only what is actually drawn. Putting the InterlockedAdd before the
    // discard would count the transparent parts of every glyph cell, turning the
    // instrument into a measure of quad area rather than of visible text.
    if (a < 0.02) discard;

    InterlockedAdd(gCount[gPass], 1);
    // gColour.w tints the glyph: 1 leaves the caller's colour, 0 makes it black
    // for the shadow pass. A tint rather than a second colour constant, because
    // the shadow only ever needs to be darker, never a different hue.
    return float4(i.col.rgb * gColour.w, i.col.a * a * gColour.z);
}
)";

// Orange: the one hue that separates from grass, tarmac, sky and RBR's grey
// scenery at once. Written AFTER the tonemap into an 8-bit target, so these are
// literal display values rather than radiance.
const float kSelColourVisible[4]  = { 1.00f, 0.45f, 0.05f, 1.00f };
const float kSelColourOccluded[4] = { 1.00f, 0.45f, 0.05f, 0.30f };

// Counter slots in the one 16-byte UAV. Four, not two: the overlay is counted
// apart from the selection, or "the drive line is drawing" and "the highlight is
// drawing" become one number that answers neither question.
enum { kCountSelVisible = 0, kCountSelOccluded = 1,
       kCountOvVisible = 2, kCountOvOccluded = 3,
       kCountLabVisible = 4, kCountLabOccluded = 5,
       kCountSlots = 8 };            // 8, not 6: the copies stay 16-byte aligned

// The label vertex. Anchor in world space, offset in pixels -- the split is what
// gives a constant screen size without the host having to know the projection.
struct LabelVertex
{
    float    anchor[3];
    float    offset[2];
    float    uv[2];
    uint32_t colour;
};
static_assert(sizeof(LabelVertex) == 32, "LabelVertex layout");

struct SelConstants
{
    float    mvp[16];
    float    colour[4];
    uint32_t pass;
    uint32_t screenSpace;   // labels only; see VSLabel
    float    textOffset[2]; // labels only; the shadow pass shifts by a pixel
};
static_assert(sizeof(SelConstants) == 24 * 4, "SelConstants must match the root constant count");

} // namespace

bool Scene::SetSelection(const uint32_t* indices, uint32_t count)
{
    m_selection.clear();
    if (!indices || !count) return true;

    for (uint32_t i = 0; i < count; ++i)
    {
        if (indices[i] >= m_objects.size()) return false;   // reject the whole set
        m_selection.push_back(indices[i]);
    }
    return true;
}

bool Scene::CreateSelectionPipeline(ID3D12Device* dev, DXGI_FORMAT outputFormat)
{
    if (!dev) return false;

    // Root constants plus one root UAV - no descriptor heap at all, so this
    // pass cannot disturb the material heap the main pass depends on.
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;      // b0
    params[0].Constants.Num32BitValues = 24;     // 4x4 + float4 + uint + 3 pad
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;     // u0
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("selection: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&m_selRootSig))))
        return false;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vs, ps;
    const std::string src = kSelectShaderSrc;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                          "VSSel", "vs_5_1", flags, 0, &vs, &err)))
    {
        Log("selection: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                          "PSSel", "ps_5_1", flags, 0, &ps, &err)))
    {
        Log("selection: PS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    // POSITION only. The stride comes from the vertex buffer view, so the other
    // 32 bytes of each vertex are simply not fetched.
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_selRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = outputFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // NONE, matching the main pass: what is highlighted has to be what draws.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    // POSITIVE bias, which is the opposite of the conventional-depth instinct:
    // under reverse-Z nearer is LARGER, so adding to the depth value pulls the
    // wire towards the camera. Getting the sign wrong here does not fail - it
    // pushes the wire into the surface it traces and the highlight simply never
    // appears, with no error anywhere.
    //
    // A bias is needed at all because TAA jitters the depth buffer by up to half
    // a pixel relative to the unjittered matrix this pass uses (see below), and
    // on a road at a grazing angle half a pixel of slope is a real depth
    // difference. Slope-scaled is the term that covers exactly that.
    pso.RasterizerState.DepthBias = 200;
    pso.RasterizerState.SlopeScaledDepthBias = 3.0f;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;   // overlay only
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

    for (UINT i = 0; i < 8; ++i)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoSelVisible))))
    {
        Log("selection: visible-pass PSO creation failed");
        return false;
    }

    // The occluded half. LESS is the exact complement of GREATER_EQUAL under the
    // same bias, so every wire pixel lands in one pass or the other and never in
    // both - which is what makes the two counters add up to the whole wire.
    //
    // Drawing it at all is the difference between a selection you can find and
    // one that vanishes behind a hill. An editor that hides the thing you just
    // selected is worse than one with no highlight.
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoSelOccluded))))
    {
        Log("selection: occluded-pass PSO creation failed");
        return false;
    }

    // --- labels: textured quads, their own root signature -------------------
    // Separate because this is the one overlay pass that samples a texture, so it
    // needs a descriptor table and a sampler the others have no use for.
    {
        D3D12_DESCRIPTOR_RANGE fontRange = {};
        fontRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        fontRange.NumDescriptors = 1;
        fontRange.BaseShaderRegister = 0;      // t0

        D3D12_ROOT_PARAMETER lp[3] = {};
        lp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        lp[0].Constants.ShaderRegister = 0;
        lp[0].Constants.Num32BitValues = 24;
        lp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        lp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        lp[1].DescriptorTable.NumDescriptorRanges = 1;
        lp[1].DescriptorTable.pDescriptorRanges = &fontRange;
        lp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        lp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        lp[2].Descriptor.ShaderRegister = 0;   // u0
        lp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC ls = {};
        ls.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ls.AddressU = ls.AddressV = ls.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ls.MaxLOD = D3D12_FLOAT32_MAX;
        ls.ShaderRegister = 0;
        ls.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC lrs = {};
        lrs.NumParameters = 3;
        lrs.pParameters = lp;
        lrs.NumStaticSamplers = 1;
        lrs.pStaticSamplers = &ls;
        lrs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> lsig, lerr;
        if (FAILED(D3D12SerializeRootSignature(&lrs, D3D_ROOT_SIGNATURE_VERSION_1, &lsig, &lerr)))
        {
            Log("labels: root signature: %s", lerr ? (const char*)lerr->GetBufferPointer() : "?");
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, lsig->GetBufferPointer(), lsig->GetBufferSize(),
                                            IID_PPV_ARGS(&m_labelRootSig))))
            return false;

        ComPtr<ID3DBlob> lvs, lps;
        if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                              "VSLabel", "vs_5_1", flags, 0, &lvs, &err)))
        {
            Log("labels: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                              "PSLabel", "ps_5_1", flags, 0, &lps, &err)))
        {
            Log("labels: PS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }

        D3D12_INPUT_ELEMENT_DESC lay[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 28,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC lp2 = pso;   // inherits the occluded blend
        lp2.pRootSignature = m_labelRootSig.Get();
        lp2.VS = { lvs->GetBufferPointer(), lvs->GetBufferSize() };
        lp2.PS = { lps->GetBufferPointer(), lps->GetBufferSize() };
        lp2.InputLayout = { lay, _countof(lay) };
        lp2.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        lp2.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        lp2.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        lp2.RasterizerState.DepthBias = 0;
        lp2.RasterizerState.SlopeScaledDepthBias = 0.0f;
        // Depth comes from the ANCHOR, which every vertex of the quad carries, so
        // a label is occluded exactly when the point it names is.
        lp2.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        if (FAILED(dev->CreateGraphicsPipelineState(&lp2, IID_PPV_ARGS(&m_psoLabelOccluded))))
        {
            Log("labels: occluded PSO creation failed");
            return false;
        }

        lp2.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        if (FAILED(dev->CreateGraphicsPipelineState(&lp2, IID_PPV_ARGS(&m_psoLabel))))
        {
            Log("labels: visible PSO creation failed");
            return false;
        }

        lp2.DepthStencilState.DepthEnable = FALSE;
        if (FAILED(dev->CreateGraphicsPipelineState(&lp2, IID_PPV_ARGS(&m_psoLabelNoDepth))))
        {
            Log("labels: no-depth PSO creation failed");
            return false;
        }
    }

    // --- overlay lines: same placement, different vertices ------------------
    {
        ComPtr<ID3DBlob> ovs, ops;
        if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                              "VSOverlay", "vs_5_1", flags, 0, &ovs, &err)))
        {
            Log("overlay: VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        if (FAILED(D3DCompile(src.c_str(), src.size(), "ngg_select", nullptr, nullptr,
                              "PSOverlay", "ps_5_1", flags, 0, &ops, &err)))
        {
            Log("overlay: PS compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }

        D3D12_INPUT_ELEMENT_DESC ovLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC ov = pso;   // inherits the occluded blend
        ov.VS = { ovs->GetBufferPointer(), ovs->GetBufferSize() };
        ov.PS = { ops->GetBufferPointer(), ops->GetBufferSize() };
        ov.InputLayout = { ovLayout, _countof(ovLayout) };
        ov.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        ov.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;   // meaningless for lines
        // No bias: an overlay line is not tracing a surface the way a selection
        // wire is, so pulling it forward would only make it float over geometry
        // it is meant to sit against.
        ov.RasterizerState.DepthBias = 0;
        ov.RasterizerState.SlopeScaledDepthBias = 0.0f;
        ov.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        if (FAILED(dev->CreateGraphicsPipelineState(&ov, IID_PPV_ARGS(&m_psoOvOccluded))))
        {
            Log("overlay: occluded-pass PSO creation failed");
            return false;
        }

        ov.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        if (FAILED(dev->CreateGraphicsPipelineState(&ov, IID_PPV_ARGS(&m_psoOvVisible))))
        {
            Log("overlay: visible-pass PSO creation failed");
            return false;
        }

        // Always on top. Manipulator handles want this: a gizmo you cannot grab
        // because the thing you are dragging is in front of it is not a gizmo.
        ov.DepthStencilState.DepthEnable = FALSE;
        if (FAILED(dev->CreateGraphicsPipelineState(&ov, IID_PPV_ARGS(&m_psoOvNoDepth))))
        {
            Log("overlay: no-depth PSO creation failed");
            return false;
        }
    }

    // Counter buffer, its zero source and its readback. Eight slots, not six:
    // selection, overlay and labels take two each, and rounding up keeps the
    // copies 16-byte aligned.
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kCountSlots * 4;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_selCount))))
        return false;

    const uint32_t zero[kCountSlots] = {};
    m_selCountZero = MakeUploadBuffer(dev, zero, sizeof(zero));
    if (!m_selCountZero) return false;

    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&m_selCountReadback))))
        return false;

    Log("selection: overlay pipeline ready");
    return true;
}

// ---------------------------------------------------------------------------
// Overlay line layers (design 8.2).
//
// This is the shape that lets an ngg-core backend replace D3D9 at all. The old
// editor path is immediate mode -- every map object issues SetRenderState,
// SetTransform and DrawPrimitives for itself -- and no retained-mode renderer
// can implement that interface. Here the caller states what a layer CONTAINS
// and the renderer owns everything about how it is drawn.
// ---------------------------------------------------------------------------

bool Scene::SetOverlayLines(ID3D12Device* dev, uint32_t layer,
                            const OverlayVertex* verts, uint32_t count, uint32_t flags)
{
    if (!dev) return false;
    if (layer >= 64) return false;          // a typo'd id must not allocate a million slots
    if (count % 2) return false;            // segments come in pairs; an odd count is a bug

    if (m_overlays.size() <= layer) m_overlays.resize(layer + 1);
    OverlayLayer& l = m_overlays[layer];

    if (!verts || count == 0)
    {
        l.vb.Reset();
        l.vbv = {};
        l.count = 0;
        return true;
    }

    // Replaced wholesale rather than patched. The frame loop is synchronous, so
    // the previous buffer cannot still be in flight; when frames start
    // overlapping this needs the same lifetime treatment as m_frameCB.
    const size_t bytes = (size_t)count * sizeof(OverlayVertex);
    l.vb = MakeUploadBuffer(dev, verts, bytes);
    if (!l.vb) { l.count = 0; return false; }

    l.vbv.BufferLocation = l.vb->GetGPUVirtualAddress();
    l.vbv.SizeInBytes = (UINT)bytes;
    l.vbv.StrideInBytes = sizeof(OverlayVertex);
    l.count = count;
    l.flags = flags;
    return true;
}

void Scene::ClearOverlays()
{
    m_overlays.clear();
}

uint32_t Scene::OverlayVertexCount() const
{
    uint32_t n = 0;
    for (const OverlayLayer& l : m_overlays) n += l.count;
    return n;
}

void Scene::DrawOverlays(ID3D12GraphicsCommandList* list, const float* viewProj)
{
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

    SelConstants k = {};
    memcpy(k.mvp, viewProj, sizeof(k.mvp));

    // Occluded first, so a line that crosses a silhouette is overwritten by its
    // own bright half rather than tinted by the dim one.
    for (int pass = 1; pass >= 0; --pass)
    {
        for (const OverlayLayer& l : m_overlays)
        {
            if (!l.count) continue;

            const bool noDepth = (l.flags & kOverlayNoDepth) != 0;
            if (noDepth && pass) continue;    // one pass only: nothing occludes it

            list->SetPipelineState(noDepth ? m_psoOvNoDepth.Get()
                                           : (pass ? m_psoOvOccluded.Get()
                                                   : m_psoOvVisible.Get()));

            // The pass fade, and the counter slot, are the only per-pass state.
            k.colour[0] = k.colour[1] = k.colour[2] = 1.0f;
            k.colour[3] = pass ? 0.30f : 1.0f;
            k.pass = pass ? kCountOvOccluded : kCountOvVisible;

            list->SetGraphicsRoot32BitConstants(0, 24, &k, 0);
            list->IASetVertexBuffers(0, 1, &l.vbv);
            list->DrawInstanced(l.count, 1, 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// World-space text labels.
//
// The last thing the editor draws that the renderer could not do for it. Object
// names and ids sit at world positions; the host cannot paint them on top of the
// viewport because a native child window with a swap chain wins the airspace
// fight, so host-drawn text either flickers or never appears at all.
// ---------------------------------------------------------------------------

bool Scene::SetFont(ID3D12Device* dev, ID3D12CommandQueue* queue, const void* rgba,
                    uint32_t width, uint32_t height,
                    uint32_t columns, uint32_t rows, uint32_t firstChar)
{
    if (!dev || !queue || !rgba || !width || !height || !columns || !rows) return false;

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                      nullptr, IID_PPV_ARGS(&list))))
        return false;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> tex;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&tex))))
        return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = total;
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&upload))))
        return false;

    uint8_t* dst = nullptr;
    D3D12_RANGE none = { 0, 0 };
    upload->Map(0, &none, reinterpret_cast<void**>(&dst));
    // Row by row: the upload pitch is aligned to 256 bytes and the caller's is not.
    for (uint32_t y = 0; y < height; ++y)
        memcpy(dst + fp.Offset + (size_t)y * fp.Footprint.RowPitch,
               static_cast<const uint8_t*>(rgba) + (size_t)y * width * 4,
               (size_t)width * 4);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = tex.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION sloc = {};
    sloc.pResource = upload.Get();
    sloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sloc.PlacedFootprint = fp;
    list->CopyTextureRegion(&d, 0, 0, 0, &sloc, nullptr);

    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = tex.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &toSrv);

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 30000);
        CloseHandle(evt);
    }

    m_font = tex;
    m_fontColumns = columns;
    m_fontRows = rows;
    m_fontFirstChar = firstChar;

    // The descriptor lives in the scene's heap, so it has to be (re)written there.
    if (m_srvHeap) RebuildSrvHeap(dev);
    Log("labels: font %ux%u, %ux%u cells from char %u", width, height, columns, rows, firstChar);
    return true;
}

uint32_t Scene::LabelGlyphCount() const
{
    uint32_t n = 0;
    for (const LabelLayer& l : m_labels) n += l.count;
    return n / 6;                       // six vertices per glyph
}

bool Scene::SetLabels(ID3D12Device* dev, uint32_t layer, const Label* labels, uint32_t count,
                      const char* textBlob, uint32_t blobBytes,
                      float pixelHeight, uint32_t flags)
{
    if (!dev) return false;
    if (layer >= 64) return false;

    if (m_labels.size() <= layer) m_labels.resize(layer + 1);
    LabelLayer& l = m_labels[layer];

    if (!labels || count == 0)
    {
        l.vb.Reset(); l.vbv = {}; l.count = 0;
        return true;
    }
    if (!m_font) return false;                 // no atlas: refuse rather than draw blanks
    if (pixelHeight <= 0.0f) return false;

    const float cellU = 1.0f / m_fontColumns;
    const float cellV = 1.0f / m_fontRows;
    // Advance follows the cell's aspect, so a non-square atlas still lines up.
    const float aspect = (float)m_fontColumns / (float)m_fontRows;
    const float glyphW = pixelHeight / aspect;

    std::vector<LabelVertex> verts;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Label& lab = labels[i];
        if (!textBlob || lab.textOffset >= blobBytes) continue;

        const char* text = textBlob + lab.textOffset;
        uint32_t maxLen = blobBytes - lab.textOffset;
        uint32_t len = 0;
        while (len < maxLen && text[len] != '\0') ++len;
        if (len == 0) continue;

        // A WORLD label is centred on its anchor and lifted clear of it, so it
        // marks its object rather than covering it. A SCREEN label is a HUD
        // line: the anchor is its top-left, because that is the only origin a
        // caller laying out rows of text can predict without measuring them.
        const bool screen = (flags & kOverlayScreenSpace) != 0;
        float penX = screen ? 0.0f : -0.5f * glyphW * (float)len;
        const float penY = screen ? 0.0f : -pixelHeight * 1.5f;

        for (uint32_t c = 0; c < len; ++c)
        {
            uint32_t ch = (uint8_t)text[c];
            if (ch < m_fontFirstChar) { penX += glyphW; continue; }
            uint32_t cell = ch - m_fontFirstChar;
            if (cell >= m_fontColumns * m_fontRows) { penX += glyphW; continue; }

            float u0 = (cell % m_fontColumns) * cellU;
            float v0 = (cell / m_fontColumns) * cellV;
            float u1 = u0 + cellU, v1 = v0 + cellV;

            const float x0 = penX, x1 = penX + glyphW;
            const float y0 = penY, y1 = penY + pixelHeight;

            LabelVertex q[4];
            const float px[4] = { x0, x1, x1, x0 };
            const float py[4] = { y0, y0, y1, y1 };
            const float qu[4] = { u0, u1, u1, u0 };
            // v0 goes with y0, NOT v1: every glyph used to come out mirrored top
            // for bottom. The offset is in PIXELS and VSLabel scales it by
            // -2/height, so the MORE NEGATIVE offset is the HIGHER one on screen --
            // y0 (penY) is the quad's TOP edge, not its bottom. Atlas V grows
            // downward and v0 is the top of the cell, so the top edge samples v0.
            //
            // Cell SELECTION was always top-down (v0 = row * cellV, which is what
            // "first_char in the top-left cell" in ngg_api.h promises); only the
            // sampling WITHIN a cell disagreed. That is why it read as a bad font
            // rather than as a wrong atlas, and why it survived NggPreview: its
            // atlas is non-square, so its labels were squashed 2.67x as well and
            // nobody read one closely enough to notice they were upside down.
            const float qv[4] = { v0, v0, v1, v1 };
            for (int k = 0; k < 4; ++k)
            {
                q[k].anchor[0] = lab.pos[0];
                q[k].anchor[1] = lab.pos[1];
                q[k].anchor[2] = lab.pos[2];
                q[k].offset[0] = px[k];
                q[k].offset[1] = py[k];
                q[k].uv[0] = qu[k];
                q[k].uv[1] = qv[k];
                q[k].colour = lab.colour;
            }
            // Two triangles, expanded here rather than indexed: a label layer is
            // rebuilt whenever it changes, so an index buffer would be a second
            // allocation to keep in step for no saving worth having.
            verts.push_back(q[0]); verts.push_back(q[1]); verts.push_back(q[2]);
            verts.push_back(q[0]); verts.push_back(q[2]); verts.push_back(q[3]);

            penX += glyphW;
        }
    }

    if (verts.empty())
    {
        l.vb.Reset(); l.vbv = {}; l.count = 0;
        return true;
    }

    const size_t bytes = verts.size() * sizeof(LabelVertex);
    l.vb = MakeUploadBuffer(dev, verts.data(), bytes);
    if (!l.vb) { l.count = 0; return false; }
    l.vbv.BufferLocation = l.vb->GetGPUVirtualAddress();
    l.vbv.SizeInBytes = (UINT)bytes;
    l.vbv.StrideInBytes = sizeof(LabelVertex);
    l.count = (uint32_t)verts.size();
    l.flags = flags;
    return true;
}

void Scene::DrawLabels(ID3D12GraphicsCommandList* list, const float* viewProj,
                       UINT width, UINT height)
{
    if (!m_psoLabel || !m_font || !m_srvHeap || LabelGlyphCount() == 0) return;

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);

    list->SetGraphicsRootSignature(m_labelRootSig.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE fh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    fh.ptr += (UINT64)m_fontSrvSlot * m_srvStride;
    list->SetGraphicsRootDescriptorTable(1, fh);
    list->SetGraphicsRootUnorderedAccessView(2, m_selCount->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    SelConstants k = {};
    memcpy(k.mvp, viewProj, sizeof(k.mvp));
    // Pixels to NDC. Y is negated because pixel space counts down the screen.
    k.colour[0] = 2.0f / (float)width;
    k.colour[1] = -2.0f / (float)height;

    for (int pass = 1; pass >= 0; --pass)
    {
        for (const LabelLayer& l : m_labels)
        {
            if (!l.count) continue;
            // Screen space implies no depth: there is nothing meaningful to
            // test a HUD against, and a frame-time readout that a hillside can
            // hide is not a readout.
            const bool noDepth = (l.flags & (kOverlayNoDepth | kOverlayScreenSpace)) != 0;
            if (noDepth && pass) continue;

            list->SetPipelineState(noDepth ? m_psoLabelNoDepth.Get()
                                           : (pass ? m_psoLabelOccluded.Get() : m_psoLabel.Get()));
            k.pass = pass ? kCountLabOccluded : kCountLabVisible;
            k.screenSpace = (l.flags & kOverlayScreenSpace) ? 1u : 0u;
            // Behind geometry the text fades rather than vanishing, matching the
            // overlay lines: a name you cannot find is worse than a faint one.
            k.colour[2] = pass ? 0.28f : 1.0f;

            // Drop shadow: the same glyphs again, one pixel down-right and
            // tinted black, UNDER the text. Cheap and it is the difference
            // between a readout and a readout you can only read on dark
            // ground - white text over a sunlit road or a bright sky is
            // otherwise invisible, which is the one situation a HUD exists for.
            //
            // Two draws rather than an outline in the shader: the glyph atlas
            // has no distance field to grow an outline from, and a second draw
            // of 300 quads costs nothing measurable.
            const int first = (l.flags & kOverlayTextShadow) ? 1 : 0;
            for (int shadow = first; shadow >= 0; --shadow)
            {
                k.textOffset[0] = k.textOffset[1] = shadow ? 1.0f : 0.0f;
                k.colour[3] = shadow ? 0.0f : 1.0f;      // tint: black, or as authored
                list->SetGraphicsRoot32BitConstants(0, 24, &k, 0);
                list->IASetVertexBuffers(0, 1, &l.vbv);
                list->DrawInstanced(l.count, 1, 0, 0);
            }
        }
    }
}

void Scene::RenderEditorOverlay(ID3D12GraphicsCommandList* list,
                               D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                               const float* view, const float* proj, UINT width, UINT height)
{
    m_selPixVisible = m_selPixOccluded = 0;
    m_ovPixVisible = m_ovPixOccluded = 0;
    m_labPixVisible = m_labPixOccluded = 0;
    if (!m_psoSelVisible || !m_dsvHeap) return;

    const bool anySelection = Ready() && !m_selection.empty();
    const bool anyOverlay   = OverlayVertexCount() != 0;
    const bool anyLabel     = LabelGlyphCount() != 0;
    if (!anySelection && !anyOverlay && !anyLabel) return;

    // UNJITTERED, unlike Record. The image underneath has already been through
    // TAA and is temporally stable; a wire drawn with the frame's jitter would
    // crawl by half a pixel against a still picture, and on a one-pixel line
    // that is the most visible artefact on screen. The cost is that wire and
    // depth buffer disagree by up to half a pixel, absorbed by the selection
    // PSO's depth bias -- the right way round, because a sub-pixel depth error
    // is invisible and a crawling outline is not.
    float rz[16], viewProj[16];
    ReverseZ(proj, rz);
    Mul4x4(view, rz, viewProj);

    // Zero the counters ONCE for both halves. Doing it per half would make each
    // one read the other's leftovers, and the numbers exist precisely so that
    // "drew nothing" and "drew and was rejected" stay distinguishable.
    //
    // ClearUnorderedAccessViewUint would need a descriptor heap slot; this pass
    // deliberately has no heap, so a 16-byte copy is simpler and cheaper.
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_selCount.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(m_selCount.Get(), 0, m_selCountZero.Get(), 0, kCountSlots * 4);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)width, (LONG)height };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetGraphicsRootSignature(m_selRootSig.Get());
    list->SetGraphicsRootUnorderedAccessView(1, m_selCount->GetGPUVirtualAddress());

    if (anySelection) DrawSelection(list, viewProj);
    if (anyOverlay)   DrawOverlays(list, viewProj);
    if (anyLabel)     DrawLabels(list, viewProj, width, height);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(m_selCountReadback.Get(), m_selCount.Get());
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
}

void Scene::DrawSelection(ID3D12GraphicsCommandList* list, const float* viewProj)
{
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &m_vbv);
    list->IASetIndexBuffer(&m_ibv);
    int geomState = 0;

    // Occluded first, so the visible wire overwrites it wherever the two meet at
    // a silhouette rather than being tinted by it.
    for (int pass = 1; pass >= 0; --pass)
    {
        list->SetPipelineState(pass ? m_psoSelOccluded.Get() : m_psoSelVisible.Get());

        SelConstants k = {};
        memcpy(k.colour, pass ? kSelColourOccluded : kSelColourVisible, sizeof(k.colour));
        k.pass = pass ? kCountSelOccluded : kCountSelVisible;

        for (uint32_t index : m_selection)
        {
            const DrawItem& o = m_objects[index];
            if (!o.visible) continue;      // hidden means deleted; see SetSelection

            // The object transform folds into the matrix here rather than
            // costing a second root constant block per draw, and identity is
            // just the unmodified view-projection.
            if (o.identity) memcpy(k.mvp, viewProj, sizeof(k.mvp));
            else            Mul4x4(o.xform, viewProj, k.mvp);

            list->SetGraphicsRoot32BitConstants(0, 24, &k, 0);
            // Added objects are selectable like any other, so this loop needs
            // the same buffer switch the main pass has.
            BindGeometry(list, o.meshId != 0, geomState,
                         o.meshId ? m_dynVbv : m_vbv, o.meshId ? m_dynIbv : m_ibv);
            list->DrawIndexedInstanced(o.indexCount, 1, o.firstIndex, o.baseVertex, 0);
        }
    }
}

// Called AFTER the caller has waited for the GPU, exactly like LogProbe.
void Scene::ReadOverlayStats()
{
    if (!m_selCountReadback) return;
    if (m_selection.empty() && OverlayVertexCount() == 0 && LabelGlyphCount() == 0)
        return;                                                     // already zeroed

    uint32_t* p = nullptr;
    D3D12_RANGE all = { 0, kCountSlots * 4 };
    if (FAILED(m_selCountReadback->Map(0, &all, reinterpret_cast<void**>(&p)))) return;
    m_selPixVisible  = p[kCountSelVisible];
    m_selPixOccluded = p[kCountSelOccluded];
    m_ovPixVisible   = p[kCountOvVisible];
    m_ovPixOccluded  = p[kCountOvOccluded];
    m_labPixVisible  = p[kCountLabVisible];
    m_labPixOccluded = p[kCountLabOccluded];
    D3D12_RANGE none = { 0, 0 };
    m_selCountReadback->Unmap(0, &none);
}

void Scene::Record(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                   const float* view, const float* proj, UINT width, UINT height)
{
    RecordPass(list, rtv, view, proj, width, height, false);
}

void Scene::RecordPrepass(ID3D12GraphicsCommandList* list,
                          const float* view, const float* proj, UINT width, UINT height)
{
    if (!m_psoPrepass || !m_normal || !m_motion || !m_normalRtvHeap) return;
    RecordPass(list, m_normalRtvHeap->GetCPUDescriptorHandleForHeapStart(),
               view, proj, width, height, true);
    m_prepassThisFrame = true;
}

void Scene::RecordPass(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                       const float* view, const float* proj, UINT width, UINT height,
                       bool prepass)
{
    if (!Ready()) return;

    // The constant buffer is re-uploaded every frame through a fresh upload
    // allocation. Wasteful, and fine at one draw per frame; a ring buffer comes
    // with the real renderer.
    float camPos[3] = {};
    CameraPosFromView(view, camPos);
    for (int k = 0; k < 3; ++k) m_lastCamPos[k] = camPos[k];

    if (!m_anchorToCamera)
    {
        // Real geometry: draw it exactly where the file says. Any offset here
        // would silently move a whole track.
        if ((++m_recordCount % 300) == 0)
            Log("scene: camera at (%.1f %.1f %.1f)", camPos[0], camPos[1], camPos[2]);
    }
    else
    {
    // The test grid sits at the world origin, but a stage is nowhere near it -
    // the first run put the camera underneath the grid entirely. Rather than
    // guess coordinates, follow the camera in XZ, SNAPPED to a whole number of
    // checker cells so the pattern still reads as world-locked and motion is
    // still verifiable. Height is frozen on the first frame, so driving over
    // terrain shows real vertical movement instead of the grid tracking us.
    // Re-anchor on a teleport, not just once.
    //
    // Anchoring only on the first frame was wrong: the host starts while the
    // game is still in the menu, so the grid ended up pinned at the menu
    // camera's altitude and stayed there when a stage loaded - visible, but
    // far below or above the road. RBR gives every stage its own origin, and
    // start heights are arbitrary (negative Y is normal), so there is no
    // altitude that works everywhere. Detect the discontinuity instead.
    const float dx = camPos[0] - m_lastCam[0];
    const float dy = camPos[1] - m_lastCam[1];
    const float dz = camPos[2] - m_lastCam[2];
    const bool teleported = m_haveAnchor && (dx*dx + dy*dy + dz*dz) > (100.0f * 100.0f);
    const bool strayed    = m_haveAnchor && fabsf(camPos[1] - m_anchorY) > 100.0f;

    if (!m_haveAnchor || teleported || strayed)
    {
        // One-time dump of the raw view matrix. The inverse above assumes the
        // rotation block is orthonormal (no scale, no skew); if RBR ever hands
        // us something else, the row lengths in this dump say so immediately
        // instead of leaving us guessing at a mysterious offset.
        if (!m_loggedMatrix)
        {
            m_loggedMatrix = true;
            for (int r = 0; r < 4; ++r)
                Log("scene: view[%d] = %8.3f %8.3f %8.3f %8.3f  (|row|=%.4f)",
                       r, view[r*4+0], view[r*4+1], view[r*4+2], view[r*4+3],
                       sqrtf(view[r*4+0]*view[r*4+0] + view[r*4+1]*view[r*4+1] +
                             view[r*4+2]*view[r*4+2]));
        }

        m_anchorY = camPos[1] - 2.0f;
        m_haveAnchor = true;
        Log("scene: %s at (%.1f %.1f %.1f) - anchoring test grid to y=%.1f",
               teleported ? "camera teleported"
                          : (strayed ? "camera strayed from the anchor" : "first camera position"),
               camPos[0], camPos[1], camPos[2], m_anchorY);
    }
    m_lastCam[0] = camPos[0];
    m_lastCam[1] = camPos[1];
    m_lastCam[2] = camPos[2];
    }   // end of the test-scene-only anchoring block
    if (!prepass && (++m_recordCount % 300) == 0)
        Log("scene: camera at (%.1f %.1f %.1f)", camPos[0], camPos[1], camPos[2]);

    const float kSnap = 8.0f;   // two 4 m cells, so the checker phase is stable

    CameraCB cb = {};
    memcpy(cb.view, view, sizeof(cb.view));
    memcpy(cb.proj, proj, sizeof(cb.proj));

    // Reverse-Z. RBR hands us a conventional projection (near->0, far->1), which
    // is the worst possible pairing with a FLOAT depth buffer: float precision
    // is densest near 0, and a conventional projection already crowds distant
    // geometry there, so the far field ends up with almost no usable bits.
    // Flipping to near->1, far->0 puts float's dense region where the projection
    // is sparse; the two curves cancel and precision becomes near-uniform.
    //
    // Row-vector convention: clip.z = dot(v, col2), clip.w = dot(v, col3), so
    // z' = w - z is exactly col2 = col3 - col2. Paired with a 0.0 clear and
    // GREATER below.
    ReverseZ(proj, cb.proj);

    // The camera this frame, WITHOUT the jitter, kept for PostFx. Reverse-Z is
    // included because it is part of what the depth buffer means; the jitter is
    // not, and that distinction is load-bearing:
    //
    // TAA accumulates into a FIXED pixel grid. The jitter varies where inside a
    // pixel the scene is SAMPLED, not where the accumulated result sits. Build
    // the reprojection from jittered matrices on both sides and the history
    // follows the jitter instead of staying put, so the resolved image
    // oscillates over the Halton pattern - a whole-image shake of up to one
    // pixel, which looks like a tuning problem and is not one.
    //
    // Reprojecting an unjittered ray against a depth value that WAS jittered is
    // wrong by at most half a pixel of parallax. That error is real, universally
    // accepted, and orders of magnitude smaller than the shake.
    //
    // Last frame's is kept first, for the motion target: once per FRAME, by
    // whichever pass comes first, or the main pass after a prepass would make
    // "last frame" this frame. The very first frame has none and uses its own.
    const bool firstPassOfFrame = prepass || !m_prepassThisFrame;
    if (firstPassOfFrame)
        memcpy(m_prevViewProj, m_lastViewProj, sizeof(m_prevViewProj));
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += cb.view[r * 4 + k] * cb.proj[k * 4 + c];
            m_lastViewProj[r * 4 + c] = acc;
        }
    if (firstPassOfFrame && !m_havePrevViewProj)
        memcpy(m_prevViewProj, m_lastViewProj, sizeof(m_prevViewProj));
    m_havePrevViewProj = true;
    memcpy(cb.viewProjNoJitter, m_lastViewProj, sizeof(cb.viewProjNoJitter));
    memcpy(cb.prevViewProj, m_prevViewProj, sizeof(cb.prevViewProj));

    // Sub-pixel jitter, applied AFTER the matrix above and HERE rather than in
    // the client for the same reason reverse-Z is: a client that pre-jitters
    // would have to know what the renderer does with the history, and the two
    // would drift. Row-vector convention - shifting clip.x by j*clip.w means
    // col0 += j*col3, so the offset survives the perspective divide as a
    // constant NDC shift.
    if (m_jitterX != 0.0f || m_jitterY != 0.0f)
    {
        for (int r = 0; r < 4; ++r)
        {
            cb.proj[r * 4 + 0] += m_jitterX * cb.proj[r * 4 + 3];
            cb.proj[r * 4 + 1] += m_jitterY * cb.proj[r * 4 + 3];
        }
    }
    // And the jittered product, for consumers that reconstruct real positions.
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += cb.view[r * 4 + k] * cb.proj[k * 4 + c];
            m_lastViewProjJittered[r * 4 + c] = acc;
        }

    // Report what RBR actually asked for, once - the depth range drives how bad
    // the precision was, and it is a per-game constant worth having on record.
    if (m_recordCount == 1 && proj[2 * 4 + 2] != 0.0f)
    {
        const float m22 = proj[2 * 4 + 2], m32 = proj[3 * 4 + 2];
        const float nearZ = -m32 / m22;
        const float farZ  = (m22 != 1.0f) ? (m22 * nearZ / (m22 - 1.0f)) : 0.0f;
        Log("scene: RBR projection near=%.4f far=%.1f (ratio %.0f:1)",
               nearZ, farZ, (nearZ > 0.0f) ? farZ / nearZ : 0.0f);
    }
    if (m_anchorToCamera)
    {
        cb.worldOffset[0] = SnapTo(camPos[0], kSnap);
        cb.worldOffset[1] = m_anchorY;
        cb.worldOffset[2] = SnapTo(camPos[2], kSnap);
    }
    // Sun from above and slightly to the side. Direction points FROM the light.
    cb.lightDir[0] = -0.45f; cb.lightDir[1] = -0.80f; cb.lightDir[2] = -0.40f;
    cb.bakedScale = m_bakedScale;
    cb.debugMode = (float)m_debugMode;

    cb.camPos[0] = camPos[0]; cb.camPos[1] = camPos[1]; cb.camPos[2] = camPos[2];

    // Params carry the direction light TRAVELS; the model wants the direction
    // TOWARDS the sun. Same negation as Sky::Record, for the same reason.
    {
        const float* sd = m_atmosphere.sunDir;
        float len = sqrtf(sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2]);
        if (len < 1e-6f) len = 1.0f;
        cb.sunTowards[0] = -sd[0] / len;
        cb.sunTowards[1] = -sd[1] / len;
        cb.sunTowards[2] = -sd[2] / len;
    }
    cb.sunIntensity = m_atmosphere.sunIntensity;
    cb.turbidity    = m_atmosphere.turbidity;
    cb.exposure     = m_atmosphere.exposure;
    cb.aerial       = m_aerial ? 1.0f : 0.0f;
    cb.lightMode    = (float)m_lightMode;
    cb.legacyMode   = m_legacyImport ? (float)m_legacyProfile : 0.0f;
    cb.sunDiffuse   = m_atmosphere.sunDiffuse;
    cb.skyAmbient   = m_atmosphere.skyAmbient;
    for (int i = 0; i < 3; ++i) cb.groundColour[i] = m_atmosphere.groundColour[i];
    cb.groundBounce = m_atmosphere.groundBounce;
    cb.probePixel[0] = (float)m_probeX;
    cb.probePixel[1] = (float)m_probeY;
    cb.probeOn       = m_probeOn ? 1.0f : 0.0f;
    cb.envOn         = (m_env && m_env->Ready() && m_ambientSpecular) ? 1.0f : 0.0f;
    cb.envMips       = (float)Environment::kMips;
    cb.envReady      = (m_env && m_env->Ready()) ? 1.0f : 0.0f;
    // On the RESOURCE, not on m_rtSrvReady: the descriptor is (re)written
    // further down in this same Record, before any draw, so by the time the
    // GPU reads this flag the SRV exists. Gating on the flag here failed the
    // moment the denoiser started ping-ponging two history textures - a new
    // pointer every frame reset the flag every frame, this line saw it as
    // "not ready" every frame, and the lighting silently fell back to shadow
    // maps that RT mode does not render. Shadows vanished with no error.
    cb.rtReady       = (m_rtVis != nullptr) ? 1.0f : 0.0f;
    cb.shadowSource  = (m_shadowSource == 1) ? 1.0f : 0.0f;
    cb.aoReady       = (m_rtAo != nullptr) ? 1.0f : 0.0f;
    cb.aoOn          = m_aoOn ? 1.0f : 0.0f;
    cb.invScreen[0]  = width  ? 1.0f / (float)width  : 0.0f;
    cb.invScreen[1]  = height ? 1.0f / (float)height : 0.0f;

    // How far the aerial table reaches: the far plane, widened by 1.5 because
    // the far plane bounds VIEW depth and the table is indexed by RADIAL
    // distance, which at the frame corners runs ~1.4x further. A projection we
    // cannot read a far plane from gets 12 km - longer than any stage sightline.
    {
        float farDist = 12000.0f;
        const float m22 = proj[2 * 4 + 2], m32 = proj[3 * 4 + 2];
        if (m22 != 0.0f && m22 != 1.0f)
        {
            const float nearZ = -m32 / m22;
            const float f = m22 * nearZ / (m22 - 1.0f);
            if (f > 0.0f) farDist = f;
        }
        float reach = farDist * 1.5f;
        if (reach < 500.0f)   reach = 500.0f;
        if (reach > 60000.0f) reach = 60000.0f;
        cb.aerialMaxDist = reach;
    }
    cb.physical = m_physical ? 1.0f : 0.0f;
    cb.cloudCover = m_atmosphere.cloudCover;
    cb.fogBeta    = FogBeta(m_atmosphere);
    cb.fogHeight  = m_atmosphere.fogHeight;
    cb.wetness    = m_wetness;
    cb.clusterParams[0] = m_lights.TileW();
    cb.clusterParams[1] = m_lights.TileH();
    cb.clusterParams[2] = Lights::kNear;
    cb.clusterParams[3] = m_lights.SliceScale();
    cb.lightsOn    = (m_lightsReady && m_lights.Active()) ? 1.0f : 0.0f;
    cb.lightsVisOn = (cb.lightsOn > 0.5f && m_rtLights) ? 1.0f : 0.0f;
    if (!Invert4x4(m_lastViewProjJittered, cb.invViewProj)) memset(cb.invViewProj, 0, sizeof(cb.invViewProj));

    // Last frame's timings. The frame is synchronous (see the note at the end
    // of this function), so last frame's queries have certainly resolved.
    // [0] pass start, [1] after the environment, [2] after the aerial fill,
    // [3] end - one timestamp per stage, because a stage recorded before the
    // first timestamp of another silently bills its time to that one (which is
    // how the environment's cost once appeared as "aerial fill").
    const bool timed = !prepass && m_timer && m_timerReadback && m_timerHz;
    if (timed && m_timerPending)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r = { 0, 4 * sizeof(UINT64) };
        if (SUCCEEDED(m_timerReadback->Map(0, &r, reinterpret_cast<void**>(&t))))
        {
            if (t[3] >= t[2] && t[2] >= t[1] && t[1] >= t[0])
            {
                const double k = 1000.0 / double(m_timerHz);
                m_envMs    = double(t[1] - t[0]) * k;
                m_aerialMs = double(t[2] - t[1]) * k;
                m_passMs   = double(t[3] - t[0]) * k;
            }
            D3D12_RANGE none = { 0, 0 };
            m_timerReadback->Unmap(0, &none);
        }
        m_timerPending = false;
    }
    if (timed) list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // The environment for THIS frame's sun, before anything samples it. Main
    // pass only - the prepass writes depth and normals and reads no lighting.
    // The multiple-scattering table first: the environment and the aerial
    // table both integrate through it. A no-op unless the composition changed.
    if (!prepass)
        m_ms.Update(list, m_atmosphere.turbidity, m_atmosphere.groundAlbedo, m_msOn);
    if (!prepass && m_env && m_env->Ready())
        m_env->Record(list, m_atmosphere, &m_ms);

    if (timed) list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

    const bool aerialTable = m_aerial && !m_aerialMarch && m_aerialLut.Ready();
    cb.aerialLut = aerialTable ? 1.0f : 0.0f;

    // Fill the table for THIS camera, once per frame - the main pass is the
    // only consumer, so the prepass does not pay for it. Recorded here, before
    // the scene binds its own heap: the dispatch sets the table's heap and its
    // compute root signature, and everything below re-binds the scene's.
    if (aerialTable && !prepass)
    {
        const float weather[4] = { cb.cloudCover, cb.fogBeta, cb.fogHeight, m_atmosphere.groundAlbedo };
        m_aerialLut.Record(list, m_lastViewProj, cb.sunTowards,
                           cb.sunIntensity, cb.turbidity, cb.aerialMaxDist, &m_ms, weather);
    }

    if (timed) list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);

    // Camera forward, from the view matrix's third column.
    cb.camFwd[0] = view[2]; cb.camFwd[1] = view[6]; cb.camFwd[2] = view[10];

    cb.shadowParams[0] = (m_shadows && m_shadows->Ready() && m_shadowsOn) ? 1.0f : 0.0f;
    cb.shadowParams[1] = 1.0f;
    cb.shadowParams[2] = m_shadowNormalBias;
    cb.shadowParams[3] = m_shadows ? (float)m_shadows->Resolution() : 2048.0f;
    if (m_shadows && m_shadows->Ready())
    {
        for (int c = 0; c < ShadowMaps::kCascades; ++c)
        {
            cb.cascadeSplit[c] = m_shadows->SplitFar(c);
            memcpy(cb.lightViewProj[c], m_shadows->LightViewProj(c), sizeof(float) * 16);
        }
        // One texel's world size, from the first cascade - the normal-offset
        // bias is expressed in texels so it stays correct as cascades change
        // scale, but a single value keeps the shader branch-free.
        cb.shadowParams[1] = m_shadows->TexelWorldSize(0);
    }

    ID3D12Device* dev = nullptr;
    list->GetDevice(IID_PPV_ARGS(&dev));
    ComPtr<ID3D12Resource> cbRes = MakeUploadBuffer(dev, &cb, sizeof(cb));
    if (dev) dev->Release();
    if (!cbRes) return;

    // Clear the probe before the pass that fills it, so the FIRED marker in the
    // last slot distinguishes "this frame reported" from "this is what the last
    // armed frame said".
    if (m_probe && m_probeOn && m_probeZero)
    {
        D3D12_RESOURCE_BARRIER pz = {};
        pz.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        pz.Transition.pResource = m_probe.Get();
        pz.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pz.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        pz.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &pz);
        list->CopyBufferRegion(m_probe.Get(), 0, m_probeZero.Get(), 0, kProbeSlots * 16);
        std::swap(pz.Transition.StateBefore, pz.Transition.StateAfter);
        list->ResourceBarrier(1, &pz);
    }

    // Flush the dynamic pool if it changed since the last frame. Lazy, so an
    // editor operation that adds a hundred props costs one upload rather than a
    // hundred -- and so the upload happens where a device is certainly available.
    if (m_dynDirty)
    {
        ID3D12Device* d = nullptr;
        list->GetDevice(IID_PPV_ARGS(&d));
        if (d) { UploadDynamic(d); d->Release(); }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    // THE DEFERRED LOCAL LIGHTS (PSDeferredLights) while there are local lights: the opaque world leaves its surface
    // in three more targets, the lights are added after it, once per pixel. Not under a debug view (its false colour
    // is not a surface to light).
    // NGG_FORWARD_LIGHTS: the lights shaded in the main pass instead - the A/B for this pass's picture and cost.
    static const bool forwardLights = std::getenv("NGG_FORWARD_LIGHTS") != nullptr;
    const bool deferred = !prepass && m_deferredReady && cb.lightsOn > 0.5f && m_debugMode == 0 && !forwardLights;
    // The prepass writes three targets, normal, motion and plant, adjacent in
    // their heap - so `rtv` is the start of a three-descriptor range there.
    if (deferred)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE targets[3] = { rtv };
        for (int k = 0; k < 2; ++k)
        {
            targets[1 + k] = m_gbufRtvHeap->GetCPUDescriptorHandleForHeapStart();
            targets[1 + k].ptr += (SIZE_T)k * m_prepassRtvStride;
        }
        list->OMSetRenderTargets(3, targets, FALSE, &dsv);
        const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // kind 0: no surface (the sky)
        list->ClearRenderTargetView(targets[1], none, 0, nullptr);
    }
    else
        list->OMSetRenderTargets(prepass ? 3 : 1, &rtv, prepass ? TRUE : FALSE, &dsv);
    // Depth is cleared by whichever pass comes FIRST this frame. After a
    // prepass the main pass must keep it: that depth is what it tests EQUAL
    // against, and what the ray tracer already traced from.
    if (prepass || !m_prepassThisFrame)
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    if (prepass)
    {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE motionRtv = rtv;
        motionRtv.ptr += m_prepassRtvStride;
        list->ClearRenderTargetView(motionRtv, zero, 0, nullptr);   // w = 0: nothing moved
        D3D12_CPU_DESCRIPTOR_HANDLE plantRtv = motionRtv;
        plantRtv.ptr += m_prepassRtvStride;
        list->ClearRenderTargetView(plantRtv, zero, 0, nullptr);    // 0: not a plant
    }

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)width, (LONG)height };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetGraphicsRootSignature(m_rootSig.Get());
    list->SetPipelineState(prepass ? m_psoPrepass.Get() : m_pso.Get());
    list->SetGraphicsRootConstantBufferView(0, cbRes->GetGPUVirtualAddress());
    if (m_probe) list->SetGraphicsRootUnorderedAccessView(2, m_probe->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &m_vbv);
    list->IASetIndexBuffer(&m_ibv);

    if (m_srvHeap)
    {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);

        // Populate the shadow descriptor once. Reserving the slot and binding
        // the table is NOT enough - an unwritten descriptor reads as zero, and
        // a comparison against zero fails for every pixel, which presents as
        // "everything is in shadow" rather than as any kind of error.
        if (!m_shadowSrvReady && m_shadows && m_shadows->Ready())
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_shadowSrvSlot * m_srvStride;
                m_shadows->CreateSrv(d, dest);
                d->Release();
                m_shadowSrvReady = true;
                Log("scene: shadow SRV created in heap slot %u", m_shadowSrvSlot);
            }
        }

        D3D12_GPU_DESCRIPTOR_HANDLE sh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        sh.ptr += (UINT64)m_shadowSrvSlot * m_srvStride;
        list->SetGraphicsRootDescriptorTable(3, sh);
    }

    // The environment map, same shape as the shadow array above - including the
    // deferred CreateSrv, because an unwritten descriptor reads ZERO with no
    // validation error. That failure mode cost this project a day when the
    // shadow SRV was never created and every pixel came back in shadow; here it
    // would simply mean nothing reflects anything, which is even quieter.
    if (m_srvHeap)
    {
        if (m_env && m_env->Ready() && !m_envSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_envSrvSlot * m_srvStride;
                m_env->CreateSrv(d, dest);
                d->Release();
                m_envSrvReady = true;
                Log("scene: environment SRV created in heap slot %u", m_envSrvSlot);
            }
        }

        // Bound whether or not an environment exists: the table must be valid,
        // and gEnvOn is what decides if the sample is used.
        D3D12_GPU_DESCRIPTOR_HANDLE eh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        eh.ptr += (UINT64)(m_envSrvReady ? m_envSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(5, eh);

        // Same shape again for the RT visibility. Rewritten whenever the
        // resource changes (a resize recreates it), and bound to slot 0 when
        // there is none so the table is never invalid.
        if (m_rtVis && !m_rtSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                // The resource's own format: the raw trace is R16F, the denoised
                // history is RGBA16F, and the shader reads .r of either.
                D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = m_rtVis->GetDesc().Format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_rtSrvSlot * m_srvStride;
                d->CreateShaderResourceView(m_rtVis, &sd, dest);
                d->Release();
                m_rtSrvReady = true;
                Log("scene: RT visibility SRV created in heap slot %u", m_rtSrvSlot);
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE rh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        rh.ptr += (UINT64)(m_rtSrvReady ? m_rtSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(6, rh);

        // The AO term, same handling. The pointer ping-pongs between two
        // histories every frame, so the descriptor is rewritten every frame -
        // a write, not an allocation - and the CB flag tests the resource.
        if (m_rtAo && !m_aoSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = m_rtAo->GetDesc().Format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_aoSrvSlot * m_srvStride;
                d->CreateShaderResourceView(m_rtAo, &sd, dest);
                d->Release();
                m_aoSrvReady = true;
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE ah = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        ah.ptr += (UINT64)(m_aoSrvReady ? m_aoSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(7, ah);

        // The local lights' four descriptors, rewritten every frame (four writes): the buffers are fixed, but the
        // ray tracer's fraction ping-pongs between histories, and a pointer is not an identity (5.0r). Null views
        // where there is nothing, so the table is always whole; the CB flags decide what is read.
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE at = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                at.ptr += (SIZE_T)m_lightSrvSlot * m_srvStride;
                if (m_lightsReady)
                    m_lights.WriteSrvs(d, at, m_srvStride);
                else
                    for (int k = 0; k < 3; ++k)
                    {
                        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
                        nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        nd.Buffer.NumElements = 1;
                        nd.Buffer.StructureByteStride = k == 0 ? sizeof(GpuLight) : k == 1 ? 8 : 4;
                        D3D12_CPU_DESCRIPTOR_HANDLE h = at;
                        h.ptr += (SIZE_T)k * m_srvStride;
                        d->CreateShaderResourceView(nullptr, &nd, h);
                    }
                D3D12_SHADER_RESOURCE_VIEW_DESC td = {};
                td.Format = m_rtLights ? m_rtLights->GetDesc().Format : DXGI_FORMAT_R16_FLOAT;
                td.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                td.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                td.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE th = at;
                th.ptr += (SIZE_T)3 * m_srvStride;
                d->CreateShaderResourceView(m_rtLights, &td, th);
                // The deferred pass's surface (t16, t17) and the depth (t18).
                for (int k = 0; k < 3; ++k)
                {
                    ID3D12Resource* r = k < 2 ? m_gbuf[k].Get() : m_depth.Get();
                    D3D12_SHADER_RESOURCE_VIEW_DESC gd = {};
                    gd.Format = k < 2 ? kGbufFormats[k] : DXGI_FORMAT_R32_FLOAT;
                    gd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    gd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    gd.Texture2D.MipLevels = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE gh = at;
                    gh.ptr += (SIZE_T)(4 + k) * m_srvStride;
                    d->CreateShaderResourceView(r, &gd, gh);
                }
                d->Release();
            }
            D3D12_GPU_DESCRIPTOR_HANDLE lh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            lh.ptr += (UINT64)m_lightSrvSlot * m_srvStride;
            list->SetGraphicsRootDescriptorTable(11, lh);
        }

        // The aerial table: two fixed-size volumes, so the descriptors are
        // written once per heap. Bound to slot 0 when absent so the table is
        // never invalid; gAerialLut decides whether it is read.
        if (m_aerialLut.Ready() && !m_aerialSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_aerialSrvSlot * m_srvStride;
                m_aerialLut.CreateSrvs(d, dest, m_srvStride);
                d->Release();
                m_aerialSrvReady = true;
                Log("scene: aerial table SRVs created in heap slots %u-%u",
                    m_aerialSrvSlot, m_aerialSrvSlot + 1);
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE xh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        xh.ptr += (UINT64)(m_aerialSrvReady ? m_aerialSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(8, xh);

        if (m_env && m_env->Irradiance() && !m_irrSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_irrSrvSlot * m_srvStride;
                m_env->CreateIrradianceSrv(d, dest);
                d->Release();
                m_irrSrvReady = true;
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE ih = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        ih.ptr += (UINT64)(m_irrSrvReady ? m_irrSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(9, ih);

        if (m_ms.Ready() && !m_msSrvReady)
        {
            ID3D12Device* d = nullptr;
            list->GetDevice(IID_PPV_ARGS(&d));
            if (d)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dest = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                dest.ptr += (SIZE_T)m_msSrvSlot * m_srvStride;
                m_ms.CreateSrv(d, dest);
                d->Release();
                m_msSrvReady = true;
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE mh = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        mh.ptr += (UINT64)(m_msSrvReady ? m_msSrvSlot : 0) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(10, mh);
    }

    if (m_objects.empty())
    {
        // No object table (the synthetic test scene): one draw, white texture.
        if (m_srvHeap)
            list->SetGraphicsRootDescriptorTable(
                1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        list->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
        m_frameCB = cbRes;
        return;
    }

    // Objects arrive grouped by layer (terrain first, then vegetation), so
    // tracking the last state set is enough to keep PSO changes to one or two
    // per frame without sorting anything here.
    // Named viewProj, not vp: there is already a D3D12_VIEWPORT called vp in
    // this scope.
    float viewProj[16], planes[6][4];
    Mul4x4(view, proj, viewProj);
    ExtractPlanes(viewProj, planes);

    ID3D12PipelineState* bound = prepass ? m_psoPrepass.Get() : m_pso.Get();
    int  xformState = -1;
    int  geomState = 0;              // the static buffers are bound just above
    uint32_t drawn = 0, culled = 0;
    // The glass (kMaterialBlend) waits for everything opaque: it is drawn over it, after this loop. It has no part in
    // the prepass - no depth, no normal: what the ray tracer and TAA see through a window is what is behind it.
    // In the prepass it holds the glass that moved, for its mark in the motion target (PSPrepass).
    std::vector<std::pair<float, const DrawItem*>> blended;
    auto movedOf = [](const DrawItem& o) {
        return (!o.identity || !o.prevIdentity) && Travel(o.bmin0, o.bmax0, o.xform, o.prevXform) > kRtMovedMin;
    };
    auto draw = [&](const DrawItem& o, ID3D12PipelineState* want, bool glassMark = false) {
        if (want != bound)
        {
            list->SetPipelineState(want);
            bound = want;
        }
        // Descriptors are a contiguous pair per material, so the table root
        // points straight at 2*materialId.
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)o.materialId * kMaterialSrvCount * m_srvStride;
        list->SetGraphicsRootDescriptorTable(1, h);
        // Moved since last frame: compared, not flagged by SetObjectTransform,
        // because a transform set to the value it already had is not motion,
        // and a client may well set every keyed object every tick. And not
        // below kRtMovedMin either, for the same reason as the ray tracing's:
        // the idling car "moved" a hundredth of a millimetre a tick, and TAA
        // dropped the history of a band round its outline every frame - its
        // edges and its wheels flickered on a car standing still.
        const bool moved = movedOf(o);
        const uint32_t objConsts[3] = { o.layerId, o.matFlags, moved ? (glassMark ? 2u : 1u) : 0u };
        list->SetGraphicsRoot32BitConstants(4, 3, objConsts, 0);
        if (moved) list->SetGraphicsRoot32BitConstants(4, 16, o.prevXform, 32);
        {
            const MaterialSlot& m = m_materials[o.materialId];
            const float pbr[12] = {
                m.baseColourFactor[0], m.baseColourFactor[1],
                m.baseColourFactor[2], m.baseColourFactor[3],
                m.roughness, m.metallic, m.normalScale, m.occlusionStrength,
                m.emissiveStrength, m.alphaCutoff, 0.0f, 0.0f,
            };
            list->SetGraphicsRoot32BitConstants(4, 12, pbr, 20);
        }
        BindObjectXform(list, o.xform, o.identity, xformState);
        BindGeometry(list, o.meshId != 0, geomState,
                     o.meshId ? m_dynVbv : m_vbv, o.meshId ? m_dynIbv : m_ibv);
        list->DrawIndexedInstanced(o.indexCount, 1, o.firstIndex, o.baseVertex, 0);
    };
    // THE OPAQUE WORLD FRONT TO BACK - the solid first, the alpha-tested (foliage) after, each nearest first, by the
    // distance to the nearest point of the box (0 inside it: the terrain the camera stands on). What is drawn first
    // fills the depth, and what lies behind it fails the test BEFORE its pixel shader runs: without a prepass (no ray
    // tracing) that is the main pass's overdraw, with one the prepass's; the main pass after a prepass shades each
    // pixel once whatever the order. The stage's file order was arbitrary.
    m_drawOrder.clear();
    for (const DrawItem& o : m_objects)
    {
        if (!o.visible) continue;              // editor hide; see SetObjectVisible
        if (o.materialId >= m_materials.size())
            continue;
        if (m_cull && AabbOutside(planes, o.bmin, o.bmax)) { ++culled; continue; }
        ++drawn;

        const uint32_t matFlags = m_materials[o.materialId].flags;
        if (matFlags & kMaterialBlend)
        {
            if (prepass ? m_psoPrepassGlass[0] && movedOf(o) : m_psoBlend[0] != nullptr)
            {
                // Far to near by the box's centre: a pane over a pane is drawn after it.
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float c = 0.5f * (o.bmin[k] + o.bmax[k]) - camPos[k];
                    d2 += c * c;
                }
                blended.emplace_back(d2, &o);
            }
            continue;
        }
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float c = (std::max)((std::max)(o.bmin[k] - camPos[k], camPos[k] - o.bmax[k]), 0.0f);
            d2 += c * c;
        }
        m_drawOrder.push_back({ (matFlags & kMaterialAlphaTest) != 0, d2, &o });
    }
    std::sort(m_drawOrder.begin(), m_drawOrder.end(), [](const DrawOrder& a, const DrawOrder& b) {
        return a.alphaTest != b.alphaTest ? b.alphaTest : a.d2 < b.d2;
    });
    for (const DrawOrder& d : m_drawOrder)
    {
        const DrawItem& o = *d.item;
        const uint32_t matFlags = m_materials[o.materialId].flags;
        const bool wantAlphaTest = d.alphaTest;
        ID3D12PipelineState* want = prepass ? (wantAlphaTest ? m_psoPrepassAlpha.Get() : m_psoPrepass.Get())
                                            : (wantAlphaTest ? m_psoAlphaTest.Get() : m_pso.Get());
        if (!(matFlags & kMaterialTwoSided))
            if (ID3D12PipelineState* c = m_psoCulled[prepass ? 1 : 0][wantAlphaTest ? 1 : 0][Mirrors(o.xform) ? 1 : 0].Get())
                want = c;
        if (deferred)
        {
            want = wantAlphaTest ? m_psoAlphaTestG.Get() : m_psoG.Get();
            if (!(matFlags & kMaterialTwoSided))
                want = m_psoCulledG[wantAlphaTest ? 1 : 0][Mirrors(o.xform) ? 1 : 0].Get();
        }
        // Foliage over the prepass's depth: tested early, not written (m_psoAlphaKept).
        if (!prepass && wantAlphaTest && m_prepassThisFrame)
        {
            const int cull = (matFlags & kMaterialTwoSided) ? 0 : Mirrors(o.xform) ? 2 : 1;
            if (ID3D12PipelineState* k = m_psoAlphaKept[deferred ? 1 : 0][cull].Get())
                want = k;
        }
        draw(o, want);
    }

    // The deferred local lights, over the opaque world and under the glass: the surface's targets and the depth to
    // shader resources, the depth bound read-only (the glass still tests it), one full-screen triangle.
    D3D12_RESOURCE_BARRIER deferredBarriers[3] = {};
    if (deferred)
    {
        for (int k = 0; k < 3; ++k)
        {
            D3D12_RESOURCE_BARRIER& b = deferredBarriers[k];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = k < 2 ? m_gbuf[k].Get() : m_depth.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = k < 2 ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b.Transition.StateAfter = k < 2 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        }
        list->ResourceBarrier(3, deferredBarriers);
        D3D12_CPU_DESCRIPTOR_HANDLE roDsv = dsv;
        roDsv.ptr += m_dsvStride;
        list->OMSetRenderTargets(1, &rtv, FALSE, &roDsv);
        list->SetPipelineState(m_psoDeferred.Get());
        bound = m_psoDeferred.Get();
        list->DrawInstanced(3, 1, 0, 0);
    }

    // The glass, over the opaque world (and the sky, drawn before this pass), far to near.
    std::sort(blended.begin(), blended.end(),
              [](const std::pair<float, const DrawItem*>& a, const std::pair<float, const DrawItem*>& b) { return a.first > b.first; });
    for (const auto& [d2, o] : blended)
    {
        (void)d2;
        const uint32_t matFlags = m_materials[o->materialId].flags;
        const int cull = (matFlags & kMaterialTwoSided) ? 0 : Mirrors(o->xform) ? 2 : 1;
        if (prepass)
            draw(*o, m_psoPrepassGlass[cull] ? m_psoPrepassGlass[cull].Get() : m_psoPrepassGlass[0].Get(), true);
        else
            draw(*o, m_psoBlend[cull] ? m_psoBlend[cull].Get() : m_psoBlend[0].Get());
    }
    if (deferred)
    {
        for (D3D12_RESOURCE_BARRIER& b : deferredBarriers)
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(3, deferredBarriers);
    }

    if (prepass)
    {
        // The prepass is done; the main pass must not clear, and the counter
        // below belongs to the frame, not to each pass of it.
        //
        // ITS OWN keep-alive slot. Parking it in m_frameCB let the main pass
        // overwrite it before the GPU had read it, which is a page fault under
        // a root CBV - the device hung, and every barrier error that followed
        // was fallout from that one release.
        m_prepassCB = cbRes;
        return;
    }
    m_prepassThisFrame = false;

    // This frame's transforms are next frame's "where it was". After the main
    // pass, so the prepass and the main pass of one frame agree on what moved;
    // over every object, culled ones too, or one that comes back into view
    // would report the motion of all the frames it was off screen.
    // An object that STOPS moving changes nothing a client would report, yet
    // its ray-tracing instance still says "moved": the revision bump makes the
    // next frame re-describe it (an instances-only TLAS update).
    for (DrawItem& o : m_objects)
    {
        if (o.identity && o.prevIdentity) continue;
        if (memcmp(o.prevXform, o.xform, sizeof(o.xform)) == 0) continue;
        memcpy(o.prevXform, o.xform, sizeof(o.xform));
        o.prevIdentity = o.identity;
        ++m_geomRevision;
    }

    if ((m_recordCount % 300) == 0)
        Log("scene: %u objects drawn, %u culled", drawn, culled);

    // Stage the probe for readback while the command list is still open.
    if (m_probe && m_probeOn)
    {
        D3D12_RESOURCE_BARRIER toCopy = {};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = m_probe.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &toCopy);
        list->CopyResource(m_probeReadback.Get(), m_probe.Get());
        D3D12_RESOURCE_BARRIER back = toCopy;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &back);
    }

    if (timed)
    {
        list->EndQuery(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
        list->ResolveQueryData(m_timer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4,
                               m_timerReadback.Get(), 0);
        m_timerPending = true;
    }

    // The constant buffer must outlive GPU execution. The host waits for the
    // GPU before signalling the frame complete (protocol v1 is synchronous), so
    // holding it until then is enough - but this is exactly the kind of thing
    // that breaks the moment frames overlap, so it is called out here.
    m_frameCB = cbRes;
}

// Decode every DDS in the NGGT chunk, upload them all on one command list, and
// build the SRV heap. A white 1x1 fallback is appended last so a material whose
// texture was missing from the archive still draws.
bool Scene::UploadTextures(ID3D12Device* dev, ID3D12CommandQueue* queue,
                           const uint8_t* chunk, size_t bytes)
{
    struct Entry { std::string name; const uint8_t* data; uint32_t size; };
    std::vector<Entry> entries;

    if (chunk && bytes >= 4)
    {
        uint32_t count = *reinterpret_cast<const uint32_t*>(chunk);
        size_t off = 4;
        for (uint32_t i = 0; i < count && off + 8 <= bytes; ++i)
        {
            uint32_t nameLen = *reinterpret_cast<const uint32_t*>(chunk + off); off += 4;
            if (off + nameLen + 4 > bytes) break;
            std::string name(reinterpret_cast<const char*>(chunk + off), nameLen);
            off += nameLen;
            uint32_t dataSize = *reinterpret_cast<const uint32_t*>(chunk + off); off += 4;
            if (off + dataSize > bytes) break;
            entries.push_back({ name, chunk + off, dataSize });
            off += dataSize;
            off += (4 - (dataSize % 4)) % 4;
        }
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                      nullptr, IID_PPV_ARGS(&list))))
    {
        Log("scene: could not create an upload command list");
        return false;
    }

    // ---- mip chains for textures that shipped without one (mipgen.h) ----
    //
    // Measured first, per texture: a DDS that has mips is left exactly as it
    // is, so this stands down by itself the moment the converter generates
    // them. What it needs from the materials: whether the texture is a colour
    // (sRGB) or data (linear) slot, and whether any material alpha-tests it as
    // its base colour - those keep their coverage per level.
    std::vector<std::vector<uint8_t>> generated(entries.size());
    if (m_mipFallback > 0)
    {
        std::vector<float> cutoff(entries.size(), -1.0f);    // < 0: not alpha-tested
        for (const MaterialSlot& m : m_materials)
            if ((m.flags & kMaterialAlphaTest) && m.tex[0] < entries.size())
            {
                float& c = cutoff[m.tex[0]];
                c = (c < 0.0f) ? m.alphaCutoff : (std::min)(c, m.alphaCutoff);
            }

        LARGE_INTEGER t0, t1, hz;
        QueryPerformanceCounter(&t0);
        QueryPerformanceFrequency(&hz);

        std::vector<MipGenResult> results(entries.size());
        std::atomic<size_t> next(0);
        auto worker = [&]() {
            for (size_t i; (i = next.fetch_add(1)) < entries.size(); )
            {
                const uint32_t index = (uint32_t)(m_textures.size() + i);
                MipGenOptions o;
                o.srgb = !(index < m_linearTextures.size() && m_linearTextures[index]);
                o.alphaTested = cutoff[i] >= 0.0f;
                if (o.alphaTested) o.alphaCutoff = cutoff[i];
                o.preserveCoverage = (m_mipFallback >= 2);
                if (!GenerateDdsMips(entries[i].data, entries[i].size, o, generated[i], &results[i]))
                    generated[i].clear();
            }
        };
        const unsigned n = (std::max)(1u, (std::min)(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned k = 1; k < n; ++k) pool.emplace_back(worker);
        worker();
        for (std::thread& t : pool) t.join();

        QueryPerformanceCounter(&t1);
        size_t made = 0, foliage = 0, before = 0, after = 0;
        double err = 0.0;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (generated[i].empty()) continue;
            ++made;
            if (cutoff[i] >= 0.0f) ++foliage;
            before += entries[i].size;
            after  += generated[i].size();
            err    += results[i].meanAbsError;
        }
        // The coverage mechanism, measured on the textures it exists for: the
        // fraction of texels passing the alpha test at mip 0 against level 4
        // box-filtered and as written.
        for (size_t i = 0; i < entries.size(); ++i)
            if (!generated[i].empty() && results[i].coverage0 >= 0.0f)
                Log("scene:   alpha coverage %-28s mip0 %5.1f%%  L%u box %5.1f%%  written %5.1f%%",
                    entries[i].name.c_str(), 100.0f * results[i].coverage0, results[i].coverageLevel,
                    100.0f * results[i].coverageBox, 100.0f * results[i].coverageKept);
        if (made)
            Log("scene: generated mip chains for %zu texture(s) (%zu alpha-tested, coverage %s) "
                "in %.0f ms on %u threads: %.1f -> %.1f MB, re-encode error %.2f/255 mean",
                made, foliage, m_mipFallback >= 2 ? "preserved" : "NOT preserved (box)",
                1000.0 * double(t1.QuadPart - t0.QuadPart) / double(hz.QuadPart), n,
                before / 1e6, after / 1e6, err / made);
    }

    std::vector<ComPtr<ID3D12Resource>> uploads;
    for (size_t ei = 0; ei < entries.size(); ++ei)
    {
        Entry e = entries[ei];
        if (!generated[ei].empty())
        {
            e.data = generated[ei].data();
            e.size = (uint32_t)generated[ei].size();
        }
        ComPtr<ID3D12Resource> up;
        // Slot decides the transfer function, and only the material knows the
        // slot - see DdsFormat. A normal map read through the sRGB decode is
        // not obviously broken, it is subtly wrong everywhere.
        const uint32_t index = (uint32_t)m_textures.size();
        const bool linear = index < m_linearTextures.size() && m_linearTextures[index];
        ComPtr<ID3D12Resource> tex =
            CreateTextureFromDDS(dev, list.Get(), e.data, e.size, up, e.name.c_str(), linear);
        m_textures.push_back(tex);      // may be null; slot is kept so ids line up
        m_textureNames.push_back(e.name);
        uploads.push_back(up);
    }

    ComPtr<ID3D12Resource> whiteUp;
    ComPtr<ID3D12Resource> white = CreateWhiteTexture(dev, list.Get(), whiteUp);
    m_textures.push_back(white);
    uploads.push_back(whiteUp);

    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    // Block until the copies are done: the upload buffers die with this scope,
    // and the textures must be resident before the first frame draws.
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 30000);
        CloseHandle(evt);
    }

    // The white fallback is the LAST texture uploaded here, and its slot is
    // remembered rather than recomputed: the dynamic pool appends after it, so
    // "size() - 1" stops being the white one the moment a texture is added.
    m_whiteSlot = (uint32_t)(m_textures.size() - 1);

    if (!RebuildSrvHeap(dev)) return false;

    // How many textures shipped with NO MIP CHAIN. Not cosmetic: a single-level
    // texture cannot be minified, so it aliases and shimmers at distance and
    // anisotropic filtering has nothing to filter. TAA then spends its budget
    // fighting a problem the asset created. Counted rather than assumed because
    // it is entirely a property of the source: track-42 (original RBR content)
    // has 0 of 74, track-108 (community) has 76 of 88 including its 2048 atlas
    // and both 3200x800 backdrops. The cure is mip generation in the converter.
    {
        size_t noMips = 0;
        for (const Entry& e : entries)
        {
            if (e.size < 4 + 32 || memcmp(e.data, "DDS ", 4) != 0) continue;
            const uint32_t mipCount = *reinterpret_cast<const uint32_t*>(e.data + 4 + 24);
            if (mipCount <= 1) ++noMips;
        }
        if (noMips)
            Log("scene: %zu of %zu textures shipped with NO mip chain - %s; "
                "the converter should generate them",
                noMips, entries.size(),
                m_mipFallback > 0 ? "the renderer generated them (see above)"
                                  : "fallback OFF, they will alias at distance");
    }

    Log("scene: %zu texture(s) uploaded (+1 white fallback)", entries.size());
    return true;
}

// Descriptors for every material pair, plus the shadow array in the slot after
// them. Rebuilt from scratch whenever the material list grows, which is cheap --
// a descriptor is a write, not an allocation -- and far simpler than reserving
// headroom and failing loudly on the material that overruns it.
bool Scene::RebuildSrvHeap(ID3D12Device* dev)
{
    if (!dev || m_textures.empty()) return false;

    const size_t pairs = m_materials.empty() ? 1 : m_materials.size();
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // +1: the shadow cascade array shares this heap, in the slot after the last
    // material pair. Only one CBV/SRV/UAV heap can be bound at a time, so it
    // cannot live in a heap of its own.
    // +2: the shadow cascade array and the label font both share this heap,
    // because only one CBV/SRV/UAV heap can be bound at a time.
    hd.NumDescriptors = (UINT)(pairs * kMaterialSrvCount + 16);
    m_shadowSrvSlot = (UINT)(pairs * kMaterialSrvCount);
    m_fontSrvSlot = (UINT)(pairs * kMaterialSrvCount + 1);
    m_envSrvSlot  = (UINT)(pairs * kMaterialSrvCount + 2);
    m_rtSrvSlot   = (UINT)(pairs * kMaterialSrvCount + 3);
    m_aoSrvSlot   = (UINT)(pairs * kMaterialSrvCount + 4);
    m_aerialSrvSlot = (UINT)(pairs * kMaterialSrvCount + 5);   // and +6
    m_irrSrvSlot    = (UINT)(pairs * kMaterialSrvCount + 7);
    m_msSrvSlot     = (UINT)(pairs * kMaterialSrvCount + 8);
    m_lightSrvSlot  = (UINT)(pairs * kMaterialSrvCount + 9);    // and +10 .. +15 (lights.h, the deferred pass)
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap))))
    {
        Log("scene: SRV heap creation failed");
        return false;
    }
    m_srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The old heap is gone, so the shadow descriptor written into it is too.
    // Leaving this set would leave the shadow SRV reading zero, which presents
    // as "everything is in shadow" rather than as any kind of error.
    m_shadowSrvReady = false;
    m_envSrvReady = false;
    m_rtSrvReady = false;
    m_aoSrvReady = false;
    m_aerialSrvReady = false;
    m_irrSrvReady = false;
    m_msSrvReady = false;

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    auto describe = [&](uint32_t texIndex) {
        // Anything missing or undecodable borrows the white descriptor, so the
        // heap has no holes and a bad texture cannot crash the draw loop.
        ID3D12Resource* r = (texIndex < m_textures.size() && m_textures[texIndex])
                          ? m_textures[texIndex].Get() : m_textures[m_whiteSlot].Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = r->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = r->GetDesc().MipLevels;
        dev->CreateShaderResourceView(r, &sd, h);
        h.ptr += m_srvStride;
    };

    if (m_materials.empty())
    {
        for (int k = 0; k < kMaterialSrvCount; ++k) describe(kNoTexture);
    }
    else
    {
        for (const MaterialSlot& m : m_materials)
            for (int k = 0; k < kMaterialSrvCount; ++k) describe(m.tex[k]);
    }

    // The font descriptor is rewritten here too. The heap it lived in is gone
    // whenever this runs, and a stale slot reads as zero -- which for a glyph
    // alpha means every label silently draws nothing.
    if (m_font)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE fh = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        fh.ptr += (SIZE_T)m_fontSrvSlot * m_srvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = m_font->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(m_font.Get(), &sd, fh);
    }
    return true;
}

} // namespace core
} // namespace ngg
