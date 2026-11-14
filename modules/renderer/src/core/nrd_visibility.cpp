// NVIDIA NRD over the ray tracer's traces (nrd_visibility.h): NRD v4.18.0, fetched and built at configure
// (third_party/nrd/README.md), driven through its raw API - it hands out pipelines (DXIL), texture pools and, every
// frame, a list of dispatches; the root signatures, pipelines, heap, constants and barriers are ours. Without the SDK
// (WRALLY_NRD undefined) Create says so and the renderer keeps its own denoisers.
//
// OUR CONVENTION -> NRD's (CommonSettings, the guide inputs written by nrd_pack.hlsl CSPack):
//   matrices   row-vector, row-major (p' = p M)    -> NRD's column-vector, column-major: the same 16 floats, as they
//                                                     are. The client's view and CONVENTIONAL projection, unjittered
//                                                     (VisibilityFrame view/proj), and last frame's for the "prev" ones
//   view Z     reverse-Z depth, jittered           -> IN_VIEWZ R32F: the surface (inverse jittered view-projection)
//                                                     through the same view matrix, so NRD's reconstruction from it
//                                                     and the matrices agree; the sky 1e7 m, past the denoising range
//   normal     geometric, world, SNORM8, w plant   -> IN_NORMAL_ROUGHNESS R10G10B10A2 (NRD_NORMAL_ENCODING 2), linear
//                                                     roughness 1 (occlusion only), material id 1 on a moved object
//   motion     RG16F previous uv - current uv,     -> IN_MV RGBA16F, "2D + Z delta", motionVectorScale (1, 1, 1):
//              unjittered (post.hlsl PSUpscaleIn)     NRD's "pixelUvPrev = pixelUv + mv" is the same quantity, same
//                                                     signs; z last frame's view Z - this frame's (a moved object's
//                                                     from the prepass's depth there, motion.z)
//   jitter     the projection's offset, NDC        -> cameraJitter, the sample's offset in pixels: the opposite of
//                                                     the projection's, y flipped (NDC up, pixels down)
//   cut        ResetHistory                        -> accumulationMode RESTART (CLEAR_AND_RESTART after a resize)
//   moved      the raw traces' sign (EncodeRaw)    -> REBLUR's AO (NGG_NRD_AO=reblur): IN_DIFF_CONFIDENCE, the least
//                                                     history the rays of the 5x5 allow, scattered to last frame's
//                                                     pixels where REBLUR reads it. SIGMA has no such input: it is
//                                                     spatial, with a stabilization of a few frames (5 at most here)
// And back (CSUnpack): SIGMA's shadow squared (its back end), REBLUR's occlusion as it is, each into the RGBA16F layout
// of Rt::Filtered - x visibility, y depth, z the moved-object distrust TAA reads.
//
// The development switches (NGG design 10.7): NGG_NRD_AO=reblur|distance, NGG_NRD_CONF=0, NGG_NRD_CUT=1,
// NGG_NRD_FRAMES / _STAB / _BLUR / _PREBLUR / _FIXSTRIDE / _HITW / _AF (REBLUR's settings), NGG_NRD_HISTORY=1 and
// NGG_NRD_SHOW=conf (debug outputs in view 13), NGG_NRD_VALIDATION=<folder> (REBLUR's validation layer as .ppm).
#include "nrd_visibility.h"
#include "log.h"
#include "pipeline_cache.h"
#include "shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#if defined(WRALLY_NRD)
#include <NRD.h>
#endif

namespace ngg {
namespace core {

using Microsoft::WRL::ComPtr;

#if defined(WRALLY_NRD)

namespace {

// The instance's denoisers, by the identifiers its dispatches carry.
enum : nrd::Identifier { kSun = 0, kAo = 1, kLights = 2 };
const int kSignals = 3;

DXGI_FORMAT ToDxgi(nrd::Format f)
{
    using F = nrd::Format;
    switch (f)
    {
    case F::R8_UNORM:          return DXGI_FORMAT_R8_UNORM;
    case F::R8_SNORM:          return DXGI_FORMAT_R8_SNORM;
    case F::R8_UINT:           return DXGI_FORMAT_R8_UINT;
    case F::R8_SINT:           return DXGI_FORMAT_R8_SINT;
    case F::RG8_UNORM:         return DXGI_FORMAT_R8G8_UNORM;
    case F::RG8_SNORM:         return DXGI_FORMAT_R8G8_SNORM;
    case F::RG8_UINT:          return DXGI_FORMAT_R8G8_UINT;
    case F::RG8_SINT:          return DXGI_FORMAT_R8G8_SINT;
    case F::RGBA8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
    case F::RGBA8_SNORM:       return DXGI_FORMAT_R8G8B8A8_SNORM;
    case F::RGBA8_UINT:        return DXGI_FORMAT_R8G8B8A8_UINT;
    case F::RGBA8_SINT:        return DXGI_FORMAT_R8G8B8A8_SINT;
    case F::RGBA8_SRGB:        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case F::R16_UNORM:         return DXGI_FORMAT_R16_UNORM;
    case F::R16_SNORM:         return DXGI_FORMAT_R16_SNORM;
    case F::R16_UINT:          return DXGI_FORMAT_R16_UINT;
    case F::R16_SINT:          return DXGI_FORMAT_R16_SINT;
    case F::R16_SFLOAT:        return DXGI_FORMAT_R16_FLOAT;
    case F::RG16_UNORM:        return DXGI_FORMAT_R16G16_UNORM;
    case F::RG16_SNORM:        return DXGI_FORMAT_R16G16_SNORM;
    case F::RG16_UINT:         return DXGI_FORMAT_R16G16_UINT;
    case F::RG16_SINT:         return DXGI_FORMAT_R16G16_SINT;
    case F::RG16_SFLOAT:       return DXGI_FORMAT_R16G16_FLOAT;
    case F::RGBA16_UNORM:      return DXGI_FORMAT_R16G16B16A16_UNORM;
    case F::RGBA16_SNORM:      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case F::RGBA16_UINT:       return DXGI_FORMAT_R16G16B16A16_UINT;
    case F::RGBA16_SINT:       return DXGI_FORMAT_R16G16B16A16_SINT;
    case F::RGBA16_SFLOAT:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case F::R32_UINT:          return DXGI_FORMAT_R32_UINT;
    case F::R32_SINT:          return DXGI_FORMAT_R32_SINT;
    case F::R32_SFLOAT:        return DXGI_FORMAT_R32_FLOAT;
    case F::RG32_UINT:         return DXGI_FORMAT_R32G32_UINT;
    case F::RG32_SINT:         return DXGI_FORMAT_R32G32_SINT;
    case F::RG32_SFLOAT:       return DXGI_FORMAT_R32G32_FLOAT;
    case F::RGB32_UINT:        return DXGI_FORMAT_R32G32B32_UINT;
    case F::RGB32_SINT:        return DXGI_FORMAT_R32G32B32_SINT;
    case F::RGB32_SFLOAT:      return DXGI_FORMAT_R32G32B32_FLOAT;
    case F::RGBA32_UINT:       return DXGI_FORMAT_R32G32B32A32_UINT;
    case F::RGBA32_SINT:       return DXGI_FORMAT_R32G32B32A32_SINT;
    case F::RGBA32_SFLOAT:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case F::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case F::R10_G10_B10_A2_UINT:  return DXGI_FORMAT_R10G10B10A2_UINT;
    case F::R11_G11_B10_UFLOAT:   return DXGI_FORMAT_R11G11B10_FLOAT;
    case F::R9_G9_B9_E5_UFLOAT:   return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default:                      return DXGI_FORMAT_UNKNOWN;
    }
}

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

// nrd_pack.hlsl's constants.
struct PackCB
{
    float    invViewProj[16];
    float    view[16];
    float    reproj[16];
    float    prevView[16];
    float    prevInvViewProj[16];
    uint32_t size[2];
    float    motionOn;
    float    aoOn;
    uint32_t mode;
    float    historyOn;
    uint32_t aoMode;
    float    scale;
};
static_assert(sizeof(PackCB) % 16 == 0, "PackCB must be whole float4s");

// Our passes' descriptor sets at the head of the heap: [t0..t5, u0..u7] each - the pack's, then one per signal's
// unpack. NRD's dispatches take the rest, from kNrdFirst, written anew every frame.
const UINT kSetSrvs = 6, kSetUavs = 8, kSetSize = kSetSrvs + kSetUavs;
const UINT kNrdFirst = kSetSize * (1 + kSignals);
const UINT kHeapSize = 4096;
const UINT kCbBytes = 256 * 1024;        // NRD's constants, a frame's worth (a few dozen dispatches of < 1 KB)

// How the renderer's AO radius (Rt::Trace's 3 m) normalizes REBLUR's hit distance (rt_ao.hlsl).
const float kAoRadius = 3.0f;

} // namespace

struct NrdVisibility::Impl
{
    // A texture and the state it is in, for the barriers.
    struct Tex
    {
        ComPtr<ID3D12Resource> r;
        ID3D12Resource*        ext = nullptr;      // someone else's (the ray tracer's), for the frame
        DXGI_FORMAT            format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES  state = D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource* Get() const { return ext ? ext : r.Get(); }
    };

    nrd::Instance* instance = nullptr;
    const nrd::InstanceDesc* desc = nullptr;
    std::vector<ComPtr<ID3D12RootSignature>> roots;     // per NRD pipeline
    std::vector<ComPtr<ID3D12PipelineState>> psos;
    ComPtr<ID3D12RootSignature> ourRoot;
    ComPtr<ID3D12PipelineState> pack, confPso, confReset, unpack;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride = 0;
    ComPtr<ID3D12Resource> cb;
    uint8_t* cbMap = nullptr;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource>  timestampReadback;
    UINT64 timestampHz = 0;
    bool   timed = false;

    // At the render size (Resize): NRD's pools, the pack's outputs, NRD's outputs and ours, ping-ponged.
    UINT width = 0, height = 0;
    std::vector<Tex> permanent, transient;
    Tex viewZ, normal, mv, conf, aoIn, confAt, cut;
    Tex nrdOut[kSignals];                      // SIGMA's (sun), REBLUR's (AO), SIGMA's (lamps)
    Tex out[kSignals][2];
    bool outValid[kSignals] = {};
    Tex ext[kSignals];                         // this frame's Rt::NrdIn, by signal
    Tex raw[kSignals];                         // and its raw traces

    uint32_t frame = 0;
    // NGG_NRD_AO=reblur|distance: REBLUR takes the AO too (nrd_pack.hlsl's open fraction, or NRD's own hit distance).
    // By default NRD takes the shadows only and the AO keeps our denoiser: REBLUR failed the ghost behind the car
    // (tools/car-ghost's measure, user-42.phr: +15 / -7 grey behind a driving car, the limit 1; nrd-handover.md).
    bool reblurAo = false;
    uint32_t aoMode = 1;
    float historyScale = 1.0f;                 // the AO output's scale (NGG_NRD_HISTORY)
    // NGG_NRD_VALIDATION=<folder>: REBLUR's validation layer (its own view of our inputs: view Z, motion, normals,
    // history), read back every 30th frame into <folder>/nrd-validation-<frame>.ppm. Development only.
    std::string validationDir;
    Tex validation;
    ComPtr<ID3D12Resource> validationReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT validationFoot = {};
    uint32_t validationFrame = 0;
    bool validationPending = false;
    bool  restart = true, clear = true;
    bool  havePrev = false;
    float prevView[16] = {}, prevProj[16] = {}, prevViewProj[16] = {}, prevJitter[2] = {};

    ~Impl() { if (instance) nrd::DestroyInstance(*instance); }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)slot * stride;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * stride;
        return h;
    }
    void Srv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT f, UINT slot) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = f;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &v, Cpu(slot));     // a null resource: a null view, reads 0
    }
    void Uav(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT f, UINT slot) const
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = f;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(r, nullptr, &u, Cpu(slot));
    }

    bool Make(ID3D12Device* dev, Tex& t, UINT w, UINT h, DXGI_FORMAT f)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        t.r.Reset();
        t.ext = nullptr;
        t.format = f;
        t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state, nullptr,
                                                      IID_PPV_ARGS(&t.r)));
    }

    bool Resize(ID3D12Device* dev, UINT w, UINT h)
    {
        auto pool = [&](std::vector<Tex>& p, const nrd::TextureDesc* td, uint32_t n) {
            p.assign(n, Tex());
            for (uint32_t i = 0; i < n; ++i)
            {
                const UINT f = td[i].downsampleFactor ? td[i].downsampleFactor : 1;
                if (!Make(dev, p[i], (w + f - 1) / f, (h + f - 1) / f, ToDxgi(td[i].format))) return false;
            }
            return true;
        };
        width = height = 0;
        if (!pool(permanent, desc->permanentPool, desc->permanentPoolSize) ||
            !pool(transient, desc->transientPool, desc->transientPoolSize) ||
            !Make(dev, viewZ, w, h, DXGI_FORMAT_R32_FLOAT) ||
            !Make(dev, normal, w, h, DXGI_FORMAT_R10G10B10A2_UNORM) ||
            !Make(dev, mv, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !Make(dev, conf, w, h, DXGI_FORMAT_R8_UNORM) ||
            !Make(dev, aoIn, w, h, DXGI_FORMAT_R16_FLOAT) ||
            !Make(dev, confAt, w, h, DXGI_FORMAT_R32_UINT) ||
            !Make(dev, cut, w, h, DXGI_FORMAT_R8_UNORM))
            return false;
        if (!validationDir.empty())
        {
            if (!Make(dev, validation, w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
            D3D12_RESOURCE_DESC d = validation.r->GetDesc();
            UINT64 bytes = 0;
            dev->GetCopyableFootprints(&d, 0, 1, 0, &validationFoot, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC b = {};
            b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            b.Width = bytes;
            b.Height = 1;
            b.DepthOrArraySize = 1;
            b.MipLevels = 1;
            b.SampleDesc.Count = 1;
            b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            validationReadback.Reset();
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&validationReadback))))
                return false;
        }
        for (int s = 0; s < kSignals; ++s)
        {
            if (!Make(dev, nrdOut[s], w, h, DXGI_FORMAT_R16_FLOAT) ||
                !Make(dev, out[s][0], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                !Make(dev, out[s][1], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT))
                return false;
            outValid[s] = false;
        }
        width = w;
        height = h;
        restart = clear = true;
        return true;
    }

    // Constants into the ring at `cbAt` (256-aligned), for a root CBV.
    D3D12_GPU_VIRTUAL_ADDRESS Constants(const void* data, UINT bytes, UINT& cbAt)
    {
        if (cbAt + bytes > kCbBytes) cbAt = 0;
        memcpy(cbMap + cbAt, data, bytes);
        const D3D12_GPU_VIRTUAL_ADDRESS va = cb->GetGPUVirtualAddress() + cbAt;
        cbAt += (bytes + 255u) & ~255u;
        return va;
    }

    // Into `want`, now or in the batch the caller sends.
    static void To(std::vector<D3D12_RESOURCE_BARRIER>& b, Tex& t, D3D12_RESOURCE_STATES want)
    {
        if (t.state == want) return;
        b.push_back(Transition(t.Get(), t.state, want));
        t.state = want;
    }

    // An NRD resource of the dispatch of `signal`.
    Tex* Resolve(const nrd::ResourceDesc& r, int signal)
    {
        using R = nrd::ResourceType;
        switch (r.type)
        {
        case R::IN_MV:                   return &mv;
        case R::IN_NORMAL_ROUGHNESS:     return &normal;
        case R::IN_VIEWZ:                return &viewZ;
        case R::IN_DIFF_CONFIDENCE:      return &conf;
        case R::IN_DISOCCLUSION_THRESHOLD_MIX: return &cut;
        case R::IN_PENUMBRA:             return &ext[signal];
        case R::IN_DIFF_HITDIST:         return &aoIn;
        case R::OUT_SHADOW_TRANSLUCENCY: return &nrdOut[signal];
        case R::OUT_DIFF_HITDIST:        return &nrdOut[kAo];
        case R::OUT_VALIDATION:          return validation.r ? &validation : nullptr;
        case R::TRANSIENT_POOL:          return r.indexInPool < transient.size() ? &transient[r.indexInPool] : nullptr;
        case R::PERMANENT_POOL:          return r.indexInPool < permanent.size() ? &permanent[r.indexInPool] : nullptr;
        default:                         return nullptr;
        }
    }

    // NRD's dispatches for one denoiser, recorded: its resources into their states, their views into the heap at
    // `slot` onwards, its constants into the ring at `cbAt`, then the dispatch and a UAV barrier for the next.
    bool Run(ID3D12Device* dev, ID3D12GraphicsCommandList* list, nrd::Identifier id, UINT& slot, UINT& cbAt)
    {
        const nrd::DispatchDesc* d = nullptr;
        uint32_t n = 0;
        if (nrd::GetComputeDispatches(*instance, &id, 1, d, n) != nrd::Result::SUCCESS) return false;
        D3D12_GPU_VIRTUAL_ADDRESS cbVa = 0;
        std::vector<D3D12_RESOURCE_BARRIER> b;
        for (uint32_t i = 0; i < n; ++i)
        {
            const nrd::DispatchDesc& dd = d[i];
            if (dd.pipelineIndex >= psos.size() || slot + dd.resourcesNum > kHeapSize) return false;
            b.clear();
            for (uint32_t k = 0; k < dd.resourcesNum; ++k)
            {
                Tex* t = Resolve(dd.resources[k], (int)id);
                if (!t || !t->Get())
                {
                    Log("nrd: dispatch %s wants %s, which we do not have", dd.name ? dd.name : "?",
                        nrd::GetResourceTypeString(dd.resources[k].type));
                    return false;
                }
                const bool storage = dd.resources[k].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
                To(b, *t, storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (storage) Uav(dev, t->Get(), t->format, slot + k);
                else         Srv(dev, t->Get(), t->format, slot + k);
            }
            if (!b.empty()) list->ResourceBarrier((UINT)b.size(), b.data());
            if (dd.constantBufferDataSize && (!dd.constantBufferDataMatchesPreviousDispatch || !cbVa))
            {
                if (cbAt + dd.constantBufferDataSize > kCbBytes) return false;
                memcpy(cbMap + cbAt, dd.constantBufferData, dd.constantBufferDataSize);
                cbVa = cb->GetGPUVirtualAddress() + cbAt;
                cbAt += (dd.constantBufferDataSize + 255u) & ~255u;
            }
            list->SetComputeRootSignature(roots[dd.pipelineIndex].Get());
            list->SetPipelineState(psos[dd.pipelineIndex].Get());
            if (dd.resourcesNum) list->SetComputeRootDescriptorTable(0, Gpu(slot));
            if (desc->pipelines[dd.pipelineIndex].hasConstantData && cbVa) list->SetComputeRootConstantBufferView(1, cbVa);
            list->Dispatch(dd.gridWidth, dd.gridHeight, 1);
            D3D12_RESOURCE_BARRIER uav = {};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            list->ResourceBarrier(1, &uav);
            slot += dd.resourcesNum;
        }
        return true;
    }
};

NrdVisibility::NrdVisibility(Rt& rt, Gtao& gtao) : m_rt(rt), m_gtao(gtao) {}
NrdVisibility::~NrdVisibility() = default;

bool NrdVisibility::Ready() const { return m && m->instance && m->unpack; }
bool NrdVisibility::InBuild() { return true; }

bool NrdVisibility::Create(Device& dev)
{
    if (Ready()) return true;
    if (!m_rt.Ready() || !m_rt.EnableNrdInputs(dev))
    {
        Log("nrd: the ray tracer's NRD inputs are unavailable - no NRD");
        return false;
    }
    std::unique_ptr<Impl> p(new Impl());
    ID3D12Device* device = dev.device.Get();
    if (const char* v = getenv("NGG_NRD_VALIDATION")) p->validationDir = v;
    if (const char* a = getenv("NGG_NRD_AO"))
    {
        p->reblurAo = !strcmp(a, "reblur") || !strcmp(a, "distance");
        p->aoMode = !strcmp(a, "distance") ? 0u : 1u;
    }

    const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
    if (lib->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
        lib->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
    {
        Log("nrd: the library was built with another normal/roughness encoding than nrd_pack.hlsl writes - no NRD");
        return false;
    }
    const nrd::DenoiserDesc denoisers[kSignals] = {
        { kSun, nrd::Denoiser::SIGMA_SHADOW },
        { kAo, nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION },
        { kLights, nrd::Denoiser::SIGMA_SHADOW },
    };
    nrd::InstanceCreationDesc icd = {};
    icd.denoisers = denoisers;
    icd.denoisersNum = kSignals;
    if (nrd::CreateInstance(icd, p->instance) != nrd::Result::SUCCESS)
    {
        Log("nrd: CreateInstance failed - no NRD");
        return false;
    }
    p->desc = nrd::GetInstanceDesc(*p->instance);
    const nrd::InstanceDesc& id = *p->desc;

    // NRD's pipelines: a root signature each - one table of its SRV and UAV ranges (resources space, from the base
    // register), its constants as a root CBV, its samplers static - and the DXIL it embeds.
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(id.samplersNum);
    for (uint32_t i = 0; i < id.samplersNum; ++i)
    {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
        s = {};
        s.Filter = id.samplers[i] == nrd::Sampler::LINEAR_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                                                                : D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = id.samplersBaseRegisterIndex + i;
        s.RegisterSpace = id.constantBufferAndSamplersSpaceIndex;
    }
    p->roots.resize(id.pipelinesNum);
    p->psos.resize(id.pipelinesNum);
    for (uint32_t i = 0; i < id.pipelinesNum; ++i)
    {
        const nrd::PipelineDesc& pd = id.pipelines[i];
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        for (uint32_t k = 0; k < pd.resourceRangesNum && k < 2; ++k)
        {
            ranges[k].RangeType = pd.resourceRanges[k].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE
                                      ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[k].NumDescriptors = pd.resourceRanges[k].descriptorsNum;
            ranges[k].BaseShaderRegister = id.resourcesBaseRegisterIndex;
            ranges[k].RegisterSpace = id.resourcesSpaceIndex;
            ranges[k].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = pd.resourceRangesNum < 2 ? pd.resourceRangesNum : 2;
        rp[0].DescriptorTable.pDescriptorRanges = ranges;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[1].Descriptor.ShaderRegister = id.constantBufferRegisterIndex;
        rp[1].Descriptor.RegisterSpace = id.constantBufferAndSamplersSpaceIndex;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = rp;
        rs.NumStaticSamplers = id.samplersNum;
        rs.pStaticSamplers = samplers.data();
        ComPtr<ID3DBlob> sig, err;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(CreateRootSignature(device, 0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&p->roots[i]))))
        {
            Log("nrd: root signature of %s: %s - no NRD", pd.shaderIdentifier,
                err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        cd.pRootSignature = p->roots[i].Get();
        cd.CS = { pd.computeShaderDXIL.bytecode, (SIZE_T)pd.computeShaderDXIL.size };
        if (!pd.computeShaderDXIL.bytecode || FAILED(CreatePipeline(device, &cd, IID_PPV_ARGS(&p->psos[i]))))
        {
            Log("nrd: pipeline %s failed - no NRD", pd.shaderIdentifier);
            return false;
        }
    }

    // Ours (nrd_pack.hlsl): a table of six SRVs, one of five UAVs, the constants.
    {
        D3D12_DESCRIPTOR_RANGE r[2] = {};
        r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r[0].NumDescriptors = kSetSrvs;
        r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r[1].NumDescriptors = kSetUavs;
        r[1].OffsetInDescriptorsFromTableStart = kSetSrvs;
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 2;
        rp[0].DescriptorTable.pDescriptorRanges = r;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;     // (more than root constants hold)
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = rp;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(CreateRootSignature(device, 0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_PPV_ARGS(&p->ourRoot))))
        {
            Log("nrd: our root signature failed - no NRD");
            return false;
        }
        ComPtr<ID3DBlob> pk = LoadShader("nrd_pack.hlsl", "CSPack", "cs_6_0");
        ComPtr<ID3DBlob> cf = LoadShader("nrd_pack.hlsl", "CSConf", "cs_6_0");
        ComPtr<ID3DBlob> cr = LoadShader("nrd_pack.hlsl", "CSConfReset", "cs_6_0");
        ComPtr<ID3DBlob> up = LoadShader("nrd_pack.hlsl", "CSUnpack", "cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = p->ourRoot.Get();
        if (!pk || !cf || !cr || !up) return false;
        cd.CS = { pk->GetBufferPointer(), pk->GetBufferSize() };
        if (FAILED(CreatePipeline(device, &cd, IID_PPV_ARGS(&p->pack)))) return false;
        cd.CS = { cf->GetBufferPointer(), cf->GetBufferSize() };
        if (FAILED(CreatePipeline(device, &cd, IID_PPV_ARGS(&p->confPso)))) return false;
        cd.CS = { cr->GetBufferPointer(), cr->GetBufferSize() };
        if (FAILED(CreatePipeline(device, &cd, IID_PPV_ARGS(&p->confReset)))) return false;
        cd.CS = { up->GetBufferPointer(), up->GetBufferSize() };
        if (FAILED(CreatePipeline(device, &cd, IID_PPV_ARGS(&p->unpack)))) return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->heap)))) return false;
    p->stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // NRD's constants: an upload ring, mapped for good. The frames are synchronous, so it starts over every frame.
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kCbBytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&p->cb))) ||
            FAILED(p->cb->Map(0, &none, reinterpret_cast<void**>(&p->cbMap))))
            return false;
    }

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2;
    if (SUCCEEDED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&p->timestamps))))
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 2 * sizeof(UINT64);
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&p->timestampReadback))))
            p->timestamps.Reset();
        dev.queue->GetTimestampFrequency(&p->timestampHz);
    }

    m = std::move(p);
    Log("nrd: ready - NRD v%u.%u.%u, %u pipelines: SIGMA for the sun and the local lights, the AO %s",
        lib->versionMajor, lib->versionMinor, lib->versionBuild, id.pipelinesNum,
        m->reblurAo ? (m->aoMode ? "by REBLUR (the open fraction)" : "by REBLUR (its hit distance)") : "by ours");
    return true;
}

void NrdVisibility::ResetHistory()
{
    if (m) m->restart = true;
    m_gtao.ResetHistory();
}

void NrdVisibility::Record(Device& dev, ID3D12GraphicsCommandList* list, const VisibilityFrame& f, Visibility& out)
{
    if (!Ready()) return;
    Impl& p = *m;
    ID3D12Device* device = dev.device.Get();

    float sunTowards[3];
    float len = sqrtf(f.sunTravel[0] * f.sunTravel[0] + f.sunTravel[1] * f.sunTravel[1] + f.sunTravel[2] * f.sunTravel[2]);
    if (len < 1e-6f) len = 1.0f;
    for (int k = 0; k < 3; ++k) sunTowards[k] = -f.sunTravel[k] / len;

    // The traces, as RtVisibility's (visibility.cpp), in their NRD variants and without our denoisers - but for the
    // AO's, unless REBLUR takes it (NGG_NRD_AO).
    const bool traced = f.ao && m_aoSource == AoSource::Traced && m_rt.AoAvailable();
    m_rt.SetNrdInputs(true, p.reblurAo);
    m_rt.Trace(dev, list, f.depth, f.normal, f.motion, f.plant, f.viewProjJittered, f.viewProj, f.camPos, sunTowards,
               4000.0f, f.sunAngle, traced, kAoRadius, f.aoRays, f.width, f.height, f.lights);
    if (!m_rt.NrdIn(Rt::kNrdSunPenumbra)) return;
    if ((f.width != p.width || f.height != p.height) && !p.Resize(device, f.width, f.height))
    {
        Log("nrd: targets %ux%u failed", f.width, f.height);
        return;
    }
    const bool doAo = traced && m_rt.AoTraced() && p.reblurAo;
    const bool doLights = m_rt.LightsTraced();
    const bool on[kSignals] = { true, doAo, doLights };

    // The ray tracer's, for this frame: they come out of Trace as UNORDERED_ACCESS and go back so.
    const Rt::NrdInput inputs[kSignals] = { Rt::kNrdSunPenumbra, Rt::kNrdAoHitDist, Rt::kNrdLightsPenumbra };
    ID3D12Resource* raws[kSignals] = { m_rt.Result(), m_rt.RawAo(), m_rt.RawLights() };
    for (int s = 0; s < kSignals; ++s)
    {
        p.ext[s].ext = m_rt.NrdIn(inputs[s]);
        p.ext[s].format = DXGI_FORMAT_R16_FLOAT;
        p.ext[s].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        p.raw[s].ext = raws[s];
        p.raw[s].format = DXGI_FORMAT_R16_FLOAT;
        p.raw[s].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // The pack's constants first: nothing below may return once the prepass's targets have left their states.
    PackCB cb = {};
    if (!Invert(f.viewProjJittered, cb.invViewProj)) return;
    memcpy(cb.view, f.view, sizeof(cb.view));
    {
        float inv[16];
        if (p.havePrev && Invert(f.viewProj, inv)) MatMul(inv, p.prevViewProj, cb.reproj);
        else for (int k = 0; k < 16; ++k) cb.reproj[k] = (k % 5 == 0) ? 1.0f : 0.0f;    // the camera did not move
        memcpy(cb.prevView, p.havePrev ? p.prevView : f.view, sizeof(cb.prevView));
        if (!Invert(p.havePrev ? p.prevViewProj : f.viewProj, cb.prevInvViewProj)) return;
    }
    cb.size[0] = f.width;
    cb.size[1] = f.height;
    cb.motionOn = f.motion ? 1.0f : 0.0f;
    cb.aoOn = doAo ? 1.0f : 0.0f;
    // What REBLUR denoises when it takes the AO (NGG_NRD_AO, nrd_pack.hlsl u5): our open fraction ("reblur") - the AO
    // the main pass expects - or NRD's own normalized hit distance ("distance"), whose AO is lighter: in track-42's
    // start view, contact regions 33-41 grey (8 bit, view 13) lighter than ours, against 0-3 for the open fraction.
    cb.aoMode = p.aoMode;

    if (p.timestamps) list->EndQuery(p.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    // ---- pack: the prepass's targets and the raws readable, the guide inputs written
    std::vector<D3D12_RESOURCE_BARRIER> b;
    b.push_back(Transition(f.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, read));
    b.push_back(Transition(f.normal, D3D12_RESOURCE_STATE_RENDER_TARGET, read));
    if (f.motion) b.push_back(Transition(f.motion, D3D12_RESOURCE_STATE_RENDER_TARGET, read));
    for (int s = 0; s < kSignals; ++s) Impl::To(b, p.raw[s], read);
    Impl::To(b, p.viewZ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.normal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.conf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.aoIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.confAt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.cut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Impl::To(b, p.ext[kAo], read);
    list->ResourceBarrier((UINT)b.size(), b.data());

    UINT cbAt = 0;     // the constants' ring, from the start every frame
    ID3D12DescriptorHeap* heaps[] = { p.heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(p.ourRoot.Get());
    {
        const UINT set = 0;
        p.Srv(device, f.depth, DXGI_FORMAT_R32_FLOAT, set + 0);
        p.Srv(device, f.normal, DXGI_FORMAT_R8G8B8A8_SNORM, set + 1);
        p.Srv(device, f.motion, DXGI_FORMAT_R16G16B16A16_FLOAT, set + 2);
        p.Srv(device, p.raw[kAo].Get(), DXGI_FORMAT_R16_FLOAT, set + 3);
        p.Srv(device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, set + 4);
        p.Srv(device, p.ext[kAo].Get(), DXGI_FORMAT_R16_FLOAT, set + 5);
        p.Uav(device, p.viewZ.Get(), p.viewZ.format, set + kSetSrvs + 0);
        p.Uav(device, p.normal.Get(), p.normal.format, set + kSetSrvs + 1);
        p.Uav(device, p.mv.Get(), p.mv.format, set + kSetSrvs + 2);
        p.Uav(device, p.conf.Get(), p.conf.format, set + kSetSrvs + 3);
        p.Uav(device, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, set + kSetSrvs + 4);
        p.Uav(device, p.aoIn.Get(), p.aoIn.format, set + kSetSrvs + 5);
        p.Uav(device, p.confAt.Get(), p.confAt.format, set + kSetSrvs + 6);
        p.Uav(device, p.cut.Get(), p.cut.format, set + kSetSrvs + 7);
        list->SetPipelineState(p.pack.Get());
        list->SetComputeRootDescriptorTable(0, p.Gpu(set));
        list->SetComputeRootConstantBufferView(1, p.Constants(&cb, sizeof cb, cbAt));
        list->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        // The confidences, scattered to last frame's pixels, into IN_DIFF_CONFIDENCE (nrd_pack.hlsl CSConf) - REBLUR's
        // AO only.
        if (doAo)
        {
            D3D12_RESOURCE_BARRIER uav = {};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = p.confAt.Get();
            list->ResourceBarrier(1, &uav);
            list->SetPipelineState(p.confPso.Get());
            list->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
            list->ResourceBarrier(1, &uav);
            list->SetPipelineState(p.confReset.Get());
            list->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        }
    }

    // ---- NRD: the frame's settings, each denoiser's settings, then its dispatches
    {
        nrd::CommonSettings cs = {};
        memcpy(cs.viewToClipMatrix, f.proj, sizeof(cs.viewToClipMatrix));
        memcpy(cs.worldToViewMatrix, f.view, sizeof(cs.worldToViewMatrix));
        memcpy(cs.viewToClipMatrixPrev, p.havePrev ? p.prevProj : f.proj, sizeof(cs.viewToClipMatrixPrev));
        memcpy(cs.worldToViewMatrixPrev, p.havePrev ? p.prevView : f.view, sizeof(cs.worldToViewMatrixPrev));
        cs.motionVectorScale[0] = cs.motionVectorScale[1] = cs.motionVectorScale[2] = 1.0f;
        // The sample's offset in pixels: the projection's (f.jitter, NDC) the other way, y down.
        const float jx = -f.jitter[0] * 0.5f * (float)f.width, jy = f.jitter[1] * 0.5f * (float)f.height;
        cs.cameraJitter[0] = jx < -0.5f ? -0.5f : jx > 0.5f ? 0.5f : jx;
        cs.cameraJitter[1] = jy < -0.5f ? -0.5f : jy > 0.5f ? 0.5f : jy;
        cs.cameraJitterPrev[0] = p.havePrev ? p.prevJitter[0] : cs.cameraJitter[0];
        cs.cameraJitterPrev[1] = p.havePrev ? p.prevJitter[1] : cs.cameraJitter[1];
        cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = (uint16_t)f.width;
        cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = (uint16_t)f.height;
        cs.denoisingRange = 100000.0f;     // the sky's 1e7 is past it (nrd_pack.hlsl)
        cs.frameIndex = p.frame++;
        cs.accumulationMode = p.clear ? nrd::AccumulationMode::CLEAR_AND_RESTART
                            : p.restart ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
        // NGG_NRD_CONF=0: no history confidence for the AO (A/B).
        static const bool conf = [] { const char* e = getenv("NGG_NRD_CONF"); return !e || e[0] != '0'; }();
        cs.isHistoryConfidenceAvailable = doAo && conf;
        cs.enableValidation = p.validation.r != nullptr;
        // NGG_NRD_CUT=1: where the AO's rays hit a moved object, its history CUT, not only shortened - REBLUR's
        // disocclusion test mixed (IN_DISOCCLUSION_THRESHOLD_MIX, 1 - the confidence) towards a threshold nothing
        // passes. The confidence alone keeps about half the history even at 0 (REBLUR: accumSpeed *= 1 / (1 +
        // accumSpeed)), where ours keeps none (NGG design 4.5, "no ghost behind the car").
        static const bool cut = [] { const char* e = getenv("NGG_NRD_CUT"); return e && e[0] == '1'; }();
        cs.isDisocclusionThresholdMixAvailable = doAo && cut;
        cs.disocclusionThresholdAlternate = cut ? 1e-6f : cs.disocclusionThresholdAlternate;
        nrd::SetCommonSettings(*p.instance, cs);
        p.prevJitter[0] = cs.cameraJitter[0];
        p.prevJitter[1] = cs.cameraJitter[1];

        // The sun: SIGMA needs the direction to it. The lamps: local lights, none.
        nrd::SigmaSettings sun = {};
        for (int k = 0; k < 3; ++k) sun.lightDirection[k] = sunTowards[k];
        nrd::SetDenoiserSettings(*p.instance, kSun, &sun);
        nrd::SigmaSettings lamps = {};
        nrd::SetDenoiserSettings(*p.instance, kLights, &lamps);
        // The AO: the hit distance normalized by the radius alone (A = the radius, B as near 0 as NRD takes, C 1 -
        // rt_ao.hlsl writes t / radius), and the material ids compared exactly - a moved object's pixels apart from
        // the static world's (nrd_pack.hlsl).
        nrd::ReblurSettings ao = {};
        ao.hitDistanceParameters.A = kAoRadius;
        ao.hitDistanceParameters.B = 1e-6f;
        ao.hitDistanceParameters.C = 1.0f;
        ao.minMaterialForDiffuse = 0.0f;
        // NGG_NRD_FRAMES=<n>, NGG_NRD_STAB=<n>: REBLUR's accumulated and stabilized frames over its defaults (A/B).
        static const int frames = [] { const char* e = getenv("NGG_NRD_FRAMES"); return e ? atoi(e) : -1; }();
        static const int stab = [] { const char* e = getenv("NGG_NRD_STAB"); return e ? atoi(e) : -1; }();
        if (frames >= 0) ao.maxAccumulatedFrameNum = (uint32_t)frames;
        if (stab >= 0) ao.maxStabilizedFrameNum = (uint32_t)stab;
        // NGG_NRD_HITW=<0..0.2>, NGG_NRD_PREBLUR=<px>: REBLUR's least hit-distance weight and pre-pass radius (A/B).
        static const float hitW = [] { const char* e = getenv("NGG_NRD_HITW"); return e ? (float)atof(e) : -1.0f; }();
        static const float preBlur = [] { const char* e = getenv("NGG_NRD_PREBLUR"); return e ? (float)atof(e) : -1.0f; }();
        // NGG_NRD_FIXSTRIDE=<px>: the stride of its history fix's 5x5, where the history was cut (A/B).
        static const int fixStride = [] { const char* e = getenv("NGG_NRD_FIXSTRIDE"); return e ? atoi(e) : -1; }();
        if (fixStride > 0) ao.historyFixBasePixelStride = ao.historyFixAlternatePixelStride = (uint32_t)fixStride;
        // NGG_NRD_BLUR=<px>: its largest blur radius (A/B; REBLUR's own 30 washed the car's contact AO out).
        static const float maxBlur = [] { const char* e = getenv("NGG_NRD_BLUR"); return e ? (float)atof(e) : -1.0f; }();
        if (maxBlur >= 0.0f) ao.maxBlurRadius = maxBlur;
        if (hitW >= 0.0f) ao.minHitDistanceWeight = hitW;
        // NGG_NRD_AF=0: REBLUR's anti-firefly off (A/B).
        static const bool antiFirefly = [] { const char* e = getenv("NGG_NRD_AF"); return !e || e[0] != '0'; }();
        ao.enableAntiFirefly = antiFirefly;
        if (preBlur >= 0.0f) ao.diffusePrepassBlurRadius = preBlur;
        if (ao.maxFastAccumulatedFrameNum > ao.maxAccumulatedFrameNum)
            ao.maxFastAccumulatedFrameNum = ao.maxAccumulatedFrameNum;
        // NGG_NRD_HISTORY=1: REBLUR's history length per pixel in place of the AO (view 13: white = full), for
        // debugging what it keeps.
        static const bool history = [] { const char* e = getenv("NGG_NRD_HISTORY"); return e && e[0] == '1'; }();
        ao.returnHistoryLengthInsteadOfOcclusion = history;
        p.historyScale = history ? 1.0f / (float)(ao.maxAccumulatedFrameNum ? ao.maxAccumulatedFrameNum : 1) : 1.0f;
        nrd::SetDenoiserSettings(*p.instance, kAo, &ao);
    }
    b.clear();
    Impl::To(b, p.viewZ, read);
    Impl::To(b, p.normal, read);
    Impl::To(b, p.mv, read);
    Impl::To(b, p.conf, read);
    Impl::To(b, p.aoIn, read);
    Impl::To(b, p.cut, read);
    list->ResourceBarrier((UINT)b.size(), b.data());
    UINT slot = kNrdFirst;
    bool ran[kSignals] = {};
    for (int s = 0; s < kSignals; ++s)
    {
        if (!on[s]) continue;
        ran[s] = p.Run(device, list, (nrd::Identifier)s, slot, cbAt);
        if (!ran[s]) Log("nrd: the %s denoiser's dispatches failed", s == kSun ? "sun" : s == kAo ? "AO" : "lamps");
    }
    p.restart = p.clear = false;
    if (p.validation.r && ran[kAo] && ++p.validationFrame % 30 == 0)
    {
        b.clear();
        Impl::To(b, p.validation, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier((UINT)b.size(), b.data());
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = p.validationReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = p.validationFoot;
        src.pResource = p.validation.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        p.validationPending = true;
    }

    // ---- unpack into our layout, this frame's half of each pair
    const int cur = (int)(p.frame & 1), prev = cur ^ 1;
    list->SetComputeRootSignature(p.ourRoot.Get());
    list->SetPipelineState(p.unpack.Get());
    for (int s = 0; s < kSignals; ++s)
    {
        if (!ran[s]) { p.outValid[s] = false; continue; }
        b.clear();
        Impl::To(b, p.nrdOut[s], read);
        Impl::To(b, p.out[s][prev], read);
        Impl::To(b, p.out[s][cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier((UINT)b.size(), b.data());
        const UINT set = kSetSize * (1 + s);
        p.Srv(device, p.nrdOut[s].Get(), p.nrdOut[s].format, set + 0);
        p.Srv(device, p.raw[s].Get(), DXGI_FORMAT_R16_FLOAT, set + 1);
        p.Srv(device, p.out[s][prev].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, set + 2);
        p.Srv(device, f.depth, DXGI_FORMAT_R32_FLOAT, set + 3);
        p.Srv(device, p.mv.Get(), p.mv.format, set + 4);
        p.Srv(device, s == kAo ? p.conf.Get() : nullptr, DXGI_FORMAT_R8_UNORM, set + 5);
        for (UINT k = 0; k < 4; ++k) p.Uav(device, nullptr, DXGI_FORMAT_R16_FLOAT, set + kSetSrvs + k);
        p.Uav(device, p.out[s][cur].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, set + kSetSrvs + 4);
        p.Uav(device, nullptr, DXGI_FORMAT_R16_FLOAT, set + kSetSrvs + 5);
        p.Uav(device, nullptr, DXGI_FORMAT_R32_UINT, set + kSetSrvs + 6);
        p.Uav(device, nullptr, DXGI_FORMAT_R8_UNORM, set + kSetSrvs + 7);
        PackCB ucb = cb;
        // NGG_NRD_SHOW=conf: the AO's IN_DIFF_CONFIDENCE in its place (view 13), for debugging.
        static const bool showConf = [] { const char* e = getenv("NGG_NRD_SHOW"); return e && !strcmp(e, "conf"); }();
        ucb.mode = s == kAo ? (showConf ? 2u : 1u) : 0u;
        ucb.scale = s == kAo ? p.historyScale : 1.0f;
        ucb.historyOn = p.outValid[s] ? 1.0f : 0.0f;
        list->SetComputeRootDescriptorTable(0, p.Gpu(set));
        list->SetComputeRootConstantBufferView(1, p.Constants(&ucb, sizeof ucb, cbAt));
        list->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        b.clear();
        Impl::To(b, p.out[s][cur], read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Impl::To(b, p.out[s][prev], read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier((UINT)b.size(), b.data());
        p.outValid[s] = true;
    }

    if (p.timestamps)
    {
        list->EndQuery(p.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(p.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, p.timestampReadback.Get(), 0);
        p.timed = true;
    }

    // ---- everything back as it came: the prepass's targets, the ray tracer's outputs
    b.clear();
    b.push_back(Transition(f.depth, read, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    b.push_back(Transition(f.normal, read, D3D12_RESOURCE_STATE_RENDER_TARGET));
    if (f.motion) b.push_back(Transition(f.motion, read, D3D12_RESOURCE_STATE_RENDER_TARGET));
    for (int s = 0; s < kSignals; ++s)
    {
        Impl::To(b, p.raw[s], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Impl::To(b, p.ext[s], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    list->ResourceBarrier((UINT)b.size(), b.data());

    memcpy(p.prevView, f.view, sizeof(p.prevView));
    memcpy(p.prevProj, f.proj, sizeof(p.prevProj));
    memcpy(p.prevViewProj, f.viewProj, sizeof(p.prevViewProj));
    p.havePrev = true;

    out.shadowSource = 1;
    if (ran[kSun]) out.sun = p.out[kSun][cur].Get();
    if (ran[kLights])
        out.lights = out.lightsTaa = p.out[kLights][cur].Get();
    if (ran[kAo])
    {
        out.ao = out.aoTaa = p.out[kAo][cur].Get();
        out.aoTaaPrev = p.out[kAo][prev].Get();
        snprintf(m_aoName, sizeof m_aoName, "ray traced, %u ray%s, NRD", f.aoRays, f.aoRays == 1 ? "" : "s");
        out.aoSource = m_aoName;
        out.aoMs = m_rt.AoMs();
    }
    else if (traced && !p.reblurAo && m_rt.FilteredAo())
    {
        // Our AO denoiser's, as RtVisibility hands it on.
        out.ao = m_rt.FilteredAo();
        out.aoTaa = m_rt.AoDenoised();
        out.aoTaaPrev = m_rt.AoPrevDenoised();
        snprintf(m_aoName, sizeof m_aoName, "ray traced, %u ray%s", f.aoRays, f.aoRays == 1 ? "" : "s");
        out.aoSource = m_aoName;
        out.aoMs = m_rt.AoMs();
    }
    else if (m_aoSource == AoSource::ScreenSpace)
        ScreenSpaceAo(m_gtao, list, f, out);
}

void NrdVisibility::ReadStats()
{
    if (m && m->validationPending)
    {
        m->validationPending = false;
        const UINT w = m->validationFoot.Footprint.Width, h = m->validationFoot.Footprint.Height;
        const UINT pitch = m->validationFoot.Footprint.RowPitch;
        const unsigned char* px = nullptr;
        char path[512];
        snprintf(path, sizeof path, "%s/nrd-validation-%05u.ppm", m->validationDir.c_str(), m->validationFrame);
        FILE* fp = nullptr;
        if (SUCCEEDED(m->validationReadback->Map(0, nullptr, (void**)&px)) && fopen_s(&fp, path, "wb") == 0 && fp)
        {
            fprintf(fp, "P6\n%u %u\n255\n", w, h);
            std::vector<unsigned char> row(w * 3);
            for (UINT y = 0; y < h; ++y)
            {
                for (UINT x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) row[x * 3 + c] = px[(size_t)y * pitch + x * 4 + c];
                fwrite(row.data(), 1, row.size(), fp);
            }
            fclose(fp);
        }
        D3D12_RANGE none = { 0, 0 };
        if (px) m->validationReadback->Unmap(0, &none);
    }
    if (!m || !m->timed || !m->timestampReadback || !m->timestampHz) return;
    UINT64* t = nullptr;
    D3D12_RANGE all = { 0, 2 * sizeof(UINT64) };
    if (SUCCEEDED(m->timestampReadback->Map(0, &all, reinterpret_cast<void**>(&t))))
    {
        m_denoiseMs = t[1] > t[0] ? 1000.0 * double(t[1] - t[0]) / double(m->timestampHz) : 0.0;
        D3D12_RANGE none = { 0, 0 };
        m->timestampReadback->Unmap(0, &none);
    }
}

#else

// A build without the NRD SDK (third_party/nrd/README.md): no third source; the renderer keeps its own denoisers.
struct NrdVisibility::Impl {};
NrdVisibility::NrdVisibility(Rt& rt, Gtao& gtao) : m_rt(rt), m_gtao(gtao) {}
NrdVisibility::~NrdVisibility() = default;
bool NrdVisibility::Create(Device&)
{
    Log("nrd: not in this build (WRALLY_NRD off, or its SDK not fetched)");
    return false;
}
bool NrdVisibility::Ready() const { return false; }
bool NrdVisibility::InBuild() { return false; }
void NrdVisibility::Record(Device&, ID3D12GraphicsCommandList*, const VisibilityFrame&, Visibility&) {}
void NrdVisibility::ResetHistory() { m_gtao.ResetHistory(); }
void NrdVisibility::ReadStats() {}

#endif

} // namespace core
} // namespace ngg
