#include "post.h"
#include "log.h"
#include "shaders.h"

#include <d3dcompiler.h>
#include <string>
#include <math.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {



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

// Descriptor groups, ten wide so one root signature serves both passes.
// t0 is "the colour input", which differs per pass; t1/t2 are only read by TAA.
enum { kGroupTaa0 = 0, kGroupTaa1 = 1, kGroupTone0 = 2, kGroupTone1 = 3, kGroupToneHdr = 4, kGroupToneUp = 5 };
enum { kGroupSize = 10, kGroupCount = 6 };
const UINT kExposureCopyOffset = 512;   // gState[128]: D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
const DXGI_FORMAT kUpMotionFormat   = DXGI_FORMAT_R16G16_FLOAT;
const DXGI_FORMAT kSettledFormat    = DXGI_FORMAT_R8_UNORM;   // PSTaa's `settled`, beside the history
const DXGI_FORMAT kUpReactiveFormat = DXGI_FORMAT_R8_UNORM;   // colour, history, depth, motion, RT shadow, RT AO, its last

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
    // and last frame's, the glass's motion, the RT lights and the history's
    // `settled` at t4..t10 - t3 is the exposure state's root SRV - from the
    // group's fourth to tenth slots.
    D3D12_DESCRIPTOR_RANGE range[2] = {};
    range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range[0].NumDescriptors = 3;
    range[0].BaseShaderRegister = 0;
    range[0].OffsetInDescriptorsFromTableStart = 0;
    range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range[1].NumDescriptors = 7;
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

    ComPtr<ID3DBlob> vs = LoadShader("post.hlsl", "VSMain", "vs_5_1");
    ComPtr<ID3DBlob> psTaa = LoadShader("post.hlsl", "PSTaa", "ps_5_1");
    ComPtr<ID3DBlob> psTone = LoadShader("post.hlsl", "PSTonemap", "ps_5_1");
    ComPtr<ID3DBlob> psUpIn = LoadShader("post.hlsl", "PSUpscaleIn", "ps_5_1");
    if (!vs || !psTaa || !psTone || !psUpIn) return false;

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
    pd.NumRenderTargets = 2;
    pd.RTVFormats[1] = kSettledFormat;
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.IndependentBlendEnable = TRUE;
    const bool taaPso = SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoTaa)));
    pd.NumRenderTargets = 1;
    pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    pd.BlendState.IndependentBlendEnable = FALSE;
    if (!taaPso)
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

    pd.PS = { psUpIn->GetBufferPointer(), psUpIn->GetBufferSize() };
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = kUpMotionFormat;
    pd.RTVFormats[1] = kUpReactiveFormat;
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoUpIn))))
    {
        Log("post: upscaler inputs pipeline creation failed");
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

    ComPtr<ID3DBlob> csHist = LoadShader("meter.hlsl", "CSHistogram", "cs_5_1");
    ComPtr<ID3DBlob> csAdapt = LoadShader("meter.hlsl", "CSAdapt", "cs_5_1");
    if (!csHist || !csAdapt) return false;

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
        !buffer(kExposureCopyOffset + 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
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

bool PostFx::OnResize(ID3D12Device* dev, UINT width, UINT height, UINT outputWidth, UINT outputHeight)
{
    if (!width || !height || !outputWidth || !outputHeight) return false;

    m_width = width;
    m_height = height;
    m_outWidth = outputWidth;
    m_outHeight = outputHeight;
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
        D3D12_RESOURCE_DESC sdesc = desc;
        sdesc.Format = kSettledFormat;
        D3D12_CLEAR_VALUE sclear = {};
        sclear.Format = kSettledFormat;
        m_settled[i].Reset();
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sdesc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                &sclear, IID_PPV_ARGS(&m_settled[i]))))
        {
            Log("post: the history's companion %d creation failed", i);
            return false;
        }
    }

    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 7;   // + the upscaler's motion and reactive mask, + the two `settled`
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
    // The history's `settled`, at t10 (every group has one: the tonemap's read history 0's).
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ss = sd;
        ss.Format = kSettledFormat;
        for (int g = 0; g < kGroupCount; ++g)
            dev->CreateShaderResourceView(m_settled[g == kGroupTaa1 ? 1 : 0].Get(), &ss, slot(g, 9));
        D3D12_CPU_DESCRIPTOR_HANDLE srtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        srtv.ptr += (SIZE_T)5 * m_rtvStride;
        dev->CreateRenderTargetView(m_settled[0].Get(), nullptr, srtv);
        srtv.ptr += m_rtvStride;
        dev->CreateRenderTargetView(m_settled[1].Get(), nullptr, srtv);
    }

    // The upscaler's targets, and the upscaler sized to them.
    m_upReady = false;
    m_upMotion.Reset();
    m_upReactive.Reset();
    m_upExposure.Reset();
    m_upOutput.Reset();
    if (m_upscaler)
    {
        auto texture = [&](UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                           ComPtr<ID3D12Resource>& out) {
            D3D12_RESOURCE_DESC d = desc;
            d.Width = w;
            d.Height = h;
            d.Format = f;
            d.Flags = flags;
            return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                          IID_PPV_ARGS(&out)));
        };
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (texture(width, height, kUpMotionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, read, m_upMotion) &&
            texture(width, height, kUpReactiveFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, read, m_upReactive) &&
            texture(1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, read, m_upExposure) &&
            texture(outputWidth, outputHeight, kHdrFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_upOutput))
        {
            rtv.ptr += m_rtvStride;
            dev->CreateRenderTargetView(m_upMotion.Get(), nullptr, rtv);
            rtv.ptr += m_rtvStride;
            dev->CreateRenderTargetView(m_upReactive.Get(), nullptr, rtv);
            dev->CreateShaderResourceView(m_upOutput.Get(), &sd, slot(kGroupToneUp, 0));
            dev->CreateShaderResourceView(m_upOutput.Get(), &sd, slot(kGroupToneUp, 1));
            m_upReady = m_upscaler->Resize(dev, width, height, outputWidth, outputHeight);
            m_upPhases = m_upReady ? m_upscaler->JitterPhases(width, outputWidth) : 8;
        }
        if (!m_upReady) Log("post: the upscaler could not be set up - our own TAA, at the render size");
    }

    Log("post: %u x %u HDR + 2 history buffers%s", width, height, m_upReady ? ", upscaled" : "");
    if (m_upReady) Log("post: upscaled to %u x %u by %s, %d jitter phases", outputWidth, outputHeight,
                       m_upscaler->Name(), m_upPhases);
    return true;
}

void PostFx::Jitter(UINT width, UINT height, float* outX, float* outY) const
{
    if (!m_taa || !width || !height) { *outX = *outY = 0.0f; return; }

    if (Upscaling())
    {
        float px = 0.0f, py = 0.0f;   // render pixels, +y down
        m_upscaler->JitterOffset((int)(m_frame % (UINT)m_upPhases), m_upPhases, &px, &py);
        *outX =  2.0f * px / (float)width;
        *outY = -2.0f * py / (float)height;
        return;
    }

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

// The upscaler in place of PSTaa: its inputs at the render size (PSUpscaleIn), last frame's exposure into its 1x1
// texture, then the upscaler, into m_upOutput. The viewport is the render size on entry.
void PostFx::Upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion,
                     ID3D12Resource* glassMotion, const float* consts, D3D12_GPU_DESCRIPTOR_HANDLE taaGroup,
                     bool autoExposure)
{
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    {
        D3D12_RESOURCE_BARRIER pre[5];
        UINT n = 0;
        if (glassMotion)
            pre[n++] = Transition(glassMotion, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pre[n++] = Transition(m_upMotion.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pre[n++] = Transition(m_upReactive.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pre[n++] = Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
        if (motion)
            pre[n++] = Transition(motion, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(n, pre);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    rtvs[0] = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvs[0].ptr += (SIZE_T)3 * m_rtvStride;
    rtvs[1] = rtvs[0];
    rtvs[1].ptr += m_rtvStride;
    list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    list->SetPipelineState(m_psoUpIn.Get());
    list->SetGraphicsRootDescriptorTable(0, taaGroup);
    list->SetGraphicsRoot32BitConstants(1, 28, consts, 0);
    if (m_exposureState)
    {
        ExposureStateTo(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->SetGraphicsRootShaderResourceView(2, m_exposureState->GetGPUVirtualAddress());
    }
    list->DrawInstanced(3, 1, 0, 0);
    {
        D3D12_RESOURCE_BARRIER mid[6];
        UINT n = 0;
        if (glassMotion)
            mid[n++] = Transition(glassMotion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        mid[n++] = Transition(m_upMotion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, read);
        mid[n++] = Transition(m_upReactive.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, read);
        mid[n++] = Transition(m_hdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, read);
        mid[n++] = Transition(m_upOutput.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (motion)
            mid[n++] = Transition(motion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(n, mid);
    }

    // THE EXPOSURE the colour will be tonemapped with, so the upscaler's own tonemapped heuristics see the screen's
    // brightness: last frame's (the meter runs on the upscaled image, after this), copied from the state buffer's
    // byte 512. None (exposure 1) until the meter has run, and with a fixed exposure.
    ID3D12Resource* exposure = nullptr;
    if (autoExposure && m_curve >= kCurveExp && m_lastExposure > 0.0f)
    {
        ExposureStateTo(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_BARRIER b = Transition(m_upExposure.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_upExposure.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_exposureState.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = kExposureCopyOffset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = Transition(m_upExposure.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
        list->ResourceBarrier(1, &b);
        exposure = m_upExposure.Get();
    }

    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    float frameMs = 16.7f;
    if (m_upTick.QuadPart) frameMs = (float)(double(now.QuadPart - m_upTick.QuadPart) * 1000.0 / double(hz.QuadPart));
    m_upTick = now;
    if (frameMs <= 0.0f || frameMs > 100.0f) frameMs = 16.7f;

    UpscaleDispatch d;
    d.list = list;
    d.colour = m_hdr.Get();
    d.depth = depth;
    d.motion = m_upMotion.Get();
    d.reactive = m_upReactive.Get();
    d.exposure = exposure;
    d.output = m_upOutput.Get();
    d.renderWidth = m_width, d.renderHeight = m_height;
    d.outputWidth = m_outWidth, d.outputHeight = m_outHeight;
    // The jitter this frame was rendered with, back in render pixels (Jitter).
    d.jitterX =  consts[18] * 0.5f * (float)m_width;
    d.jitterY = -consts[19] * 0.5f * (float)m_height;
    d.frameMs = frameMs;
    d.cameraNear = m_near, d.cameraFar = m_far, d.fovY = m_fovY;
    d.reset = !m_historyValid;
    m_upscaler->Dispatch(d);

    D3D12_RESOURCE_BARRIER post[3];
    post[0] = Transition(depth, read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    post[1] = Transition(m_hdr.Get(), read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    post[2] = Transition(m_upOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(3, post);
}

void PostFx::Resolve(ID3D12GraphicsCommandList* list,
                     D3D12_CPU_DESCRIPTOR_HANDLE outRtv,
                     ID3D12Resource* depth,
                     ID3D12Resource* motion,
                     ID3D12Resource* glassMotion,
                     ID3D12Resource* rtShadow,
                     ID3D12Resource* rtAo,
                     ID3D12Resource* rtAoPrev,
                     ID3D12Resource* rtLights,
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
            h.ptr += m_srvStride;
            D3D12_SHADER_RESOURCE_VIEW_DESC gd = md;
            gd.Format = DXGI_FORMAT_R16G16_FLOAT;   // (Scene's kGlassMotionFormat)
            dev->CreateShaderResourceView(glassMotion, &gd, h);
            h.ptr += m_srvStride;
            dev->CreateShaderResourceView(rtLights, &md, h);
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
    UINT toneWidth = width, toneHeight = height;   // the image the tonemap reads
    if (taa && Upscaling())
    {
        Upscale(list, depth, motion, glassMotion, consts, gpuGroup(kGroupTaa0), autoExposure);
        // The upscaler bound its own heaps and root signature: ours again.
        list->SetGraphicsRootSignature(m_rootSig.Get());
        list->SetDescriptorHeaps(1, heaps);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        toneGroup = kGroupToneUp;
        toneWidth = m_outWidth, toneHeight = m_outHeight;
        m_historyValid = true;
    }
    else if (taa)
    {
        D3D12_RESOURCE_BARRIER pre[5];
        UINT np = 0;
        pre[np++] = Transition(m_history[writeIndex].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pre[np++] = Transition(m_settled[writeIndex].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pre[np++] = Transition(depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (motion)
            pre[np++] = Transition(motion,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (glassMotion)
            pre[np++] = Transition(glassMotion,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(np, pre);

        D3D12_CPU_DESCRIPTOR_HANDLE hrtv[2] = { m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                m_rtvHeap->GetCPUDescriptorHandleForHeapStart() };
        hrtv[0].ptr += (SIZE_T)(1 + writeIndex) * m_rtvStride;
        hrtv[1].ptr += (SIZE_T)(5 + writeIndex) * m_rtvStride;
        list->OMSetRenderTargets(2, hrtv, FALSE, nullptr);
        list->SetPipelineState(m_psoTaa.Get());
        list->SetGraphicsRootDescriptorTable(0, gpuGroup(readIndex == 0 ? kGroupTaa0 : kGroupTaa1));
        list->SetGraphicsRoot32BitConstants(1, 28, consts, 0);
        if (m_exposureState)
        {
            ExposureStateTo(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->SetGraphicsRootShaderResourceView(2, m_exposureState->GetGPUVirtualAddress());
        }
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER post[5];
        UINT nq = 0;
        post[nq++] = Transition(m_history[writeIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        post[nq++] = Transition(m_settled[writeIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        post[nq++] = Transition(depth,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (motion)
            post[nq++] = Transition(motion,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (glassMotion)
            post[nq++] = Transition(glassMotion,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(nq, post);

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
        Meter(list, (toneGroup == kGroupToneHdr) ? m_hdr.Get() : (toneGroup == kGroupToneUp) ? m_upOutput.Get()
                    : m_history[(toneGroup == kGroupTone0) ? 0 : 1].Get(),
              gpuGroup(toneGroup), toneWidth, toneHeight);
    else
        m_snapExposure = true;     // whatever it held is stale when it resumes

    // The tonemap writes the OUTPUT size, whatever it reads (an upscaler's image, or the render-size one stretched
    // when the upscaler does not run); its sharpen steps by the texels of what it reads.
    if (m_outWidth != width || m_outHeight != height)
    {
        const D3D12_VIEWPORT ovp = { 0.0f, 0.0f, (float)m_outWidth, (float)m_outHeight, 0.0f, 1.0f };
        const D3D12_RECT oscissor = { 0, 0, (LONG)m_outWidth, (LONG)m_outHeight };
        list->RSSetViewports(1, &ovp);
        list->RSSetScissorRects(1, &oscissor);
    }
    consts[16] = 1.0f / (float)toneWidth;
    consts[17] = 1.0f / (float)toneHeight;

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
