// editor-api-check: the renderer's editor table (wrally/iface/renderer_edit.h) through WRenderer.dll, on a real stage.
//
// What ngg-preview's --selftest checked against ngg-api.dll (Wallaby tools/NggPreview), against the table that
// replaces it: per-object hide and move, the selection highlight, overlay lines, labels, live materials, added
// instances and the pixel probe. Each check is built so that it can fail - a feature that is a stub must not pass:
//   - the transform: the scene and the camera moved together leave a probed pixel exactly as it was, and the scene
//     moved alone does not (moving the scene far away and seeing "the same" would also be what a stub does);
//   - the highlight, the lines and the labels: the renderer counts the pixels it wrote, split by the depth test, and
//     hiding the geometry in front must MOVE pixels from occluded to visible without changing the total;
//   - labels: twice the text, exactly twice the glyph pixels (a label drawn as one quad would not scale).
//
// usage: editor-api-check <WRenderer.dll> <stage.ngg>      exit code 0 when every check passes.

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wrally/iface/renderer.h"
#include "wrally/iface/renderer_edit.h"

using namespace wrally::iface;

namespace {

int g_failed = 0;
std::string g_log;   // what the renderer said since the last TakeLog

void Check(bool ok, const char* what, const char* fmt = "", ...)
{
    char detail[256] = {};
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, a);
    va_end(a);
    std::printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", what, detail);
    g_failed += !ok;
}

std::string TakeLog()
{
    std::string s;
    s.swap(g_log);
    return s;
}

// ---- the host

void WRALLY_CALL HostLog(void*, LogLevel, const char* message) { g_log += message; }
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
        dir = std::string(tmp) + "editor-api-check";
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

// ---- matrices: row-major, row vectors, left-handed (NGG's conventions)

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

void Identity(float m[16])
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
void Translation(float m[16], V3 t)
{
    Identity(m);
    m[12] = t.x, m[13] = t.y, m[14] = t.z;
}
void Mul(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}
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

// ---- the window

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) { return DefWindowProcW(h, msg, w, l); }

HWND MakeWindow(uint32_t w, uint32_t h)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"editor-api-check";
    RegisterClassW(&wc);
    RECT r = {0, 0, LONG(w), LONG(h)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // Never shown: the frames are read back through the renderer's own counters and probe, not looked at.
    return CreateWindowW(wc.lpszClassName, L"editor-api-check", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left,
                         r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
}

// ---- the frame

constexpr uint32_t kW = 1280, kH = 720;

struct Rig {
    const RendererApi* api = nullptr;
    const RendererEditApi* edit = nullptr;
    Renderer* r = nullptr;
    FrameDesc frame{};
    V3 eye{}, at{};

    void Aim(V3 e, V3 a)
    {
        eye = e, at = a;
        LookAtLH(frame.camera.view, e, a, {0, 1, 0});
        PerspectiveFovLH(frame.camera.proj, 1.0f, float(kW) / float(kH), 0.5f, 30000.0f);
        frame.camera.nearZ = 0.5f, frame.camera.farZ = 30000.0f, frame.camera.fovY = 1.0f;
    }
    bool Render()
    {
        TakeLog();
        return api->renderFrame(r, &frame) == Status::Ok;
    }
    EditOverlayStats Stats()
    {
        EditOverlayStats s{};
        edit->overlayStats(r, &s);
        return s;
    }
};

// One probe: the numbers of every term the renderer logged for the pixel, or empty when nothing was there.
std::string g_probeText;   // the last probe's lines, as logged

std::vector<double> Probe(Rig& rig, int x, int y)
{
    rig.edit->probePixel(rig.r, x, y);
    if (!rig.Render()) return {};
    const std::string log = TakeLog();
    g_probeText = log.find("--- pixel probe") == std::string::npos ? "" : log.substr(log.find("--- pixel probe"));
    std::vector<double> v;
    const size_t at = log.find("--- pixel probe");
    if (at == std::string::npos || log.find("DID NOT FIRE", at) != std::string::npos) return v;
    // Every number after the header line's own coordinates.
    const char* p = log.c_str() + log.find('\n', at);
    while (*p) {
        char* end = nullptr;
        if ((*p >= '0' && *p <= '9') || ((*p == '-' || *p == '.') && p[1] >= '0' && p[1] <= '9')) {
            const double d = std::strtod(p, &end);
            if (end && end != p) {
                v.push_back(d);
                p = end;
                continue;
            }
        }
        ++p;
    }
    return v;
}

double MaxDelta(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size() || a.empty()) return 1e30;
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = (std::max)(d, std::fabs(a[i] - b[i]));
    return d;
}

void Bounds(Rig& rig, uint32_t o, V3& lo, V3& hi)
{
    float a[3] = {}, b[3] = {};
    rig.edit->objectBounds(rig.r, o, a, b);
    lo = {a[0], a[1], a[2]}, hi = {b[0], b[1], b[2]};
}

void ShowAll(Rig& rig, uint32_t n)
{
    for (uint32_t o = 0; o < n; ++o) rig.edit->setObjectVisible(rig.r, o, 1);
}

// A monospace atlas, square (the renderer's pen advances by pixelHeight * rows / columns), every printable glyph the
// same solid block inside its cell, so a glyph's pixel count does not depend on which character it is.
std::vector<uint8_t> Atlas(uint32_t& size, uint32_t& columns, uint32_t& rows)
{
    columns = 12, rows = 8;
    const uint32_t cw = 14, ch = 21;
    size = columns * cw;   // 168 = rows * ch
    std::vector<uint8_t> rgba(size_t(size) * size * 4, 0);
    for (uint32_t c = 0; c < columns * rows; ++c) {
        const uint32_t cx = (c % columns) * cw, cy = (c / columns) * ch;
        if (c == 0) continue;   // ' '
        for (uint32_t y = cy + 4; y < cy + ch - 4; ++y)
            for (uint32_t x = cx + 3; x < cx + cw - 3; ++x) {
                uint8_t* p = &rgba[(size_t(y) * size + x) * 4];
                p[0] = p[1] = p[2] = p[3] = 255;
            }
    }
    return rgba;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: editor-api-check <WRenderer.dll> <stage.ngg>\n");
        return 2;
    }
    std::ifstream f(argv[2], std::ios::binary);
    const std::vector<char> ngg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ngg.empty()) return std::printf("cannot read %s\n", argv[2]), 2;

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return std::printf("cannot load %s\n", argv[1]), 2;
    auto ver = reinterpret_cast<uint32_t(WRALLY_CALL*)(ModuleKind)>(GetProcAddress(dll, "GetWInterfaceVer"));
    auto init = reinterpret_cast<Status(WRALLY_CALL*)(uint32_t, const StructHeader*, StructHeader*)>(
        GetProcAddress(dll, "InitWInterface"));
    if (!ver || !init || ver(ModuleKind::Renderer) != kRendererInterfaceVer)
        return std::printf("%s: not a renderer at interface %u\n", argv[1], kRendererInterfaceVer), 2;

    HWND hwnd = MakeWindow(kW, kH);
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
    in.width = kW, in.height = kH;
    // TAA and auto exposure off: both change a pixel from one frame to the next by design, and "identical" has to
    // mean identical. No ray tracing either: its denoisers accumulate over frames for the same reason.
    in.flags = kRendererNoRayTracing;
    in.settings.mode = WindowMode::Windowed;
    in.settings.quality = 1;
    in.settings.shadows = 1;
    in.settings.renderScale = 1.0f;
    in.settings.upscaler = kUpscalerNone;
    if (init(MakeVersion(0, 1, 0), &in.header, &out.header) != Status::Ok)
        return std::printf("InitWInterface failed:\n%s", TakeLog().c_str()), 2;

    Rig rig;
    rig.api = out.api;
    rig.r = out.instance;
    const RendererApi& api = *out.api;

    // ---- the table
    const RendererEditApi* edit = nullptr;
    Check(api.editApi(rig.r, kRendererEditVer + 1, &edit) == Status::ErrVersion && !edit, "another editor version is refused");
    Check(api.editApi(rig.r, kRendererEditVer, &edit) == Status::Ok && edit, "editApi hands over the table", "version %u",
          kRendererEditVer);
    if (!edit) return std::printf("editor-api-check: %d FAILED\n", g_failed), 1;
    rig.edit = edit;
    Check(edit->objectCount(rig.r) == 0 && edit->setObjectVisible(rig.r, 0, 0) == Status::ErrInvalidArg,
          "no scene: no objects, and none to hide");

    SceneHandle scene = 0;
    if (api.loadScene(rig.r, ngg.data(), ngg.size(), &scene) != Status::Ok)
        return std::printf("loadScene failed: %s\n%s", api.lastError(rig.r), TakeLog().c_str()), 2;
    {
        const std::string loadLog = TakeLog();
        for (size_t at = 0; (at = loadLog.find("scene:", at)) != std::string::npos; ++at)
            std::printf("      %s\n", loadLog.substr(at, loadLog.find('\n', at) - at).c_str());
    }
    const uint32_t n = edit->objectCount(rig.r);
    Check(n > 0 && edit->visibleObjectCount(rig.r) == n, "the stage's objects, all visible", "%u", n);

    // ---- hide (deletion is hiding)
    edit->setObjectVisible(rig.r, 0, 0);
    const uint32_t oneHidden = edit->visibleObjectCount(rig.r);
    edit->setObjectVisible(rig.r, 0, 1);
    Check(oneHidden == n - 1 && edit->visibleObjectCount(rig.r) == n, "hiding one object, and showing it again",
          "%u then %u", oneHidden, edit->visibleObjectCount(rig.r));
    Check(edit->setObjectVisible(rig.r, n, 0) == Status::ErrInvalidArg, "an object past the end is refused");

    // ---- move: a world-space delta, identity restores it
    {
        uint32_t o = 0;
        V3 lo{}, hi{};
        for (; o < n; ++o) {
            Bounds(rig, o, lo, hi);
            if (hi.x - lo.x > 0.5f && hi.z - lo.z > 0.5f) break;
        }
        const V3 c0 = (lo + hi) * 0.5f;
        float m[16];
        Translation(m, {10, 0, 0});
        edit->setObjectTransform(rig.r, o, m);
        V3 lo1, hi1;
        Bounds(rig, o, lo1, hi1);
        const V3 d = (lo1 + hi1) * 0.5f - c0;
        Check(std::fabs(d.x - 10) < 1e-3f && std::fabs(d.y) < 1e-3f && std::fabs(d.z) < 1e-3f, "translate +10 m in x",
              "moved (%.4f, %.4f, %.4f)", d.x, d.y, d.z);

        // 90 degrees about y: composed about the object's own centre it stays put; about the origin it does not.
        const float rot[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        float a[16], b[16], about[16];
        Translation(a, {-c0.x, -c0.y, -c0.z});
        Mul(a, rot, b);
        Translation(a, c0);
        Mul(b, a, about);
        edit->setObjectTransform(rig.r, o, about);
        Bounds(rig, o, lo1, hi1);
        const V3 dc = (lo1 + hi1) * 0.5f - c0;
        edit->setObjectTransform(rig.r, o, rot);
        Bounds(rig, o, lo1, hi1);
        const V3 dO = (lo1 + hi1) * 0.5f - c0;
        Check(std::sqrt(Dot(dc, dc)) < 1e-2f && std::sqrt(Dot(dO, dO)) > 1.0f, "turning about its centre keeps it in place",
              "%.4f m about the centre, %.1f m about the origin", std::sqrt(Dot(dc, dc)), std::sqrt(Dot(dO, dO)));

        Identity(m);
        edit->setObjectTransform(rig.r, o, m);
        Bounds(rig, o, lo1, hi1);
        const V3 e0 = lo1 - lo, e1 = hi1 - hi;
        Check(Dot(e0, e0) + Dot(e1, e1) < 1e-8f, "identity puts it back where the file has it");
    }

    // ---- a camera on the ground mid-stage, and the weather
    float focus[3] = {};
    const bool haveFocus = edit->focusPoint(rig.r, focus) == Status::Ok;
    Check(haveFocus, "a focus point on the ground", "(%.1f, %.1f, %.1f)", focus[0], focus[1], focus[2]);
    const V3 fp{focus[0], focus[1], focus[2]};
    rig.frame.scene = scene;
    rig.Aim(fp + V3{-120, 45, -120}, fp);
    if (api.weatherPresetCount(rig.r, scene)) {
        rig.frame.weatherPreset = 0;
    } else {
        rig.frame.weatherPreset = -1;
        WeatherDesc& w = rig.frame.weather;
        w.sunDirection[0] = -0.843f, w.sunDirection[1] = -0.504f, w.sunDirection[2] = 0.187f;
        w.sunIntensity = 22, w.turbidity = 2, w.groundAlbedo = 0.12f;
        w.groundColour[0] = w.groundColour[1] = w.groundColour[2] = 0.3f;
        w.exposure = 0.8f;
    }
    rig.Render();   // the first frame builds what the rest reuse

    // ---- the probe, and the transform it can see
    {
        // FOUND, not assumed: a pixel with geometry behind it (the screen's centre can be sky on a bent stage).
        int px = -1, py = -1;
        std::vector<double> base;
        for (int gy = 4; gy >= 1 && px < 0; --gy)
            for (int gx = 1; gx <= 7 && px < 0; ++gx) {
                base = Probe(rig, int(kW) * gx / 8, int(kH) * gy / 5);
                if (!base.empty()) px = int(kW) * gx / 8, py = int(kH) * gy / 5;
            }
        Check(px >= 0, "the probe fires on geometry", "pixel (%d, %d), %zu terms", px, py, base.size());
        if (px >= 0) {
            // Without sun shadows: the cascades snap to a texel grid fixed in the WORLD, so moving the camera moves the
            // snap and the shadow lookup with it (measured: shadowUV 0.5338 -> 0.5333 for 64 m). That is the maps'
            // stability design, not the transform; the transform is what this asks about.
            RenderSettings flat = in.settings;
            flat.shadows = 0;
            api.applySettings(rig.r, &flat);
            base = Probe(rig, px, py);
            const std::vector<double> again = Probe(rig, px, py);
            Check(MaxDelta(base, again) == 0.0, "the same frame twice is the same pixel", "max delta %g", MaxDelta(base, again));

            const V3 t{64, 0, 0};   // along the ground: the air and the fog depend on height, not on x
            float m[16];
            Translation(m, t);
            for (uint32_t o = 0; o < n; ++o) edit->setObjectTransform(rig.r, o, m);
            rig.Aim(rig.eye + t, rig.at + t);
            const std::string baseText = g_probeText;
            const std::vector<double> both = Probe(rig, px, py);
            if (MaxDelta(base, both) > 0) {
                // Which terms moved: the two probes' lines side by side where they differ.
                auto split = [](const std::string& s) {
                    std::vector<std::string> lines;
                    for (size_t p = 0, e; p < s.size(); p = e + 1) {
                        e = s.find('\n', p);
                        if (e == std::string::npos) e = s.size();
                        lines.push_back(s.substr(p, e - p));
                    }
                    return lines;
                };
                const std::vector<std::string> a = split(baseText), b = split(g_probeText);
                for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                    if (a[i] != b[i]) std::printf("      before %s\n      after  %s\n", a[i].c_str(), b[i].c_str());
            }
            // Not 0: the world now sits 64 m further along x, and positions there round differently (the distance moves by
            // 1e-4 m; the surface frame comes from their screen-space derivatives, the lit colour by 2e-4). The scene
            // moved alone changes the same pixel by tens.
            Check(MaxDelta(base, both) <= 1e-3, "scene and camera moved together: the same pixel", "max delta %g",
                  MaxDelta(base, both));

            rig.Aim(rig.eye - t, rig.at - t);
            const std::vector<double> sceneOnly = Probe(rig, px, py);
            Check(MaxDelta(base, sceneOnly) > 1e-3, "the scene moved alone: the pixel changes",
                  sceneOnly.empty() ? "the geometry left the pixel" : "max delta %g", MaxDelta(base, sceneOnly));

            Identity(m);
            for (uint32_t o = 0; o < n; ++o) edit->setObjectTransform(rig.r, o, m);
            const std::vector<double> back = Probe(rig, px, py);
            Check(MaxDelta(base, back) <= 1e-4, "restored to identity: the pixel is back", "max delta %g", MaxDelta(base, back));
            api.applySettings(rig.r, &in.settings);
        }
    }

    // The objects nearby the focus, for the highlight.
    std::vector<uint32_t> nearby;
    for (uint32_t o = 0; o < n && nearby.size() < 64; ++o) {
        V3 lo, hi;
        Bounds(rig, o, lo, hi);
        const V3 c = (lo + hi) * 0.5f - fp;
        if (Dot(c, c) < 150.0f * 150.0f) nearby.push_back(o);
    }
    auto hideAllBut = [&](const std::vector<uint32_t>& keep) {
        for (uint32_t o = 0; o < n; ++o) edit->setObjectVisible(rig.r, o, 0);
        for (uint32_t o : keep) edit->setObjectVisible(rig.r, o, 1);
    };

    // ---- the selection highlight
    {
        rig.Render();
        const EditOverlayStats none = rig.Stats();
        Check(none.selectionVisible + none.selectionOccluded == 0, "nothing selected: no wire");

        edit->setSelection(rig.r, nearby.data(), uint32_t(nearby.size()));
        rig.Render();
        const EditOverlayStats sel = rig.Stats();
        const uint32_t total = sel.selectionVisible + sel.selectionOccluded;
        Check(total > 0, "the selection draws its wires", "%zu objects: %u visible, %u occluded", nearby.size(),
              sel.selectionVisible, sel.selectionOccluded);

        hideAllBut(nearby);
        rig.Render();
        const EditOverlayStats bare = rig.Stats();
        Check(bare.selectionVisible + bare.selectionOccluded == total && bare.selectionOccluded <= sel.selectionOccluded,
              "occluders hidden: the same wire pixels, fewer behind", "%u visible, %u occluded", bare.selectionVisible,
              bare.selectionOccluded);

        for (uint32_t o : nearby) edit->setObjectVisible(rig.r, o, 0);
        rig.Render();
        const EditOverlayStats hidden = rig.Stats();
        Check(hidden.selectionVisible + hidden.selectionOccluded == 0, "selected but hidden: no wire (hidden is deleted)");
        ShowAll(rig, n);
        edit->setSelection(rig.r, nullptr, 0);
        Check(edit->setSelection(rig.r, &n, 1) == Status::ErrInvalidArg, "selecting an object past the end is refused");
        edit->setSelection(rig.r, nullptr, 0);
    }

    // ---- overlay lines: twelve verticals through the focus point, half above the ground and half in it
    {
        std::vector<LineVertex> v;
        for (int i = 0; i < 12; ++i) {
            const float a = float(i) * 0.5236f;
            const V3 p = fp + V3{std::cos(a) * 25.0f, 0, std::sin(a) * 25.0f};
            v.push_back({{p.x, p.y - 400, p.z}, 0xFF00FFFFu});
            v.push_back({{p.x, p.y + 50, p.z}, 0xFF00FFFFu});
        }
        edit->setOverlayLines(rig.r, 0, v.data(), uint32_t(v.size()), 0);
        rig.Render();
        const EditOverlayStats lit = rig.Stats();
        const uint32_t total = lit.linesVisible + lit.linesOccluded;
        Check(lit.linesVisible > 0 && lit.linesOccluded > 0, "depth-tested: some visible, some behind the ground",
              "%u visible, %u occluded", lit.linesVisible, lit.linesOccluded);

        hideAllBut({});
        rig.Render();
        const EditOverlayStats bare = rig.Stats();
        Check(bare.linesOccluded == 0 && bare.linesVisible == total, "scene hidden: the same pixels, none behind",
              "%u visible, %u occluded", bare.linesVisible, bare.linesOccluded);
        ShowAll(rig, n);

        edit->setOverlayLines(rig.r, 0, v.data(), uint32_t(v.size()), kEditNoDepth);
        rig.Render();
        const EditOverlayStats through = rig.Stats();
        Check(through.linesOccluded == 0 && through.linesVisible == total, "kEditNoDepth: drawn through the ground",
              "%u visible, %u occluded", through.linesVisible, through.linesOccluded);

        // The game's FrameDesc::lines have a layer of their own: a frame that drops them must not take the editor's.
        const LineVertex game[2] = {{{fp.x, fp.y + 5, fp.z}, 0xFF0000FFu}, {{fp.x + 1, fp.y + 5, fp.z}, 0xFF0000FFu}};
        rig.frame.lines = game, rig.frame.lineVertexCount = 2;
        rig.Render();
        rig.frame.lines = nullptr, rig.frame.lineVertexCount = 0;
        rig.Render();
        const EditOverlayStats after = rig.Stats();
        Check(after.linesVisible == total && after.linesOccluded == 0,
              "a frame dropping the game's lines leaves the editor's", "%u visible", after.linesVisible);

        Check(edit->setOverlayLines(rig.r, 0, v.data(), 3, 0) == Status::ErrInvalidArg, "an odd vertex count is refused");
        Check(edit->setOverlayLines(rig.r, kEditOverlayLayers, v.data(), 2, 0) == Status::ErrInvalidArg,
              "a layer past the last is refused");
        edit->setOverlayLines(rig.r, 0, nullptr, 0, 0);
        rig.Render();
        const EditOverlayStats cleared = rig.Stats();
        Check(cleared.linesVisible + cleared.linesOccluded == 0, "an empty layer draws nothing");
    }

    // ---- labels
    {
        const EditLabel one = {{fp.x, fp.y, fp.z}, 0xFFFFFFFFu, 0};
        Check(edit->setLabels(rig.r, 0, &one, 1, "x", 2, 21, 0) == Status::ErrInvalidArg, "labels before a font are refused");

        uint32_t size = 0, columns = 0, rows = 0;
        const std::vector<uint8_t> atlas = Atlas(size, columns, rows);
        Check(edit->setFont(rig.r, atlas.data(), size, size, columns, rows, 32) == Status::Ok, "the font atlas uploads",
              "%ux%u, %ux%u cells", size, size, columns, rows);

        rig.Render();
        const EditOverlayStats none = rig.Stats();
        Check(none.labelsVisible + none.labelsOccluded == 0, "no labels: no glyph pixels");

        // Eight labels round the focus, four standing above the ground and four buried in it.
        auto setLabels = [&](const char* text, uint32_t flags) {
            std::string blob;
            std::vector<EditLabel> l;
            // On the camera's line of sight through the focus, so all of them are on screen: halfway there is air,
            // twice as far is under whatever surface the focus stands on.
            const V3 side = Normalize(Cross({0, 1, 0}, rig.at - rig.eye));
            for (int i = 0; i < 8; ++i) {
                const float along = (i & 1) ? 0.5f : 2.0f;
                const V3 p = rig.eye + (fp - rig.eye) * along + side * (float(i - 4) * 4.0f * along);
                l.push_back({{p.x, p.y, p.z}, 0xFFFFFFFFu, uint32_t(blob.size())});
                blob += text;
                blob.push_back('\0');
            }
            return edit->setLabels(rig.r, 0, l.data(), uint32_t(l.size()), blob.data(), uint32_t(blob.size()), 21.0f, flags);
        };
        setLabels("LABEL", 0);
        rig.Render();
        const EditOverlayStats lit = rig.Stats();
        const uint32_t total = lit.labelsVisible + lit.labelsOccluded;
        Check(lit.labelsVisible > 0 && lit.labelsOccluded > 0, "depth-tested: the buried ones are behind",
              "%u visible, %u occluded", lit.labelsVisible, lit.labelsOccluded);

        hideAllBut({});
        rig.Render();
        const EditOverlayStats bare = rig.Stats();
        Check(bare.labelsOccluded == 0 && bare.labelsVisible == total, "scene hidden: the same pixels, none behind",
              "%u visible, %u occluded", bare.labelsVisible, bare.labelsOccluded);
        ShowAll(rig, n);

        setLabels("LABEL", kEditNoDepth);
        rig.Render();
        const EditOverlayStats through = rig.Stats();
        setLabels("LABELLABEL", kEditNoDepth);
        rig.Render();
        const EditOverlayStats twice = rig.Stats();
        Check(through.labelsOccluded == 0 && through.labelsVisible == total, "kEditNoDepth: drawn through the ground",
              "%u visible", through.labelsVisible);
        Check(twice.labelsVisible == 2 * through.labelsVisible && twice.labelsVisible > 0,
              "twice the text, exactly twice the glyph pixels", "%u vs %u", twice.labelsVisible, through.labelsVisible);
        edit->setLabels(rig.r, 0, nullptr, 0, nullptr, 0, 21.0f, 0);
    }

    // ---- materials: read, change, read back, restore
    {
        const uint32_t count = edit->materialCount(rig.r);
        uint32_t mat = 0;
        edit->objectMaterial(rig.r, nearby.empty() ? 0 : nearby[0], &mat);
        Check(count > 0 && mat >= 1 && mat <= count, "an object's material is in the table", "%u of %u", mat, count);
        char name[128] = {};
        edit->materialName(rig.r, mat, name, sizeof name);
        Check(name[0] != 0, "it has a name", "%s", name);

        MaterialDesc before{}, after{}, back{};
        edit->getMaterial(rig.r, mat, &before);
        MaterialDesc changed = before;
        changed.roughness = before.roughness == 0.37f ? 0.41f : 0.37f;
        edit->setMaterial(rig.r, mat, &changed);
        edit->getMaterial(rig.r, mat, &after);
        Check(std::memcmp(&after, &changed, sizeof after) == 0, "a changed roughness reads back, the rest untouched",
              "%.3f -> %.3f", before.roughness, after.roughness);
        edit->setMaterial(rig.r, mat, &before);
        edit->getMaterial(rig.r, mat, &back);
        Check(std::memcmp(&back, &before, sizeof back) == 0, "restored exactly");
        Check(edit->getMaterial(rig.r, count + 1, &back) == Status::ErrInvalidArg &&
                  edit->getMaterial(rig.r, 0, &back) == Status::ErrInvalidArg,
              "handles 0 and past the end are refused");
    }

    // ---- an added instance is an object like the rest
    {
        const V3 fwd = Normalize(rig.at - rig.eye), side = Normalize(Cross({0, 1, 0}, fwd));
        const V3 c = rig.eye + fwd * 20.0f;
        Vertex q[4] = {};
        const V3 corners[4] = {c - side * 2 + V3{0, -2, 0}, c + side * 2 + V3{0, -2, 0}, c + side * 2 + V3{0, 2, 0},
                               c - side * 2 + V3{0, 2, 0}};
        for (int i = 0; i < 4; ++i) {
            q[i].pos[0] = corners[i].x, q[i].pos[1] = corners[i].y, q[i].pos[2] = corners[i].z;
            q[i].normal[0] = -fwd.x, q[i].normal[1] = -fwd.y, q[i].normal[2] = -fwd.z;
            q[i].colour = 0xFFFFFFFFu;
        }
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        const MaterialDesc mat = DefaultMaterialDesc();
        ModelMesh mesh{};
        mesh.vertices = q, mesh.vertexCount = 4, mesh.indices = idx, mesh.indexCount = 6, mesh.material = 0, mesh.node = 0;
        ModelNode node{};
        node.parent = -1;
        Identity(node.local);
        ModelDesc md{};
        md.nodes = &node, md.nodeCount = 1, md.meshes = &mesh, md.meshCount = 1, md.materials = &mat, md.materialCount = 1;
        ModelHandle model = 0;
        InstanceHandle inst = 0;
        api.createModel(rig.r, &md, &model);
        api.createInstance(rig.r, model, &inst);
        uint32_t objs[4] = {}, cnt = 0;
        edit->instanceObjects(rig.r, inst, objs, 4, &cnt);
        Check(cnt == 1 && objs[0] == n && edit->objectCount(rig.r) == n + 1, "an instance's mesh is the next object",
              "object %u of %u", objs[0], edit->objectCount(rig.r));

        InstanceUpdate u{};
        u.instance = inst, u.visible = 1;
        Identity(u.world);
        rig.frame.instances = &u, rig.frame.instanceCount = 1;
        edit->setSelection(rig.r, objs, 1);
        rig.Render();
        rig.frame.instances = nullptr, rig.frame.instanceCount = 0;
        const EditOverlayStats s = rig.Stats();
        Check(s.selectionVisible > 0, "and it can be selected", "%u wire pixels visible", s.selectionVisible);
        edit->setSelection(rig.r, nullptr, 0);

        api.destroyInstance(rig.r, inst);
        Check(edit->instanceObjects(rig.r, inst, objs, 4, &cnt) == Status::ErrInvalidArg, "a destroyed instance has none");
    }

    // ---- switches
    Check(edit->lightModeName(0) && edit->lightModeName(1) && !edit->lightModeName(2), "light modes are named, then null",
          "%s, %s", edit->lightModeName(0), edit->lightModeName(1));
    Check(edit->setLightMode(rig.r, 1) == Status::Ok && edit->setCull(rig.r, 1) == Status::Ok, "light mode and culling set");

    api.destroy(rig.r);
    DestroyWindow(hwnd);
    if (g_failed) {
        std::printf("editor-api-check: %d FAILED\n", g_failed);
        return 1;
    }
    std::printf("editor-api-check: all good\n");
    return 0;
}
