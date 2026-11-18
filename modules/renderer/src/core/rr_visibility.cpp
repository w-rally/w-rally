#include "rr_visibility.h"
#include "log.h"
#include "pipeline_cache.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// rr_guides.hlsl's constants.
struct RrCB
{
    float    invViewProj[16];
    float    camPos[3];
    float    aoOn;
    uint32_t size[2];
    float    lightsOn;
    float    glassOn;
    float    biasScale;
    float    guideMode;
    float    pad[2];
};
static_assert(sizeof(RrCB) % 16 == 0, "RrCB must be whole float4s");

// Each pass's descriptors in the heap: t0..t7, then u0..u7. Decode's set, then the guides'.
const UINT kSrvs = 8, kUavs = 8, kSet = kSrvs + kUavs;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t = {};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    t.Transition.StateBefore = a;
    t.Transition.StateAfter = b;
    return t;
}

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

} // namespace

bool RrVisibility::Create(Device& dev)
{
    if (Ready()) return true;
    ID3D12Device* device = dev.device.Get();
    D3D12_DESCRIPTOR_RANGE r[2] = {};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = kSrvs;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = kUavs;
    r[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2;
    rp[0].DescriptorTable.pDescriptorRanges = r;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = sizeof(RrCB) / 4;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = rp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(CreateRootSignature(device, 0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_root))))
    {
        Log("rr: root signature failed - no Ray Reconstruction");
        return false;
    }
    ComPtr<ID3DBlob> dc = LoadShader("rr_guides.hlsl", "CSDecode", "cs_6_0");
    ComPtr<ID3DBlob> gc = LoadShader("rr_guides.hlsl", "CSGuides", "cs_6_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = m_root.Get();
    ComPtr<ID3D12PipelineState> decode, guides;
    if (dc && gc)
    {
        cd.CS = { dc->GetBufferPointer(), dc->GetBufferSize() };
        CreatePipeline(device, &cd, IID_PPV_ARGS(&decode));
        cd.CS = { gc->GetBufferPointer(), gc->GetBufferSize() };
        CreatePipeline(device, &cd, IID_PPV_ARGS(&guides));
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * kSet;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!decode || !guides || FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap))))
    {
        Log("rr: its passes (rr_guides.hlsl) failed - no Ray Reconstruction");
        return false;
    }
    m_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_decode = decode;
    m_guides = guides;
    return true;
}

bool RrVisibility::Resize(ID3D12Device* device, UINT width, UINT height)
{
    auto make = [&](Tex& t, DXGI_FORMAT f) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        t.r.Reset();
        t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state, nullptr,
                                                         IID_PPV_ARGS(&t.r)));
    };
    m_width = m_height = 0;
    if (!make(m_sun, DXGI_FORMAT_R16_FLOAT) || !make(m_ao, DXGI_FORMAT_R16_FLOAT) ||
        !make(m_lights, DXGI_FORMAT_R16_FLOAT) || !make(m_moved, DXGI_FORMAT_R8_UNORM) ||
        !make(m_diffuse, DXGI_FORMAT_R8G8B8A8_UNORM) || !make(m_specular, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !make(m_normal, DXGI_FORMAT_R16G16B16A16_FLOAT) || !make(m_bias, DXGI_FORMAT_R8_UNORM))
        return false;
    m_width = width;
    m_height = height;
    return true;
}

void RrVisibility::Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out)
{
    if (!Ready()) return;
    ID3D12Device* device = dev.device.Get();
    float sunTowards[3];
    float len = sqrtf(f.sunTravel[0] * f.sunTravel[0] + f.sunTravel[1] * f.sunTravel[1] + f.sunTravel[2] * f.sunTravel[2]);
    if (len < 1e-6f) len = 1.0f;
    for (int k = 0; k < 3; ++k) sunTowards[k] = -f.sunTravel[k] / len;

    // The traces as RtVisibility's (visibility.cpp), none of our denoisers after them.
    const bool traced = f.ao && m_aoSource == AoSource::Traced && m_rt.AoAvailable();
    m_rt.SetNrdInputs(false);
    m_rt.SetRawOnly(true);
    m_rt.Trace(dev, list, f.depth, f.normal, f.motion, f.plant, f.viewProjJittered, f.viewProj, f.camPos, sunTowards,
               4000.0f, f.sunAngle, traced, 3.0f, f.aoRays, f.width, f.height, f.lights);
    if (!m_rt.Result()) return;
    if ((f.width != m_width || f.height != m_height) && !Resize(device, f.width, f.height))
    {
        Log("rr: targets %ux%u failed", f.width, f.height);
        return;
    }
    const bool doAo = traced && m_rt.AoTraced();
    const bool doLights = m_rt.LightsTraced();

    RrCB cb = {};
    cb.size[0] = f.width;
    cb.size[1] = f.height;
    cb.aoOn = doAo ? 1.0f : 0.0f;
    cb.lightsOn = doLights ? 1.0f : 0.0f;

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES both = read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* raws[3] = { m_rt.Result(), m_rt.RawAo(), m_rt.RawLights() };
    Tex* outs[4] = { &m_sun, &m_ao, &m_lights, &m_moved };
    D3D12_RESOURCE_BARRIER b[7];
    UINT n = 0;
    for (ID3D12Resource* r : raws) b[n++] = Transition(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    for (Tex* t : outs)
        if (t->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            b[n++] = Transition(t->r.Get(), t->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            t->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    list->ResourceBarrier(n, b);

    auto at = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * m_stride;
        return h;
    };
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f2, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = f2;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &v, at(slot));
    };
    auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f2, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = f2;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &u, at(slot));
    };
    for (UINT k = 0; k < 3; ++k) srv(raws[k], DXGI_FORMAT_R16_FLOAT, k);
    for (UINT k = 3; k < kSrvs; ++k) srv(nullptr, DXGI_FORMAT_R16_FLOAT, k);
    uav(m_sun.r.Get(), DXGI_FORMAT_R16_FLOAT, kSrvs + 0);
    uav(m_ao.r.Get(), DXGI_FORMAT_R16_FLOAT, kSrvs + 1);
    uav(m_lights.r.Get(), DXGI_FORMAT_R16_FLOAT, kSrvs + 2);
    uav(m_moved.r.Get(), DXGI_FORMAT_R8_UNORM, kSrvs + 3);
    for (UINT k = 4; k < kUavs; ++k) uav(nullptr, DXGI_FORMAT_R8_UNORM, kSrvs + k);

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root.Get());
    list->SetPipelineState(m_decode.Get());
    list->SetComputeRootDescriptorTable(0, m_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, sizeof(RrCB) / 4, &cb, 0);
    list->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);

    n = 0;
    for (ID3D12Resource* r : raws) b[n++] = Transition(r, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (Tex* t : outs)
    {
        b[n++] = Transition(t->r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, both);
        t->state = both;
    }
    list->ResourceBarrier(n, b);

    out.shadowSource = 1;
    out.sun = m_sun.r.Get();
    if (doLights) out.lights = m_lights.r.Get();
    if (doAo)
    {
        out.ao = m_ao.r.Get();
        snprintf(m_aoName, sizeof m_aoName, "ray traced, %u ray%s, raw for RR", f.aoRays, f.aoRays == 1 ? "" : "s");
        out.aoSource = m_aoName;
        out.aoMs = m_rt.AoMs();
    }
    else if (m_aoSource == AoSource::ScreenSpace)
        ScreenSpaceAo(m_gtao, list, f, out);
}

bool RrVisibility::RecordGuides(Device& dev, ID3D12GraphicsCommandList* list, ID3D12Resource* gbufNormal,
                                ID3D12Resource* gbufAlbedo, ID3D12Resource* depth, ID3D12Resource* glassMotion,
                                const float* viewProjJittered, const float* camPos, const float* view,
                                const float* proj, UINT width, UINT height, bool traced, UpscaleGuides& out)
{
    if (!Ready() || !gbufNormal || !gbufAlbedo || !depth || !viewProjJittered) return false;
    ID3D12Device* device = dev.device.Get();
    if ((width != m_width || height != m_height) && !Resize(device, width, height)) return false;

    // Our moved-object mask in RR's bias-current-colour mask only with NGG_RR_BIAS=1: RR keeps the road behind a driving
    // car clean on its own (the final image's ghost, user-42.phr: -0.48 / -0.70 grey without it, -0.82 / -0.70 with
    // it; native TAA with our denoiser -0.47 / -0.31) - moving glass's reactive value stays.
    static const float biasScale = [] { const char* e = getenv("NGG_RR_BIAS"); return e && e[0] == '1' ? 1.0f : 0.0f; }();
    RrCB cb = {};
    if (!Invert(viewProjJittered, cb.invViewProj)) return false;
    for (int k = 0; k < 3; ++k) cb.camPos[k] = camPos ? camPos[k] : 0.0f;
    cb.size[0] = width;
    cb.size[1] = height;
    cb.glassOn = glassMotion ? 1.0f : 0.0f;
    cb.biasScale = traced ? biasScale : 0.0f;
    // NGG_RR_GUIDES=nospec|white: the specular albedo 0, or the diffuse albedo 1 (A/B of what RR makes of our guides).
    static const float guideMode = [] {
        const char* e = getenv("NGG_RR_GUIDES");
        return !e ? 0.0f : !strcmp(e, "nospec") ? 1.0f : !strcmp(e, "white") ? 2.0f : 0.0f;
    }();
    cb.guideMode = guideMode;

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES both = read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Tex* outs[4] = { &m_diffuse, &m_specular, &m_normal, &m_bias };
    D3D12_RESOURCE_BARRIER b[10];
    UINT n = 0;
    b[n++] = Transition(gbufNormal, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    b[n++] = Transition(gbufAlbedo, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    b[n++] = Transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
    if (glassMotion) b[n++] = Transition(glassMotion, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    if (m_moved.state != both && m_moved.r)
    {
        b[n++] = Transition(m_moved.r.Get(), m_moved.state, both);
        m_moved.state = both;
    }
    for (Tex* t : outs)
        if (t->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            b[n++] = Transition(t->r.Get(), t->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            t->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    list->ResourceBarrier(n, b);

    auto at = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(kSet + slot) * m_stride;
        return h;
    };
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f2, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = f2;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &v, at(slot));
    };
    auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f2, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = f2;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &u, at(slot));
    };
    for (UINT k = 0; k < 3; ++k) srv(nullptr, DXGI_FORMAT_R16_FLOAT, k);
    srv(gbufNormal, DXGI_FORMAT_R10G10B10A2_UNORM, 3);
    srv(gbufAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4);     // read linear
    srv(depth, DXGI_FORMAT_R32_FLOAT, 5);
    srv(glassMotion, DXGI_FORMAT_R16G16_FLOAT, 6);
    srv(m_moved.r.Get(), DXGI_FORMAT_R8_UNORM, 7);
    for (UINT k = 0; k < 4; ++k) uav(nullptr, DXGI_FORMAT_R16_FLOAT, kSrvs + k);
    uav(m_diffuse.r.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvs + 4);
    uav(m_specular.r.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvs + 5);
    uav(m_normal.r.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvs + 6);
    uav(m_bias.r.Get(), DXGI_FORMAT_R8_UNORM, kSrvs + 7);

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root.Get());
    list->SetPipelineState(m_guides.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = m_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)kSet * m_stride;
    list->SetComputeRootDescriptorTable(0, g);
    list->SetComputeRoot32BitConstants(1, sizeof(RrCB) / 4, &cb, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    n = 0;
    b[n++] = Transition(gbufNormal, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    b[n++] = Transition(gbufAlbedo, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    b[n++] = Transition(depth, read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (glassMotion) b[n++] = Transition(glassMotion, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (Tex* t : outs)
    {
        b[n++] = Transition(t->r.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, both);
        t->state = both;
    }
    list->ResourceBarrier(n, b);

    out.diffuseAlbedo = m_diffuse.r.Get();
    out.specularAlbedo = m_specular.r.Get();
    out.normalRoughness = m_normal.r.Get();
    out.bias = m_bias.r.Get();
    memcpy(out.view, view, sizeof out.view);
    memcpy(out.proj, proj, sizeof out.proj);
    return true;
}

} // namespace core
} // namespace ngg
