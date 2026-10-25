#include "shadow.h"
#include "log.h"

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// Row-major, row-vector matrix helpers - the same convention as everything else
// in ngg-core (see the camera contract in scene.h). These are D3DX's LookAtLH
// and OrthoLH written out.

void Normalise(float* v)
{
    const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

void Cross(const float* a, const float* b, float* out)
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

float Dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

void LookAtLH(const float* eye, const float* dir, const float* upHint, float* m)
{
    float z[3] = { dir[0], dir[1], dir[2] };
    Normalise(z);
    float x[3]; Cross(upHint, z, x);
    if (Dot3(x, x) < 1e-8f)                 // looking straight up or down
    {
        const float alt[3] = { 1.0f, 0.0f, 0.0f };
        Cross(alt, z, x);
    }
    Normalise(x);
    float y[3]; Cross(z, x, y);

    m[0]=x[0]; m[1]=y[0]; m[2]=z[0];  m[3]=0.0f;
    m[4]=x[1]; m[5]=y[1]; m[6]=z[1];  m[7]=0.0f;
    m[8]=x[2]; m[9]=y[2]; m[10]=z[2]; m[11]=0.0f;
    m[12]=-Dot3(x,eye); m[13]=-Dot3(y,eye); m[14]=-Dot3(z,eye); m[15]=1.0f;
}

void OrthoLH(float w, float h, float zn, float zf, float* m)
{
    memset(m, 0, sizeof(float) * 16);
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[10] = 1.0f / (zf - zn);
    m[14] = -zn / (zf - zn);
    m[15] = 1.0f;
}

void Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c];
            out[r*4+c] = s;
        }
}

} // namespace

bool ShadowMaps::Create(ID3D12Device* dev, UINT resolution)
{
    m_resolution = resolution;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = kCascades;
    desc.MipLevels = 1;
    // TYPELESS so the same memory can be a depth target and an SRV: D3D12 only
    // permits that cast when the resource itself is typeless.
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;      // conventional depth here, not reverse-Z

    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                            &clear, IID_PPV_ARGS(&m_texture))))
    {
        Log("shadow: %ux%u x%d depth array creation failed", resolution, resolution, (int)kCascades);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kCascades;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)))) return false;
    m_dsvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (int i = 0; i < kCascades; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = i;
        dv.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * m_dsvStride;
        dev->CreateDepthStencilView(m_texture.Get(), &dv, h);
    }

    Log("shadow: %u x %u x %d cascades ready", resolution, resolution, (int)kCascades);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowMaps::Dsv(int cascade) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)cascade * m_dsvStride;
    return h;
}

void ShadowMaps::CreateSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = kCascades;
    dev->CreateShaderResourceView(m_texture.Get(), &sv, dest);
}

void ShadowMaps::ToDepthWrite(ID3D12GraphicsCommandList* list)
{
    if (m_inDepthWrite || !m_texture) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_texture.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    list->ResourceBarrier(1, &b);
    m_inDepthWrite = true;
}

void ShadowMaps::ToShaderResource(ID3D12GraphicsCommandList* list)
{
    if (!m_inDepthWrite || !m_texture) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_texture.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &b);
    m_inDepthWrite = false;
}

void ShadowMaps::Update(const float* view, const float* proj, const float* sunTowards,
                        float nearZ, float farZ)
{
    // Camera basis out of the row-vector view matrix: it lives in the COLUMNS.
    const float camX[3] = { view[0], view[4], view[8] };
    const float camY[3] = { view[1], view[5], view[9] };
    const float camZ[3] = { view[2], view[6], view[10] };

    float eye[3];
    {
        const float tx = view[12], ty = view[13], tz = view[14];
        for (int j = 0; j < 3; ++j)
            eye[j] = -(tx * view[j*4+0] + ty * view[j*4+1] + tz * view[j*4+2]);
    }

    const float tanX = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
    const float tanY = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;

    // Practical split scheme: a blend of logarithmic and uniform. Pure log
    // starves the far cascades on a stage that can see kilometres; pure uniform
    // wastes almost all resolution on ground the camera never looks at closely.
    const float lambda = 0.85f;
    for (int i = 0; i < kCascades; ++i)
    {
        const float p = (float)(i + 1) / (float)kCascades;
        const float logSplit = nearZ * powf(farZ / nearZ, p);
        const float uniSplit = nearZ + (farZ - nearZ) * p;
        m_splitFar[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
    }

    float sliceNear = nearZ;
    for (int i = 0; i < kCascades; ++i)
    {
        const float sliceFar = m_splitFar[i];

        // Bounding SPHERE of this frustum slice. Rotation-invariant, which is
        // what keeps the projection from breathing as the camera turns.
        float centre[3], radius = 0.0f;
        {
            float corners[8][3];
            int n = 0;
            for (int f = 0; f < 2; ++f)
            {
                const float d = f ? sliceFar : sliceNear;
                for (int sy = -1; sy <= 1; sy += 2)
                    for (int sx = -1; sx <= 1; sx += 2)
                    {
                        for (int k = 0; k < 3; ++k)
                            corners[n][k] = eye[k] + camZ[k] * d
                                          + camX[k] * (d * tanX * sx)
                                          + camY[k] * (d * tanY * sy);
                        ++n;
                    }
            }
            for (int k = 0; k < 3; ++k)
            {
                centre[k] = 0.0f;
                for (int c = 0; c < 8; ++c) centre[k] += corners[c][k];
                centre[k] /= 8.0f;
            }
            for (int c = 0; c < 8; ++c)
            {
                const float dx = corners[c][0] - centre[0];
                const float dy = corners[c][1] - centre[1];
                const float dz = corners[c][2] - centre[2];
                const float r = sqrtf(dx*dx + dy*dy + dz*dz);
                if (r > radius) radius = r;
            }
        }

        // Round the radius up so a small numeric wobble cannot change it and
        // resize the projection from frame to frame.
        radius = ceilf(radius * 16.0f) / 16.0f;

        const float diameter = radius * 2.0f;
        m_texelWorld[i] = diameter / (float)m_resolution;

        // Pull the light far enough back to enclose anything that could cast
        // into the slice. A caster outside the sphere still matters - a cliff
        // above the road shadows the road.
        const float back = radius + 2000.0f;
        float lightEye[3];
        for (int k = 0; k < 3; ++k) lightEye[k] = centre[k] + sunTowards[k] * back;

        float toward[3] = { -sunTowards[0], -sunTowards[1], -sunTowards[2] };
        const float up[3] = { 0.0f, 1.0f, 0.0f };

        float lview[16];
        LookAtLH(lightEye, toward, up, lview);

        // TEXEL SNAP. Project the centre into light space, round it to a whole
        // texel, and translate back by the remainder. Without this, sub-texel
        // camera motion changes which texel a world point falls in and the
        // shadow edge crawls - the classic "shimmering shadows" artefact, and
        // one that reads as a filtering problem rather than a fitting one.
        {
            const float cx = centre[0]*lview[0] + centre[1]*lview[4] + centre[2]*lview[8]  + lview[12];
            const float cy = centre[0]*lview[1] + centre[1]*lview[5] + centre[2]*lview[9]  + lview[13];
            const float texel = m_texelWorld[i];
            const float sx = floorf(cx / texel) * texel - cx;
            const float sy = floorf(cy / texel) * texel - cy;
            lview[12] += sx;
            lview[13] += sy;
        }

        float lproj[16];
        OrthoLH(diameter, diameter, 0.0f, back + radius + 2000.0f, lproj);

        Mul(lview, lproj, m_lightViewProj[i]);

        sliceNear = sliceFar;
    }
}

} // namespace core
} // namespace ngg
