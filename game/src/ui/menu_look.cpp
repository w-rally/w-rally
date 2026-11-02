#include "ui/menu_look.h"

#include <algorithm>

namespace wrally::ui {

namespace {

// Where the livery is on every full screen (px at 1080p right of the design system's place), so its stripes hold still
// from screen to screen.
constexpr float kLiveryShift = 150;
float g_videoOn = 0;   // the video window's fade in, 0..1 (once there is a frame, over ~300 ms)
float g_stripeOn = 1;  // the video's stripe itself, 0..1: eased toward the screen's (~200 ms), so it fades in and out
float g_liveryOn = 1;  // the signal and white stripes, 0..1: the same, for a screen without them (the Garage)
double g_entrance = -1;  // when the menus' entrance began (ImGui's time; -1 none)
bool g_entranceStripes = true;   // the entrance's stripes fall in too (the intro's), else they hold (the loading's)
float g_lift = 0;        // the livery lifted out of the window, 0..1 (a stage's start)

// A stripe `q` lifted up out of the window as far as the entrance has it not yet in (its start `delay` s in), and as
// far as it is lifted out.
void Drop(ImVec2 q[4], float H, float k, float delay)
{
    const float off = (H + 80 * k) * ((g_entranceStripes ? 1 - Entrance(delay) : 0.0f) + g_lift);
    for (int i = 0; i < 4; ++i) q[i].y -= off;
}

// A screen's ground and its livery: the signal stripe and the white one beside it, each fading into the ground toward
// the bottom as the video's window does, as far as `on` (0..1) shows them. `shift` (px at 1080p) moves them right.
void Livery(ImDrawList* d, float W, float H, float k, float shift, ImU32 ground, float on)
{
    d->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), ground);
    if (on <= 0.01f) return;
    ImVec2 q[4];
    Stripe(W, H, k, 1430, 220, shift, q);
    Drop(q, H, k, 0.1f);
    FadeQuad(d, q, Alpha(kSignal, on), Alpha(ground, on));
    Stripe(W, H, k, 1690, 28, shift, q);
    Drop(q, H, k, 0.0f);
    FadeQuad(d, q, Alpha(kInk, on), Alpha(ground, on));
}

// The livery's third stripe, left of the signal one: a window onto the video loop, the video covering the stripe's
// bounds (cropped, centred) and fading in over the ground when its first frame comes. `on` 0..1: how far the video
// shows; `stripe` 0..1: the whole stripe. Under the video the dark of the design's placeholder (overlay to the ground,
// top to bottom), as see-through as the ground.
void VideoWindow(ImDrawList* d, float W, float H, float k, float shift, ImU32 ground, const VideoFrame& video, float on,
                 float stripe)
{
    if (stripe <= 0.01f) return;
    ImVec2 q[4];
    Stripe(W, H, k, 990, 400, shift, q);
    Drop(q, H, k, 0.2f);
    const float see = float((ground >> IM_COL32_A_SHIFT) & 0xff) / 255 * stripe;
    FadeQuad(d, q, Alpha(kSurfaceOverlay, see), Alpha(kSurfaceBase, see));
    on *= stripe;
    if (!video.texture || video.width <= 0 || video.height <= 0 || on <= 0.01f) return;

    const float x0 = q[3].x, x1 = q[1].x, y0 = q[0].y, y1 = q[2].y, bw = x1 - x0, bh = y1 - y0;
    const float s = std::max(bw / video.width, bh / video.height);
    const float uw = bw / (video.width * s), vh = bh / (video.height * s);
    auto uv = [&](ImVec2 p) { return ImVec2((1 - uw) * 0.5f + (p.x - x0) / bw * uw, (1 - vh) * 0.5f + (p.y - y0) / bh * vh); };
    d->AddImageQuad(static_cast<ImTextureID>(video.texture), q[0], q[1], q[2], q[3], uv(q[0]), uv(q[1]), uv(q[2]), uv(q[3]),
                    Alpha(kInk, on));
}

} // namespace

ImU32 Lerp(ImU32 a, ImU32 b, float t)
{
    auto ch = [&](int s) {
        const float x = float((a >> s) & 0xff), y = float((b >> s) & 0xff);
        return ImU32(x + (y - x) * t + 0.5f) << s;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImU32 Alpha(ImU32 c, float a) { return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xff) * a + 0.5f) << IM_COL32_A_SHIFT); }

float Tracked(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const char* text, float spacing)
{
    const float x0 = at.x;
    for (const char* p = text; *p;) {
        const char* q = p + 1;   // (a whole UTF-8 character)
        while ((*q & 0xc0) == 0x80) ++q;
        if (d) d->AddText(f, size, at, col, p, q);
        at.x += f->CalcTextSizeA(size, FLT_MAX, 0, p, q).x + spacing * size;
        p = q;
    }
    return at.x - x0 - (*text ? spacing * size : 0);
}

float KeyHint(ImDrawList* d, ImFont* f, float k, ImVec2 at, const char* key, const char* does)
{
    const float size = 17 * k, h = 34 * k, pad = 10 * k;
    const float w = std::max(h, Tracked(nullptr, f, size, at, 0, key, 0.08f) + 2 * pad);
    d->AddRectFilled(at, ImVec2(at.x + w, at.y + h), kSurfaceOverlay, 2 * k);
    d->AddRect(at, ImVec2(at.x + w, at.y + h), kLineStrong, 2 * k, 0, std::max(1.0f, k));
    const float ty = at.y + (h - size) * 0.5f;
    Tracked(d, f, size, ImVec2(at.x + (w - Tracked(nullptr, f, size, at, 0, key, 0.08f)) * 0.5f, ty), kInk, key, 0.08f);
    return w + 8 * k + Tracked(d, f, size, ImVec2(at.x + w + 8 * k, ty), kInkSecondary, does, 0.16f);
}

void Stripe(float W, float H, float k, float left, float w, float shift, ImVec2 q[4])
{
    const float x = W - (1920 - left - shift) * k, top = -40 * k, bottom = H + 40 * k, run = (bottom - top) * kTanStripe;
    q[0] = ImVec2(x, top), q[1] = ImVec2(x + w * k, top), q[2] = ImVec2(x + w * k - run, bottom), q[3] = ImVec2(x - run, bottom);
}

void FadeQuad(ImDrawList* d, const ImVec2 q[4], ImU32 top, ImU32 bottom)
{
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    const ImDrawIdx i = ImDrawIdx(d->_VtxCurrentIdx);
    d->PrimReserve(6, 4);
    d->PrimWriteIdx(i), d->PrimWriteIdx(ImDrawIdx(i + 1)), d->PrimWriteIdx(ImDrawIdx(i + 2));
    d->PrimWriteIdx(i), d->PrimWriteIdx(ImDrawIdx(i + 2)), d->PrimWriteIdx(ImDrawIdx(i + 3));
    d->PrimWriteVtx(q[0], white, top), d->PrimWriteVtx(q[1], white, top);
    d->PrimWriteVtx(q[2], white, bottom), d->PrimWriteVtx(q[3], white, bottom);
}

void Backdrop(ImDrawList* d, float W, float H, float k, ImU32 ground, const VideoFrame& video, bool window, bool stripes)
{
    const float dt = ImGui::GetIO().DeltaTime, ease = 1 - std::exp(-dt / 0.07f);
    g_liveryOn += ((stripes ? 1.0f : 0.0f) - g_liveryOn) * ease;
    Livery(d, W, H, k, kLiveryShift, ground, g_liveryOn);
    if (video.texture) g_videoOn = std::min(1.0f, g_videoOn + dt / 0.3f);
    g_stripeOn += ((window ? 1.0f : 0.0f) - g_stripeOn) * ease;
    VideoWindow(d, W, H, k, kLiveryShift, ground, video, g_videoOn, g_stripeOn);
}

void HideVideoStripe() { g_stripeOn = 0; }

void StartEntrance(bool stripes) { g_entrance = ImGui::GetTime(), g_entranceStripes = stripes; }

void LiftLivery(float lift) { g_lift = lift; }

float Entrance(float delay, float duration)
{
    if (g_entrance < 0) return 1;
    const double t = (ImGui::GetTime() - g_entrance) * 1.25;   // (its delays and durations played 25 % faster)
    if (t > 3) return g_entrance = -1, 1.0f;   // (long over: none)
    const float x = std::clamp(static_cast<float>(t - delay) / duration, 0.0f, 1.0f);
    return 1 - (1 - x) * (1 - x) * (1 - x);
}

void Plate(ImDrawList* d, ImVec2 a, float w, float h, float cut, ImU32 col)
{
    d->AddQuadFilled(a, ImVec2(a.x + w, a.y), ImVec2(a.x + w - cut, a.y + h), ImVec2(a.x, a.y + h), col);
}

void Marker(ImDrawList* d, float x, float mid, float w, float h, float on, float k)
{
    if (on <= 0.01f) return;
    const float mx = x - 12 * k * (1 - on), half = h * 0.5f, lean = half * kTanPlate;
    d->AddQuadFilled(ImVec2(mx + lean, mid - half), ImVec2(mx + w + lean, mid - half), ImVec2(mx + w - lean, mid + half),
                     ImVec2(mx - lean, mid + half), Alpha(kSignal, on));
}

void Chevron(ImDrawList* d, ImVec2 c, float size, bool right, ImU32 col)
{
    const float u = size / 24, s = right ? 1.0f : -1.0f;
    const ImVec2 p[3] = {ImVec2(c.x - 3 * u * s, c.y - 7 * u), ImVec2(c.x + 4 * u * s, c.y), ImVec2(c.x - 3 * u * s, c.y + 7 * u)};
    d->AddPolyline(p, 3, col, 0, 2.5f * u);
}

float Ease() { return 1.0f - std::exp(-ImGui::GetIO().DeltaTime / kEase); }

} // namespace wrally::ui
