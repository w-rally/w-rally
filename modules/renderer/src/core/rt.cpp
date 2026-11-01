#include "rt.h"
#include "log.h"

#include <string>
#include <algorithm>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

const char* kRtCommonSrc = R"(
RaytracingAccelerationStructure gScene        : register(t0);
Texture2D<float>                gDepth        : register(t1);
Texture2D<float4>               gNormal       : register(t6);   // prepass, SNORM8
// For the any-hit test only. Raw views over the SAME buffers the rasteriser
// draws from, so a leaf is cut out by exactly the pixels that were drawn.
ByteAddressBuffer               gVerts        : register(t2);
ByteAddressBuffer               gAlphaIndices : register(t3);
StructuredBuffer<uint>          gTriMaterial  : register(t4);
// The editor's dynamic pool, for instances of the meshes it added.
ByteAddressBuffer               gDynVerts     : register(t7);
// Per TLAS instance (indexed by InstanceID): where its alpha triangles start
// in gAlphaIndices/gTriMaterial, what to add to its indices, which vertex
// buffer they index, and its material when the mesh's triangles do not say.
// moved: the instance's transform changed since last frame (see Occluded);
// was0..was3 then its LAST frame's object-to-world rows (row vectors), so a
// ray's hit point can be put where it was.
struct Inst { uint triBase; uint vertexBase; uint vb; uint material; uint moved;
              float3 was0; float3 was1; float3 was2; float3 was3; };
StructuredBuffer<Inst>          gInst         : register(t8);
// Foliage: the plant each alpha triangle belongs to (~0 = none), and the
// prepass's per-pixel plant (+ 1, 0 = none) - scene.cpp FindPlants.
StructuredBuffer<uint>          gTriPlant     : register(t9);
Texture2D<uint>                 gPlantId      : register(t10);
struct Mat { uint tex; float cutoff; };
StructuredBuffer<Mat>           gMaterials    : register(t5);
Texture2D                       gTextures[]   : register(t0, space1);
SamplerState                    gSamp         : register(s0);
RWTexture2D<float>              gOut          : register(u0);
RWStructuredBuffer<uint>        gCounters     : register(u1);

cbuffer K : register(b0)
{
    row_major float4x4 gInvViewProj;
    float3 gSunTowards;
    float  gTMax;
    uint2  gSize;
    float  gBias;
    uint   gStride;         // bytes per vertex; uv0 at +24, colour at +40
    float3 gCamPos;
    float  gSunAngle;       // the sun's angular RADIUS in radians; 0 = a point
    uint   gFrame;          // decorrelates the samples frame to frame
    float  gAoRadius;       // metres; occluders beyond this do not count
    uint   gAoRays;         // rays per pixel per frame
    uint   gPad2;
    float3 gCamFwd;         // the local lights' clusters: their depth axis (lights.h)
    float  gLightsOn;
    float4 gClusterParams;  // tile w, h (px), slices' near plane, their log scale
};

// A cheap, good integer hash (PCG-style). White noise per pixel per frame;
// the denoiser does the averaging.
uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float2 Rand2(uint2 pixel, uint frame, uint salt)
{
    uint h = Hash(pixel.x * 1973u + pixel.y * 9277u + frame * 26699u + salt * 7919u);
    return float2((h & 0xFFFFu) / 65535.0, (h >> 16) / 65535.0);
}

// STRATIFIED over the screen, as the AO's samples are (AoSample): every 4x4
// tile of pixels takes each of 16 strata once, with a white jitter inside its
// stratum, the whole pattern shifted every frame. White noise let a 5x5
// window draw its samples unevenly - one lamp of three for twelve of its
// pixels and another for five, half the sun's disc for most of them - and
// that luck was the blotches the denoiser blurred but kept: under the
// Garage's car, its floor. Stratified, a window's mean has little luck left.
// StratU: one number (which lamp, Pick), its stratum the tile's (x + 2y,
// 2x + y) cell in 16ths - any 2x2 block of it spans the four quarters.
float StratU(uint2 px, uint frame)
{
    const uint k = ((px.x + 2u * px.y) & 3u) + 4u * ((2u * px.x + px.y) & 3u);
    return frac((float(k) + Rand2(px, frame, 101u).x) / 16.0 + float(frame) * 0.6180339887);
}
// StratDisc: a point on a light's disc (the square, mapped by the caller); a
// second bijection of the tile, (x + 3y, 3x + 2y), so which lamp a pixel
// takes says nothing of where on it the ray goes.
float2 StratDisc(uint2 px, uint frame)
{
    const uint2 cell = uint2((px.x + 3u * px.y) & 3u, (3u * px.x + 2u * px.y) & 3u);
    const float2 j = Rand2(px, frame, 7u);
    return frac((float2(cell) + j) * 0.25 + frac(float(frame) * float2(0.7548776662, 0.5698402910)));
}

// Surface position from depth and the frame's JITTERED matrix, and the
// prepass normal. Shared by every ray type, because a ray origin that differs
// between them by half a pixel would be a different bias problem per feature.
// `plant`: the plant the pixel is a card of, + 1; 0 for anything else. Its
// normal is then the card's own plane (PSPrepass), w = 0.5.
bool SurfaceAt(uint2 px, out float3 p, out float3 N, out uint plant)
{
    plant = 0u;
    float depth = gDepth.Load(int3(px, 0));
    if (depth <= 0.0) return false;                      // reverse-Z far plane: sky
    float2 uv  = (float2(px) + 0.5) / float2(gSize);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wp  = mul(float4(ndc, depth, 1.0), gInvViewProj);
    p = wp.xyz / wp.w;
    const float4 n4 = gNormal.Load(int3(px, 0));
    N = n4.xyz;
    plant = (n4.w > 0.25 && n4.w < 0.75) ? gPlantId.Load(int3(px, 0)) : 0u;
    if (dot(N, N) < 0.25) N = float3(0, 1, 0);
    N = normalize(N);
    return true;
}

// A basis around a direction, for scattering rays about it.
void Basis(float3 d, out float3 T, out float3 B)
{
    float3 up = abs(d.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, d));
    B = cross(d, T);
}

// THE ANY-HIT TEST, shared. Opaque geometry (BLAS geometry 0) commits on its
// own; alpha-tested geometry (geometry 1) surfaces as a candidate and is
// accepted or refused by the rasteriser's own test - texture alpha times
// vertex alpha against the material's cutoff - so a leaf blocks exactly the
// pixels it was drawn with. Returns true if anything blocks the ray, and in
// `mover` - if what blocked it is an object that moved this frame - how far
// the point the ray hit moved since last frame and how far along the ray it
// is, metres; x < 0 when it did not move (Allow).
//
// FROM A PLANT (`fromPlant` = its index + 1), that plant's own cards do not
// count. A tree here is 2-3 crossed flat cards, and a card's ray towards the
// sun crosses the other cards of the same tree, so visibility flipped card by
// card: one lit, the one crossing it in shadow - a split no real canopy has.
// The cascades never showed it because the foliage caster bias (400) is
// deeper than a tree. Other plants, and everything opaque, still shadow it -
// EXACTLY its own plant, by id: a first version skipped every plant whose box
// held the ray's start, and grass under a tree is inside the tree's box, so it
// lost the tree's shadow and glowed. What shading the plant's own volume gets
// comes from the PIXEL shader (FoliageSelfShadow), the same on every card.
// How far from a legacy card its rays ignore other cards, metres: past a grass tuft (~0.5 m) and the near cards of a
// tree, short of the canopy over the grass.
static const float kLegacyCardSkip = 1.5;
bool Occluded(RayDesc ray, bool count, uint fromPlant, out float2 mover)
{
    mover = float2(-1.0, 0.0);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // From a legacy card (~0, scene.cpp PSPrepass): the cards within kLegacyCardSkip of the start do not count -
    // the card's own tuft or tree, whose crossing cards split it card by card - while cards farther off (a
    // canopy over the grass) still shadow it, as everything opaque does.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (fromPlant == 0xFFFFFFFFu && q.CandidateTriangleRayT() < kLegacyCardSkip) continue;

        // Which mesh, and where its alpha triangles live: per INSTANCE now,
        // because the world, a moved object and an editor-added mesh are
        // separate BLASes sharing one index buffer and one material table.
        const Inst  inst = gInst[q.CandidateInstanceID()];
        const uint  tri  = inst.triBase + q.CandidatePrimitiveIndex();
        if (fromPlant != 0u && gTriPlant[tri] + 1u == fromPlant)
        {
            if (count) InterlockedAdd(gCounters[3], 1);
            continue;
        }
        const uint3 idx  = gAlphaIndices.Load3(tri * 12) + inst.vertexBase;
        const float2 b   = q.CandidateTriangleBarycentrics();
        const float3 w   = float3(1.0 - b.x - b.y, b.x, b.y);

        // What a plant casts (scene.cpp Plant::shadow, in its vertices' uv1.y above kPlantShadowPack; a plant's
        // vertex has a volume normal, shorter than 0.55): grass nothing - its sun and AO shadows are its cards'
        // outlines - and a bush half its hits, which the denoise turns into a lighter, softer shadow.
        {
            const uint at = idx.x * gStride;
            const float3 pn = asfloat(inst.vb == 0 ? gVerts.Load3(at + 12) : gDynVerts.Load3(at + 12));
            if (dot(pn, pn) < 0.3025)
            {
                const float packed = max(asfloat(inst.vb == 0 ? gVerts.Load(at + 36) : gDynVerts.Load(at + 36)), 0.0);
                const float mode = floor(packed / 1000.0);
                if (mode > 0.5 && mode < 1.5) continue;
                if (mode > 1.5 && frac(sin(dot(ray.Origin + ray.Direction * 7.0 + float(tri) * 0.001,
                                                float3(12.9898, 78.233, 37.719))) * 43758.5453) < 0.5)
                    continue;
            }
        }

        float2 uv0, uv1, uv2;
        float  a0, a1, a2;
        if (inst.vb == 0)
        {
            uv0 = asfloat(gVerts.Load2(idx.x * gStride + 24));
            uv1 = asfloat(gVerts.Load2(idx.y * gStride + 24));
            uv2 = asfloat(gVerts.Load2(idx.z * gStride + 24));
            a0 = float(gVerts.Load(idx.x * gStride + 40) >> 24);
            a1 = float(gVerts.Load(idx.y * gStride + 40) >> 24);
            a2 = float(gVerts.Load(idx.z * gStride + 40) >> 24);
        }
        else
        {
            uv0 = asfloat(gDynVerts.Load2(idx.x * gStride + 24));
            uv1 = asfloat(gDynVerts.Load2(idx.y * gStride + 24));
            uv2 = asfloat(gDynVerts.Load2(idx.z * gStride + 24));
            a0 = float(gDynVerts.Load(idx.x * gStride + 40) >> 24);
            a1 = float(gDynVerts.Load(idx.y * gStride + 40) >> 24);
            a2 = float(gDynVerts.Load(idx.z * gStride + 40) >> 24);
        }
        const float2 tuv = uv0 * w.x + uv1 * w.y + uv2 * w.z;
        const float  va  = (a0 * w.x + a1 * w.y + a2 * w.z) / 255.0;

        const Mat m = gMaterials[inst.material != 0xFFFFFFFFu ? inst.material : gTriMaterial[tri]];
        const float ta = gTextures[NonUniformResourceIndex(m.tex)].SampleLevel(gSamp, tuv, 2.0).a;
        if (ta * va >= m.cutoff)
        {
            q.CommitNonOpaqueTriangleHit();
            if (count) InterlockedAdd(gCounters[2], 1);
        }
        else if (count) InterlockedAdd(gCounters[3], 1);
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    const Inst hit = gInst[q.CommittedInstanceID()];
    if (hit.moved != 0)
    {
        const float  t   = q.CommittedRayT();
        const float3 o   = q.CommittedObjectRayOrigin() + q.CommittedObjectRayDirection() * t;
        const float3 was = o.x * hit.was0 + o.y * hit.was1 + o.z * hit.was2 + hit.was3;
        mover = float2(length(ray.Origin + ray.Direction * t - was), t);
    }
    return true;
}

// HOW MUCH HISTORY a pixel may keep when its ray hit a moved object (`mover`,
// from Occluded), as a history weight; -1 when it hit none. Its history holds
// the shadow where it was: with weight b the shadow on the ground lags its
// occluder by b / (1 - b) frames of the occluder's travel. What that costs is
// the lag against how SOFT the shadow is there - its penumbra, the hit's
// distance times the light's angular size (`penumbra`, per metre): a lag well
// inside the penumbra moves the visibility by little, while one frame's own
// coin tosses are off by far more. So the weight is whatever keeps the lag
// within kLagShare of the penumbra. A car driving lags a contact shadow by its
// whole width in one frame: the weight goes to nothing, and the denoiser keeps
// gBlendMoved, as it always did - no trail. A car on the Garage's turntable
// moves its sill some 3-7 mm a frame under a soft spotlight's shadow and keeps
// most of it: at a quarter of history the floor around it boiled every frame
// it turned (the lights' frame-to-frame noise 6x the standing car's). 0.3:
// 0.15 left half as much noise again; full history (0.85) a visible lag line
// along the sill.
static const float kLagShare = 0.3;
float Allow(float2 mover, float penumbra)
{
    if (mover.x < 0.0) return -1.0;
    const float w = kLagShare * mover.y * penumbra;
    return w / (w + mover.x + 1e-6);
}

// The raw result, and whether a ray of it hit a moved object: R16_FLOAT,
// negative for "touched" - the value then -(1 + v + 2 k), k the history weight
// it allows (Allow) in sevenths, rounded down. The denoiser decodes it.
float EncodeRaw(float v, float allow)
{
    return allow < 0.0 ? v : -(1.0 + v + 2.0 * floor(saturate(allow) * 7.0));
}
)";

// ---- sun shadow: one ray towards a random point on the sun's disc ---------
const char* kTraceMainSrc = R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
    if (!SurfaceAt(tid.xy, p, N, plant)) { gOut[tid.xy] = 1.0; return; }

    // Normal-offset origin, the normal flipped towards the sun so a two-sided
    // leaf card is pushed to whichever side the ray leaves from. The offset
    // grows with distance because depth precision does.
    if (dot(N, gSunTowards) < 0.0) N = -N;
    float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);

    // SOFT SHADOWS: the sun is a DISC. A random point on it per pixel per
    // frame; the denoiser turns the binary answers into the penumbra. Uniform
    // over the disc (sqrt on the radius), or the centre is over-sampled.
    float3 dir = gSunTowards;
    if (gSunAngle > 0.0)
    {
        float3 T, B; Basis(dir, T, B);
        float2 r  = StratDisc(tid.xy, gFrame);
        float rad = sqrt(r.x) * tan(gSunAngle);
        float ang = r.y * 6.28318530;
        dir = normalize(dir + T * (rad * cos(ang)) + B * (rad * sin(ang)));
    }

    RayDesc ray;
    ray.Origin    = p + N * offset;
    ray.Direction = dir;
    ray.TMin      = 0.0;
    ray.TMax      = gTMax;

    float2 mover;
    bool hit = Occluded(ray, true, plant, mover);
    // The sun's disc: the penumbra per metre of the hit's distance.
    gOut[tid.xy] = EncodeRaw(hit ? 0.0 : 1.0, Allow(mover, 2.0 * tan(gSunAngle)));
    InterlockedAdd(gCounters[0], 1);
    if (hit) InterlockedAdd(gCounters[1], 1);
}
)";

// ---- ambient occlusion: cosine-weighted rays over the hemisphere ----------
const char* kAoMainSrc = R"(
// STRATIFIED over the screen. A 4x4 tile of pixels takes the 16 cells of a
// 4x4 grid on the sample square, one each - (x + 2y, 2x + y) mod 4 is a
// bijection, and any 2x2 block of it already has four different azimuths and
// four different elevations - with a white jitter inside the cell. The
// denoiser's AO window spans exactly one period each way (kDenoiseSrc), so on
// an even surface its mean is a 16-sample STRATIFIED estimate rather than 25
// independent coin tosses: most of the variance of white noise is the luck
// of where the samples fall, and this takes the luck out. Every frame the
// whole grid is shifted on the square (toroidally, by the R2 sequence), which
// keeps the cells apart and walks each pixel's sample over the hemisphere.
float2 AoSample(uint2 px, uint frame, uint i, uint n)
{
    const uint2  cell = uint2((px.x + 2u * px.y) & 3u, (2u * px.x + px.y) & 3u);
    const float2 j    = Rand2(px, frame, i + 1u);
    // n rays split their cell along the azimuth, so they stay apart too.
    const float2 u    = (float2(cell) + float2((float(i) + j.x) / float(n), j.y)) * 0.25;
    return frac(u + frac(float(frame) * float2(0.7548776662, 0.5698402910)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
    if (!SurfaceAt(tid.xy, p, N, plant)) { gOut[tid.xy] = 1.0; return; }

    // The prepass normal already faces the camera, which is the side the
    // ambient light arrives from as far as this pixel is concerned.
    float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);
    // A PLANT's card is not a surface facing one way: its lighting uses the
    // plant's volume normal, and a hemisphere about the card's plane - mostly
    // horizontal - sends half its rays into the ground. That ground is not
    // missing sky; the ambient term already lights the card from it (E_ground
    // * down, scene.cpp). It made grass ~7% darker than under the cascades,
    // which have no AO. So a plant looks UP: the sky it is occluded from is
    // the sky, by its neighbours and anything over it. The origin still
    // leaves the card along its own plane's normal.
    const float3 axis = plant != 0u ? float3(0.0, 1.0, 0.0) : N;
    float3 T, B; Basis(axis, T, B);

    // COSINE-WEIGHTED hemisphere: rays are distributed the way the diffuse
    // ambient integral weights them, so the average of hit/miss IS the
    // occluded fraction of the ambient term with no extra weighting. The
    // radius is where this stops being "occlusion" and starts being "global
    // illumination": an occluder ten metres away blocks sky but a wall that
    // far away also reflects light we are not simulating, so beyond the radius
    // the honest answer is "unknown", which is what unoccluded means here.
    float open = 0.0;
    float allow = -1.0;       // the least any ray allows (Allow), -1 none touched
    for (uint i = 0; i < gAoRays; ++i)
    {
        float2 r = AoSample(tid.xy, gFrame, i, gAoRays);
        float phi = r.x * 6.28318530;
        float cosT = sqrt(1.0 - r.y);            // cosine-weighted
        float sinT = sqrt(r.y);
        float3 dir = T * (cos(phi) * sinT) + B * (sin(phi) * sinT) + axis * cosT;

        RayDesc ray;
        ray.Origin    = p + N * offset;
        ray.Direction = dir;
        ray.TMin      = 0.0;
        ray.TMax      = gAoRadius;
        float2 mover;
        if (!Occluded(ray, false, plant, mover)) open += 1.0;
        // An occluder this far off changes the occlusion over about as far
        // across the ground: its "penumbra" is its distance.
        const float a = Allow(mover, 1.0);
        if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
    }
    gOut[tid.xy] = EncodeRaw(open / max(1.0, float(gAoRays)), allow);
}
)";

// ---- local lights: one ray to one light, chosen by what it gives (lights.h) -
// The RATIO ESTIMATOR's stochastic half: among the shadowed lights of this pixel's cluster, one chosen with a
// probability in proportion to what it gives here (its irradiance's luminance times the cosine - the same terms the
// scene pass sums), by a single weighted-reservoir pass; then one ray to a random point on its emitter. The 0/1 is
// then an unbiased estimate of the shadowed fraction of the scene pass's sum, and the denoiser averages it as it does
// the sun's. No light here: 1, which the scene multiplies into nothing.
const char* kLightsRegsSrc = R"(
#define NGG_LIGHTS_T0 t11
#define NGG_LIGHTS_T1 t12
#define NGG_LIGHTS_T2 t13
)";

const char* kLightMainSrc = R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gSize.x || tid.y >= gSize.y) return;
    float3 p, N;
    uint plant;
    if (!SurfaceAt(tid.xy, p, N, plant)) { gOut[tid.xy] = 1.0; return; }

    const float viewZ = dot(p - gCamPos, gCamFwd);
    const uint2 c = gClusters[LightCluster(float2(tid.xy) + 0.5, viewZ, gClusterParams)];
#if LIGHTS_REF == 2
    // NGG_LIGHTS_REF=2: the estimator itself, 16 picks per pixel averaged - what the one pick converges to.
    {
        float vSum = 0.0;
        float allow = -1.0;
        for (uint s = 0; s < 16u; ++s)
        {
            float total = 0.0;
            uint chosen = 0xFFFFFFFFu;
            for (uint k = 0; k < c.y; ++k)
            {
                const uint li = gLightIndex[c.x + k];
                if ((gLights[li].flags & kLightShadows) == 0u) continue;
                float3 l;
                float dist;
                const float3 E = LightIrradiance(li, p, l, dist);
                const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
                if (w <= 0.0) continue;
                total += w;
                if (Rand2(tid.xy, gFrame * 17u + s, 101u + k).x * total < w) chosen = li;
            }
            if (chosen == 0xFFFFFFFFu) { vSum += 1.0; continue; }
            const float3 lpos = gLights[chosen].pos;
            const float3 toL = lpos - p;
            const float3 d0 = toL / max(length(toL), 1e-4);
            float3 T, B; Basis(d0, T, B);
            const float2 r = Rand2(tid.xy, gFrame * 17u + s, 7u);
            const float rad = sqrt(r.x) * gLights[chosen].radius, ang = r.y * 6.28318530;
            const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));
            float3 Nn = N;
            if (dot(Nn, d0) < 0.0) Nn = -Nn;
            RayDesc ray;
            ray.Origin = p + Nn * (gBias * (1.0 + length(p - gCamPos) * 0.002));
            const float3 dv = target - ray.Origin;
            const float len = length(dv);
            ray.Direction = dv / max(len, 1e-4);
            ray.TMin = 0.0;
            ray.TMax = max(len - 0.02, 0.0);
            float2 mover;
            const bool hit = Occluded(ray, false, plant, mover);
            const float a = Allow(mover, 2.0 * gLights[chosen].radius / max(len, 1e-3));
            if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
            vSum += hit ? 0.0 : 1.0;
        }
        gOut[tid.xy] = EncodeRaw(vSum / 16.0, allow);
        return;
    }
#elif defined(LIGHTS_REF)
    // NGG_LIGHTS_REF=1: THE REFERENCE - a ray to EVERY light, the exact weighted visibility. The one-ray estimate below
    // is checked against it (design 5.4 N3: the lamp lab, the test lamps' shadows).
    {
        float sumW = 0.0, sumV = 0.0;
        float allow = -1.0;
        for (uint k = 0; k < c.y; ++k)
        {
            const uint li = gLightIndex[c.x + k];
            if ((gLights[li].flags & kLightShadows) == 0u) continue;
            float3 l;
            float dist;
            const float3 E = LightIrradiance(li, p, l, dist);
            const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
            if (w <= 0.0) continue;
            const float3 lpos = gLights[li].pos;
            const float3 toL = lpos - p;
            const float3 d0 = toL / max(length(toL), 1e-4);
            float3 T, B; Basis(d0, T, B);
            const float2 r = Rand2(tid.xy, gFrame, 7u + k);
            const float rad = sqrt(r.x) * gLights[li].radius, ang = r.y * 6.28318530;
            const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));
            float3 Nn = N;
            if (dot(Nn, d0) < 0.0) Nn = -Nn;
            RayDesc ray;
            ray.Origin = p + Nn * (gBias * (1.0 + length(p - gCamPos) * 0.002));
            const float3 dv = target - ray.Origin;
            const float len = length(dv);
            ray.Direction = dv / max(len, 1e-4);
            ray.TMin = 0.0;
            ray.TMax = max(len - 0.02, 0.0);
            float2 mover;
            const bool hit = Occluded(ray, false, plant, mover);
            const float a = Allow(mover, 2.0 * gLights[li].radius / max(len, 1e-3));
            if (a >= 0.0) allow = allow < 0.0 ? a : min(allow, a);
            sumW += w;
            sumV += hit ? 0.0 : w;
        }
        gOut[tid.xy] = EncodeRaw(sumW > 0.0 ? sumV / sumW : 1.0, allow);
        return;
    }
#endif
    // Each lamp in proportion to what it gives here: their sum, then the one
    // whose share of it holds this pixel's stratified number (StratU).
    float total = 0.0;
    for (uint k = 0; k < c.y; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        if ((gLights[li].flags & kLightShadows) == 0u) continue;
        float3 l;
        float dist;
        const float3 E = LightIrradiance(li, p, l, dist);
        // A plant's card takes light from both sides (its shading wraps), any other surface from its front.
        total += max(dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l))), 0.0);
    }
    if (total <= 0.0) { gOut[tid.xy] = 1.0; return; }
    float pick = StratU(tid.xy, gFrame) * total;
    uint chosen = 0xFFFFFFFFu;
    for (uint k = 0; k < c.y && pick >= 0.0; ++k)
    {
        const uint li = gLightIndex[c.x + k];
        if ((gLights[li].flags & kLightShadows) == 0u) continue;
        float3 l;
        float dist;
        const float3 E = LightIrradiance(li, p, l, dist);
        const float w = dot(E, float3(0.2126, 0.7152, 0.0722)) * (plant != 0u ? 1.0 : saturate(dot(N, l)));
        if (w <= 0.0) continue;
        chosen = li;
        pick -= w;
    }
    if (chosen == 0xFFFFFFFFu) { gOut[tid.xy] = 1.0; return; }

    const float3 lpos = gLights[chosen].pos;
    const float lradius = gLights[chosen].radius;
    // A random point on the emitter, a disc of its radius facing this surface: the penumbra, per pixel per frame.
    const float3 toL = lpos - p;
    const float3 d0 = toL / max(length(toL), 1e-4);
    float3 T, B; Basis(d0, T, B);
    const float2 r = StratDisc(tid.xy, gFrame);
    const float rad = sqrt(r.x) * lradius, ang = r.y * 6.28318530;
    const float3 target = lpos + T * (rad * cos(ang)) + B * (rad * sin(ang));

    if (dot(N, d0) < 0.0) N = -N;
    const float offset = gBias * (1.0 + length(p - gCamPos) * 0.002);
    RayDesc ray;
    ray.Origin = p + N * offset;
    const float3 dv = target - ray.Origin;
    const float len = length(dv);
    ray.Direction = dv / max(len, 1e-4);
    ray.TMin = 0.0;
    ray.TMax = max(len - 0.02, 0.0);    // short of the emitter itself
    float2 mover;
    const bool hit = Occluded(ray, false, plant, mover);
    // The emitter's disc as seen from here: the penumbra per metre of the hit's distance.
    gOut[tid.xy] = EncodeRaw(hit ? 0.0 : 1.0, Allow(mover, 2.0 * lradius / max(len, 1e-3)));
}
)";

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
const char* kDenoiseSrc = R"(
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
// per pixel were most of this pass's cost.
groupshared float sDepth[144];
groupshared float sRaw[144];

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int2 origin = int2(gid.xy) * 8 - 2;
    for (uint k = gi; k < 144u; k += 64u)
    {
        const int2 q = origin + int2(k % 12u, k / 12u);
        float d = 0.0, r = 1.0;
        if (all(q >= 0) && all(q < int2(gSize)))
        {
            d = max(gDepth.Load(int3(q, 0)), 0.0);
            r = gRaw.Load(int3(q, 0));
            if (gMotionOn > 0.5 && gMotion.Load(int3(q, 0)).w > 0.5) d = -d;
        }
        sDepth[k] = d;
        sRaw[k]   = r;
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
        float w = (rel < 0.03 && qMoved == movedPixel) ? 1.0 : 0.0;
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
            // sample disagrees with it.
            const float still = movedPixel ? 0.0
                              : saturate(1.0 - length(puv * float2(gSize) - (float2(px) + 0.5)) * 4.0);
            const float top = lerp(gBlend, gBlendStill, still);
            // Ground the car uncovered: last frame this spot was the moved
            // object, so its history is cut below - and that, too, is "a moved
            // object was involved". Without it the distrust broke on every
            // such strip: they started over as merely fresh, and TAA, which
            // reads the distrust (post.cpp), kept the shadow each strip was
            // first seen in - a band behind the car per frame.
            if (h.y > 0.0 && h.w > 0.5 && !movedPixel) { touched = true; allow = 0.0; }
            const bool histTouched = h.z > 0.5;
            // IDENTITY first, as TAA's (post.cpp): a moved object's history
            // never passes to a static pixel, nor the reverse. Depth cannot
            // tell them apart where they meet - the road just behind a
            // bumper is within 3% of the bumper - and the back of a car, in
            // its own shadow, then printed a dark band of itself on the sunlit
            // road behind it every frame.
            if (h.y > 0.0 && (h.w > 0.5) == movedPixel)
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
    gOut[px] = float4(vis, depth, level, movedPixel ? 1.0 : 0.45 * allowed);
}
)";

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
const char* kAoWideSrc = R"(
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
    if (c.y <= 0.0 || c.z < 0.25 || c.w > 0.44) { gWide[px] = c.x; return; }
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
        if (h.y <= 0.0 || h.w > 0.5) continue;
        const float e = max(c.y + dot(slope, float2(x, y) * 3.0), 1e-7);
        if (abs(1.0 / h.y - 1.0 / e) * e >= 0.03) continue;
        sum += h.x; wsum += 1.0;
    }
    gWide[px] = sum / wsum;
}
)";

struct DenoiseCB
{
    float    reproj[16];
    uint32_t size[2];
    float    blend;
    float    valid;
    float    motionOn;
    float    blendMoved;
    float    period4;
    float    blendStill;
};
static_assert(sizeof(DenoiseCB) % 16 == 0, "DenoiseCB must be whole float4s");

struct TraceCB
{
    float    invViewProj[16];
    float    sunTowards[3];
    float    tMax;
    uint32_t size[2];
    float    bias;
    uint32_t stride;
    float    camPos[3];
    float    sunAngle;
    uint32_t frame;
    float    aoRadius;
    uint32_t aoRays;
    uint32_t pad2;
    float    camFwd[3];
    float    lightsOn;
    float    clusterParams[4];
};
static_assert(sizeof(TraceCB) % 16 == 0, "TraceCB must be whole float4s - see scene.cpp");

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                                  D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t = {};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

ComPtr<ID3D12Resource> MakeUploadBuffer(ID3D12Device* dev, const void* data, size_t bytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 4;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    void* p = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(r->Map(0, &none, &p))) return nullptr;
    memcpy(p, data, bytes);
    r->Unmap(0, nullptr);
    return r;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, UINT64 bytes,
                                  D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;

    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 1;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;

    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                 IID_PPV_ARGS(&res));
    return res;
}

void MatMul(const float* a, const float* b, float* out)   // row-vector: v*a*b
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = acc;
        }
}

// General 4x4 inverse; the view-projection is not affine, so the cheap
// rigid-transform shortcut does not apply. Same routine as post.cpp - kept
// local rather than shared because two copies of twelve lines is cheaper than
// a header that exists only to hold them.
bool Invert(const float* m, float* out)
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

// One heap, one descriptor table. Fixed slots first, then every scene texture
// for the any-hit test - so the heap is sized per scene and (re)created in
// Build rather than in Create.
enum { kSlotTlas = 0, kSlotDepth = 1, kSlotOut = 2, kSlotCounters = 3,
       kSlotVerts = 4, kSlotAlphaIdx = 5, kSlotTriMat = 6, kSlotMaterials = 7,
       kSlotNormal = 8,
       // Denoise: two contiguous [raw, hist] SRV pairs so one table base
       // selects which history is read, plus a UAV per history for writing.
       kSlotPair0 = 9, kSlotPair1 = 11, kSlotHistUav0 = 13, kSlotHistUav1 = 14,
       // Ambient occlusion: its own raw output (u0) followed by counters (u1)
       // so the UAV table can point at either feature's pair, then its own
       // denoise pairs and history UAVs.
       kSlotAoOut = 15, kSlotAoCounters = 16,
       kSlotAoPair0 = 17, kSlotAoPair1 = 19, kSlotAoHistUav0 = 21, kSlotAoHistUav1 = 22,
       // Instancing (5.0o): the dynamic pool's vertices and the per-instance table.
       kSlotDynVerts = 23, kSlotInst = 24,
       // The scene's motion target, for the denoiser (a null view when none).
       kSlotMotion = 25,
       // The AO's wide pass output (kAoWideSrc).
       kSlotAoWideUav = 26,
       // Foliage (scene.cpp FindPlants): the plant of each alpha triangle and
       // the prepass's plant per pixel, for the rays that must not hit their
       // own plant.
       kSlotTriPlant = 27, kSlotPlantId = 28,
       // Local lights (lights.h): their raw output (u0) and counters (u1), denoise pairs and history UAVs - the AO's
       // shape.
       kSlotLightOut = 29, kSlotLightCounters = 30,
       kSlotLightPair0 = 31, kSlotLightPair1 = 33, kSlotLightHistUav0 = 35, kSlotLightHistUav1 = 36,
       // Their wide pass output (kAoWideSrc, as the AO's).
       kSlotLightWideUav = 37,
       kSlotTextures = 38 };

} // namespace

bool Rt::Create(Device& dev)
{
    if (!dev.device) return false;

    if (!dev.SupportsInlineRaytracing())
    {
        Log("rt: adapter reports tier %d.%d / SM %d.%d - inline ray tracing needs "
            "1.1 and 6.5, so the RT path is unavailable here",
            dev.rtTier / 10, dev.rtTier % 10, dev.shaderModel >> 4, dev.shaderModel & 0xF);
        return false;
    }
    if (!m_dxc.Load()) return false;

    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5)))
    {
        Log("rt: ID3D12Device5 unavailable despite a tier 1.1 report");
        return false;
    }

    // One table over the whole heap, in kSlot* order. The texture range is
    // UNBOUNDED: the shader indexes it by material at the hit point, and the
    // count is only known once a scene is loaded.
    D3D12_DESCRIPTOR_RANGE ranges[6] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                    // t0 TLAS, t1 depth
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = kSlotTlas;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;                    // u0 output, u1 counters
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = kSlotOut;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 5;                    // t2..t6 verts, alpha idx, tri mat, materials, normal
    ranges[2].BaseShaderRegister = 2;
    ranges[2].OffsetInDescriptorsFromTableStart = kSlotVerts;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors = UINT_MAX;             // t0.., space1: the textures
    ranges[3].BaseShaderRegister = 0;
    ranges[3].RegisterSpace = 1;
    ranges[3].OffsetInDescriptorsFromTableStart = kSlotTextures;
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].NumDescriptors = 2;                    // t7 dynamic vertices, t8 instance table
    ranges[4].BaseShaderRegister = 7;
    ranges[4].OffsetInDescriptorsFromTableStart = kSlotDynVerts;
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[5].NumDescriptors = 2;                    // t9 plant per alpha triangle, t10 plant per pixel
    ranges[5].BaseShaderRegister = 9;
    ranges[5].OffsetInDescriptorsFromTableStart = kSlotTriPlant;

    // The UAV pair is its OWN table so a dispatch can point it at either the
    // shadow output or the AO output; the SRVs are one table over the heap.
    D3D12_DESCRIPTOR_RANGE srvRanges[5] = { ranges[0], ranges[2], ranges[4], ranges[5], ranges[3] };
    D3D12_DESCRIPTOR_RANGE uavRange = ranges[1];
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 5;
    params[0].DescriptorTable.pDescriptorRanges = srvRanges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = sizeof(TraceCB) / 4;
    // The local lights (lights.h): root SRVs over the Lights buffers - t11 lights, t12 clusters, t13 indices.
    for (int k = 0; k < 3; ++k)
    {
        params[3 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3 + k].Descriptor.ShaderRegister = 11 + k;
        params[3 + k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // Linear, WRAP: the same addressing the rasteriser uses for these textures
    // (scene.cpp's static samplers), so the cut-out matches the drawn leaf.
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 6;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        Log("rt: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rootSig))))
        return false;

    ComPtr<ID3DBlob> cs = m_dxc.Compile(std::string(kRtCommonSrc) + kTraceMainSrc,
                                        L"CSMain", L"cs_6_5", "ngg_rt_trace");
    if (!cs) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    {
        Log("rt: compute pipeline creation failed - the DXIL was probably rejected");
        return false;
    }

    ComPtr<ID3DBlob> aocs = m_dxc.Compile(std::string(kRtCommonSrc) + kAoMainSrc,
                                          L"CSMain", L"cs_6_5", "ngg_rt_ao");
    if (aocs)
    {
        pd.CS = { aocs->GetBufferPointer(), aocs->GetBufferSize() };
        if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_psoAo))))
            Log("rt: AO pipeline creation failed - shadows still work, AO does not");
    }

    // NGG_LIGHTS_REF=1|2: the local lights' shadows by a reference (kLightMainSrc) - for checking, never for play.
    const char* refEnv = std::getenv("NGG_LIGHTS_REF");
    const char* lightsRef = !refEnv ? "" : refEnv[0] == '2' ? "#define LIGHTS_REF 2\n" : "#define LIGHTS_REF 1\n";
    ComPtr<ID3DBlob> lcs = m_dxc.Compile(std::string(lightsRef) + kRtCommonSrc + kLightsRegsSrc + kLightsHlsl + kLightMainSrc,
                                         L"CSMain", L"cs_6_5", "ngg_rt_lights");
    if (lcs)
    {
        pd.CS = { lcs->GetBufferPointer(), lcs->GetBufferSize() };
        if (FAILED(dev.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_psoLights))))
            m_psoLights.Reset();
    }
    if (!m_psoLights) Log("rt: local-light pipeline creation failed - local lights go unshadowed");

    // --- the denoiser: its own root signature, same heap --------------------
    {
        D3D12_DESCRIPTOR_RANGE dr[4] = {};
        dr[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t0 raw, t1 hist (a pair)
        dr[0].NumDescriptors = 2;
        dr[0].BaseShaderRegister = 0;
        dr[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t2 depth
        dr[1].NumDescriptors = 1;
        dr[1].BaseShaderRegister = 2;
        dr[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;   // u0 history out
        dr[2].NumDescriptors = 1;
        dr[2].BaseShaderRegister = 0;
        dr[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;   // t3 motion
        dr[3].NumDescriptors = 1;
        dr[3].BaseShaderRegister = 3;

        D3D12_ROOT_PARAMETER dp[5] = {};
        for (int i = 0; i < 3; ++i)
        {
            dp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            dp[i].DescriptorTable.NumDescriptorRanges = 1;
            dp[i].DescriptorTable.pDescriptorRanges = &dr[i];
        }
        dp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        dp[3].Constants.ShaderRegister = 0;
        dp[3].Constants.Num32BitValues = sizeof(DenoiseCB) / 4;
        dp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dp[4].DescriptorTable.NumDescriptorRanges = 1;
        dp[4].DescriptorTable.pDescriptorRanges = &dr[3];

        D3D12_ROOT_SIGNATURE_DESC drs = {};
        drs.NumParameters = 5;
        drs.pParameters = dp;
        ComPtr<ID3DBlob> dsig, derr;
        if (FAILED(D3D12SerializeRootSignature(&drs, D3D_ROOT_SIGNATURE_VERSION_1, &dsig, &derr)) ||
            FAILED(dev.device->CreateRootSignature(0, dsig->GetBufferPointer(), dsig->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_denoiseRootSig))))
        {
            Log("rt: denoise root signature failed");
            return false;
        }
        ComPtr<ID3DBlob> dcs = m_dxc.Compile(kDenoiseSrc, L"CSMain", L"cs_6_5", "ngg_rt_denoise");
        if (!dcs) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC dpd = {};
        dpd.pRootSignature = m_denoiseRootSig.Get();
        dpd.CS = { dcs->GetBufferPointer(), dcs->GetBufferSize() };
        if (FAILED(dev.device->CreateComputePipelineState(&dpd, IID_PPV_ARGS(&m_psoDenoise))))
        {
            Log("rt: denoise pipeline creation failed");
            return false;
        }
        // The AO's wide pass, on the same root signature (kAoWideSrc). Without
        // it the AO is the denoise's own output, as before.
        ComPtr<ID3DBlob> wcs = m_dxc.Compile(kAoWideSrc, L"CSMain", L"cs_6_5", "ngg_rt_ao_wide");
        if (wcs)
        {
            dpd.CS = { wcs->GetBufferPointer(), wcs->GetBufferSize() };
            if (FAILED(dev.device->CreateComputePipelineState(&dpd, IID_PPV_ARGS(&m_psoAoWide))))
                m_psoAoWide.Reset();
        }
        if (!m_psoAoWide) Log("rt: AO wide pass pipeline creation failed - the AO goes unwidened");
    }

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 5;     // start, after the shadow, after the AO, after the local lights, after the denoise
    dev.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timestamps));
    m_timestampReadback = MakeBuffer(dev.device.Get(), 5 * sizeof(UINT64),
                                     D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    dev.queue->GetTimestampFrequency(&m_timestampHz);

    m_stride = dev.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The descriptor heap is created in Build: its size depends on how many
    // textures the scene has, and there is no scene yet.

    m_counters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_aoCounters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_lightCounters = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_countersReadback = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t),
                                    D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    m_countersZero = MakeBuffer(dev.device.Get(), 4 * sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    if (m_countersZero)
    {
        void* p = nullptr;
        D3D12_RANGE none = { 0, 0 };
        if (SUCCEEDED(m_countersZero->Map(0, &none, &p)))
        {
            memset(p, 0, 4 * sizeof(uint32_t));
            m_countersZero->Unmap(0, nullptr);
        }
    }

    Log("rt: ready - inline RayQuery, tier %d.%d, SM %d.%d",
        dev.rtTier / 10, dev.rtTier % 10, dev.shaderModel >> 4, dev.shaderModel & 0xF);
    return true;
}

// Instance transform: our matrices are row-vector (p' = p * M, translation in
// the last row), D3D12's is a 3x4 for column vectors - the transpose of the
// upper three columns.
void ToInstanceTransform(const float* m, float t[3][4])
{
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c) t[r][c] = m[c * 4 + r];
        t[r][3] = m[12 + r];
    }
}

bool Rt::Rebuild(Device& dev, const Scene::RtShared& shared,
                 std::vector<Scene::RtMeshData>& newMeshes,
                 const std::vector<uint64_t>& liveKeys,
                 const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !shared.staticVb) return false;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5))) return false;

    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&hz);

    // ---- drop the BLASes nothing uses any more
    for (auto it = m_blas.begin(); it != m_blas.end(); )
    {
        if (std::find(liveKeys.begin(), liveKeys.end(), it->first) == liveKeys.end())
            it = m_blas.erase(it);
        else
            ++it;
    }

    // ---- build the new ones, in one list. Each has its own scratch, so they
    // need no barriers between them; all scratch dies after the wait.
    std::vector<ComPtr<ID3D12Resource>> keep;      // uploads + scratch, alive until EndAndWait
    UINT64 blasBytes = 0;
    uint32_t builtTris = 0;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (!newMeshes.empty())
    {
        if (!dev.Begin()) return false;
        if (FAILED(dev.list.As(&list4))) { dev.EndAndWait(); return false; }

        for (Scene::RtMeshData& m : newMeshes)
        {
            ComPtr<ID3D12Resource> ibo = m.opaque.empty() ? nullptr :
                MakeUploadBuffer(dev.device.Get(), m.opaque.data(), m.opaque.size() * 4);
            ComPtr<ID3D12Resource> iba = m.alpha.empty() ? nullptr :
                MakeUploadBuffer(dev.device.Get(), m.alpha.data(), m.alpha.size() * 4);
            keep.push_back(ibo); keep.push_back(iba);

            // Opaque geometry commits on its own; alpha-tested geometry surfaces
            // as a candidate for the any-hit test. PrimitiveIndex counts per
            // geometry, which is what lets the alpha one index from 0.
            D3D12_RAYTRACING_GEOMETRY_DESC geos[2] = {};
            UINT n = 0;
            auto describe = [&](ID3D12Resource* ib, size_t count, bool opaque) {
                if (!ib || !count) return;
                D3D12_RAYTRACING_GEOMETRY_DESC& g = geos[n++];
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                g.Triangles.VertexBuffer.StartAddress = m.vb->GetGPUVirtualAddress() + m.vbOffset;
                g.Triangles.VertexBuffer.StrideInBytes = shared.stride;
                g.Triangles.VertexCount = m.vertexCount;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexBuffer = ib->GetGPUVirtualAddress();
                g.Triangles.IndexCount = (UINT)count;
                g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            };
            describe(ibo.Get(), m.opaque.size(), true);
            describe(iba.Get(), m.alpha.size(), false);
            if (!n) continue;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
            in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            // PREFER_FAST_TRACE: a BLAS is built when its mesh appears and then
            // traced for as long as it exists - moving it is the TLAS's job.
            in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            in.NumDescs = n;
            in.pGeometryDescs = geos;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            dev5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);

            BlasEntry e;
            e.as = MakeBuffer(dev.device.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            // COMMON: buffers are created in it whatever is asked, and promote
            // to UNORDERED_ACCESS on first use - asking for UAV only earned a
            // validation warning per rebuild.
            ComPtr<ID3D12Resource> scratch = MakeBuffer(dev.device.Get(), info.ScratchDataSizeInBytes,
                              D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!e.as || !scratch) { Log("rt: BLAS allocation failed"); continue; }
            keep.push_back(scratch);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
            bd.Inputs = in;
            bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            bd.DestAccelerationStructureData = e.as->GetGPUVirtualAddress();
            list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

            e.alpha.swap(m.alpha);
            e.alphaMat.swap(m.alphaMat);
            e.alphaPlant.swap(m.alphaPlant);
            if (e.alphaPlant.size() != e.alphaMat.size()) e.alphaPlant.assign(e.alphaMat.size(), 0xFFFFFFFFu);
            e.vbSelect = m.vbSelect;
            e.vertexBase = m.vertexBase;
            blasBytes += info.ResultDataMaxSizeInBytes;
            builtTris += (uint32_t)((m.opaque.size() + e.alpha.size()) / 3);
            m_blas[m.key] = std::move(e);
        }
        dev.EndAndWait();
    }

    // ---- the shared alpha index buffer and per-triangle materials: every
    // live mesh's alpha triangles end to end, each BLAS knowing where its own
    // start. Rebuilt whenever the mesh set changes, which is not per drag.
    std::vector<uint32_t> alpha, alphaMat, alphaPlant;
    for (uint64_t k : liveKeys)
    {
        auto it = m_blas.find(k);
        if (it == m_blas.end()) continue;
        it->second.triBase = (uint32_t)(alphaMat.size());
        alpha.insert(alpha.end(), it->second.alpha.begin(), it->second.alpha.end());
        alphaMat.insert(alphaMat.end(), it->second.alphaMat.begin(), it->second.alphaMat.end());
        alphaPlant.insert(alphaPlant.end(), it->second.alphaPlant.begin(), it->second.alphaPlant.end());
    }
    if (alpha.empty()) { alpha.assign(3, 0); alphaMat.assign(1, 0); alphaPlant.assign(1, 0xFFFFFFFFu); }   // never an empty view
    m_triPlant = MakeUploadBuffer(dev.device.Get(), alphaPlant.data(), alphaPlant.size() * 4);
    m_ibAlpha = MakeUploadBuffer(dev.device.Get(), alpha.data(), alpha.size() * 4);
    m_triMaterial = MakeUploadBuffer(dev.device.Get(), alphaMat.data(), alphaMat.size() * 4);
    m_materials = shared.materials;
    m_vertexStride = shared.stride;
    m_staticVb = shared.staticVb;
    m_dynVb = shared.dynVb;

    // ---- the descriptor heap, sized for THIS scene's textures
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlotTextures + (UINT)(shared.textures.empty() ? 1 : shared.textures.size());
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_heap.Reset();
    if (FAILED(dev.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    m_width = m_height = 0;        // the output UAVs live in the new heap: recreate on first Trace

    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * m_stride;
        return h;
    };
    auto raw = [&](ID3D12Resource* r, UINT64 bytes, UINT at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev.device->CreateShaderResourceView(r, &sd, slot(at));
    };
    auto structured = [&](ID3D12Resource* r, UINT count, UINT strideBytes, UINT at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = count ? count : 1;
        sd.Buffer.StructureByteStride = strideBytes;
        dev.device->CreateShaderResourceView(r, &sd, slot(at));
    };
    raw(shared.staticVb, (UINT64)shared.staticVertexCount * shared.stride, kSlotVerts);
    // No dynamic pool yet: point the view at the static buffer, so the table
    // is never holed. No instance selects it until there is one.
    if (shared.dynVb && shared.dynVertexCount)
        raw(shared.dynVb, (UINT64)shared.dynVertexCount * shared.stride, kSlotDynVerts);
    else
        raw(shared.staticVb, (UINT64)shared.staticVertexCount * shared.stride, kSlotDynVerts);
    raw(m_ibAlpha.Get(), (UINT64)alpha.size() * 4, kSlotAlphaIdx);
    structured(m_triMaterial.Get(), (UINT)alphaMat.size(), 4, kSlotTriMat);
    structured(m_triPlant.Get(), (UINT)alphaPlant.size(), 4, kSlotTriPlant);
    structured(shared.materials ? shared.materials.Get() : m_triMaterial.Get(),
               shared.materials ? shared.materialCount : 1, 8, kSlotMaterials);
    for (size_t i = 0; i < shared.textures.size(); ++i)
    {
        ID3D12Resource* r = shared.textures[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = r->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = r->GetDesc().MipLevels;
        dev.device->CreateShaderResourceView(r, &sd, slot(kSlotTextures + (UINT)i));
    }

    const bool ok = UpdateInstances(dev, instances);

    QueryPerformanceCounter(&t1);
    Log("rt: %zu BLAS(es) built (%u triangles, %.1f MB) in %.0f ms; %zu live, %zu instance(s), "
        "%zu alpha-tested triangles for the any-hit test, %zu textures",
        newMeshes.size(), builtTris, blasBytes / 1048576.0,
        1000.0 * double(t1.QuadPart - t0.QuadPart) / double(hz.QuadPart),
        m_blas.size(), instances.size(), alphaMat.size(), shared.textures.size());
    return ok;
}

// The TLAS and the instance table only. What a drag costs: a handful of
// instances, rebuilt whole - FAST_BUILD, because it is rebuilt far more often
// than any single version of it is traced.
bool Rt::UpdateInstances(Device& dev, const std::vector<Scene::RtInstanceDesc>& instances)
{
    if (!Ready() || !m_heap) return false;
    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev.device.As(&dev5))) return false;

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
    struct InstInfo { uint32_t triBase, vertexBase, vb, material, moved; float was[12]; };
    std::vector<InstInfo> info;
    for (const Scene::RtInstanceDesc& d : instances)
    {
        auto it = m_blas.find(d.mesh);
        if (it == m_blas.end() || !it->second.as) continue;
        D3D12_RAYTRACING_INSTANCE_DESC id = {};
        ToInstanceTransform(d.xform, id.Transform);
        id.InstanceID = (UINT)descs.size();          // indexes the table below
        id.InstanceMask = 0xFF;
        id.Flags = d.forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE
                                 : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        id.AccelerationStructure = it->second.as->GetGPUVirtualAddress();
        descs.push_back(id);
        InstInfo in = { it->second.triBase, it->second.vertexBase, it->second.vbSelect, d.material,
                        d.moved ? 1u : 0u };
        for (int r = 0; r < 4; ++r)                  // the rows' xyz (row vectors)
            for (int c = 0; c < 3; ++c) in.was[r * 3 + c] = d.prevXform[r * 4 + c];
        info.push_back(in);
    }
    if (descs.empty()) { m_tlas.Reset(); return false; }

    m_instances = MakeUploadBuffer(dev.device.Get(), descs.data(), descs.size() * sizeof(descs[0]));
    m_instInfo  = MakeUploadBuffer(dev.device.Get(), info.data(), info.size() * sizeof(info[0]));
    if (!m_instances || !m_instInfo) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top = {};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.NumDescs = (UINT)descs.size();
    top.InstanceDescs = m_instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);

    if (!m_tlas || m_tlas->GetDesc().Width < topInfo.ResultDataMaxSizeInBytes)
        m_tlas = MakeBuffer(dev.device.Get(), topInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_scratch || m_scratch->GetDesc().Width < topInfo.ScratchDataSizeInBytes)
        m_scratch = MakeBuffer(dev.device.Get(), topInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!m_tlas || !m_scratch) return false;

    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (!dev.Begin()) return false;
    if (FAILED(dev.list.As(&list4))) { dev.EndAndWait(); return false; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = top;
    td.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    dev.EndAndWait();

    auto slot = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * m_stride;
        return h;
    };
    // The TLAS descriptor has no resource: a raytracing SRV names a GPU address.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.RaytracingAccelerationStructure.Location = m_tlas->GetGPUVirtualAddress();
        dev.device->CreateShaderResourceView(nullptr, &sd, slot(kSlotTlas));
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)info.size();
        sd.Buffer.StructureByteStride = sizeof(InstInfo);
        dev.device->CreateShaderResourceView(m_instInfo.Get(), &sd, slot(kSlotInst));
    }
    m_instanceCount = (uint32_t)descs.size();
    return true;
}

bool Rt::CreateTargets(ID3D12Device* dev, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    m_result.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_result))))
        return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R16_FLOAT;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)kSlotOut * m_stride;
    dev->CreateUnorderedAccessView(m_result.Get(), nullptr, &uv, h);

    // Raw visibility as an SRV, for the denoiser - twice, at the head of each
    // [raw, hist] pair.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE a = m_heap->GetCPUDescriptorHandleForHeapStart();
        a.ptr += (SIZE_T)kSlotPair0 * m_stride;
        dev->CreateShaderResourceView(m_result.Get(), &rs, a);
        a = m_heap->GetCPUDescriptorHandleForHeapStart();
        a.ptr += (SIZE_T)kSlotPair1 * m_stride;
        dev->CreateShaderResourceView(m_result.Get(), &rs, a);
    }

    // The filtered history, two of them, RGBA16F: filtered visibility, the
    // NDC depth it was computed at - so next frame can tell whether a
    // reprojected sample landed on the same surface - and two flags (a ray
    // hit a moved object; the pixel is a moved object's), see kDenoiseSrc.
    D3D12_RESOURCE_DESC hd2 = d;
    hd2.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (int i = 0; i < 2; ++i)
    {
        m_hist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_hist[i]))))
            return false;
        m_histState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotPair0 : kSlotPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_hist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotHistUav0 : kSlotHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_hist[i].Get(), nullptr, &hu, uh);
    }
    m_histValid = false;

    // ---- the same again for ambient occlusion ------------------------------
    m_aoResult.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_aoResult))))
        return false;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC au = {};
        au.Format = DXGI_FORMAT_R16_FLOAT;
        au.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoOut * m_stride;
        dev->CreateUnorderedAccessView(m_aoResult.Get(), nullptr, &au, ah);

        D3D12_UNORDERED_ACCESS_VIEW_DESC acv = {};
        acv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        acv.Buffer.NumElements = 4;
        acv.Buffer.StructureByteStride = sizeof(uint32_t);
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoCounters * m_stride;
        dev->CreateUnorderedAccessView(m_aoCounters.Get(), nullptr, &acv, ah);

        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoPair0 * m_stride;
        dev->CreateShaderResourceView(m_aoResult.Get(), &rs, ah);
        ah = m_heap->GetCPUDescriptorHandleForHeapStart();
        ah.ptr += (SIZE_T)kSlotAoPair1 * m_stride;
        dev->CreateShaderResourceView(m_aoResult.Get(), &rs, ah);
    }
    for (int i = 0; i < 2; ++i)
    {
        m_aoHist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_aoHist[i]))))
            return false;
        m_aoHistState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotAoPair0 : kSlotAoPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_aoHist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotAoHistUav0 : kSlotAoHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_aoHist[i].Get(), nullptr, &hu, uh);
    }
    m_aoHistValid = false;

    // ---- and again for the local lights (no wide pass) ----------------------
    m_lightResult.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_lightResult))))
        return false;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC lu = {};
        lu.Format = DXGI_FORMAT_R16_FLOAT;
        lu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightOut * m_stride;
        dev->CreateUnorderedAccessView(m_lightResult.Get(), nullptr, &lu, lh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC lcv = {};
        lcv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        lcv.Buffer.NumElements = 4;
        lcv.Buffer.StructureByteStride = sizeof(uint32_t);
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightCounters * m_stride;
        dev->CreateUnorderedAccessView(m_lightCounters.Get(), nullptr, &lcv, lh);

        D3D12_SHADER_RESOURCE_VIEW_DESC rs = {};
        rs.Format = DXGI_FORMAT_R16_FLOAT;
        rs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        rs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rs.Texture2D.MipLevels = 1;
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightPair0 * m_stride;
        dev->CreateShaderResourceView(m_lightResult.Get(), &rs, lh);
        lh = m_heap->GetCPUDescriptorHandleForHeapStart();
        lh.ptr += (SIZE_T)kSlotLightPair1 * m_stride;
        dev->CreateShaderResourceView(m_lightResult.Get(), &rs, lh);
    }
    for (int i = 0; i < 2; ++i)
    {
        m_lightHist[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd2,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                nullptr, IID_PPV_ARGS(&m_lightHist[i]))))
            return false;
        m_lightHistState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_SHADER_RESOURCE_VIEW_DESC hs = {};
        hs.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hs.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = m_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)((i == 0 ? kSlotLightPair0 : kSlotLightPair1) + 1) * m_stride;
        dev->CreateShaderResourceView(m_lightHist[i].Get(), &hs, sh);

        D3D12_UNORDERED_ACCESS_VIEW_DESC hu = {};
        hu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uh = m_heap->GetCPUDescriptorHandleForHeapStart();
        uh.ptr += (SIZE_T)(i == 0 ? kSlotLightHistUav0 : kSlotLightHistUav1) * m_stride;
        dev->CreateUnorderedAccessView(m_lightHist[i].Get(), nullptr, &hu, uh);
    }
    m_lightHistValid = false;

    // The AO's wide pass output: visibility only, read by the scene like the
    // histories (its SRV follows this format).
    m_aoWide.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_aoWide))))
        return false;
    m_aoWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC wu = {};
        wu.Format = DXGI_FORMAT_R16_FLOAT;
        wu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE wh = m_heap->GetCPUDescriptorHandleForHeapStart();
        wh.ptr += (SIZE_T)kSlotAoWideUav * m_stride;
        dev->CreateUnorderedAccessView(m_aoWide.Get(), nullptr, &wu, wh);
    }
    // And the local lights' (the same pass, the same format).
    m_lightWide.Reset();
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&m_lightWide))))
        return false;
    m_lightWideState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC wu = {};
        wu.Format = DXGI_FORMAT_R16_FLOAT;
        wu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE wh = m_heap->GetCPUDescriptorHandleForHeapStart();
        wh.ptr += (SIZE_T)kSlotLightWideUav * m_stride;
        dev->CreateUnorderedAccessView(m_lightWide.Get(), nullptr, &wu, wh);
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC cv = {};
    cv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    cv.Buffer.NumElements = 4;
    cv.Buffer.StructureByteStride = sizeof(uint32_t);
    h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)kSlotCounters * m_stride;
    dev->CreateUnorderedAccessView(m_counters.Get(), nullptr, &cv, h);

    m_width = width;
    m_height = height;
    m_resultReadable = false;
    return true;
}

void Rt::PrepareForRead(ID3D12GraphicsCommandList* list)
{
    // The filtered history is already left in PIXEL_SHADER_RESOURCE by the
    // denoiser; nothing to do. Kept so callers do not have to know that.
    (void)list;
    m_resultReadable = m_filtered != nullptr;
}

void Rt::Trace(Device& dev, ID3D12GraphicsCommandList* list, ID3D12Resource* depth,
               ID3D12Resource* normal, ID3D12Resource* motion, ID3D12Resource* plant,
               const float* viewProj, const float* unjitteredViewProj,
               const float* camPos, const float* sunTowards, float tMax, float sunAngle,
               bool ao, float aoRadius, uint32_t aoRays, UINT width, UINT height, const Lights* lights)
{
    if (!Ready() || !HasScene() || !m_heap || !depth || !normal || !viewProj) return;
    if ((width != m_width || height != m_height) && !CreateTargets(dev.device.Get(), width, height))
        return;

    TraceCB cb = {};
    if (!Invert(viewProj, cb.invViewProj)) return;
    for (int k = 0; k < 3; ++k) cb.sunTowards[k] = sunTowards[k];
    cb.tMax = tMax;
    cb.size[0] = width;
    cb.size[1] = height;
    // In METRES along the ray, not in shadow-map texels: there is no texel here,
    // and the reconstruction error this has to clear is a depth-buffer
    // quantisation, not a projection footprint.
    // In METRES along the normal. Small, because the normal offset clears the
    // depth quantisation far more efficiently than a step along the ray did;
    // the distance term in the shader grows it for the far field.
    cb.bias = 0.02f;
    cb.stride = m_vertexStride;
    for (int k = 0; k < 3; ++k) cb.camPos[k] = camPos ? camPos[k] : 0.0f;
    cb.sunAngle = sunAngle;
    cb.frame = m_frame++;
    cb.aoRadius = aoRadius;
    cb.aoRays = aoRays ? aoRays : 1;
    const bool doLights = lights && lights->Active() && m_psoLights && m_lightResult;
    if (lights)
    {
        for (int k = 0; k < 3; ++k) cb.camFwd[k] = lights->CamFwd()[k];
        cb.clusterParams[0] = lights->TileW();
        cb.clusterParams[1] = lights->TileH();
        cb.clusterParams[2] = Lights::kNear;
        cb.clusterParams[3] = lights->SliceScale();
    }
    cb.lightsOn = doLights ? 1.0f : 0.0f;

    // Depth becomes a NON-pixel shader resource: this is a compute dispatch, and
    // naming the wrong stage is a validation error rather than a wrong picture.
    D3D12_RESOURCE_BARRIER pre[4] = {
        Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(normal, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    UINT nPre = 2;
    if (motion) pre[nPre++] = Transition(motion, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (plant)  pre[nPre++] = Transition(plant, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(nPre, pre);

    // The motion view, or a null one (reads zero: nothing moved), so the
    // denoiser's table is never an unwritten descriptor.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC md = {};
        md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        md.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        md.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        md.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE mh = m_heap->GetCPUDescriptorHandleForHeapStart();
        mh.ptr += (SIZE_T)kSlotMotion * m_stride;
        dev.device->CreateShaderResourceView(motion, &md, mh);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8G8B8A8_SNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE nh = m_heap->GetCPUDescriptorHandleForHeapStart();
        nh.ptr += (SIZE_T)kSlotNormal * m_stride;
        dev.device->CreateShaderResourceView(normal, &nd, nh);
    }
    // The plant per pixel, or a null view (reads 0: no plants).
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC pd = {};
        pd.Format = DXGI_FORMAT_R32_UINT;
        pd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        pd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ph = m_heap->GetCPUDescriptorHandleForHeapStart();
        ph.ptr += (SIZE_T)kSlotPlantId * m_stride;
        dev.device->CreateShaderResourceView(plant, &pd, ph);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    dd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE dh = m_heap->GetCPUDescriptorHandleForHeapStart();
    dh.ptr += (SIZE_T)kSlotDepth * m_stride;
    dev.device->CreateShaderResourceView(depth, &dd, dh);

    list->CopyBufferRegion(m_counters.Get(), 0, m_countersZero.Get(), 0, 4 * sizeof(uint32_t));
    D3D12_RESOURCE_BARRIER toUav = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &toUav);


    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    list->SetPipelineState(m_pso.Get());
    auto gpuSlot = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootDescriptorTable(0, m_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotOut));
    list->SetComputeRoot32BitConstants(2, sizeof(TraceCB) / 4, &cb, 0);

    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

    // AMBIENT OCCLUSION, on the same acceleration structure and the same
    // surface reconstruction, into its own output. Timed together with the
    // shadow: one number for "what the ray tracing costs".
    const bool doAo = ao && m_psoAo && m_aoResult;
    // A frame without AO leaves its history describing an older camera: drop
    // it, so switching AO back on converges from fresh rather than smearing a
    // reprojection several frames stale.
    if (!doAo) m_aoHistValid = false;
    if (doAo)
    {
        list->SetPipelineState(m_psoAo.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotAoOut));
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);

    // LOCAL LIGHTS: one ray per pixel to one of them (kLightMainSrc), into their own output.
    if (!doLights) m_lightHistValid = false;
    if (doLights)
    {
        list->SetPipelineState(m_psoLights.Get());
        list->SetComputeRootDescriptorTable(1, gpuSlot(kSlotLightOut));
        list->SetComputeRootShaderResourceView(3, lights->LightsVa());
        list->SetComputeRootShaderResourceView(4, lights->ClustersVa());
        list->SetComputeRootShaderResourceView(5, lights->IndicesVa());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    if (m_timestamps) list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);

    // ---- denoise, once per feature ------------------------------------------
    DenoiseCB dc = {};
    {
        float inv[16];
        bool ok = unjitteredViewProj && Invert(unjitteredViewProj, inv);
        if (ok) MatMul(inv, m_prevViewProj, dc.reproj);
        dc.size[0] = width; dc.size[1] = height;
        dc.blend = 0.85f;
        dc.valid = ok ? 1.0f : 0.0f;
        dc.motionOn = motion ? 1.0f : 0.0f;
        // Where a moved object was hit: enough of the past to keep the
        // car's own shadow from boiling, little enough that a stale one is
        // gone in two or three frames (0.25^3 < 2%).
        dc.blendMoved = 0.25f;
        // A pixel still on the screen: three times the samples (see kDenoiseSrc's STILL).
        dc.blendStill = 0.95f;
        // The sun's and the lamps' samples are stratified over 4x4 tiles (StratU, StratDisc), as the AO's.
        dc.period4 = 1.0f;
        if (unjitteredViewProj) memcpy(m_prevViewProj, unjitteredViewProj, sizeof(m_prevViewProj));
    }
    Denoise(list, dc, m_result.Get(), kSlotPair0, kSlotPair1, kSlotHistUav0, kSlotHistUav1,
            m_hist, m_histState, m_histValid, m_filtered);
    DenoiseCB dcAo = dc;
    // AoSample's stratification. And the history stays 0.85: 0.92 was smoother
    // standing still but lagged - noisier and further from a 32-ray reference -
    // once the car (and the camera) drove.
    dcAo.period4 = 1.0f;
    if (doAo)
    {
        Denoise(list, dcAo, m_aoResult.Get(), kSlotAoPair0, kSlotAoPair1, kSlotAoHistUav0, kSlotAoHistUav1,
                m_aoHist, m_aoHistState, m_aoHistValid, m_filteredAo);
        if (m_psoAoWide && m_aoWide)
            Widen(list, dcAo, m_aoHist, m_aoHistState, kSlotAoPair0, kSlotAoPair1, m_aoWide.Get(), m_aoWideState,
                  kSlotAoWideUav, m_filteredAo);
    }
    else
        m_filteredAo = nullptr;
    // The local lights' fraction: denoised as the sun's visibility is (the same constants: it is the same kind of
    // signal, a binary coin per pixel whose mean is smooth).
    if (doLights)
        Denoise(list, dc, m_lightResult.Get(), kSlotLightPair0, kSlotLightPair1, kSlotLightHistUav0,
                kSlotLightHistUav1, m_lightHist, m_lightHistState, m_lightHistValid, m_filteredLights);
    // And widened where their history was cut, as the AO is: under the Garage's turning car, the floor's
    // spotlit shadow - one ray a pixel among three lamps, its rays' hits right above it allowing little
    // history (Allow) - stayed grainy, and those penumbrae are wider than the pass's 13 pixels anyway.
    if (doLights && m_psoAoWide && m_lightWide)
        Widen(list, dc, m_lightHist, m_lightHistState, kSlotLightPair0, kSlotLightPair1, m_lightWide.Get(),
              m_lightWideState, kSlotLightWideUav, m_filteredLights);
    else
        m_filteredLights = nullptr;
    if (m_timestamps)
    {
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 5,
                               m_timestampReadback.Get(), 0);
    }

    D3D12_RESOURCE_BARRIER toCopy = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &toCopy);
    list->CopyResource(m_countersReadback.Get(), m_counters.Get());
    D3D12_RESOURCE_BARRIER back = Transition(m_counters.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &back);

    D3D12_RESOURCE_BARRIER post[4] = {
        Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE),
        Transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    UINT nPost = 2;
    if (motion) post[nPost++] = Transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (plant)  post[nPost++] = Transition(plant, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(nPost, post);
}

// Spatial + temporal filter of one binary-per-frame ray result; see
// kDenoiseSrc. Generic over which pair of history textures it works on, so the
// shadow and the AO share the shader, the root signature and the reasoning.
void Rt::Denoise(ID3D12GraphicsCommandList* list, const DenoiseCB& base, ID3D12Resource* raw,
                 UINT pair0, UINT pair1, UINT uav0, UINT uav1,
                 Microsoft::WRL::ComPtr<ID3D12Resource>* hist, D3D12_RESOURCE_STATES* histState,
                 bool& histValid, ID3D12Resource*& filtered)
{
    const int write = (int)((m_frame - 1) & 1), read = write ^ 1;   // m_frame already advanced
    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(hist[write].Get(), histState[write], D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, pre);
    histState[write] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Readable by pixel shaders too: TAA compares this frame's AO with it (AoPrevDenoised).
    const D3D12_RESOURCE_STATES readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (histState[read] != readState)
    {
        D3D12_RESOURCE_BARRIER b = Transition(hist[read].Get(), histState[read], readState);
        list->ResourceBarrier(1, &b);
        histState[read] = readState;
    }

    DenoiseCB dc = base;
    dc.valid = (base.valid > 0.5f && histValid) ? 1.0f : 0.0f;

    auto gpu = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootSignature(m_denoiseRootSig.Get());
    list->SetPipelineState(m_psoDenoise.Get());
    list->SetComputeRootDescriptorTable(0, gpu(read == 0 ? pair0 : pair1));
    list->SetComputeRootDescriptorTable(1, gpu(kSlotDepth));
    list->SetComputeRootDescriptorTable(2, gpu(write == 0 ? uav0 : uav1));
    list->SetComputeRoot32BitConstants(3, sizeof(DenoiseCB) / 4, &dc, 0);
    list->SetComputeRootDescriptorTable(4, gpu(kSlotMotion));
    list->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(hist[write].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, post);
    histState[write] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    filtered = hist[write].Get();
    histValid = true;
}

// kAoWideSrc over this frame's denoised result (the history just written, of
// the pair pair0/pair1), into `wide`, which then is what the scene reads.
void Rt::Widen(ID3D12GraphicsCommandList* list, const DenoiseCB& dc,
               Microsoft::WRL::ComPtr<ID3D12Resource>* hists, D3D12_RESOURCE_STATES* histState,
               UINT pair0, UINT pair1, ID3D12Resource* wide, D3D12_RESOURCE_STATES& wideState, UINT uav,
               ID3D12Resource*& filtered)
{
    const int write = (int)((m_frame - 1) & 1);
    ID3D12Resource* hist = hists[write].Get();
    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(hist, histState[write], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(wide, wideState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(wideState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? 1 : 2, pre);

    auto gpu = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * m_stride;
        return h;
    };
    list->SetComputeRootSignature(m_denoiseRootSig.Get());
    list->SetPipelineState(m_psoAoWide.Get());
    // The pair whose t1 is the history just written.
    list->SetComputeRootDescriptorTable(0, gpu(write == 0 ? pair0 : pair1));
    list->SetComputeRootDescriptorTable(1, gpu(kSlotDepth));
    list->SetComputeRootDescriptorTable(2, gpu(uav));
    list->SetComputeRoot32BitConstants(3, sizeof(DenoiseCB) / 4, &dc, 0);
    list->SetComputeRootDescriptorTable(4, gpu(kSlotMotion));
    list->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(hist, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, histState[write]),
        Transition(wide, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, post);
    wideState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    filtered = wide;
}

void Rt::ReadStats()
{
    if (!m_countersReadback) return;

    uint32_t* c = nullptr;
    D3D12_RANGE all = { 0, 4 * sizeof(uint32_t) };
    if (SUCCEEDED(m_countersReadback->Map(0, &all, reinterpret_cast<void**>(&c))))
    {
        m_traced = c[0];
        m_shadowed = c[1];
        m_leafHits = c[2];
        m_leafMisses = c[3];
        D3D12_RANGE none = { 0, 0 };
        m_countersReadback->Unmap(0, &none);
    }

    if (!m_timestampReadback || !m_timestampHz) return;
    UINT64* t = nullptr;
    D3D12_RANGE ts = { 0, 5 * sizeof(UINT64) };
    if (SUCCEEDED(m_timestampReadback->Map(0, &ts, reinterpret_cast<void**>(&t))))
    {
        const double k = 1000.0 / (double)m_timestampHz;
        if (t[3] > t[0]) m_lastMs = k * (double)(t[3] - t[0]);
        m_aoMs      = t[2] >= t[1] ? k * (double)(t[2] - t[1]) : 0.0;
        m_lightsMs  = t[3] >= t[2] ? k * (double)(t[3] - t[2]) : 0.0;
        m_denoiseMs = t[4] >= t[3] ? k * (double)(t[4] - t[3]) : 0.0;
        D3D12_RANGE none = { 0, 0 };
        m_timestampReadback->Unmap(0, &none);
    }
}

} // namespace core
} // namespace ngg
