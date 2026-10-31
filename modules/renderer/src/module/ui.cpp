#include "module/ui.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace wrally::render {

namespace {

const char kShader[] = R"(
cbuffer Screen : register(b0) { float2 scale; float2 offset; };   // pixels -> clip: p * scale + offset
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD; float4 colour : COLOR; };
struct PsIn { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 colour : COLOR; };
PsIn VSMain(VsIn v)
{
    PsIn o;
    o.pos = float4(v.pos * scale + offset, 0, 1);
    o.uv = v.uv;
    o.colour = v.colour;
    return o;
}
float4 PSMain(PsIn i) : SV_Target { return i.colour * tex.Sample(samp, i.uv); }
)";

bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& error)
{
    ComPtr<ID3DBlob> errors;
    if (SUCCEEDED(D3DCompile(kShader, sizeof kShader - 1, "wrally_ui", nullptr, nullptr, entry, target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors)))
        return true;
    error = std::string("ui: shader ") + entry + ": " +
            (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?");
    return false;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

ComPtr<ID3D12Resource> UploadBuffer(ID3D12Device* device, uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

} // namespace

bool UiPass::Create(ngg::core::Device& dev, DXGI_FORMAT target, std::string& error)
{
    ID3D12Device* device = dev.device.Get();

    // Root: the screen transform (4 constants, b0), the texture (a one-SRV table, t0), a linear clamp sampler (s0).
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root))))
        return error = "ui: root signature", false;

    ComPtr<ID3DBlob> vs, ps;
    if (!Compile("VSMain", "vs_5_0", vs, error) || !Compile("PSMain", "ps_5_0", ps, error)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {layout, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = target;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)))) return error = "ui: pipeline state", false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) return error = "ui: descriptor heap", false;
    m_srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Slot 0: white, for untextured commands.
    m_textures.clear();
    const uint32_t white = 0xffffffffu;
    return Make(dev, &white, 1, 1, 0, error);
}

uint32_t UiPass::AddTexture(ngg::core::Device& dev, const void* rgba, uint32_t width, uint32_t height, std::string& error)
{
    if (!rgba || !width || !height) return error = "ui texture: empty", 0;
    uint32_t slot = 1;
    while (slot < m_textures.size() && m_textures[slot]) ++slot;
    if (slot >= kSlots) return error = "ui texture: all slots taken", 0;
    return Make(dev, rgba, width, height, slot, error) ? slot : 0;
}

bool UiPass::Make(ngg::core::Device& dev, const void* rgba, uint32_t width, uint32_t height, uint32_t slot,
                  std::string& error)
{
    ID3D12Device* device = dev.device.Get();

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&tex))))
        return error = "ui texture: creation failed", 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &foot, nullptr, nullptr, &bytes);
    ComPtr<ID3D12Resource> upload = UploadBuffer(device, bytes);
    if (!upload) return error = "ui texture: upload buffer", 0;
    uint8_t* p = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return error = "ui texture: map", 0;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(p + foot.Offset + size_t(y) * foot.Footprint.RowPitch, static_cast<const uint8_t*>(rgba) + size_t(y) * width * 4,
                    size_t(width) * 4);
    upload->Unmap(0, nullptr);

    if (!dev.Begin()) return error = "ui texture: command list would not open", 0;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = foot;
    dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER b = Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dev.list->ResourceBarrier(1, &b);
    dev.EndAndWait();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * m_srvStride;
    device->CreateShaderResourceView(tex.Get(), &sd, h);
    if (slot >= m_textures.size()) m_textures.resize(slot + 1);
    m_textures[slot] = tex;
    return true;
}

void UiPass::ReleaseTexture(uint32_t slot)
{
    if (slot > 0 && slot < m_textures.size()) m_textures[slot].Reset();   // (the GPU is idle between frames)
}

bool UiPass::UpdateTexture(ngg::core::Device& dev, uint32_t slot, const void* rgba, std::string& error)
{
    if (slot == 0 || slot >= m_textures.size() || !m_textures[slot]) return error = "ui texture: no such texture", false;
    ID3D12Device* device = dev.device.Get();
    ID3D12Resource* tex = m_textures[slot].Get();
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    const uint32_t width = static_cast<uint32_t>(td.Width), height = td.Height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &foot, nullptr, nullptr, &bytes);
    if (bytes > m_updateBytes) {
        m_update = UploadBuffer(device, bytes);
        m_updateBytes = m_update ? bytes : 0;
        if (!m_update) return error = "ui texture: upload buffer", false;
    }
    uint8_t* p = nullptr;
    if (FAILED(m_update->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return error = "ui texture: map", false;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(p + foot.Offset + size_t(y) * foot.Footprint.RowPitch, static_cast<const uint8_t*>(rgba) + size_t(y) * width * 4,
                    size_t(width) * 4);
    m_update->Unmap(0, nullptr);

    if (!dev.Begin()) return error = "ui texture: command list would not open", false;
    D3D12_RESOURCE_BARRIER b = Transition(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    dev.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = m_update.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = foot;
    dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dev.list->ResourceBarrier(1, &b);
    dev.EndAndWait();
    return true;
}

bool UiPass::Upload(ngg::core::Device& dev, const iface::UiDrawList& ui)
{
    const uint64_t vb = uint64_t(ui.vertexCount) * sizeof(iface::UiVertex), ib = uint64_t(ui.indexCount) * 4;
    if (vb + ib > m_bufferBytes) {
        m_bufferBytes = std::max<uint64_t>((vb + ib) * 3 / 2, 1u << 20);
        m_buffer = UploadBuffer(dev.device.Get(), m_bufferBytes);
        if (!m_buffer) return m_bufferBytes = 0, false;
    }
    uint8_t* p = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (FAILED(m_buffer->Map(0, &none, reinterpret_cast<void**>(&p)))) return false;
    std::memcpy(p, ui.vertices, vb);
    std::memcpy(p + vb, ui.indices, ib);
    m_buffer->Unmap(0, nullptr);
    return true;
}

void UiPass::Record(ngg::core::Device& dev, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height,
                    const iface::UiDrawList& ui)
{
    if (!ui.commandCount || !ui.vertexCount || !ui.indexCount || !ui.vertices || !ui.indices || !ui.commands) return;
    if (!m_pso || !Upload(dev, ui)) return;
    ID3D12GraphicsCommandList* list = dev.list.Get();
    const uint64_t vb = uint64_t(ui.vertexCount) * sizeof(iface::UiVertex);
    D3D12_VERTEX_BUFFER_VIEW vbv = {m_buffer->GetGPUVirtualAddress(), static_cast<UINT>(vb), sizeof(iface::UiVertex)};
    D3D12_INDEX_BUFFER_VIEW ibv = {m_buffer->GetGPUVirtualAddress() + vb, ui.indexCount * 4u, DXGI_FORMAT_R32_UINT};
    const D3D12_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
    const float screen[4] = {2.0f / float(width), -2.0f / float(height), -1.0f, 1.0f};

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &vp);
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetPipelineState(m_pso.Get());
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRoot32BitConstants(0, 4, screen, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetIndexBuffer(&ibv);
    const D3D12_GPU_DESCRIPTOR_HANDLE base = m_heap->GetGPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < ui.commandCount; ++i) {
        const iface::UiCommand& c = ui.commands[i];
        if (!c.indexCount || c.firstIndex + uint64_t(c.indexCount) > ui.indexCount) continue;
        const LONG x0 = std::max(0L, LONG(c.clip[0])), y0 = std::max(0L, LONG(c.clip[1]));
        const LONG x1 = std::min(LONG(width), LONG(c.clip[2])), y1 = std::min(LONG(height), LONG(c.clip[3]));
        if (x1 <= x0 || y1 <= y0) continue;
        const D3D12_RECT scissor = {x0, y0, x1, y1};
        list->RSSetScissorRects(1, &scissor);
        const uint32_t named = (c.texture & kTextureBit) ? c.texture & ~kTextureBit : 0;
        const uint32_t slot = named < m_textures.size() && m_textures[named] ? named : 0;
        D3D12_GPU_DESCRIPTOR_HANDLE h = base;
        h.ptr += uint64_t(slot) * m_srvStride;
        list->SetGraphicsRootDescriptorTable(1, h);
        list->DrawIndexedInstanced(c.indexCount, 1, c.firstIndex, static_cast<INT>(c.vertexOffset), 0);
    }
}

} // namespace wrally::render
