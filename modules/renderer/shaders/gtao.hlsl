// XeGTAO (Intel, MIT, third_party/xegtao unchanged) with this renderer's registers and entry points (core/gtao.cpp).
//
// Our entry points, after XeGTAO.h and before XeGTAO.hlsli (Record builds the whole source). They are Intel's sample
// ones (third_party/xegtao/vaGTAO.hlsl) with this renderer's inputs: the prepass's normal and the frame's noise.
// FP32 depths, which XeGTAO supports only at full precision. VA_SATURATE is Intel's framework's (vaShaderCore.h), which
// XeGTAO.hlsli uses unasked. XE_GTAO_USE_DEFAULT_CONSTANTS 0: the thin-occluder compensation is ours to set.
// GTAO_SLICES / GTAO_STEPS: the quality (gtao.cpp kSlices, kSteps); XE_GTAO_GENERATE_NORMALS_INPLACE: depth normals.
// XeGTAO.hlsli includes XeGTAO.h again: its guard makes that nothing.
#define XE_GTAO_FP32_DEPTHS 1
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define VA_SATURATE saturate
#include "../../../third_party/xegtao/XeGTAO.h"

cbuffer GtaoCB : register(b0)
{
    GTAOConstants g_GTAOConsts;
    float4        gViewRow[3];     // the view's rotation, row-vector convention: world -> view = mul(n, (float3x3)V)
};
SamplerState g_samplerPointClamp : register(s0);
#include "../../../third_party/xegtao/XeGTAO.hlsli"

// pass 1 (XeGTAO_PrefilterDepths16x16): the reverse-Z depth -> view-space depth, 5 MIPs
Texture2D<float>        g_srcRawDepth         : register(t0);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP0 : register(u0);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP1 : register(u1);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP2 : register(u2);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP3 : register(u3);
RWTexture2D<lpfloat>    g_outWorkingDepthMIP4 : register(u4);
// pass 2 (XeGTAO_MainPass)
Texture2D<lpfloat>      g_srcWorkingDepth     : register(t0);
Texture2D<float4>       g_srcNormal           : register(t1);   // the prepass's: world space, R8G8B8A8_SNORM
RWTexture2D<uint>       g_outWorkingAOTerm    : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges    : register(u1);
// pass 3 (XeGTAO_Denoise, one pass: the last)
Texture2D<uint>         g_srcWorkingAOTerm    : register(t0);
Texture2D<lpfloat>      g_srcWorkingEdges     : register(t1);
RWTexture2D<uint>       g_outFinalAOTerm      : register(u0);

// The prepass's world normal in XeGTAO's view space. The sky (cleared to 0) and anything degenerate face the camera.
lpfloat3 LoadNormal(int2 pos)
{
    const float3 w = g_srcNormal.Load(int3(pos, 0)).xyz;
    if (dot(w, w) < 0.25) return lpfloat3(0, 0, -1);
    const float3 v = float3(dot(w, float3(gViewRow[0].x, gViewRow[1].x, gViewRow[2].x)),
                            dot(w, float3(gViewRow[0].y, gViewRow[1].y, gViewRow[2].y)),
                            dot(w, float3(gViewRow[0].z, gViewRow[1].z, gViewRow[2].z)));
    return (lpfloat3)normalize(v);
}

// Hilbert curve driving R2 (vaGTAO.hlsl's), the index walking with the frame: 0 when the noise stands still.
lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x % 64, pixCoord.y % 64);
    index += 288 * (temporalIndex % 64);
    return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

[numthreads(8, 8, 1)]
void CSPrefilterDepths16x16(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp,
                                g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2,
                                g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAO(const uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, GTAO_SLICES, GTAO_STEPS, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
                    LoadNormal(pixCoord), g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoisePass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLastPass(const uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);   // two horizontal pixels a thread
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, true);
}
