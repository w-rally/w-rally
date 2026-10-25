// WRally, phase 1 (docs/design.md 9): phys-drive, imported from plugin-rbr (docs/imported.md) - drive the car: the
// native physics (sim/native_sim.h) loaded from the game's files, stepped in real time with keyboard controls, the
// stage rendered by NGG (ngg-api.dll) with a camera behind the car. The session is written as a recording (data/rec.h)
// on exit. (plugin-rbr's --classic, the game's own code in an arena, is not part of WRally.)
//
//   WRally <game dir> <stage .ngg> [--stage <the .ngg's>] [--sgc Cars/i20_R5/i20_R5.sgc] [--car s_i2003]
//              [--surface <the stage's>] [--tyre-type <its>] [--tyres michelin] [--aids 0,0,0,0] [--camera 0] [--out drive.phr]
//              [--replay run.phr]
//              [--skip-to <tick>] [--pause-at <tick>] [--exit-at <tick>] [--step <seconds>] [--camera-angle <degrees>]
//              [--chase-distance <metres>] [--chase-pull <metres per 100 km/h>] [--entry <seconds>] [--quick-start]
//              [--no-sound] [--eng Focus03-08] [--mix sound-mix.ini] [--overrides cars/s_i2003.ini | none]
//              [--launch-rpm 7400] [--als-crack 0.25] [--als-bangs <folder of .wav>] [--als-idle-bangs <folder>]
//
// The stage (Maps/<stage>.*: the physics' ground) defaults to the .ngg's: track-34_E.ngg that one, track-34.ngg the
// map's one stage in Maps (several: the morning one, _M, or --stage needed); a --stage of another map than the .ngg's
// is warned about (the car would run on ground that is not drawn).
//
// The surface (the car's <surface>.lsp, setup and options) and the tyre type (TyreType00..06: tarmac dry /
// intermediate / wet, gravel dry / intermediate / wet, snow; a number or its name) default to the stage's, as the game
// picks them (sim::StageSurface: the exe's map table or Maps/Tracks.ini's Surface=; the surface's dry tyres); --surface
// and --tyre-type override them. --tyres is the tyre brand (Physics/<brand>/tyres.lsp).
//
// --skip-to runs the physics to that tick at full speed before the first frame (nothing drawn), --pause-at pauses on
// reaching a tick, --exit-at quits there (as Esc would: the recording is written), --step advances every frame by a fixed time (the same frames at the same ticks run to run).
// PHYS_DRIVE_JITTER=<tick> prints how steadily the car sits in the picture up to that tick, then quits.
//
// Keys (by their place, as on a US keyboard - the same keys on any layout: Y brakes on QWERTZ): A throttle, Z brake,
// Space handbrake, Left / Right steering, S / X gear up / down, T / O / L / K the renderer's TAA / AO / shadows / ray tracing off and on, V its debug views, F1 its readout, P pause, F2 a screenshot, C the camera (chase, bonnet, above, beside, and in a replay the stage's TV cameras; --camera n to start
// with one), , / . (US: the keys right of M) turn the beside camera round the car, left / right, paused too (it starts at
// the left front wheel, --camera-angle <degrees> elsewhere: 90 the front, 180 the right side, 270 the back), [ / ] (US: the keys right of P) move the chase camera closer / further (5.2 m behind the car by default, --chase-distance
// to start elsewhere; at speed it drops back --chase-pull metres per 100 km/h, 1.5 by default), R back on the road (the game's outside assistance: stopped or nearly, below 1.5 m/s), Esc quit. A stalled
// engine restarts in neutral with the throttle. M mutes the sound and back.
//
// The sound (audio/audio.h) hears the car's records on a signal bus (signals/car.h) the physics emits after its steps;
// Launch control (drive/launch_control.h; --launch-rpm, 0 off): standing in neutral with the throttle floored, the revs
// are held at 7400 instead of bouncing off the limiter; a gear going in is the launch. The anti-lag (drive/anti_lag.h;
// --als-crack, 0 off): lifting at revs in gear the throttle stays cracked open (the engine about torque-free, the
// turbo spinning) and the sound adds the exhaust's bangs; N arms it at standstill (in neutral off the pedal the revs hunt
// and bubble with its bangs - a rally car's before the start).
//
// --no-sound leaves it out. Its sounds are RBR's (<game dir>/audio), the engine's loops from Cars/<--eng>.eng ("none":
// S0's pulse engine). The mix by hand: --mix's file (default game/config/sound-mix.ini, WRallyFile), read again
// whenever it is saved - a volume and a solo per layer (audio/mix.h).
//
// The start, as the game's (sim::StageClock): the entry (--entry seconds from the load, 2 by default; the car held by
// its brakes), then the game's 7 s countdown - 3, 2, 1 shown big in its last 3 s, then GO (9 s after the load, as in
// the game's recording of stage 41: the car off at tick 1291 of 0.007 s, no false start) and the stage time running.
// Moving faster than 1 m/s in the countdown's last 4 s is a false start: the clock starts at once with what was left of
// the countdown and 10 s on it. --quick-start: no entry, the countdown from 3.5 s. A replay gets the same start (its
// own controls; nothing held). The stage time and the splits are on the screen and printed; the splits and the finish
// are the stage's .dls marks along the drive line.
//
// The physics' frame has z up; the renderer's (the .ngg's, RBR's) y up: (x, y, z) physics = (x, z, y) renderer.

#include <windows.h>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/device.h"
#include "audio/mix.h"
#include "data/rec.h"
#include "data/sgc.h"
#include "ngg_api.h"
#include "drive/car_model.h"
#include "drive/anti_lag.h"
#include "drive/launch_control.h"
#include "drive/replay_camera.h"
#include "signals/bus.h"
#include "sim/native_sim.h"
#include "sim/sim.h"

namespace {

bool g_quit;
int g_width, g_height;   // the client area, as the renderer's size

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE)) {
        g_quit = true;
        return 0;
    }
    if (m == WM_SIZE && LOWORD(l) && HIWORD(l)) {
        g_width = LOWORD(l);
        g_height = HIWORD(l);
    }
    return DefWindowProcW(h, m, w, l);
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// A key by its place on the keyboard (its scan code), whatever the layout prints on it: the brake is the key left of X
// on QWERTY (Z), QWERTZ (Y) and AZERTY (W) alike. The layout is asked each time, so switching it while driving works.
enum Place : UINT { kThrottle = 0x1e, kGearUp = 0x1f, kBrake = 0x2c, kGearDown = 0x2d, kCamera = 0x2e, kReset = 0x13,
             kTaa = 0x14, kPause = 0x19, kAo = 0x18, kShadows = 0x26, kView = 0x2f,
             kOrbitLeft = 0x33, kOrbitRight = 0x34, kChaseIn = 0x1a, kChaseOut = 0x1b, kRt = 0x25, kMute = 0x32, kAntiLagIdle = 0x31 };   // US: A S Z X C R, T P O L V , . [ ] K M N
bool DownAt(Place p) { return Down(static_cast<int>(MapVirtualKeyExW(p, MAPVK_VSC_TO_VK, GetKeyboardLayout(0)))); }

// A key's press, once: true on the frame it goes down.
struct Press {
    bool was = false;
    bool operator()(bool down) { const bool p = down && !was; was = down; return p; }
};

// The window's client area to a 24-bit .bmp, as shown (PrintWindow with the full content, which reads a flip-model
// swap chain too).
bool SaveScreenshot(HWND wnd, const char* path)
{
    RECT rc;
    GetClientRect(wnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(wnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool printed = PrintWindow(wnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = w;
    bi.biHeight = h;   // bottom-up, as a .bmp stores it
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
    GetDIBits(mdc, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(wnd, wdc);
    if (!printed) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
    std::fwrite(&fh, sizeof fh, 1, f);
    std::fwrite(&bi, sizeof bi, 1, f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    return std::fclose(f) == 0;
}

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a)
{
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 0 ? a * (1.0f / l) : a;
}
V3 ToRender(V3 p) { return {p.x, p.z, p.y}; }

// A vector of the body's frame into the world's (the pose's quaternion x, y, z, w).
V3 Rotate(const float q[4], V3 v)
{
    const V3 u{q[0], q[1], q[2]};
    const V3 t = Cross(u, v) * 2.0f;
    return v + t * q[3] + Cross(u, t);
}

// What is drawn between two physics steps (a frame falls between them): a + (b - a) t, and for rotations the normalised
// blend along the shorter way. Equal ends give the end exactly (a resting object stays as it is).
void Lerp(const float* a, const float* b, float t, int n, float* out)
{
    for (int i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}
void Nlerp(const float a[4], const float b[4], float t, float out[4])
{
    if (!std::memcmp(a, b, 16)) {
        std::memcpy(out, b, 16);
        return;
    }
    const float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
    float l = 0;
    for (int i = 0; i < 4; ++i) l += (out[i] = a[i] + (b[i] * sign - a[i]) * t) * out[i];
    l = l > 0 ? 1.0f / std::sqrt(l) : 1.0f;
    for (int i = 0; i < 4; ++i) out[i] *= l;
}

void LookAt(V3 eye, V3 at, float* m)
{
    const V3 f = Norm(at - eye), r = Norm(Cross({0, 1, 0}, f)), u = Cross(f, r);
    const V3 ax[3] = {r, u, f};
    for (int c = 0; c < 3; ++c) {
        m[0 * 4 + c] = ax[c].x;
        m[1 * 4 + c] = ax[c].y;
        m[2 * 4 + c] = ax[c].z;
        m[3 * 4 + c] = -(ax[c].x * eye.x + ax[c].y * eye.y + ax[c].z * eye.z);
    }
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

void Perspective(float fovY, float aspect, float zn, float zf, float* m)
{
    std::memset(m, 0, 16 * sizeof(float));
    const float ys = 1.0f / std::tan(fovY * 0.5f), q = zf / (zf - zn);
    m[0] = ys / aspect;
    m[5] = ys;
    m[10] = q;
    m[11] = 1.0f;
    m[14] = -q * zn;
}

// The car's body hull (its collision mesh, MrCarCollision: the body's frame, physics axes) as a flat-shaded mesh in
// the renderer's axes.
ngg_mesh HullMesh(ngg_scene* s, const phys::data::RecMesh& m)
{
    std::vector<ngg_vertex> v;
    std::vector<uint32_t> idx;
    auto at = [&](uint8_t i) {
        float p[3];
        std::memcpy(p, &m.verts[4 * i], 12);
        return ToRender({p[0], p[1], p[2]});
    };
    for (size_t f = 0; f + 7 <= m.faces.size(); f += 7) {
        const V3 a = at(m.faces[f]), b = at(m.faces[f + 1]), c = at(m.faces[f + 2]);
        const V3 n = Norm(Cross(b - a, c - a));
        for (const V3& p : {a, b, c}) {
            ngg_vertex x{};
            x.pos[0] = p.x;
            x.pos[1] = p.y;
            x.pos[2] = p.z;
            x.normal[0] = n.x;
            x.normal[1] = n.y;
            x.normal[2] = n.z;
            x.colour = 0xff2040c0;   // ABGR: a red car
            idx.push_back(static_cast<uint32_t>(v.size()));
            v.push_back(x);
        }
    }
    return ngg_add_mesh(s, v.data(), static_cast<uint32_t>(v.size()), idx.data(), static_cast<uint32_t>(idx.size()));
}

// The car's placement as the renderer's row-major matrix (row vectors, translation in row 3): the pose's rotation
// conjugated by the axis swap.
void CarMatrix(const phys::sim::Pose& p, float* m)
{
    const V3 cols[3] = {ToRender(Rotate(p.rotation, {1, 0, 0})), ToRender(Rotate(p.rotation, {0, 0, 1})),
                        ToRender(Rotate(p.rotation, {0, 1, 0}))};   // the images of the renderer's x, y, z
    for (int r = 0; r < 3; ++r) {
        m[r * 4 + 0] = cols[r].x;
        m[r * 4 + 1] = cols[r].y;
        m[r * 4 + 2] = cols[r].z;
        m[r * 4 + 3] = 0;
    }
    const V3 t = ToRender({p.position[0], p.position[1], p.position[2]});
    m[12] = t.x;
    m[13] = t.y;
    m[14] = t.z;
    m[15] = 1;
}

// A car part's placement in the car's frame (the pose's: the centre of gravity), as the renderer's row-major matrix
// (row vectors), to be followed by CarMatrix. The model's frame is the car body's: a body part sits at -cog. A wheel's
// part (in its hub's frame, the model's axes) goes with the physics' wheel: its knuckle's rotation and hub, the spin
// about the knuckle's x for the parts turning with it; the right wheels' knuckles are turned half about z, which the
// model's right wheels already are (car_model.h), so that half turn is taken out first.
void PartMatrix(const float cog[3], const phys::sim::Wheel* w, bool right, bool spins, float* out)
{
    V3 cols[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // physics axes: the images of x, y, z
    V3 t{-cog[0], -cog[1], -cog[2]};
    if (w) {
        const float c = spins ? std::cos(w->spin) : 1.0f, s = spins ? std::sin(w->spin) : 0.0f;
        const V3 local[3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};   // the spin about x
        const float f = right ? -1.0f : 1.0f;                   // the half turn about z (its own inverse)
        const V3 flipped[3] = {local[0] * f, local[1] * f, local[2]};
        for (int i = 0; i < 3; ++i) cols[i] = Rotate(w->rotation, flipped[i]);
        t = {w->position[0], w->position[1], w->position[2]};
    }
    // the renderer's axes (y and z swapped): the image of the renderer's axis i is the swapped image of physics' axis
    // swap(i)
    const V3 r[3] = {ToRender(cols[0]), ToRender(cols[2]), ToRender(cols[1])};
    for (int i = 0; i < 3; ++i) {
        out[4 * i + 0] = r[i].x;
        out[4 * i + 1] = r[i].y;
        out[4 * i + 2] = r[i].z;
        out[4 * i + 3] = 0;
    }
    const V3 tr = ToRender(t);
    out[12] = tr.x;
    out[13] = tr.y;
    out[14] = tr.z;
    out[15] = 1;
}

// a then b (row vectors: a * b)
void Mul44(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}

// The HUD's font: a monospace atlas (ASCII 32..127, 16 x 6 cells) rasterised by GDI, white glyphs in the alpha, as
// ngg_set_font takes it.
bool UploadFont(ngg_scene* s)
{
    const int cw = 36, ch = 64, cols = 16, rows = 6, w = cw * cols, h = ch * rows;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return DeleteDC(dc), false;
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-ch * 3 / 4, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all{0, 0, w, h};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    for (int c = 32; c < 128; ++c) {
        const int k = c - 32;
        RECT cell{(k % cols) * cw, (k / cols) * ch, (k % cols + 1) * cw, (k / cols + 1) * ch};
        const wchar_t t[2] = {static_cast<wchar_t>(c), 0};
        DrawTextW(dc, t, 1, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    GdiFlush();
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[4 * i + 0] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255;
        rgba[4 * i + 3] = src[4 * i + 1];   // the glyph's coverage (green: any channel of white on black)
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ngg_set_font(s, rgba.data(), w, h, cols, rows, 32) != 0;
}

// The HUD as labels: ngg-api's labels sit at world points and keep their pixel size, so each line is anchored 1 m in
// front of the camera, where the screen position (x, y in -1 .. 1, y up) projects - drawn over everything.
struct Hud {
    V3 eye, right, up, fwd;
    float tanX, tanY;
    V3 At(float x, float y) const { return eye + fwd + right * (x * tanX) + up * (y * tanY); }
};

void DrawHud(ngg_scene* s, const Hud& h, const phys::sim::Dashboard& d, int width, int height)
{
    // Each line twice: a dark shadow a few pixels down-right (layers 0..2), then the text (layers 3..5).
    auto put = [&](uint32_t layer, float x, float y, const char* text, uint32_t colour, float px) {
        const float dx = px * 0.06f * 2.0f / static_cast<float>(width), dy = px * 0.06f * 2.0f / static_cast<float>(height);
        for (int pass = 0; pass < 2; ++pass) {
        const V3 p = pass ? h.At(x, y) : h.At(x + dx, y - dy);
        ngg_label l{};
        l.pos[0] = p.x;
        l.pos[1] = p.y;
        l.pos[2] = p.z;
        l.colour = pass ? colour : 0xc0000000;
        l.text_offset = 0;
        ngg_set_labels(s, layer + 3 * pass, &l, 1, text, static_cast<uint32_t>(std::strlen(text) + 1), px, NGG_OVERLAY_NO_DEPTH);
        }
    };
    char speed[32], rpm[32], gear[8];
    std::snprintf(speed, sizeof speed, "%3.0f km/h", std::fabs(d.speed) * 3.6f);
    std::snprintf(rpm, sizeof rpm, "%5.0f rpm", d.rpm > 0 ? d.rpm : 0.0f);
    std::snprintf(gear, sizeof gear, "%s", d.gear < 0 ? "R" : d.gear == 0 ? "N" : std::to_string(d.gear).c_str());
    const float textPx = height * 0.045f, gearPx = height * 0.12f;
    const uint32_t white = 0xf0ffffff, redline = d.rpm > 7000 ? 0xf03050ff : white;
    put(0, 0.72f, -0.62f, speed, white, textPx);
    put(1, 0.72f, -0.75f, rpm, redline, textPx);
    put(2, 0.90f, -0.68f, gear, 0xf040d0ff, gearPx);
}

// The renderer's readout (F1), as the viewer's HUD: lines of screen-space text at the top left, layer 10. Off, the
// layer is emptied.
void DrawDebugHud(ngg_scene* s, bool on, const std::vector<std::string>& lines, int height)
{
    constexpr uint32_t kLayer = 10;
    if (!on || lines.empty()) {
        ngg_set_labels(s, kLayer, nullptr, 0, nullptr, 0, 1.0f, 0);
        return;
    }
    const float px = std::max(12.0f, height * 0.022f);
    std::string blob;
    std::vector<ngg_label> labels;
    for (size_t i = 0; i < lines.size(); ++i) {
        ngg_label l{};
        l.pos[0] = px * 0.6f;
        l.pos[1] = px * 0.6f + static_cast<float>(i) * px * 1.25f;
        l.colour = 0xf0ffffff;
        l.text_offset = static_cast<uint32_t>(blob.size());
        labels.push_back(l);
        blob += lines[i];
        blob += '\0';
    }
    ngg_set_labels(s, kLayer, labels.data(), static_cast<uint32_t>(labels.size()), blob.data(), static_cast<uint32_t>(blob.size()),
                   px, NGG_LABEL_SCREEN | NGG_LABEL_SHADOW);
}

std::string StageTimeText(float t) { return phys::sim::StageClock::Text(t); }

// The stage clock on the screen (screen-space labels): the countdown big in the middle (layer 11) and the stage time
// with the splits at the top right (layer 12).
void DrawStageHud(ngg_scene* s, const phys::sim::StageClock& clock, int width, int height)
{
    constexpr uint32_t kCountdownLayer = 11, kTimeLayer = 12;
    auto put = [&](uint32_t layer, const std::vector<std::string>& lines, const std::vector<uint32_t>& colours, float px,
                   float right, float y0, bool centre) {
        if (lines.empty()) {
            ngg_set_labels(s, layer, nullptr, 0, nullptr, 0, 1.0f, 0);
            return;
        }
        std::string blob;
        std::vector<ngg_label> labels;
        for (size_t i = 0; i < lines.size(); ++i) {
            const float w = static_cast<float>(lines[i].size()) * px * 0.55f;   // about the font's advance
            ngg_label l{};
            l.pos[0] = centre ? (static_cast<float>(width) - w) * 0.5f : right - w;
            l.pos[1] = y0 + static_cast<float>(i) * px * 1.2f;
            l.colour = colours[std::min(i, colours.size() - 1)];
            l.text_offset = static_cast<uint32_t>(blob.size());
            labels.push_back(l);
            blob += lines[i];
            blob += '\0';
        }
        ngg_set_labels(s, layer, labels.data(), static_cast<uint32_t>(labels.size()), blob.data(),
                       static_cast<uint32_t>(blob.size()), px, NGG_LABEL_SCREEN | NGG_LABEL_SHADOW);
    };
    const std::string shown = clock.Shown();
    // the countdown's digits big; GO green, a false start red, a split / the finish smaller
    const bool go = shown == "GO", warn = shown == "FALSE START", count = shown.size() == 1;
    const bool finished = clock.GetState() == phys::sim::StageClock::State::Finished;
    const float big = height * (count || go ? 0.22f : 0.09f);
    put(kCountdownLayer, shown.empty() ? std::vector<std::string>{} : std::vector<std::string>{shown},
        {warn ? 0xf03030ffu : go || finished ? 0xf040e040u : 0xf0ffffffu}, big, 0, height * 0.32f - big * 0.5f, true);
    std::vector<std::string> lines;
    std::vector<uint32_t> colours;
    const float px = std::max(14.0f, height * 0.04f);
    lines.push_back(StageTimeText(clock.Time()));
    colours.push_back(clock.GetState() == phys::sim::StageClock::State::Finished ? 0xf040e040u : 0xf0ffffffu);
    if (clock.FalseStart()) {
        lines.push_back("false start +10 s");
        colours.push_back(0xf03030ffu);
    }
    for (size_t i = 0; i < clock.Splits().size(); ++i) {
        lines.push_back("split " + std::to_string(i + 1) + "  " + StageTimeText(clock.Splits()[i]));
        colours.push_back(0xf0c0e8ffu);
    }
    put(kTimeLayer, lines, colours, px, static_cast<float>(width) - px * 0.8f, px * 0.6f, false);
}

// The stage's movable objects drawn where the physics has them: the .ngg keys every object built from a movable
// placement with RBR's instance id (NGGK, ngg_object_key), the same id the .trk's collision instance - the physics'
// solid - has. Each frame a solid that has moved gives its objects its world-space move since the load
// (ngg_set_transform on loaded geometry).
struct Follower {
    std::unordered_map<int32_t, std::vector<uint32_t>> byKey;   // instance id -> the .ngg objects keyed with it
    std::vector<uint8_t> moving;

    size_t Load(ngg_scene* s, uint32_t count)
    {
        size_t keyed = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (const int32_t k = ngg_object_key(s, i); k >= 0) {
                byKey[k].push_back(i);
                ++keyed;
            }
        return keyed;
    }

    void Update(ngg_scene* s, const std::vector<phys::sim::Body>& bodies)
    {
        if (moving.size() != bodies.size()) moving.assign(bodies.size(), 0);
        for (size_t i = 0; i < bodies.size(); ++i) {
            const phys::sim::Body& b = bodies[i];
            if (!moving[i]) {
                if (!std::memcmp(b.position, b.startPosition, 12) && !std::memcmp(b.rotation, b.startRotation, 16)) continue;
                moving[i] = 1;
            }
            const auto it = byKey.find(b.id);
            if (it == byKey.end()) continue;
            // The move since the load: q = now * start^-1, then x' = q (x - start) + now; as the renderer's row-major
            // matrix (rows: the images of its axes; translation in row 3).
            const float* a = b.rotation;
            const float bs[4] = {-b.startRotation[0], -b.startRotation[1], -b.startRotation[2], b.startRotation[3]};
            const float q[4] = {a[3] * bs[0] + a[0] * bs[3] + a[1] * bs[2] - a[2] * bs[1],
                                a[3] * bs[1] - a[0] * bs[2] + a[1] * bs[3] + a[2] * bs[0],
                                a[3] * bs[2] + a[0] * bs[1] - a[1] * bs[0] + a[2] * bs[3],
                                a[3] * bs[3] - a[0] * bs[0] - a[1] * bs[1] - a[2] * bs[2]};
            float m[16];
            const V3 rows[3] = {ToRender(Rotate(q, {1, 0, 0})), ToRender(Rotate(q, {0, 0, 1})), ToRender(Rotate(q, {0, 1, 0}))};
            for (int r = 0; r < 3; ++r) {
                m[r * 4 + 0] = rows[r].x;
                m[r * 4 + 1] = rows[r].y;
                m[r * 4 + 2] = rows[r].z;
                m[r * 4 + 3] = 0;
            }
            const V3 st = ToRender({b.startPosition[0], b.startPosition[1], b.startPosition[2]});
            const V3 now = ToRender({b.position[0], b.position[1], b.position[2]});
            const V3 turned = rows[0] * st.x + rows[1] * st.y + rows[2] * st.z;   // the start point turned
            m[12] = now.x - turned.x;
            m[13] = now.y - turned.y;
            m[14] = now.z - turned.z;
            m[15] = 1;
            for (uint32_t o : it->second) ngg_set_transform(s, o, m);
        }
    }
};

// A path's file name without its extension.
std::string Stem(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\"), from = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    return path.substr(from, dot == std::string::npos || dot < from ? std::string::npos : dot - from);
}

// The stage an .ngg draws, from its name: track-34_E.ngg that stage; track-34.ngg the map's one stage in Maps (its
// .trk), none when it has several (`why` names them) or none.
std::string NggStage(const std::string& game, const std::string& nggPath, std::string& why)
{
    const std::string stem = Stem(nggPath);
    const int map = phys::sim::StageMap(stem);
    if (map < 0) return why = "no track-<n> in its name", "";
    if (stem.find('_') != std::string::npos) {
        if (GetFileAttributesA((game + "/Maps/" + stem + ".trk").c_str()) != INVALID_FILE_ATTRIBUTES) return why = "its name", stem;
        return why = "no Maps/" + stem + ".trk", "";
    }
    std::vector<std::string> found;
    WIN32_FIND_DATAA fd;
    if (HANDLE h = FindFirstFileA((game + "/Maps/track-" + std::to_string(map) + "_*.trk").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do found.push_back(Stem(fd.cFileName));
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (found.size() == 1) return why = "map " + std::to_string(map) + "'s only stage in Maps", found[0];
    why = found.empty() ? "no Maps/track-" + std::to_string(map) + "_*.trk" : "map " + std::to_string(map) + " has";
    for (const std::string& f : found) why += " " + f;
    for (const std::string& f : found)   // several: the morning one (_M), as before
        if (!_stricmp(f.c_str(), ("track-" + std::to_string(map) + "_M").c_str())) return why += "; the morning one taken", f;
    return "";
}

std::string Arg(int argc, char** argv, const char* name, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
bool HasFlag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], name)) return true;
    return false;
}

// One of WRally's own files (`rel` from the repository root: content/..., game/config/...): beside the exe as the game
// ships, else in the source tree the build came from (WRALLY_SOURCE_DIR) - where editing it shows at once, as the
// watched sound mix wants. "" if neither has it.
std::string WRallyFile(const std::string& rel)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.erase(dir.find_last_of("\\/") + 1);
    for (const std::string& path : {dir + rel, std::string(WRALLY_SOURCE_DIR) + "/" + rel})
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    return "";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: WRally <game dir> <stage .ngg> [--stage track-34_E] [--sgc <car .sgc>] [--car s_i2003] "
                             "[--surface gravel] [--tyre-type 3] [--tyres michelin] [--out drive.phr]\n");
        return 2;
    }
    const std::string game = argv[1], nggPath = argv[2];
    std::string stage = Arg(argc, argv, "--stage", "");
    {
        std::string why;
        const std::string fromNgg = NggStage(game, nggPath, why);
        if (stage.empty()) {
            if (fromNgg.empty()) return std::fprintf(stderr, "%s: --stage needed (%s)\n", nggPath.c_str(), why.c_str()), 2;
            stage = fromNgg;
            std::printf("stage %s from the .ngg's name (%s)\n", stage.c_str(), why.c_str());
        } else if (const int a = phys::sim::StageMap(stage), b = phys::sim::StageMap(Stem(nggPath)); a >= 0 && b >= 0 && a != b) {
            std::printf("WARNING: --stage %s is map %d, the .ngg (%s) map %d: the car runs on another stage's ground than "
                        "the one drawn\n", stage.c_str(), a, nggPath.c_str(), b);
        }
    }
    const std::string maps = game + "/Maps/" + stage;
    phys::sim::Config c;
    c.rbz = game + "/physics.rbz";
    c.trk = maps + ".trk";
    c.col = maps + ".col";
    c.mat = maps + ".mat";
    c.fnc = maps + ".fnc";
    c.lbs = maps + ".lbs";
    c.sgc = game + "/" + Arg(argc, argv, "--sgc", "Cars/i20_R5/i20_R5.sgc");
    c.car = Arg(argc, argv, "--car", "s_i2003");
    // WRally's values on top of RBR's car: --overrides, else content/cars/<car>.ini (WRallyFile) if there is one
    // ("none": RBR's car as it is).
    c.overrides = Arg(argc, argv, "--overrides", "");
    if (c.overrides.empty()) {
        c.overrides = WRallyFile("content/cars/" + c.car + ".ini");
    } else if (c.overrides == "none") {
        c.overrides.clear();
    }
    // The surface and the tyre type: the stage's unless given.
    {
        std::string from = "--surface";
        c.surface = Arg(argc, argv, "--surface", "");
        if (c.surface.empty()) c.surface = phys::sim::StageSurface(game, phys::sim::StageMap(stage), &from);
        if (c.surface != "tarmac" && c.surface != "gravel" && c.surface != "snow")
            return std::fprintf(stderr, "--surface %s: tarmac, gravel or snow\n", c.surface.c_str()), 2;
        const std::string type = Arg(argc, argv, "--tyre-type", "");
        c.tyreIndex = phys::sim::SurfaceTyreType(c.surface, c.wetness >= 0 ? c.wetness : static_cast<int32_t>(c.weather / 3));
        if (!type.empty()) {
            char* end;
            c.tyreIndex = std::strtoul(type.c_str(), &end, 10);
            if (end == type.c_str() || *end)
                for (c.tyreIndex = 0; c.tyreIndex < 7 && type != phys::sim::kTyreTypeNames[c.tyreIndex]; ++c.tyreIndex) {}
            if (c.tyreIndex >= 7) return std::fprintf(stderr, "--tyre-type %s: 0..6 or a TyreTypeNN name\n", type.c_str()), 2;
        }
        c.tyres = Arg(argc, argv, "--tyres", "michelin");
        std::printf("stage %s: surface %s (%s), tyres TyreType%02u %s%s, brand %s\n", stage.c_str(), c.surface.c_str(),
                    from.c_str(), c.tyreIndex, phys::sim::kTyreTypeNames[c.tyreIndex], type.empty() ? "" : " (--tyre-type)",
                    c.tyres.c_str());
    }
    const std::string out = Arg(argc, argv, "--out", "drive.phr");
    // The driving aids (AutoGears, GearGuard, NeutralLock, ClutchHelp): none by default, as the game's. Without the
    // clutch help the engine stalls when a gear goes in at idle with no throttle; it restarts in neutral with the
    // throttle (StarterMotor). --aids 0,0,0,1 keeps it running.
    {
        const std::string aids = Arg(argc, argv, "--aids", "0,0,0,0");
        const char* a = aids.c_str();
        for (int k = 0; k < 4 && a; ++k) {
            char* end;
            c.aids[k] = std::strtoul(a, &end, 0);
            a = *end == ',' ? end + 1 : nullptr;
        }
    }

    // --replay <run.phr>: a recording's controls and steps instead of the keys, with its settings and animation objects'
    // pick (a recording does not hold the driving aids: none unless --aids says).
    phys::data::Recording replay;
    size_t replayAt = 0;
    const std::string replayPath = Arg(argc, argv, "--replay", "");
    if (!replayPath.empty()) {
        try {
            replay = phys::data::ReadRecording(replayPath);
        } catch (const std::exception& e) {
            return std::fprintf(stderr, "%s\n", e.what()), 3;
        }
        if (replay.settings & 1) c.settings = replay.settings;
        c.anims = replay.anims;
        if (Arg(argc, argv, "--aids", "").empty())
            for (uint32_t& a : c.aids) a = 0;
    }
    // The physics: the native one (sim/native_sim.h). It has not got the replays' animation objects (spectators) yet.
    if (!c.anims.empty()) {   // (stage 41's recording replays bit-exact without its spectators)
        std::printf("the replay's %zu animation objects left out (the native physics has none yet)\n", c.anims.size());
        c.anims.clear();
    }
    std::unique_ptr<phys::sim::Physics> physics = std::make_unique<phys::sim::NativeSim>();
    phys::sim::Physics& sim = *physics;
    std::printf("loading the native physics...\n");
    if (const std::string e = sim.Open(c); !e.empty()) return std::fprintf(stderr, "physics: %s\n", e.c_str()), 3;
    std::printf("%sdt %.4f s\n", sim.Log().c_str(), sim.Dt());
    // The modules hear the physics on the bus: the sound plays from the device's thread, a few ticks behind it.
    signals::Bus bus;
    std::string mixPath;          // the mix file, watched ("" none)
    FILETIME mixWritten{};
    audio::Bank bank;
    audio::Audio sound(audio::Audio::Mode::RealTime);
    audio::Device device;
    if (!HasFlag(argc, argv, "--no-sound")) {
        if (const std::string engine = Arg(argc, argv, "--eng", "Focus03-08"); engine != "none") {
            std::string missing = bank.LoadRbr(game, engine);
            if (const std::string b = Arg(argc, argv, "--als-bangs", ""); !b.empty()) missing += bank.LoadBangs(b);
            if (const std::string b = Arg(argc, argv, "--als-idle-bangs", ""); !b.empty()) missing += bank.LoadBangs(b, true);
            std::printf("sounds: %zu engine loops (%s)%s%s", bank.engine.size(), engine.c_str(),
                        missing.empty() ? "\n" : "; missing or odd:\n", missing.c_str());
            sound.SetBank(&bank);
        }
        sound.Connect(bus);
        sim.Connect(&bus);
        mixPath = Arg(argc, argv, "--mix", "");
        if (mixPath.empty()) mixPath = WRallyFile("game/config/sound-mix.ini");
        if (const std::string e = device.Open(sound); !e.empty()) std::printf("no sound: %s\n", e.c_str());
    }
    const std::vector<phys::data::RecMesh> meshes = phys::data::CarCollisionMeshes(phys::data::ReadSgcCollision(c.sgc));

    std::vector<char> bytes;
    if (FILE* f = std::fopen(nggPath.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
        std::fclose(f);
    }
    if (bytes.empty()) return std::fprintf(stderr, "cannot read %s\n", nggPath.c_str()), 3;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // the renderer's pixels are the screen's
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"phys-drive";
    RegisterClassW(&wc);
    // 3/4 of the screen's work area, 16:9
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = (work.right - work.left) * 3 / 4, height = width * 9 / 16;
    if (height > (work.bottom - work.top) * 3 / 4) height = (work.bottom - work.top) * 3 / 4, width = height * 16 / 9;
    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"phys-drive", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    GetClientRect(wnd, &rc);
    g_width = rc.right - rc.left;
    g_height = rc.bottom - rc.top;
    // PHYS_DRIVE_NGGLOG: the renderer's log on stdout (otherwise only the debugger's output has it).
    if (std::getenv("PHYS_DRIVE_NGGLOG")) ngg_set_log_sink([](const char* m) { std::printf("ngg: %s\n", m); });
    ngg_scene* s = ngg_create(wnd, g_width, g_height);
    int shownWidth = g_width, shownHeight = g_height;
    if (!s) return std::fprintf(stderr, "ngg_create failed\n"), 3;
    if (!ngg_load_memory(s, bytes.data(), bytes.size())) return std::fprintf(stderr, "ngg: %s\n", ngg_last_error(s)), 3;
    bytes.clear();
    Follower follower;
    if (const size_t keyed = follower.Load(s, ngg_instance_count(s)); keyed)   // the stage's objects, before ours
        std::printf("movable objects: %zu drawn objects keyed to %zu solids\n", keyed, follower.byKey.size());
    else
        std::printf("movable objects: the .ngg has no object keys (NGGK) - reconvert it; nothing will move\n");
    const ngg_material carMaterial = ngg_add_material(s, NGG_INVALID, NGG_INVALID, NGG_MATERIAL_TWO_SIDED);
    {   // a red, glossy paint (the material's factors: the texture slots are empty)
        ngg_material_desc d{};
        if (ngg_get_material(s, carMaterial, &d)) {
            d.base_colour_factor[0] = 0.55f;
            d.base_colour_factor[1] = 0.04f;
            d.base_colour_factor[2] = 0.03f;
            d.base_colour_factor[3] = 1.0f;
            d.roughness = 0.35f;
            ngg_set_material(s, carMaterial, &d);
        }
    }
    // The car: its render model from the .sgc (the exterior, this surface's wheels), each part an instance moved with
    // the car; the body hull (its collision mesh) when the model gives nothing.
    struct Placed {
        ngg_instance instance;
        int wheel;     // -1: the car body's; else 0 lf, 1 rf, 2 lb, 3 rb (in that hub's frame)
        bool spins;    // turns with the wheel
    };
    std::vector<Placed> carParts;
    try {
        const phys::drive::CarModel model = phys::drive::ReadCarModel(c.sgc, c.surface);
        std::vector<ngg_material> mats;
        size_t textured = 0;
        for (const phys::drive::CarTexture& t : model.materials) {
            const ngg_texture tex = t.dds.empty() ? NGG_INVALID : ngg_add_texture(s, t.dds.data(), t.dds.size());
            textured += tex != NGG_INVALID;
            if (std::getenv("PHYS_DRIVE_MATERIALS")) std::printf("material %s: %zu bytes dds%s, diffuse %.2f %.2f %.2f\n", t.material.c_str(), t.dds.size(), tex == NGG_INVALID && !t.dds.empty() ? " (rejected)" : "", t.diffuse[0], t.diffuse[1], t.diffuse[2]);
            // One-sided, as the game draws them: the body is a shell whose inner faces (the texture's black) lie close
            // behind its paint, and drawn they show through it. The glass both ways, seen from inside too.
            const ngg_material m = ngg_add_material(s, tex, NGG_INVALID, t.transparent ? NGG_MATERIAL_TWO_SIDED | NGG_MATERIAL_ALPHA_TEST : 0u);
            ngg_material_desc d{};
            if (ngg_get_material(s, m, &d)) {
                if (tex == NGG_INVALID)
                    for (int k = 0; k < 3; ++k) d.base_colour_factor[k] = t.diffuse[k];
                d.roughness = 0.45f;
                ngg_set_material(s, m, &d);
            }
            mats.push_back(m);
        }
        for (const phys::drive::CarPart& part : model.parts) {
            std::vector<ngg_vertex> v(part.vertices.size());
            for (size_t k = 0; k < v.size(); ++k) {
                std::memcpy(v[k].pos, part.vertices[k].pos, 12);
                std::memcpy(v[k].normal, part.vertices[k].normal, 12);
                std::memcpy(v[k].uv0, part.vertices[k].uv, 8);
                std::memcpy(v[k].uv1, part.vertices[k].uv, 8);
                v[k].colour = 0xffffffff;
            }
            size_t mi = 0;
            while (mi < model.materials.size() && model.materials[mi].material != part.material) ++mi;
            const ngg_mesh mesh = ngg_add_mesh(s, v.data(), static_cast<uint32_t>(v.size()), part.indices.data(),
                                               static_cast<uint32_t>(part.indices.size()));
            if (mesh == NGG_INVALID || mi >= mats.size()) continue;
            carParts.push_back({ngg_add_instance(s, mesh, mats[mi], 0, nullptr), part.wheel, part.spins});
        }
        std::printf("the car: %zu parts, %zu materials (%zu textured)\n", carParts.size(), model.materials.size(), textured);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "the car's model: %s\n", e.what());
    }
    if (carParts.empty()) carParts.push_back({ngg_add_instance(s, HullMesh(s, meshes.at(0)), carMaterial, 0, nullptr), -1, false});
    const bool hud = UploadFont(s);
    if (!hud) std::fprintf(stderr, "the HUD's font: %s\n", ngg_last_error(s));

    // The recording of this drive (data/rec.h): the settings, the steps, the controls and the poses.
    phys::data::Recording rec;
    rec.settings = c.settings | 1;
    auto record = [&](char type, uint32_t tick, uint32_t steps, const uint32_t* w, int n) {
        phys::data::RecEvent e{};
        e.type = type;
        e.tick = tick;
        e.steps = steps;
        if (w) std::memcpy(e.words, w, 4 * n);
        rec.events.push_back(e);
    };
    auto step = [&](uint32_t n) {
        const uint32_t tick = sim.Tick();
        if (!sim.Step(n)) return false;
        record('S', tick, n, nullptr, 0);
        uint32_t pose[13];
        sim.PoseWords(pose);
        record('P', 0, 0, pose, 13);
        return true;
    };
    // The start as the game settles it: StepForward(40) until tick 160 (a replay has its own).
    for (int k = 0; k < (replay.events.empty() ? 4 : 0); ++k)
        if (!step(40)) return std::fprintf(stderr, "physics: %s\n", sim.Error().c_str()), 3;

    const double dt = sim.Dt();
    // The stage clock (sim::StageClock): the .dls's marks, the entry, the countdown.
    phys::sim::StageClock clock;
    {
        float finish = -1;
        std::vector<float> checkpoints;
        try {
            for (const phys::data::DlsPacenote& n : phys::data::ReadDls(maps + ".dls").pacenotes) {
                if (n.type == phys::data::kDlsFinish) finish = n.distance;
                else if (n.type == phys::data::kDlsCheckpoint) checkpoints.push_back(n.distance);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "the stage's marks: %s\n", e.what());
        }
        clock.SetMarks(finish, checkpoints);
        std::printf("stage marks: %zu splits, finish %.0f m along the drive line; the car at %.0f m\n", checkpoints.size(),
                    finish, sim.StageDistance());
    }
    const bool quickStart = HasFlag(argc, argv, "--quick-start");
    // The countdown starts this long after the load (the physics' time, tick 0 its load).
    const double countdownAt = quickStart ? 0.0 : std::max(0.0, std::atof(Arg(argc, argv, "--entry", "2").c_str()));
    auto inEntry = [&]() { return sim.Tick() * dt < countdownAt; };
    clock.Reset(quickStart ? 3.5f : phys::sim::StageClock::kCountdown);
    // After every physics step: the clock, its events printed (in the entry nothing).
    auto clockStep = [&]() {
        if (inEntry()) return;
        const float time = clock.Time();
        switch (clock.Advance(static_cast<float>(dt), sim.Dash().speed, sim.StageDistance())) {
        case phys::sim::StageClock::kGo:
            std::printf("GO (tick %u)\n", sim.Tick());
            break;
        case phys::sim::StageClock::kFalseStart:
            std::printf("FALSE START (tick %u): %.2f s of the countdown left + %.0f s\n", sim.Tick(),
                        clock.Time() - phys::sim::StageClock::kFalseStartPenalty, phys::sim::StageClock::kFalseStartPenalty);
            break;
        case phys::sim::StageClock::kSplit:
            std::printf("split %zu: %s (tick %u)\n", clock.Splits().size(), StageTimeText(clock.Splits().back()).c_str(), sim.Tick());
            break;
        case phys::sim::StageClock::kFinish:
            std::printf("FINISH: %s%s (tick %u)\n", StageTimeText(time).c_str(), clock.FalseStart() ? " (the false start's 10 s in)" : "",
                        sim.Tick());
            break;
        default:
            break;
        }
    };
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double acc = 0, title = 0;
    float steering = 0;
    bool upWas = false, downWas = false, camWas = false, resetWas = false;
    // The renderer's diagnostics: T its TAA, O its ambient occlusion (quality medium / low), L its shadows, P a pause
    // (the physics, a replay and the rendering stop - the picture stays as it was; T / O / L / V draw one frame), F2 a screenshot (shot-NNN.bmp beside the recording).
    // V steps through the renderer's debug views (no textures, the RT sun visibility, the RT AO, ...), their names
    // printed. --pause-at <tick> (or PHYS_DRIVE_PAUSE_AT): pause on reaching that tick, to look at a stretch of a replay.
    Press taaKey, pauseKey, aoKey, shadowKey, shotKey, viewKey;
    int debugView = 0;
    auto argOrEnv = [&](const char* name, const char* env) {
        const std::string a = Arg(argc, argv, name, "");
        return !a.empty() ? a : std::string(std::getenv(env) ? std::getenv(env) : "");
    };
    uint32_t pauseAt = std::strtoul(argOrEnv("--pause-at", "PHYS_DRIVE_PAUSE_AT").c_str(), nullptr, 0);
    // --exit-at <tick> (or PHYS_DRIVE_EXIT_AT): quit on reaching that tick, as Esc would.
    const uint32_t exitAt = std::strtoul(argOrEnv("--exit-at", "PHYS_DRIVE_EXIT_AT").c_str(), nullptr, 0);
    const uint32_t skipTo = std::strtoul(Arg(argc, argv, "--skip-to", "0").c_str(), nullptr, 0);
    bool taa = true, paused = false, ao = true, shadows = true;
    // K the ray tracing (off: the cascaded shadow maps, no RT AO); F1 the renderer's readout, as the viewer's HUD.
    Press rtKey, debugHudKey, muteKey, antiLagIdleKey;
    bool antiLagIdle = false;   // N: the anti-lag armed at standstill (the driver's switch before a start)
    bool rt = true, debugHud = false;
    if (std::getenv("PHYS_DRIVE_READOUT")) debugHud = true;   // PHYS_DRIVE_READOUT: the readout on from the start
    double fpsSmooth = 0.0;   // wall-clock frames per second, eased, for the readout
    LARGE_INTEGER fpsLast{};
    int shots = 0;
    // PHYS_DRIVE_OFF=taa,ao,shadows,rt: any of them off from the start, as the keys would.
    if (const char* off = std::getenv("PHYS_DRIVE_OFF")) {
        if (std::strstr(off, "taa")) ngg_set_taa(s, 0), taa = false;
        if (std::strstr(off, "ao")) ngg_set_quality(s, 0), ao = false;
        if (std::strstr(off, "shadows")) ngg_set_shadows(s, 0), shadows = false;
        if (std::strstr(off, "rt")) ngg_set_ray_tracing(s, 0), rt = false;
    }
    // A replay's TV cameras (camera 4, a replay's first): the stage's .dls cameras by the car's distance along its drive
    // line; the chase camera where none covers it.
    std::unique_ptr<phys::drive::ReplayCameras> tv;
    if (!replay.events.empty()) {
        try {
            tv = std::make_unique<phys::drive::ReplayCameras>(phys::data::ReadDls(maps + ".dls"), phys::data::ReadLbsScripts(c.lbs).driveLine);
            if (tv->Empty()) tv.reset();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "the replay cameras: %s\n", e.what());
        }
    }
    const int cameras = tv ? 5 : 4;
    int camera = std::atoi(Arg(argc, argv, "--camera", tv ? "4" : "0").c_str()) % cameras;
    int32_t tvCamera = -1;
    // The beside camera's place round the car (camera 3), radians from its left side toward its front.
    float orbit = static_cast<float>(std::atof(Arg(argc, argv, "--camera-angle", "0").c_str()) * 3.14159265 / 180.0);
    uint32_t gearFlags = 0;
    // What the frames draw: the physics' last two steps, a frame placed between them by the time left over (acc / dt),
    // so the car moves with the clock however the steps fall into frames (at 30 fps 4 or 5 of them). The chase camera's
    // eye trails the car step by step, on the physics' time, and is drawn between the steps the same way. Drawing
    // only: the physics and the recording do not see it.
    struct Shown {
        phys::sim::Pose pose;
        phys::sim::Wheel wheels[4];
        std::vector<phys::sim::Body> bodies;
        V3 chase;   // the chase camera's eye
        V3 behind;  // the way from the car to it, flat and unit: what trails, smoothed step by step
        float pull = 0.0f;   // metres further back for the speed, eased
    };
    // How far behind the car the chase camera sits, metres: --chase-distance, and [ / ] move it in and out while
    // driving. 5.2 by default, a fifth closer than the 6.5 it was.
    float chaseDistance = static_cast<float>(std::atof(Arg(argc, argv, "--chase-distance", "5.2").c_str()));
    chaseDistance = std::max(2.0f, std::min(20.0f, chaseDistance));
    bool chaseChanged = false;   // reported once the key is let go
    // The chase: at speed the camera drops back, --chase-pull metres per 100 km/h (1.5 by default, 0 off), at most
    // twice that, eased over about a second - so it falls back as the car accelerates and closes in as it brakes.
    const float chasePull = std::max(0.0f, static_cast<float>(std::atof(Arg(argc, argv, "--chase-pull", "1.5").c_str())));
    auto pullWant = [&](const phys::sim::Pose& p) {
        const float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
        return std::min(2.0f * chasePull, chasePull * speed * 3.6f / 100.0f);
    };
    // The eye: chaseDistance behind the car along `behind`, 2.2 m up. Only the DIRECTION trails the car's heading.
    // Smoothing the eye's position instead made it trail the car itself - a lag of 6 / s is speed / 6 behind its
    // target, 5 m more at 108 km/h - so the faster the car, the further back the camera fell.
    auto chaseBehind = [](const phys::sim::Pose& p) {
        const V3 fwd = Rotate(p.rotation, {0, -1, 0});
        return Norm({-fwd.x, -fwd.y, 0});
    };
    auto chaseEye = [&](const phys::sim::Pose& p, V3 behind, float pull) {
        return V3{p.position[0], p.position[1], p.position[2]} + behind * (chaseDistance + pull) + V3{0, 0, 2.2f};
    };
    Shown before, after;   // the steps a frame falls between
    auto capture = [&](Shown& out) {
        out.pose = sim.CarPose();
        sim.Wheels(out.wheels);
        out.bodies = sim.Bodies();
    };
    auto restart = [&]() {   // no step before: both the same, the eye where it settles
        capture(after);
        after.behind = chaseBehind(after.pose);
        after.pull = pullWant(after.pose);
        after.chase = chaseEye(after.pose, after.behind, after.pull);
        before = after;
    };
    auto stepped = [&]() {
        std::swap(before, after);
        capture(after);
        const V3 want = chaseBehind(after.pose);
        const V3 mixed = before.behind + (want - before.behind) * std::min(1.0f, static_cast<float>(dt) * 6.0f);
        // Opposite directions mix to nothing (a spin): take the new one then.
        after.behind = (mixed.x * mixed.x + mixed.y * mixed.y > 1e-6f) ? Norm(mixed) : want;
        after.pull = before.pull + (pullWant(after.pose) - before.pull) * std::min(1.0f, static_cast<float>(dt) * 1.2f);
        after.chase = chaseEye(after.pose, after.behind, after.pull);
    };
    // One step of a replay: its controls up to its next step, then the step. False at its end or on an error.
    auto replayStep = [&]() {
        while (replayAt < replay.events.size() && replay.events[replayAt].type != 'S') {
            if (replay.events[replayAt].type == 'C') sim.ApplyWords(replay.events[replayAt].words);
            ++replayAt;
        }
        if (replayAt == replay.events.size()) return false;
        if (!sim.Step(replay.events[replayAt++].steps)) {
            std::fprintf(stderr, "physics: %s\n", sim.Error().c_str());
            g_quit = true;
            return false;
        }
        clockStep();
        return true;
    };
    // --skip-to <tick>: the physics run there at full speed before the first frame, nothing drawn - a replay's steps,
    // or for a drive no controls (recorded, so the recording still replays).
    if (skipTo > sim.Tick()) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const uint32_t from = sim.Tick();
        while (!g_quit && sim.Tick() < skipTo) {
            if (!replay.events.empty()) {
                if (!replayStep()) break;
                continue;
            }
            phys::sim::Control ctl;
            uint32_t w[8];
            std::memcpy(w, &ctl, 32);
            sim.Apply(ctl);
            record('C', 0, 0, w, 8);
            if (!step(1)) {
                std::fprintf(stderr, "physics: %s\n", sim.Error().c_str());
                g_quit = true;
            }
            clockStep();
        }
        QueryPerformanceCounter(&t1);
        std::printf("skipped ticks %u to %u in %.1f s\n", from, sim.Tick(), double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart));
        QueryPerformanceCounter(&last);   // the skip is not a frame's time
    }
    restart();
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        // --step <seconds> (or PHYS_DRIVE_STEP): every frame advances the physics by exactly that, whatever the frame
        // took - the same frames at the same ticks run to run, for comparing renderer changes shot by shot.
        static const double fixedFrame = std::atof(argOrEnv("--step", "PHYS_DRIVE_STEP").c_str());
        const double frame = fixedFrame > 0.0 ? fixedFrame : std::min(0.1, double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));
        last = now;
        const bool focused = GetForegroundWindow() == wnd;
        // Gear changes on a key's press: one record carries the flag.
        const bool up = focused && DownAt(kGearUp), down = focused && DownAt(kGearDown), cam = focused && DownAt(kCamera);
        if (up && !upWas) gearFlags |= 1;
        if (down && !downWas) gearFlags |= 2;
        if (cam && !camWas) camera = (camera + 1) % cameras;
        bool redraw = false;      // a renderer switch changed: draw one frame even while paused
        if (taaKey(focused && DownAt(kTaa))) {
            taa = !taa;
            ngg_set_taa(s, taa ? 1 : 0); redraw = true;
            std::printf("TAA %s\n", taa ? "on" : "off");
        }
        if (aoKey(focused && DownAt(kAo))) {
            ao = !ao;
            ngg_set_quality(s, ao ? 1 : 0); redraw = true;
            std::printf("ambient occlusion %s\n", ao ? "on" : "off");
        }
        if (shadowKey(focused && DownAt(kShadows))) {
            shadows = !shadows;
            ngg_set_shadows(s, shadows ? 1 : 0); redraw = true;
            std::printf("shadows %s\n", shadows ? "on" : "off");
        }
        if (rtKey(focused && DownAt(kRt))) {
            rt = !rt;
            ngg_set_ray_tracing(s, rt ? 1 : 0); redraw = true;
            std::printf("ray tracing %s\n", rt ? "on" : "off (cascaded shadow maps)");
        }
        // The mix file saved: read again (checked about twice a second).
        static double mixCheck = 0;
        if (!mixPath.empty() && (mixCheck += frame) > 0.5) {
            mixCheck = 0;
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExA(mixPath.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &mixWritten)) {
                mixWritten = a.ftLastWriteTime;
                audio::Mix mix;
                const std::string e = mix.Read(mixPath);
                sound.SetMix(mix);
                std::printf("%smix %s:%s\n", e.c_str(), mixPath.c_str(), mix.Describe().c_str());
            }
        }
        if (antiLagIdleKey(focused && DownAt(kAntiLagIdle))) {
            antiLagIdle = !antiLagIdle;
            std::printf("anti-lag at standstill %s\n", antiLagIdle ? "armed" : "off");
        }
        if (muteKey(focused && DownAt(kMute))) {
            sound.muted = !sound.muted;
            std::printf("sound %s\n", sound.muted ? "muted" : "on");
        }
        if (debugHudKey(focused && Down(VK_F1))) {
            debugHud = !debugHud;
            redraw = true;
        }
        if (pauseKey(focused && DownAt(kPause))) {
            paused = !paused;
            std::printf("%s at tick %u\n", paused ? "paused" : "running", sim.Tick());
        }
        if (pauseAt && sim.Tick() >= pauseAt) {
            pauseAt = 0;
            paused = true;
            std::printf("paused at tick %u (--pause-at)\n", sim.Tick());
        }
        if (exitAt && sim.Tick() >= exitAt) {
            std::printf("exit at tick %u (--exit-at)\n", sim.Tick());
            g_quit = true;
        }
        if (viewKey(focused && DownAt(kView))) {
            if (!ngg_debug_view_name(++debugView)) debugView = 0;
            ngg_set_debug_view(s, debugView); redraw = true;
            std::printf("view %d: %s\n", debugView, ngg_debug_view_name(debugView));
        }
        // , / .: the beside camera round the car at 90 degrees / s; while paused each frame it turns is drawn.
        const int turn = (focused && DownAt(kOrbitRight) ? 1 : 0) - (focused && DownAt(kOrbitLeft) ? 1 : 0);
        if (turn && camera == 3) {
            orbit = std::remainder(orbit + turn * static_cast<float>(frame) * 1.5707963f, 6.2831853f);
            redraw = true;
        }
        // [ / ]: the chase camera in / out at 2 m / s. The eye the steps keep is moved with it, so the change shows at
        // once, paused too.
        const int zoom = (focused && DownAt(kChaseOut) ? 1 : 0) - (focused && DownAt(kChaseIn) ? 1 : 0);
        if (zoom && (camera == 0 || camera == 4)) {
            chaseDistance = std::max(2.0f, std::min(20.0f, chaseDistance + zoom * static_cast<float>(frame) * 2.0f));
            for (Shown* sh : {&before, &after}) sh->chase = chaseEye(sh->pose, sh->behind, sh->pull);
            redraw = true;
            chaseChanged = true;
        } else if (chaseChanged) {
            chaseChanged = false;
            std::printf("chase camera %.1f m behind the car\n", chaseDistance);
        }
        if (!paused) acc += frame;   // a pause keeps the frame where it is, between the same steps
        // R: the game's outside assistance (flag 0x10) - the car put back on the stage's drive line after 0.5 s and held
        // for 4 s. The game refuses it while the car moves faster than 1.5 m/s.
        const bool reset = focused && DownAt(kReset);
        if (reset && !resetWas) {
            if (sim.Dash().speed > 1.5f) std::printf("back on the road: stop the car first (%.1f m/s)\n", sim.Dash().speed);
            else gearFlags |= 0x10;
        }
        resetWas = reset;
        upWas = up;
        downWas = down;
        camWas = cam;
        const float target = focused ? (Down(VK_LEFT) ? -1.0f : 0.0f) + (Down(VK_RIGHT) ? 1.0f : 0.0f) : 0.0f;
        while (acc >= dt && !replay.events.empty()) {
            acc -= dt;
            if (!replayStep()) break;
            stepped();
        }
        while (acc >= dt && replay.events.empty()) {
            acc -= dt;
            // The steering moves toward the keys' target at 3 / s (back to the centre at 5 / s), as a keyboard
            // driver's filter would.
            const float rate = static_cast<float>(dt) * (target == 0.0f ? 5.0f : 3.0f);
            steering += std::max(-rate, std::min(rate, target - steering));
            phys::sim::Control ctl;
            ctl.throttle = focused && DownAt(kThrottle) ? 1.0f : 0.0f;
            {   // launch control between the pedal and the engine
                static phys::drive::LaunchControl launch = [&] {
                    phys::drive::LaunchControl l;
                    l.target = static_cast<float>(std::atof(Arg(argc, argv, "--launch-rpm", "7400").c_str()));
                    return l;
                }();
                static phys::drive::AntiLag antiLag = [&] {
                    phys::drive::AntiLag a;
                    a.crack = static_cast<float>(std::atof(Arg(argc, argv, "--als-crack", "0.25").c_str()));
                    a.enabled = a.crack > 0;
                    return a;
                }();
                const phys::sim::Dashboard dash = sim.Dash();
                const float pedal = ctl.throttle;
                const bool was = launch.Active();
                ctl.throttle = launch.Throttle(ctl.throttle, dash.rpm, dash.speed, dash.gear, static_cast<float>(dt));
                if (launch.Active() != was)
                    std::printf("launch control %s at %.0f rpm (tick %u)\n", launch.Active() ? "holding" : "off", dash.rpm, sim.Tick());
                antiLag.idleArmed = antiLagIdle;
                ctl.throttle = antiLag.Throttle(ctl.throttle, dash.rpm, dash.gear, dash.speed, static_cast<float>(dt));
                signals::DriverTick driver;   // for the sound: the anti-lag's crack is no driver's part throttle
                driver.stamp = {0, sim.Tick()};
                driver.pedal = pedal;
                driver.throttle = ctl.throttle;
                driver.launchControl = launch.Active();
                driver.antiLag = antiLag.Active();
                bus.Emit(driver);
            }
            ctl.brake = focused && DownAt(kBrake) ? 1.0f : 0.0f;
            ctl.handbrake = focused && Down(VK_SPACE) ? 1.0f : 0.0f;
            ctl.steering = steering;
            ctl.flags = gearFlags;
            gearFlags = 0;
            if (inEntry()) ctl.brake = ctl.handbrake = 1.0f;   // the entry: the car held
            uint32_t w[8];
            std::memcpy(w, &ctl, 32);
            sim.Apply(ctl);
            record('C', 0, 0, w, 8);
            if (!step(1)) {
                std::fprintf(stderr, "physics: %s\n", sim.Error().c_str());
                g_quit = true;
                break;
            }
            clockStep();
            stepped();
        }
        // This frame between the last two steps (past the last at a replay's end: the last).
        const float t = static_cast<float>(std::min(1.0, acc / dt));
        static std::vector<phys::sim::Body> bodies;
        bodies = after.bodies;
        for (size_t i = 0; i < bodies.size() && i < before.bodies.size(); ++i) {
            Lerp(before.bodies[i].position, after.bodies[i].position, t, 3, bodies[i].position);
            Nlerp(before.bodies[i].rotation, after.bodies[i].rotation, t, bodies[i].rotation);
        }
        follower.Update(s, bodies);
        phys::sim::Pose p;
        Lerp(before.pose.position, after.pose.position, t, 3, p.position);
        Nlerp(before.pose.rotation, after.pose.rotation, t, p.rotation);
        Lerp(before.pose.velocity, after.pose.velocity, t, 3, p.velocity);
        float m[16], cog[3];
        CarMatrix(p, m);
        sim.CenterOfGravity(cog);
        phys::sim::Wheel wheels[4];
        for (int k = 0; k < 4; ++k) {
            const phys::sim::Wheel &a = before.wheels[k], &b = after.wheels[k];
            Lerp(a.position, b.position, t, 3, wheels[k].position);
            Nlerp(a.rotation, b.rotation, t, wheels[k].rotation);
            // the spin is wrapped: the shorter way round
            const float d = std::remainder(b.spin - a.spin, 6.2831853f);
            wheels[k].spin = a.spin + d * t;
        }
        for (const Placed& part : carParts) {
            float local[16], world[16];
            PartMatrix(cog, part.wheel >= 0 ? &wheels[part.wheel] : nullptr, part.wheel == 1 || part.wheel == 3, part.spins, local);
            Mul44(local, m, world);
            ngg_set_transform(s, part.instance, world);
        }
        // The camera: behind and above the car (chase), on its roof looking ahead (bonnet), above it, beside it, or the
        // replay's TV camera.
        const V3 pos{p.position[0], p.position[1], p.position[2]};
        const V3 fwd = Rotate(p.rotation, {0, -1, 0}), upv = Rotate(p.rotation, {0, 0, 1});
        V3 eye, at;
        float fovY = 1.0f;
        phys::drive::ReplayView shot;
        tvCamera = -1;
        if (camera == 4 && tv->View(p.position, &shot)) {
            eye = {shot.eye[0], shot.eye[1], shot.eye[2]};
            at = {shot.at[0], shot.at[1], shot.at[2]};
            fovY = shot.fovY;
            tvCamera = shot.camera;
        } else if (camera == 0 || camera == 4) {
            eye = before.chase + (after.chase - before.chase) * t;
            at = pos + V3{0, 0, 0.8f};
        } else if (camera == 3) {   // beside the car, at its left front wheel (, / . turn it round): a check of the wheels
            const V3 left = Rotate(p.rotation, {1, 0, 0});
            const float ca = std::cos(orbit), sa = std::sin(orbit);
            const V3 side = left * ca + fwd * sa, ahead = fwd * ca - left * sa;   // left and fwd turned by the orbit
            at = pos + fwd * 0.4f;
            eye = at + side * 4.5f + ahead * 0.2f + upv * 0.6f;
        } else if (camera == 2) {   // above the car, north up: a check of the car's placement
            eye = pos + V3{0, -0.01f, 25.0f};
            at = pos;
        } else {
            eye = pos + upv * 1.25f - fwd * 0.3f;
            at = eye + fwd * 10.0f;
        }
        if (g_width != shownWidth || g_height != shownHeight) {
            ngg_resize(s, g_width, g_height);
            shownWidth = g_width;
            shownHeight = g_height;
        }
        float view[16], proj[16];
        LookAt(ToRender(eye), ToRender(at), view);
        const float aspect = shownHeight ? float(shownWidth) / float(shownHeight) : 16.0f / 9.0f;
        Perspective(fovY, aspect, 0.25f, 8000.0f, proj);
        if (hud) {
            Hud h;
            h.eye = ToRender(eye);
            h.fwd = Norm(ToRender(at) - h.eye);
            h.right = Norm(Cross({0, 1, 0}, h.fwd));
            h.up = Cross(h.fwd, h.right);
            h.tanY = std::tan(fovY * 0.5f);
            h.tanX = h.tanY * aspect;
            DrawHud(s, h, sim.Dash(), shownWidth, shownHeight);
            DrawStageHud(s, clock, shownWidth, shownHeight);
            // The readout: last frame's numbers (the renderer's are read after its frame).
            {
                LARGE_INTEGER t;
                QueryPerformanceCounter(&t);
                if (fpsLast.QuadPart && t.QuadPart > fpsLast.QuadPart) {
                    const double fps = double(freq.QuadPart) / double(t.QuadPart - fpsLast.QuadPart);
                    fpsSmooth = fpsSmooth > 0.0 ? fpsSmooth + (fps - fpsSmooth) * 0.1 : fps;
                }
                fpsLast = t;
            }
            std::vector<std::string> lines;
            ngg_stats st{};
            if (debugHud && ngg_get_stats(s, &st)) {
                char b[192];
                auto line = [&](const char* fmt, auto... a) { std::snprintf(b, sizeof b, fmt, a...); lines.emplace_back(b); };
                static const char* const kQuality[] = {"low", "medium", "high"};
                line("%.1f fps   %ux%u   gpu main %.2f ms (env %.2f, aerial fill %.2f)", fpsSmooth, shownWidth, shownHeight,
                     st.pass_ms, st.env_ms, st.aerial_ms);
                line("tick %u%s   camera %d   chase %.1f m%s", sim.Tick(), paused ? " PAUSED" : "", camera, chaseDistance,
                     fixedFrame > 0.0 ? "   fixed step" : "");
                line("K ray tracing %s   L shadows %s   O AO %s   T TAA %s   quality %s",
                     !st.rt_available ? "n/a" : !rt ? "off" : st.rt_active ? "ON" : "wanted", st.shadows ? "on" : "off",
                     st.ambient_occlusion ? "on" : "off", st.taa ? "on" : "off", kQuality[st.quality > 2 ? 1 : st.quality]);
                if (st.rt_active)
                    line("   rt %.2f ms (ao %.2f, denoise %.2f)   %u px, %.1f%% shadowed", st.rt_ms, st.ao_ms, st.denoise_ms,
                         st.rt_pixels, st.rt_pixels ? 100.0 * st.rt_shadowed / st.rt_pixels : 0.0);
                else if (st.rt_fallback[0])
                    line("   shadows from cascades: %s", st.rt_fallback);
                if (st.auto_exposure)
                    line("exposure x%.2f   metered %+.2f EV   adapted %+.2f EV", st.exposure, st.metered_ev, st.adapted_ev);
                else
                    line("exposure x%.2f (manual)", st.exposure);
                line("V view %u: %s", st.debug_view, ngg_debug_view_name(static_cast<int>(st.debug_view)));
                line("F1 readout  P pause  F2 shot  C camera  [ ] chase  , . orbit");
            }
            DrawDebugHud(s, debugHud, lines, shownHeight);
        }
        ngg_set_camera(s, view, proj);
        // PHYS_DRIVE_JITTER=<tick>: how steadily the car sits in the picture - per frame its centre's place on the screen
        // (pixels) and the eye's distance to it; their second differences (frame to frame changes of the motion) are
        // printed as RMS on reaching that tick, and the drive ends. A steady car against the camera has them near 0.
        static const uint32_t jitterTo = std::getenv("PHYS_DRIVE_JITTER") ? std::strtoul(std::getenv("PHYS_DRIVE_JITTER"), nullptr, 0) : 0;
        if (jitterTo && !paused) {
            static double hist[3][3];   // the last frames' x, y, distance
            static double sum[3];
            static int frames = 0;
            const V3 r = ToRender(pos);
            float clip[4];
            for (int j = 0; j < 4; ++j) {
                float v = view[12 + j];
                for (int i = 0; i < 3; ++i) v += (&r.x)[i] * view[4 * i + j];
                clip[j] = v;
            }
            float c4[4];
            for (int j = 0; j < 4; ++j) c4[j] = clip[0] * proj[j] + clip[1] * proj[4 + j] + clip[2] * proj[8 + j] + clip[3] * proj[12 + j];
            const V3 d = pos - eye;
            const double now3[3] = {(c4[0] / c4[3] * 0.5 + 0.5) * shownWidth, (0.5 - c4[1] / c4[3] * 0.5) * shownHeight,
                                    std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * 1000.0};
            for (int k = 0; k < 3; ++k) {
                hist[k][0] = hist[k][1];
                hist[k][1] = hist[k][2];
                hist[k][2] = now3[k];
                if (frames >= 30) {   // the camera settled
                    const double dd = hist[k][2] - 2 * hist[k][1] + hist[k][0];
                    sum[k] += dd * dd;
                }
            }
            ++frames;
            if (sim.Tick() >= jitterTo) {
                const int n = std::max(1, frames - 30);
                std::printf("jitter over %d frames to tick %u: screen x %.3f px, y %.3f px, distance %.2f mm (RMS of second differences)\n",
                            n, sim.Tick(), std::sqrt(sum[0] / n), std::sqrt(sum[1] / n), std::sqrt(sum[2] / n));
                g_quit = true;
            }
        }
        // PHYS_DRIVE_SHOTS=<n>: a screenshot every n ticks as well, for a replay looked at afterwards. Those come from
        // the renderer's own frame (ngg_capture_frame), which a covered or minimised window does not spoil.
        static const uint32_t shotEvery = std::getenv("PHYS_DRIVE_SHOTS") ? std::strtoul(std::getenv("PHYS_DRIVE_SHOTS"), nullptr, 0) : 0;
        static uint32_t shotTick = 0;
        const bool timedShot = shotEvery && !paused && sim.Tick() >= shotTick + shotEvery;
        if (timedShot) shotTick = sim.Tick();
        const bool keyShot = shotKey(focused && Down(VK_F2));
        std::string shotPath;
        if (keyShot || timedShot) {
            const size_t slash = out.find_last_of("/\\");
            char name[32];
            std::snprintf(name, sizeof name, "shot-%03d.bmp", shots++);
            shotPath = (slash == std::string::npos ? std::string() : out.substr(0, slash + 1)) + name;
            if (timedShot) ngg_capture_frame(s, shotPath.c_str());
        }
        // PAUSED, nothing is drawn either, so the picture stays as it was when the pause began: rendering on, TAA
        // would settle the still image within a few frames and wipe out what was being looked at. A renderer switch
        // (T / O / L / V) draws one frame - that frame alone, no settling.
        if (!paused || redraw || timedShot) ngg_render(s);
        else Sleep(15);
        if (!shotPath.empty()) {
            const bool saved = timedShot || SaveScreenshot(wnd, shotPath.c_str());
            std::printf("screenshot %s%s (tick %u)\n", shotPath.c_str(), saved ? "" : " failed", sim.Tick());
        }
        title += frame;
        if (title > 0.25) {
            title = 0;
            const float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
            wchar_t t[128];
            std::swprintf(t, 128, L"phys-drive - %.0f km/h - tick %u - %.0f fps", speed * 3.6f, sim.Tick(), frame > 0 ? 1.0 / frame : 0.0);
            if (tv) {
                const size_t n = std::wcslen(t);
                if (tvCamera >= 0) std::swprintf(t + n, 128 - n, L" - %.0f m, camera %d", tv->Distance(), tvCamera);
                else std::swprintf(t + n, 128 - n, L" - %.0f m", tv->Distance());
            }
            SetWindowTextW(wnd, t);
        }
    }
    ngg_destroy(s);
    try {
        phys::data::WriteRecording(out, rec);
        std::printf("the drive: %zu events to %s (tick %u)\n", rec.events.size(), out.c_str(), sim.Tick());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
    return 0;
}
