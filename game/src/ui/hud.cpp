#include "ui/hud.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "ui/menu_look.h"

namespace wrally::ui {

namespace {

// The design system's HUD colours beside the menus' (ui/menu_look.h).
constexpr ImU32 kScrim = IM_COL32(10, 10, 11, 230), kWarning = IM_COL32(255, 194, 26, 255), kCritical = IM_COL32(255, 46, 58, 255);
constexpr float kSafe = 48;   // the safe area's margin, px at 1080p
// The HUD's plates and their text at 1.5 the design system's sizes (its 1080p px); the middle's big messages (the
// countdown, GO, a false start, the finish) at 0.7 of theirs.
constexpr float kHudSize = 1.5f, kMiddleSize = 0.7f;

// A HUD's sizes: px at 1080p times `k`, as the menus'.
float Scale(int width, int height) { return std::min(height / 1080.0f, width / 1280.0f); }

// Under every window: the menus over a stage (Esc, Settings) veil the HUD with the stage.
ImDrawList* Draw() { return ImGui::GetBackgroundDrawList(); }

// `text`'s width at `size` (tracked by `spacing` em).
float Width(ImFont* f, float size, const char* text, float spacing = 0) { return Tracked(nullptr, f, size, ImVec2(), 0, text, spacing); }

// `text` with its capitals' middle on `mid` (the faces' capitals sit ~0.6 of the size below the line's top).
float Line(ImFont* f, float size, float x, float mid, ImU32 col, const char* text, float spacing = 0)
{
    return Tracked(Draw(), f, size, ImVec2(x, mid - size * 0.6f), col, text, spacing);
}

// mm:ss.hh, the design's timer.
std::string Clock(float t)
{
    const long cs = std::lround(std::max(0.0f, t) * 100);
    char s[32];
    std::snprintf(s, sizeof s, "%02ld:%02ld.%02ld", cs / 6000, cs / 100 % 60, cs % 100);
    return s;
}

std::string Upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// The pacenote's arrow (the design's 24-unit glyph: up its stem, then across, the head at the end), `size` px square
// at `at` (its top left), 4.5 units thick (bolder than the design's 3), pointing left or right.
void Arrow(ImDrawList* d, ImVec2 at, float size, bool right, ImU32 col)
{
    const float u = size / 24, t = 4.5f * u;
    auto p = [&](float x, float y) { return ImVec2(at.x + (right ? 24 - x : x) * u, at.y + y * u); };
    const ImVec2 stem[3] = {p(18, 21), p(18, 10), p(7, 10)}, head[3] = {p(11, 5), p(6, 10), p(11, 15)};
    d->AddPolyline(stem, 3, col, 0, t);
    d->AddPolyline(head, 3, col, 0, t);
    // (Square caps: the stem's foot and the head's ends squared off by a half-thickness square each.)
    for (const ImVec2& e : {stem[0], head[0], head[2]}) d->AddRectFilled(ImVec2(e.x - t * 0.5f, e.y - t * 0.5f), ImVec2(e.x + t * 0.5f, e.y + t * 0.5f), col);
}

ImU32 Severity(int grade) { return CornerColour(grade); }

// One call at the top centre from `y` (its top): the grade's block (the pictogram, 1.5 the plate's size), then its
// plate, centred on the block. `s` scales the design's sizes (the next call is smaller); `a` its opacity. Returns its
// height.
float Call(const ImGuiLayer& ui, const std::string& call, float centre, float y, float k, float s, float a)
{
    ImFont* item = ui.MenuItem();
    const float size = 24 * k * s, pad = 8 * k * s, gap = 8 * k * s, h = size * 1.1f + 2 * pad, cut = 12 * k * s;
    const float p = 1.5f;   // the pictogram's scale over the plate's
    // The corner's grade (L1 .. R7, + / - on it) first, the rest after it; its numbers (distances) dimmer.
    std::vector<std::string> words;
    for (size_t i = 0; i < call.size();) {
        const size_t j = std::min(call.find(' ', i), call.size());
        if (j > i) words.push_back(Upper(call.substr(i, j - i)));
        i = j + 1;
    }
    if (words.empty()) return 0;
    const std::string first = words[0];   // (a copy: the word is erased below)
    const bool corner = first.size() >= 2 && (first[0] == 'L' || first[0] == 'R') && first[1] >= '1' && first[1] <= '7';
    const std::string grade = corner ? first.substr(1) : "";
    if (corner) words.erase(words.begin());

    const float ps = size * p, pp = 12 * k * s * p, arrow = 22 * k * s * p, pg = gap * p;
    const float block = corner ? pp + arrow + pg + Width(ui.Wordmark(), ps, grade.c_str()) + pp : 0, bh = corner ? h * p : h;
    float text = 0;
    for (const std::string& w : words) text += Width(item, size, w.c_str()) + gap;
    const float plate = words.empty() ? 0 : 12 * k * s + text - gap + 16 * k * s + cut;
    float x = centre - (block + plate) * 0.5f;
    const float mid = y + bh * 0.5f;
    ImDrawList* d = Draw();
    if (corner) {
        const ImU32 fill = Alpha(Severity(first[1] - '0'), a), ink = Alpha(kSurfaceBase, a);
        d->AddRectFilled(ImVec2(x, y), ImVec2(x + block, y + bh), fill);
        Arrow(d, ImVec2(x + pp, mid - arrow * 0.5f), arrow, first[0] == 'R', ink);
        Line(ui.Wordmark(), ps, x + pp + arrow + pg, mid, ink, grade.c_str());   // (the black italic: as bold as the arrow)
        x += block;
    }
    if (!words.empty()) {
        Plate(d, ImVec2(x, mid - h * 0.5f), plate, h, cut, Alpha(kScrim, a));
        float tx = x + 12 * k * s;
        for (const std::string& w : words) {
            const bool number = std::isdigit(static_cast<unsigned char>(w[0])) != 0;
            tx += Line(item, size, tx, mid, Alpha(number ? kInkSecondary : kInk, a), w.c_str()) + gap;
        }
    }
    return bh;
}

// The last split as it was crossed: the plate flashes once (~300 ms) when a new one comes.
size_t g_splits = 0;
float g_flash = 0;

} // namespace

ImU32 CornerColour(int grade)
{
    constexpr ImU32 kGreen = IM_COL32(62, 207, 90, 255);
    const float t = std::clamp((grade - 1) / 3.0f, 0.0f, 2.0f);
    return t <= 1 ? Lerp(kGreen, kWarning, t) : Lerp(kWarning, kCritical, t - 1);
}

void DrawDashboard(const ImGuiLayer& ui, const physics::Dashboard& dash, float rpmLimit, int width, int height)
{
    const float b = Scale(width, height), k = b * kHudSize, W = float(width), H = float(height);
    ImDrawList* d = Draw();
    ImFont* numeral = ui.Wordmark();
    ImFont* text = ui.MenuText();

    // The plate: the speed (its unit under it) and the gear after a white rule, its left edge cut at the slant.
    char speed[16];
    std::snprintf(speed, sizeof speed, "%.0f", std::fabs(dash.speed) * 3.6f);
    const char* gear = dash.gear < 0 ? "R" : dash.gear == 0 ? "N" : nullptr;
    char gearNum[8];
    if (!gear) std::snprintf(gearNum, sizeof gearNum, "%d", dash.gear), gear = gearNum;
    const float size = 48 * k, pad = 8 * k, cut = 12 * k;
    const float speedW = Width(numeral, size, "888"), gearW = Width(numeral, size, "8");
    const float w = (16 + 12) * k + speedW + 12 * k + 2 * k + 12 * k + gearW + 16 * k, h = pad + size + 2 * k + 14 * k + pad;
    const float x1 = W - kSafe * b, y1 = H - kSafe * b, x0 = x1 - w, y0 = y1 - h;
    d->AddQuadFilled(ImVec2(x0 + cut, y0), ImVec2(x1, y0), ImVec2(x1, y1), ImVec2(x0, y1), kScrim);
    const float sx = x0 + (16 + 12) * k + speedW;   // the speed's right edge
    Tracked(d, numeral, size, ImVec2(sx - Width(numeral, size, speed), y0 + pad - size * 0.18f), kInk, speed, 0);
    Tracked(d, text, 12 * k, ImVec2(sx - Width(text, 12 * k, "KM/H", 0.08f), y0 + pad + size + 2 * k), kInkSecondary, "KM/H", 0.08f);
    const float rx = sx + 12 * k;
    d->AddRectFilled(ImVec2(rx, y0 + pad), ImVec2(rx + 2 * k, y1 - pad), kInk);
    const float gx = rx + 2 * k + 12 * k;
    Tracked(d, numeral, size, ImVec2(gx + (gearW - Width(numeral, size, gear)) * 0.5f, y1 - pad - size * 1.18f), kSignal, gear, 0);

    // The shift lights over it, right-aligned: lit from 60 % of the limiter, the last two hot; the limiter the whole bar.
    const float limit = rpmLimit > 0 ? rpmLimit : 8000.0f, r = std::max(0.0f, dash.rpm) / limit;
    const int lit = std::clamp(static_cast<int>(std::floor((r - 0.6f) / 0.4f * 8 + 0.5f)), 0, 8);
    const bool limiter = r >= 0.98f;
    const float segW = 10 * k, segH = 14 * k, segGap = 4 * k, lean = segH * 0.5f * kTanPlate;
    const float top = y0 - 8 * k - segH;
    for (int i = 0; i < 8; ++i) {
        const float sx0 = x1 - (8 - i) * segW - (7 - i) * segGap - lean;
        const ImU32 col = limiter ? kSignal : i < lit ? (i >= 6 ? kSignal : kInk) : kLineStrong;
        d->AddQuadFilled(ImVec2(sx0 + lean, top), ImVec2(sx0 + segW + lean, top), ImVec2(sx0 + segW - lean, top + segH),
                         ImVec2(sx0 - lean, top + segH), col);
    }
}

void DrawStageClock(const ImGuiLayer& ui, const rules::StageClock& clock, int width, int height)
{
    const float b = Scale(width, height), k = b * kHudSize, W = float(width), H = float(height);
    ImDrawList* d = Draw();
    ImFont* item = ui.MenuItem();
    ImFont* text = ui.MenuText();
    const bool finished = clock.GetState() == rules::StageClock::State::Finished;

    // The timer's plate: STAGE, the time.
    const float size = 26 * k, h = 44 * k, cut = 12 * k, x = kSafe * b;
    float y = kSafe * b;
    auto plate = [&](const char* label, const std::string& value, ImU32 ink, ImU32 back) {
        const float lw = Width(text, 14 * k, label, 0.08f), vw = Width(item, size, value.c_str(), 0);
        const float w = 12 * k + lw + 12 * k + vw + 16 * k + cut;
        Plate(d, ImVec2(x, y), w, h, cut, back);
        const float mid = y + h * 0.5f;
        Line(text, 14 * k, x + 12 * k, mid, kInkMuted, label, 0.08f);
        Line(item, size, x + 12 * k + lw + 12 * k, mid, ink, value.c_str());
        y += h + 4 * k;
    };
    plate("STAGE", Clock(clock.Time()), kInk, kScrim);
    // The last split: flashed once as it comes, then held.
    const std::vector<float>& splits = clock.Splits();
    if (splits.size() < g_splits) g_splits = 0;   // (a restart)
    if (splits.size() > g_splits) g_splits = splits.size(), g_flash = 0.3f;
    g_flash = std::max(0.0f, g_flash - ImGui::GetIO().DeltaTime);
    if (!splits.empty()) {
        const std::string label = "SPLIT " + std::to_string(splits.size());
        plate(label.c_str(), Clock(splits.back()), kInkSecondary, g_flash > 0 ? Alpha(kSurfaceOverlay, 0.95f) : kScrim);
    }
    if (clock.FalseStart()) plate("FALSE START", "+10.00", kWarning, kScrim);

    // The middle: the countdown's digits and GO, big; a false start's word; the finish's time on its plate.
    const std::string shown = clock.Shown();
    const float cx = W * 0.5f, cy = H * 0.32f, m = b * kMiddleSize, mcut = 12 * m;
    if (finished) {
        const std::string t = Clock(clock.Time());
        ImFont* numeral = ui.Wordmark();
        const float tw = Width(numeral, 48 * m, t.c_str()), w = std::max(tw, Width(text, 14 * m, "FINISH", 0.16f)) + 2 * 28 * m;
        const float ph = 112 * m, px = cx - w * 0.5f, py = cy - ph * 0.5f;
        Plate(d, ImVec2(px, py), w + mcut, ph, mcut, kScrim);
        Tracked(d, text, 14 * m, ImVec2(px + 28 * m, py + 18 * m), kInkMuted, "FINISH", 0.16f);
        Tracked(d, numeral, 48 * m, ImVec2(px + 28 * m, py + 34 * m), kInk, t.c_str(), 0);
    } else if (shown == "FALSE START") {
        const float fw = Width(item, 48 * m, shown.c_str()), ph = 80 * m, px = cx - fw * 0.5f - 28 * m;
        Plate(d, ImVec2(px, cy - ph * 0.5f), fw + 56 * m + mcut, ph, mcut, kScrim);
        Line(item, 48 * m, px + 28 * m, cy, kCritical, shown.c_str());
    } else if (shown == "GO" || (shown.size() == 1 && shown[0] >= '1' && shown[0] <= '9')) {
        ImFont* big = ui.HudDisplay();
        const float bs = 180 * m, bw = Width(big, bs, shown.c_str());
        Line(big, bs, cx - bw * 0.5f, cy, shown == "GO" ? kSignal : kInk, shown.c_str());
    }
}

void DrawPacenotes(const ImGuiLayer& ui, const std::string& current, const std::string& next, int width, int height)
{
    if (current.empty()) return;
    const float b = Scale(width, height), k = b * kHudSize, cx = width * 0.5f, y = kSafe * b;
    const float h = Call(ui, current, cx, y, k, 1.0f, 1.0f);
    if (!next.empty()) Call(ui, next, cx, y + h + 8 * k, k, 0.75f, 0.75f);
}

void DrawReadout(const ImGuiLayer& ui, bool on, const std::vector<std::string>& lines, const std::string& warning, int height)
{
    // (Monospaced, a debugging aid: not the design's.) Under the timer's plates.
    const float px = std::max(12.0f, height * 0.022f);
    ImFont* font = ui.HudSmall();
    ImDrawList* d = Draw();
    auto put = [&](float x, float y, const char* text, ImU32 col) {
        const float off = std::max(1.0f, px * 0.06f);
        d->AddText(font, px, ImVec2(x + off, y + off), 0xc0000000u, text);
        d->AddText(font, px, ImVec2(x, y), col, text);
    };
    float y = height * (270.0f / 1080.0f);
    for (size_t i = 0; on && i < lines.size(); ++i, y += px * 1.25f) put(px * 0.6f, y, lines[i].c_str(), 0xf0ffffffu);
    if (on && !lines.empty()) y += px * 0.5f;
    if (!warning.empty()) put(px * 0.6f, y, warning.c_str(), kCritical);
}

} // namespace wrally::ui
