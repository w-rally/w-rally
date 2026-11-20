// foliage-shot: one still view of a stage through WRenderer.dll - the lit frame and the sun's visibility (debug view
// 7) as PNGs, and the GPU frame time over a run of frames. For judging the foliage shadows (NGG design §2.6
// "Leaf-exact foliage shadows"): the renderer's switches (NGG_FOLIAGE_SHADOW, NGG_FOLIAGE_NEAR, NGG_FOLIAGE_CASCADES,
// NGG_MODEL_LOD_SCALE...) are its environment's, so a script runs this once per variant with the same view.
//
// usage: foliage-shot <WRenderer.dll> <stage.ngg> <out prefix> [--variant 41] [--eye x y z --at x y z]
//                     [--size 1920 1080] [--settle 90] [--time 11.5] [--frames 0] [--quality 2] [--vegetation 1]
//   --eye/--at default: above the stage's start (NGGS of the variant), looking down the road.
//   --frames n: then n more frames timed (median GPU frame, main pass, ray tracing), the camera still.
// Writes <prefix>-lit.png, <prefix>-vis.png and <prefix>-mask.png (the alpha-tested pixels, debug view 14).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wrally/iface/renderer.h"

using namespace wrally::iface;

namespace {

std::string g_log;
void WRALLY_CALL HostLog(void*, LogLevel, const char* message)
{
    g_log += message;
    if (std::getenv("FOLIAGE_SHOT_LOG")) std::printf("%s", message);
}
void* WRALLY_CALL HostAlloc(void*, size_t bytes, size_t alignment) { return _aligned_malloc(bytes, alignment < 16 ? 16 : alignment); }
void WRALLY_CALL HostFree(void*, void* p) { _aligned_free(p); }
Status WRALLY_CALL HostRead(void*, const char*, Blob*) { return Status::ErrIo; }
void WRALLY_CALL HostRelease(void*, Blob*) {}
int32_t WRALLY_CALL HostExists(void*, const char*) { return 0; }
const char* WRALLY_CALL HostCacheDir(void*)
{
    static std::string dir;
    if (dir.empty()) {
        char tmp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        dir = std::string(tmp) + "foliage-shot";
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    return dir.c_str();
}
double WRALLY_CALL HostNow(void*)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }
void LookAtLH(float m[16], V3 eye, V3 at, V3 up)
{
    const V3 z = Normalize(at - eye), x = Normalize(Cross(up, z)), y = Cross(z, x);
    const float r[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
    std::memcpy(m, r, sizeof r);
}
void PerspectiveFovLH(float m[16], float fovY, float aspect, float zn, float zf)
{
    const float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect;
    const float r[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    std::memcpy(m, r, sizeof r);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) { return DefWindowProcW(h, msg, w, l); }

// The variant's start (NGGR's record, else NGGS): position, facing.
bool StartOf(const std::vector<char>& f, const char* variant, V3& pos, V3& facing)
{
    if (f.size() < 16) return false;
    uint32_t n = 0;
    std::memcpy(&n, f.data() + 8, 4);
    size_t at = 16;
    bool got = false;
    for (uint32_t i = 0; i < n && at + 8 <= f.size(); ++i) {
        char tag[5] = {};
        uint32_t size = 0;
        std::memcpy(tag, f.data() + at, 4);
        std::memcpy(&size, f.data() + at + 4, 4);
        const char* d = f.data() + at + 8;
        if (!std::strcmp(tag, "NGGS") && size >= 24 && !got) {
            float v[6];
            std::memcpy(v, d, 24);
            pos = {v[0], v[1], v[2]}, facing = {v[3], v[4], v[5]};
            got = true;
        }
        if (!std::strcmp(tag, "NGGR") && size >= 16 && variant) {
            uint32_t h[4];
            std::memcpy(h, d, 16);
            for (uint32_t k = 0; k < h[1]; ++k) {
                const char* r = d + 16 + k * h[2];
                if (!_stricmp(r, variant)) {
                    float v[6];
                    std::memcpy(v, r + 32, 24);
                    pos = {v[0], v[1], v[2]}, facing = {v[3], v[4], v[5]};
                    return true;
                }
            }
        }
        at += 8 + ((size + 3) & ~3u);
    }
    return got;
}

float Median(std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: foliage-shot <WRenderer.dll> <stage.ngg> <out prefix> [--variant 41] [--eye x y z --at x y z] "
                    "[--size w h] [--settle n] [--time h] [--frames n] [--quality q] [--vegetation v]\n");
        return 2;
    }
    const char* variant = "41";
    bool eyeSet = false;
    V3 eye{}, at{};
    uint32_t W = 1920, H = 1080, settle = 90, frames = 0, quality = 2, vegetation = 1;
    float hour = -1.0f;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--variant") && i + 1 < argc) variant = argv[++i];
        else if (!std::strcmp(argv[i], "--eye") && i + 3 < argc)
            eye = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])}, i += 3, eyeSet = true;
        else if (!std::strcmp(argv[i], "--at") && i + 3 < argc)
            at = {(float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])}, i += 3;
        else if (!std::strcmp(argv[i], "--size") && i + 2 < argc) W = atoi(argv[i + 1]), H = atoi(argv[i + 2]), i += 2;
        else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc) settle = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--time") && i + 1 < argc) hour = (float)atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quality") && i + 1 < argc) quality = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--vegetation") && i + 1 < argc) vegetation = atoi(argv[++i]);
    }
    std::ifstream f(argv[2], std::ios::binary);
    const std::vector<char> ngg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ngg.empty()) return std::printf("cannot read %s\n", argv[2]), 2;
    if (!eyeSet) {
        V3 p{}, fwd{};
        if (!StartOf(ngg, variant, p, fwd)) return std::printf("no start in %s - give --eye and --at\n", argv[2]), 2;
        fwd = Normalize({fwd.x, 0.0f, fwd.z});
        eye = p - fwd * 6.0f + V3{0.0f, 9.0f, 0.0f};
        at = p + fwd * 45.0f;
    }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return std::printf("cannot load %s\n", argv[1]), 2;
    auto ver = reinterpret_cast<uint32_t(WRALLY_CALL*)(ModuleKind)>(GetProcAddress(dll, "GetWInterfaceVer"));
    auto init = reinterpret_cast<Status(WRALLY_CALL*)(uint32_t, const StructHeader*, StructHeader*)>(
        GetProcAddress(dll, "InitWInterface"));
    if (!ver || !init || ver(ModuleKind::Renderer) != kRendererInterfaceVer)
        return std::printf("%s: not a renderer at interface %u\n", argv[1], kRendererInterfaceVer), 2;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"foliage-shot";
    RegisterClassW(&wc);
    RECT rc = {0, 0, LONG(W), LONG(H)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"foliage-shot", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);

    RendererInput in{};
    RendererOutput out{};
    InitHeader(in, ModuleKind::Renderer, kRendererInterfaceVer);
    InitHeader(out, ModuleKind::Renderer, kRendererInterfaceVer);
    in.host.log = &HostLog;
    in.host.alloc = &HostAlloc;
    in.host.free = &HostFree;
    in.host.files = {&HostRead, &HostRelease, &HostExists, &HostCacheDir};
    in.host.now = &HostNow;
    in.window = hwnd;
    in.width = W, in.height = H;
    in.settings.mode = WindowMode::Windowed;
    in.settings.width = W, in.settings.height = H;
    in.settings.quality = quality;
    in.settings.rayTracing = 1;
    in.settings.taa = 0;            // a still picture, the same every run: no history in the colour
    in.settings.autoExposure = 0;
    in.settings.shadows = 1;
    in.settings.renderScale = 1.0f;
    in.settings.upscaler = kUpscalerNone;
    in.settings.denoiser = kDenoiserOurs;
    in.settings.vegetationDetail = vegetation;
    in.settings.vsync = 0;
    if (init(MakeVersion(0, 1, 0), &in.header, &out.header) != Status::Ok)
        return std::printf("InitWInterface failed:\n%s", g_log.c_str()), 2;
    const RendererApi& api = *out.api;
    Renderer* r = out.instance;

    SceneHandle scene = 0;
    if (api.loadSceneVariant(r, ngg.data(), ngg.size(), variant, &scene) != Status::Ok)
        return std::printf("load failed: %s\n%s", api.lastError(r), g_log.c_str()), 2;

    FrameDesc fd{};
    fd.scene = scene;
    LookAtLH(fd.camera.view, eye, at, {0, 1, 0});
    PerspectiveFovLH(fd.camera.proj, 1.0f, float(W) / float(H), 0.3f, 30000.0f);
    fd.camera.nearZ = 0.3f, fd.camera.farZ = 30000.0f, fd.camera.fovY = 1.0f;
    fd.weatherPreset = 0;
    if (hour >= 0.0f) {
        fd.skyTime.set = 1;
        fd.skyTime.year = 2026, fd.skyTime.month = 6, fd.skyTime.day = 21;
        fd.skyTime.localHour = hour;
        fd.skyTime.utcOffset = 2.0f;
        fd.skyTime.latitude = 50.0f, fd.skyTime.longitude = 14.4f;
    }
    std::printf("view: eye (%.1f, %.1f, %.1f) at (%.1f, %.1f, %.1f), %ux%u\n", eye.x, eye.y, eye.z, at.x, at.y, at.z, W, H);

    auto frame = [&]() {
        api.renderFrame(r, &fd);
        api.present(r);
        fd.camera.cut = 0;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    };
    // Settle: the BLASes land off the frame, the denoisers converge on a still view.
    fd.camera.cut = 1;
    for (uint32_t i = 0; i < settle; ++i) frame();
    RenderStats st{};
    api.stats(r, &st);
    std::printf("rt %s (%s), %ux%u\n", st.rtActive ? "on" : "off", st.rtFallback, st.renderWidth, st.renderHeight);

    const std::string prefix = argv[3];
    api.captureFrame(r, (prefix + "-lit.png").c_str());
    frame();
    frame();
    if (frames) {
        std::vector<float> fm, pm, rm;
        for (uint32_t i = 0; i < frames; ++i) {
            frame();
            api.stats(r, &st);
            fm.push_back(st.frameMs), pm.push_back(st.passMs), rm.push_back(st.rtMs);
        }
        std::printf("timing: %u frames, GPU frame median %.2f ms, main pass %.2f ms, ray tracing %.2f ms\n", frames,
                    Median(fm), Median(pm), Median(rm));
    }
    // The sun's visibility as the main pass applies it (debug view 7: the traced sun times the foliage map, or the
    // crown's ellipsoid). Debug views take no TAA anyway; let the RT history settle again.
    api.setDebugView(r, 7);
    for (int i = 0; i < 8; ++i) frame();
    api.captureFrame(r, (prefix + "-vis.png").c_str());
    frame();
    frame();
    // Which pixels are foliage (debug view 14: alpha-tested - green a card plant's, red the rest, a model's leaves).
    api.setDebugView(r, 14);
    frame();
    api.captureFrame(r, (prefix + "-mask.png").c_str());
    frame();
    frame();
    for (size_t p = 0; (p = g_log.find("foliage", p)) != std::string::npos; ++p) {
        const size_t s = g_log.rfind('\n', p);
        const size_t e = g_log.find('\n', p);
        std::printf("  %s\n", g_log.substr(s == std::string::npos ? 0 : s + 1, e - (s == std::string::npos ? 0 : s + 1)).c_str());
        p = e == std::string::npos ? g_log.size() : e;
    }
    api.destroy(r);
    DestroyWindow(hwnd);
    return 0;
}
