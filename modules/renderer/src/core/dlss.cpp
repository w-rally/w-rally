// NVIDIA DLSS Super Resolution through NGX (NVIDIA's DLSS SDK v310.9.16,
// fetched at configure: third_party/nvidia-dlss/README.md). The NGX library
// linked in finds the driver's NGX at run time, which loads the DLSS runtime
// nvngx_dlss.dll from beside this module. Our device and command lists stay
// ours: NGX records into the list it is given. Without an NVIDIA GPU, a
// driver new enough or the DLL there is simply no DLSS, and PostFx keeps its
// own TAA (logged); a build without the SDK (or a Debug one) has the stub at
// the end.
//
// OUR CONVENTION (upscaler.h) -> DLSS's:
//   colour    linear HDR before exposure      -> IsHDR; pre-exposure 1
//   exposure  1x1 R32F, last frame's           -> pInExposureTexture (DLSS's own
//             auto exposure off); none until the meter has run: exposure 1
//   depth     reverse-Z (1 near .. 0 far)      -> DepthInverted; DLSS needs no
//             planes. R32_TYPELESS is read as R32_FLOAT by NGX's own views
//   motion    uv, previous - current, render   -> MVLowRes (render size), not
//             size, unjittered                    MVJittered; DLSS's vectors are
//             also previous - current, in render pixels: scale = render size,
//             both axes +y down, so no sign change
//   reactive  R8, how little history to keep   -> pInBiasCurrentColorMask, the
//             same meaning (1: take the current colour)
//   jitter    render pixels, +x right, +y down -> InJitterOffsetX/Y as they are:
//             DLSS takes the projection's offset in render pixels with the same
//             signs as FSR does
//   reset     a cut                            -> InReset
//   states    inputs PIXEL|NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS:
//             what NGX's compute passes read and write
#include "upscaler.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(WRALLY_DLSS)
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

namespace ngg {
namespace core {

#if defined(WRALLY_DLSS)

namespace {

// W-Rally's NGX project id (a custom engine's, any fixed GUID).
const char* const kProjectId = "8dea3913-a354-4354-9fc1-60d7b2763d08";

// This module's folder, where nvngx_dlss.dll is copied (not the exe's).
bool ModuleFolder(wchar_t* path, DWORD size)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleFolder), &self) ||
        !GetModuleFileNameW(self, path, size))
        return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = 0;
    return true;
}

const char* ResultText(NVSDK_NGX_Result r)
{
    static char text[128];
    const wchar_t* w = GetNGXResultAsString(r);
    WideCharToMultiByte(CP_UTF8, 0, w ? w : L"?", -1, text, sizeof text, nullptr, nullptr);
    return text;
}

float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index; index /= base)
    {
        f /= (float)base;
        r += f * (float)(index % base);
    }
    return r;
}

// The mode NGX tunes itself for, from the render-to-output ratio per axis
// (settings.h's DLSS modes are NVIDIA's ratios: 1, 0.667, 0.58, 0.5).
NVSDK_NGX_PerfQuality_Value ModeFor(UINT renderWidth, UINT outputWidth, const char** name)
{
    const float r = outputWidth ? (float)renderWidth / (float)outputWidth : 1.0f;
    if (r >= 0.99f) { *name = "DLAA"; return NVSDK_NGX_PerfQuality_Value_DLAA; }
    if (r >= 0.62f) { *name = "Quality"; return NVSDK_NGX_PerfQuality_Value_MaxQuality; }
    if (r >= 0.54f) { *name = "Balanced"; return NVSDK_NGX_PerfQuality_Value_Balanced; }
    if (r >= 0.42f) { *name = "Performance"; return NVSDK_NGX_PerfQuality_Value_MaxPerf; }
    *name = "Ultra Performance";
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

class Dlss : public Upscaler
{
public:
    ~Dlss() override
    {
        Release();
        if (m_caps) NVSDK_NGX_D3D12_DestroyParameters(m_caps);
        if (m_params) NVSDK_NGX_D3D12_DestroyParameters(m_params);
        if (m_init) NVSDK_NGX_D3D12_Shutdown1(m_device);
    }

    bool Load(ID3D12Device* device)
    {
        m_device = device;
        // NVIDIA's GPUs only: elsewhere NGX is not even loaded (the settings menu probes every adapter).
        {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
                SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId != 0x10DE)
            {
                Log("dlss: not an NVIDIA GPU (vendor %04x) - no DLSS", desc.VendorId);
                return false;
            }
        }
        wchar_t folder[MAX_PATH], data[MAX_PATH];
        if (!ModuleFolder(folder, MAX_PATH)) folder[0] = 0;
        // NGX's own files (its logs, when on) under the temp folder: the module's may be Program Files.
        if (!GetTempPathW(MAX_PATH, data)) data[0] = 0;
        const wchar_t* paths[] = { folder };
        NVSDK_NGX_FeatureCommonInfo info = {};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = folder[0] ? 1 : 0;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1",
                                                                 data, device, &info);
        if (NVSDK_NGX_FAILED(r))
        {
            Log("dlss: NGX not available (%s) - no DLSS", ResultText(r));
            return false;
        }
        m_init = true;
        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_caps);
        if (NVSDK_NGX_FAILED(r) || !m_caps)
        {
            Log("dlss: NGX capability query failed (%s) - no DLSS", ResultText(r));
            return false;
        }
        int available = 0, needsDriver = 0, initResult = 0;
        NVSDK_NGX_Parameter_GetI(m_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (!available)
        {
            NVSDK_NGX_Parameter_GetI(m_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
            NVSDK_NGX_Parameter_GetI(m_caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            Log("dlss: DLSS Super Resolution not available here (%s%s) - no DLSS",
                needsDriver ? "the driver is too old; " : "", ResultText((NVSDK_NGX_Result)initResult));
            return false;
        }
        r = NVSDK_NGX_D3D12_AllocateParameters(&m_params);
        if (NVSDK_NGX_FAILED(r) || !m_params)
        {
            Log("dlss: NGX parameters not allocated (%s) - no DLSS", ResultText(r));
            return false;
        }
        return true;
    }

    const char* Name() const override { return m_name; }

    // The feature is created at the next Dispatch: NGX records its set-up into a command list.
    bool Resize(ID3D12Device* device, UINT renderWidth, UINT renderHeight,
                UINT outputWidth, UINT outputHeight) override
    {
        Release();
        m_device = device;
        m_renderWidth = renderWidth, m_renderHeight = renderHeight;
        m_outputWidth = outputWidth, m_outputHeight = outputHeight;
        m_mode = ModeFor(renderWidth, outputWidth, &m_modeName);

        // NVIDIA's own render size for this mode, for the log: ours (settings.h's ratio, rounded even) may differ by
        // a pixel or two, which DLSS takes within its dynamic range.
        unsigned int w = 0, h = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
        float sharpness = 0.0f;
        if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(m_caps, outputWidth, outputHeight, m_mode, &w, &h,
                                                            &maxW, &maxH, &minW, &minH, &sharpness)))
            Log("dlss: %s, %ux%u -> %ux%u (NVIDIA's %ux%u, %ux%u..%ux%u)", m_modeName, renderWidth, renderHeight,
                outputWidth, outputHeight, w, h, minW, minH, maxW, maxH);
        else
            Log("dlss: %s, %ux%u -> %ux%u", m_modeName, renderWidth, renderHeight, outputWidth, outputHeight);
        return true;
    }

    // NVIDIA's recommendation: a Halton(2,3) sequence of at least 8 x (output / render)^2 phases - 8 at DLAA, 18 at
    // Quality, 32 at Performance.
    int JitterPhases(UINT renderWidth, UINT outputWidth) override
    {
        const float k = renderWidth ? (float)outputWidth / (float)renderWidth : 1.0f;
        const int phases = (int)ceilf(8.0f * k * k);
        return phases < 8 ? 8 : phases;
    }

    void JitterOffset(int index, int phases, float* outX, float* outY) override
    {
        const uint32_t i = (uint32_t)(index % (phases > 0 ? phases : 8)) + 1;
        *outX = Halton(i, 2) - 0.5f;
        *outY = Halton(i, 3) - 0.5f;
    }

    bool Dispatch(const UpscaleDispatch& d) override
    {
        if (!m_params) return false;
        if (!m_feature && !Create(d.list)) return false;

        NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
        e.Feature.pInColor = d.colour;
        e.Feature.pInOutput = d.output;
        e.pInDepth = d.depth;
        e.pInMotionVectors = d.motion;
        e.pInExposureTexture = d.exposure;
        e.pInBiasCurrentColorMask = d.reactive;
        e.InJitterOffsetX = d.jitterX;
        e.InJitterOffsetY = d.jitterY;
        e.InRenderSubrectDimensions = { d.renderWidth, d.renderHeight };
        e.InReset = d.reset ? 1 : 0;
        // uv -> render pixels
        e.InMVScaleX = (float)d.renderWidth;
        e.InMVScaleY = (float)d.renderHeight;
        e.InPreExposure = 1.0f;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = d.frameMs;
        const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(d.list, m_feature, m_params, &e);
        if (NVSDK_NGX_FAILED(r) && !m_failLogged)
        {
            Log("dlss: evaluate failed (%s)", ResultText(r));
            m_failLogged = true;
        }
        return NVSDK_NGX_SUCCEED(r);
    }

private:
    bool Create(ID3D12GraphicsCommandList* list)
    {
        if (m_createFailed) return false;
        NVSDK_NGX_DLSS_Create_Params c = {};
        c.Feature.InWidth = m_renderWidth;
        c.Feature.InHeight = m_renderHeight;
        c.Feature.InTargetWidth = m_outputWidth;
        c.Feature.InTargetHeight = m_outputHeight;
        c.Feature.InPerfQualityValue = m_mode;
        c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                 NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        // The model: K for every mode. NVIDIA's default for Performance, M, lifts the dark floor of the Garage by ~6%
        // and doubles its frame-to-frame flicker (rt-grain sg.py, 2026-11-01); K there matches the other modes.
        // NGG_DLSS_PRESET=<letter> picks another for all of them (development: comparing the presets).
        unsigned int preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        if (const char* e = getenv("NGG_DLSS_PRESET"))
            if (e[0] >= 'A' && e[0] <= 'O') preset = (unsigned int)(e[0] - 'A' + 1);
        const char* const modes[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance };
        for (const char* p : modes) NVSDK_NGX_Parameter_SetUI(m_params, p, preset);
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &m_feature, m_params, &c);
        if (NVSDK_NGX_FAILED(r) || !m_feature)
        {
            Log("dlss: feature creation failed (%s) for %ux%u -> %ux%u", ResultText(r), m_renderWidth, m_renderHeight,
                m_outputWidth, m_outputHeight);
            m_feature = nullptr;
            m_createFailed = true;
            return false;
        }

        // The runtime NGX loaded, by its file version, for the readout: "DLSS 310.9.16".
        snprintf(m_name, sizeof m_name, "DLSS");
        wchar_t path[MAX_PATH];
        if (HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll"))
            if (GetModuleFileNameW(m, path, MAX_PATH))
            {
                DWORD handle = 0;
                const DWORD size = GetFileVersionInfoSizeW(path, &handle);
                char info[4096];
                VS_FIXEDFILEINFO* v = nullptr;
                UINT len = 0;
                if (size && size <= sizeof info && GetFileVersionInfoW(path, 0, size, info) &&
                    VerQueryValueW(info, L"\\", reinterpret_cast<void**>(&v), &len) && v)
                    snprintf(m_name, sizeof m_name, "DLSS %u.%u.%u", HIWORD(v->dwFileVersionMS),
                             LOWORD(v->dwFileVersionMS), HIWORD(v->dwFileVersionLS));
            }
        Log("dlss: %s %s, %ux%u -> %ux%u", m_name, m_modeName, m_renderWidth, m_renderHeight, m_outputWidth,
            m_outputHeight);
        return true;
    }

    void Release()
    {
        if (m_feature) NVSDK_NGX_D3D12_ReleaseFeature(m_feature);
        m_feature = nullptr;
        m_createFailed = false;
    }

    ID3D12Device*        m_device = nullptr;
    bool                 m_init = false;
    NVSDK_NGX_Parameter* m_caps = nullptr;      // the capabilities, and the optimal-settings query
    NVSDK_NGX_Parameter* m_params = nullptr;    // the feature's create and evaluate parameters
    NVSDK_NGX_Handle*    m_feature = nullptr;
    NVSDK_NGX_PerfQuality_Value m_mode = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    const char*          m_modeName = "Quality";
    UINT m_renderWidth = 0, m_renderHeight = 0, m_outputWidth = 0, m_outputHeight = 0;
    char                 m_name[48] = "DLSS";
    bool                 m_createFailed = false;
    bool                 m_failLogged = false;
};

} // namespace

Upscaler* CreateDlssUpscaler(ID3D12Device* device)
{
    Dlss* u = new Dlss();
    if (!u->Load(device))
    {
        delete u;
        return nullptr;
    }
    return u;
}

#else

Upscaler* CreateDlssUpscaler(ID3D12Device*)
{
    Log("dlss: not in this build (no DLSS SDK fetched at configure, or a Debug build) - no DLSS");
    return nullptr;
}

#endif

} // namespace core
} // namespace ngg
