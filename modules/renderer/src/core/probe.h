// ---------------------------------------------------------------------------
// ReflectionProbe - what a car's paint reflects: the world around the car, as a dynamic cube probe.
//
// The environment map (env.h) is the sky alone, so a car's clear coat (MaterialDesc::clearCoat) reflected a blurred
// gradient and nothing beside it. Screen-space reflections cannot do the car: its side reflects what stands beside it,
// which is off screen. So the stage is drawn again around the car - six 90-degree faces at a low resolution (kFace),
// a few a frame (Scene::SetReflectionFocus, NGG_PROBE_FACES), with the scene's own pipelines and the same lighting
// minus what belongs to the main camera's pixels (the ray-traced shadows and AO, the aerial table, the local lights).
// The car itself is left out: a probe at its centre would see its inside.
//
// Then resolved as env.h's map is, and sampled the same way: OCTAHEDRAL (kOct, its mip chain the roughness axis), the
// sky (env.h's mip 0) wherever a face saw none of the world. Two compute passes - the faces into mip 0, then a 2x2
// box down the chain - a frame late: the faces drawn this frame are resolved at the start of the next one, before the
// scene binds its own heap.
//
// The faces are each drawn from where the car was when that face's turn came, so at speed the near world in the
// reflection lags by a frame or two of travel - in a blurred reflection on a curved panel, not a thing a driver reads.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

namespace ngg {
namespace core {

class ReflectionProbe
{
public:
    enum { kFace = 128, kOct = 256, kOctMips = 9 };   // 256 .. 1
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;   // as the HDR target: the scene's PSOs

    bool Create(ID3D12Device* dev);
    bool Ready() const { return m_psoResolve != nullptr; }

    // A face's view (row-vector convention, render axes, LEFT-handed: it looks down its +z) from `centre`, and the
    // conventional 90-degree projection all six share. Face f: +x, -x, +y, -y, +z, -z.
    static void FaceView(int face, const float centre[3], float view[16]);
    static void FaceProj(float nearZ, float farZ, float proj[16]);

    // Drawing face `face`: its slice to RENDER_TARGET, cleared (alpha 0: no world - the sky goes there), and its depth
    // cleared for reverse-Z. EndFace gives the slice back to the resolve.
    void BeginFace(ID3D12GraphicsCommandList* list, int face, D3D12_CPU_DESCRIPTOR_HANDLE& rtv,
                   D3D12_CPU_DESCRIPTOR_HANDLE& dsv);
    void EndFace(ID3D12GraphicsCommandList* list, int face);

    // The faces drawn so far into the octahedral chain, the sky (`env`, env.h's chain) behind them. Binds its own root
    // signature and heap: call it before the scene binds its own. Leaves the chain in PIXEL_SHADER_RESOURCE.
    void Resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* env);

    // The chain, for the scene pass (t19).
    void CreateSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_faces;    // Texture2DArray, 6 slices
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_depth;    // D32, 6 slices
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_oct;      // the octahedral chain
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;     // faces SRV, env SRV, then per mip {dst, src} UAVs
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoResolve, m_psoDown;
    ID3D12Device* m_dev = nullptr;
    UINT m_rtvStride = 0, m_dsvStride = 0, m_srvStride = 0;
};

} // namespace core
} // namespace ngg
