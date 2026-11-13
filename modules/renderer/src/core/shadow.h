// ---------------------------------------------------------------------------
// Cascaded shadow maps.
//
// The sun's shadow where there is no inline ray tracing: the raster path's
// visibility, beside XeGTAO and the local lights' shadow maps (design §4.6).
//
// Four cascades over a rally stage's view distance. Two choices are worth
// stating because both are about STABILITY, which matters more here than in
// most scenes - a rally camera moves fast and rotates constantly, and shadow
// edges that crawl are far more noticeable than shadow edges that are soft:
//
//   * each cascade is fitted to the BOUNDING SPHERE of its frustum slice, not
//     the frustum itself. A sphere's extent does not change as the camera
//     rotates, so the projection stays the same size instead of breathing.
//   * the light-space origin is SNAPPED to whole texels, so a sub-texel camera
//     movement cannot shift which texel a given world point lands in.
//
// Without those two, shadows shimmer under motion and it looks like a filtering
// bug rather than a fitting one.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

namespace ngg {
namespace core {

class ShadowMaps
{
public:
    enum { kCascades = 4 };

    bool Create(ID3D12Device* device, UINT resolution = 2048);
    bool Ready() const { return m_texture != nullptr; }

    UINT Resolution() const { return m_resolution; }

    // Fit the cascades to this frame's camera and sun.
    //
    // view/proj follow ngg-core's camera contract (scene.h): row-major,
    // row-vector. Pass the ORIGINAL projection - only its two tangent terms are
    // read, and reverse-Z does not touch those.
    void Update(const float* view, const float* proj, const float* sunTowards,
                float nearZ, float farZ);

    // Row-major, row-vector, ready to multiply a world position by.
    const float* LightViewProj(int cascade) const { return m_lightViewProj[cascade]; }
    // View-space distance at which this cascade stops applying.
    float SplitFar(int cascade) const { return m_splitFar[cascade]; }
    float TexelWorldSize(int cascade) const { return m_texelWorld[cascade]; }

    ID3D12Resource* Texture() const { return m_texture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(int cascade) const;

    // DEPTH_WRITE <-> PIXEL_SHADER_RESOURCE. Called around the shadow passes.
    void ToDepthWrite(ID3D12GraphicsCommandList* list);
    void ToShaderResource(ID3D12GraphicsCommandList* list);

    void CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT  m_dsvStride = 0;
    UINT  m_resolution = 0;
    bool  m_inDepthWrite = true;

    float m_lightViewProj[kCascades][16] = {};
    float m_splitFar[kCascades] = {};
    float m_texelWorld[kCascades] = {};
};

} // namespace core
} // namespace ngg
