// AMD FSR through the ffx-api (third_party/fidelityfx, FSR SDK v2.4.0): the
// AMD-signed amd_fidelityfx_loader_dx12.dll, loaded by name from beside this
// module, picks the upscaler amd_fidelityfx_upscaler_dx12.dll holds for this
// adapter - FSR 3.1 anywhere, FSR 4 on RDNA 4. Nothing is linked: without the
// DLLs there is simply no FSR, and PostFx keeps its own TAA.
#include "upscaler.h"
#include "log.h"

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <math.h>
#include <stdio.h>

namespace ngg {
namespace core {

namespace {

void FfxMessage(uint32_t type, const wchar_t* message)
{
    char text[512];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof text, nullptr, nullptr);
    Log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

// A module's own folder: the DLLs are copied beside WRenderer.dll, which is not the exe's folder.
HMODULE LoadBeside(const wchar_t* name)
{
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadBeside), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH))
    {
        if (wchar_t* slash = wcsrchr(path, L'\\'))
        {
            slash[1] = 0;
            if (wcslen(path) + wcslen(name) < MAX_PATH)
            {
                wcscat_s(path, name);
                // The folder's own DLLs first: the loader finds the upscaler beside itself.
                if (HMODULE m = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                    return m;
            }
        }
    }
    return LoadLibraryW(name);
}

FfxApiResource Resource(ID3D12Resource* r, uint32_t state)
{
    FfxApiResource f = ffxApiGetResourceDX12(r, state);
    // The scene's depth is R32_TYPELESS (it is a depth target and read as R32_FLOAT): FSR's views need the type.
    if (r && f.description.format == FFX_API_SURFACE_FORMAT_R32_TYPELESS)
        f.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    return f;
}

class Fsr : public Upscaler
{
public:
    ~Fsr() override
    {
        Destroy();
        if (m_dll) FreeLibrary(m_dll);
    }

    bool Load(ID3D12Device* device)
    {
        m_dll = LoadBeside(L"amd_fidelityfx_loader_dx12.dll");
        if (!m_dll)
        {
            Log("fsr: amd_fidelityfx_loader_dx12.dll not found - no FSR");
            return false;
        }
        ffxLoadFunctions(&m_ffx, m_dll);
        if (!m_ffx.CreateContext || !m_ffx.DestroyContext || !m_ffx.Query || !m_ffx.Dispatch)
        {
            Log("fsr: the loader DLL lacks the ffx-api entry points - no FSR");
            return false;
        }
        m_device = device;
        return true;
    }

    const char* Name() const override { return m_name; }

    bool Resize(ID3D12Device* device, UINT renderWidth, UINT renderHeight,
                UINT outputWidth, UINT outputHeight) override
    {
        Destroy();
        m_device = device;

        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // HDR linear colour, reverse-Z with a finite far plane (the game's 8 km), the exposure given - and the motion
        // at render size, unjittered.
        cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        cd.maxRenderSize = { renderWidth, renderHeight };
        cd.maxUpscaleSize = { outputWidth, outputHeight };
        cd.fpMessage = &FfxMessage;
        ffxCreateBackendDX12Desc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        be.device = device;
        ffxCreateContextDescUpscaleVersion ver = {};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        ver.version = FFX_UPSCALER_VERSION;
        cd.header.pNext = &be.header;
        be.header.pNext = &ver.header;

        const ffxReturnCode_t rc = m_ffx.CreateContext(&m_ctx, &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK)
        {
            Log("fsr: context creation failed (%u) for %ux%u -> %ux%u", rc, renderWidth, renderHeight,
                outputWidth, outputHeight);
            m_ctx = nullptr;
            return false;
        }

        // Which upscaler the loader chose, for the readout.
        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (m_ffx.Query(&m_ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            snprintf(m_name, sizeof m_name, "FSR %s", pv.versionName);
        Log("fsr: %s, %ux%u -> %ux%u", m_name, renderWidth, renderHeight, outputWidth, outputHeight);
        return true;
    }

    int JitterPhases(UINT renderWidth, UINT outputWidth) override
    {
        int32_t phases = 8;
        ffxQueryDescUpscaleGetJitterPhaseCount q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        q.renderWidth = renderWidth;
        q.displayWidth = outputWidth;
        q.pOutPhaseCount = &phases;
        m_ffx.Query(m_ctx ? &m_ctx : nullptr, &q.header);
        return phases > 0 ? phases : 8;
    }

    void JitterOffset(int index, int phases, float* outX, float* outY) override
    {
        *outX = *outY = 0.0f;
        ffxQueryDescUpscaleGetJitterOffset q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        q.index = index;
        q.phaseCount = phases;
        q.pOutX = outX;
        q.pOutY = outY;
        m_ffx.Query(m_ctx ? &m_ctx : nullptr, &q.header);
    }

    bool Dispatch(const UpscaleDispatch& d) override
    {
        if (!m_ctx) return false;
        const uint32_t in = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        ffxDispatchDescUpscale u = {};
        u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        u.commandList = d.list;
        u.color = Resource(d.colour, in);
        u.depth = Resource(d.depth, in);
        u.motionVectors = Resource(d.motion, in);
        u.exposure = Resource(d.exposure, in);
        u.reactive = Resource(d.reactive, in);
        u.output = Resource(d.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        u.jitterOffset = { d.jitterX, d.jitterY };
        // uv -> render pixels
        u.motionVectorScale = { (float)d.renderWidth, (float)d.renderHeight };
        u.renderSize = { d.renderWidth, d.renderHeight };
        u.upscaleSize = { d.outputWidth, d.outputHeight };
        u.enableSharpening = false;            // PostFx sharpens after the tonemap, as it does the TAA's image
        u.sharpness = 0.0f;
        u.frameTimeDelta = d.frameMs;
        u.preExposure = 1.0f;
        u.reset = d.reset;
        u.cameraNear = d.cameraNear;
        u.cameraFar = d.cameraFar;
        u.cameraFovAngleVertical = d.fovY;
        u.viewSpaceToMetersFactor = 1.0f;      // the world is in metres
        const ffxReturnCode_t rc = m_ffx.Dispatch(&m_ctx, &u.header);
        if (rc != FFX_API_RETURN_OK && !m_failLogged)
        {
            Log("fsr: dispatch failed (%u)", rc);
            m_failLogged = true;
        }
        return rc == FFX_API_RETURN_OK;
    }

private:
    void Destroy()
    {
        if (m_ctx && m_ffx.DestroyContext) m_ffx.DestroyContext(&m_ctx, nullptr);
        m_ctx = nullptr;
    }

    HMODULE       m_dll = nullptr;
    ffxFunctions  m_ffx = {};
    ffxContext    m_ctx = nullptr;
    ID3D12Device* m_device = nullptr;
    char          m_name[48] = "FSR";
    bool          m_failLogged = false;
};

} // namespace

Upscaler* CreateFsrUpscaler(ID3D12Device* device)
{
    Fsr* f = new Fsr();
    if (!f->Load(device))
    {
        delete f;
        return nullptr;
    }
    return f;
}

void UpscaleRenderSize(UINT outputWidth, UINT outputHeight, float scale, UINT* outWidth, UINT* outHeight)
{
    if (!(scale > 0.0f) || scale > 1.0f) scale = 1.0f;
    UINT w = (UINT)floorf((float)outputWidth * scale + 0.5f) & ~1u;
    UINT h = (UINT)floorf((float)outputHeight * scale + 0.5f) & ~1u;
    *outWidth = w < 64 ? (outputWidth < 64 ? outputWidth : 64) : w;
    *outHeight = h < 64 ? (outputHeight < 64 ? outputHeight : 64) : h;
}

} // namespace core
} // namespace ngg
