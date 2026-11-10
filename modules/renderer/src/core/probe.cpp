#include "probe.h"
#include "log.h"
#include "shaders.h"

#include <d3dcompiler.h>
#include <string.h>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// The faces' axes, in the order FaceView numbers them; the shader below keeps the same table.
const float kForward[6][3] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
const float kUp[6][3]      = { {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0} };

ComPtr<ID3DBlob> CompileCs(const char* entry)
{
    return LoadShader("probe.hlsl", entry, "cs_5_1");
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

} // namespace

bool ReflectionProbe::Create(ID3D12Device* dev)
{
    m_dev = dev;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kFace;
    rd.Height = kFace;
    rd.DepthOrArraySize = 6;
    rd.MipLevels = 1;
    rd.Format = kFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = kFormat;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            &cv, IID_PPV_ARGS(&m_faces))))
        return false;
    rd.Format = DXGI_FORMAT_D32_FLOAT;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_CLEAR_VALUE dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.DepthStencil.Depth = 0.0f;   // reverse-Z
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv,
                                            IID_PPV_ARGS(&m_depth))))
        return false;
    rd.Width = kOct;
    rd.Height = kOct;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = kOctMips;
    rd.Format = kFormat;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                            nullptr, IID_PPV_ARGS(&m_oct))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 6;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 + 2 * kOctMips;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;
    m_rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT f = 0; f < 6; ++f)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = kFormat;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = f;
        rv.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)f * m_rtvStride;
        dev->CreateRenderTargetView(m_faces.Get(), &rv, h);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsd = {};
        dsd.Format = DXGI_FORMAT_D32_FLOAT;
        dsd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsd.Texture2DArray.FirstArraySlice = f;
        dsd.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE d = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        d.ptr += (SIZE_T)f * m_dsvStride;
        dev->CreateDepthStencilView(m_depth.Get(), &dsd, d);
    }
    // The faces (t0); the env (t1) is written at each Resolve; per mip m the pair {u0 dst = m, u1 src = m - 1}.
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = kFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 6;
    dev->CreateShaderResourceView(m_faces.Get(), &sd, h);
    for (UINT m = 0; m < kOctMips; ++m)
        for (UINT k = 0; k < 2; ++k)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = kFormat;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = k == 0 ? m : (m ? m - 1 : 0);
            D3D12_CPU_DESCRIPTOR_HANDLE u = h;
            u.ptr += (SIZE_T)(2 + 2 * m + k) * m_srvStride;
            dev->CreateUnorderedAccessView(m_oct.Get(), nullptr, &ud, u);
        }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;                       // t0 faces, t1 env
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;                       // u0 dst, u1 src
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    for (int k = 0; k < 2; ++k)
    {
        params[1 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + k].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + k].DescriptorTable.pDescriptorRanges = &ranges[k];
    }
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
        return false;
    ComPtr<ID3DBlob> resolve = CompileCs("CSResolve"), down = CompileCs("CSDown");
    if (!resolve || !down) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {};
    cp.pRootSignature = m_rootSig.Get();
    cp.CS = { down->GetBufferPointer(), down->GetBufferSize() };
    if (FAILED(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&m_psoDown)))) return false;
    cp.CS = { resolve->GetBufferPointer(), resolve->GetBufferSize() };
    return SUCCEEDED(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&m_psoResolve)));
}

void ReflectionProbe::FaceView(int face, const float c[3], float v[16])
{
    const float* F = kForward[face];
    const float* U = kUp[face];
    const float R[3] = { U[1] * F[2] - U[2] * F[1], U[2] * F[0] - U[0] * F[2], U[0] * F[1] - U[1] * F[0] };
    for (int r = 0; r < 3; ++r)
    {
        v[r * 4 + 0] = R[r];
        v[r * 4 + 1] = U[r];
        v[r * 4 + 2] = F[r];
        v[r * 4 + 3] = 0.0f;
    }
    v[12] = -(c[0] * R[0] + c[1] * R[1] + c[2] * R[2]);
    v[13] = -(c[0] * U[0] + c[1] * U[1] + c[2] * U[2]);
    v[14] = -(c[0] * F[0] + c[1] * F[1] + c[2] * F[2]);
    v[15] = 1.0f;
}

void ReflectionProbe::FaceProj(float n, float f, float p[16])
{
    memset(p, 0, 16 * sizeof(float));
    p[0] = 1.0f;                    // cot(45 degrees), aspect 1
    p[5] = 1.0f;
    p[10] = f / (f - n);
    p[11] = 1.0f;
    p[14] = -n * f / (f - n);
}

void ReflectionProbe::BeginFace(ID3D12GraphicsCommandList* list, int face, D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
                                D3D12_CPU_DESCRIPTOR_HANDLE& dsv)
{
    const D3D12_RESOURCE_BARRIER b = Transition(m_faces.Get(), (UINT)face, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)face * m_rtvStride;
    dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += (SIZE_T)face * m_dsvStride;
    const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    list->ClearRenderTargetView(rtv, none, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
}

void ReflectionProbe::EndFace(ID3D12GraphicsCommandList* list, int face)
{
    const D3D12_RESOURCE_BARRIER b = Transition(m_faces.Get(), (UINT)face, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
}

void ReflectionProbe::Resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* env)
{
    if (!Ready() || !env) return;
    D3D12_CPU_DESCRIPTOR_HANDLE eh = m_heap->GetCPUDescriptorHandleForHeapStart();
    eh.ptr += m_srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = env->GetDesc().Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = env->GetDesc().MipLevels;
    m_dev->CreateShaderResourceView(env, &sd, eh);

    // The environment is left for the pixel shaders (env.h): readable here too while this runs.
    D3D12_RESOURCE_BARRIER b[2] = {
        Transition(m_oct.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(env, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, b);
    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSig.Get());
    const D3D12_GPU_DESCRIPTOR_HANDLE g = m_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(1, g);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_oct.Get();
    for (UINT m = 0; m < kOctMips; ++m)
    {
        const UINT size = kOct >> m;
        const UINT cb[4] = { size, 0, 0, 0 };
        list->SetPipelineState(m ? m_psoDown.Get() : m_psoResolve.Get());
        list->SetComputeRoot32BitConstants(0, 4, cb, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE u = g;
        u.ptr += (UINT64)(2 + 2 * m) * m_srvStride;
        list->SetComputeRootDescriptorTable(2, u);
        list->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
        list->ResourceBarrier(1, &uav);
    }
    for (D3D12_RESOURCE_BARRIER& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    list->ResourceBarrier(2, b);
}

void ReflectionProbe::CreateSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = kFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = kOctMips;
    dev->CreateShaderResourceView(m_oct.Get(), &sd, dest);
}

} // namespace core
} // namespace ngg
