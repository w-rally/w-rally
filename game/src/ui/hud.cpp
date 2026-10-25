#include "ui/hud.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "imgui.h"

namespace wrally::ui {

namespace {

// The HUD's text, as ngg-api's shadowed labels were: a dark copy offset down-right, then the text. Big text from the
// large font, so it is not a small one blown up.
void Text(const ImGuiLayer& ui, float px, float x, float y, const char* text, uint32_t colour)
{
    ImFont* font = px > 64.0f ? ui.HudLarge() : ui.HudSmall();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const float off = std::max(1.0f, px * 0.06f);
    d->AddText(font, px, ImVec2(x + off, y + off), 0xc0000000u, text);
    d->AddText(font, px, ImVec2(x, y), colour, text);
}

float Width(const ImGuiLayer& ui, float px, const char* text)
{
    ImFont* font = px > 64.0f ? ui.HudLarge() : ui.HudSmall();
    return (font ? font : ImGui::GetFont())->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
}

} // namespace

// Where ngg-api's world labels put them: anchored at (x, y) in -1 .. 1 (y up), centred on the anchor and lifted 1.5
// lines above it (ngg-core's Scene::SetLabels).
void DrawDashboard(const ImGuiLayer& ui, const physics::Dashboard& d, int width, int height)
{
    auto put = [&](float x, float y, const char* text, uint32_t colour, float px) {
        const float ax = (x + 1) * 0.5f * static_cast<float>(width), ay = (1 - y) * 0.5f * static_cast<float>(height);
        Text(ui, px, ax - 0.5f * Width(ui, px, text), ay - 1.5f * px, text, colour);
    };
    char speed[32], rpm[32], gear[8];
    std::snprintf(speed, sizeof speed, "%3.0f km/h", std::fabs(d.speed) * 3.6f);
    std::snprintf(rpm, sizeof rpm, "%5.0f rpm", d.rpm > 0 ? d.rpm : 0.0f);
    std::snprintf(gear, sizeof gear, "%s", d.gear < 0 ? "R" : d.gear == 0 ? "N" : std::to_string(d.gear).c_str());
    const float textPx = height * 0.045f, gearPx = height * 0.12f;
    const uint32_t white = 0xf0ffffff, redline = d.rpm > 7000 ? 0xf03050ff : white;
    put(0.72f, -0.62f, speed, white, textPx);
    put(0.72f, -0.75f, rpm, redline, textPx);
    put(0.90f, -0.68f, gear, 0xf040d0ff, gearPx);
}

void DrawStageClock(const ImGuiLayer& ui, const rules::StageClock& clock, int width, int height)
{
    auto put = [&](const std::vector<std::string>& lines, const std::vector<uint32_t>& colours, float px, float right,
                   float y0, bool centre) {
        for (size_t i = 0; i < lines.size(); ++i) {
            const float w = Width(ui, px, lines[i].c_str());
            const float x = centre ? (static_cast<float>(width) - w) * 0.5f : right - w;
            Text(ui, px, x, y0 + static_cast<float>(i) * px * 1.2f, lines[i].c_str(), colours[std::min(i, colours.size() - 1)]);
        }
    };
    const std::string shown = clock.Shown();
    // the countdown's digits big; GO green, a false start red, a split / the finish smaller
    const bool go = shown == "GO", warn = shown == "FALSE START", count = shown.size() == 1;
    const bool finished = clock.GetState() == rules::StageClock::State::Finished;
    const float big = height * (count || go ? 0.22f : 0.09f);
    put(shown.empty() ? std::vector<std::string>{} : std::vector<std::string>{shown},
        {warn ? 0xf03030ffu : go || finished ? 0xf040e040u : 0xf0ffffffu}, big, 0, height * 0.32f - big * 0.5f, true);
    std::vector<std::string> lines;
    std::vector<uint32_t> colours;
    const float px = std::max(14.0f, height * 0.04f);
    lines.push_back(rules::StageClock::Text(clock.Time()));
    colours.push_back(finished ? 0xf040e040u : 0xf0ffffffu);
    if (clock.FalseStart()) {
        lines.push_back("false start +10 s");
        colours.push_back(0xf03030ffu);
    }
    for (size_t i = 0; i < clock.Splits().size(); ++i) {
        lines.push_back("split " + std::to_string(i + 1) + "  " + rules::StageClock::Text(clock.Splits()[i]));
        colours.push_back(0xf0c0e8ffu);
    }
    put(lines, colours, px, static_cast<float>(width) - px * 0.8f, px * 0.6f, false);
}

void DrawReadout(const ImGuiLayer& ui, bool on, const std::vector<std::string>& lines, const std::string& warning, int height)
{
    const float px = std::max(12.0f, height * 0.022f);
    float y = px * 0.6f;
    for (size_t i = 0; on && i < lines.size(); ++i, y += px * 1.25f) Text(ui, px, px * 0.6f, y, lines[i].c_str(), 0xf0ffffff);
    if (on && !lines.empty()) y += px * 0.5f;
    if (!warning.empty()) Text(ui, px, px * 0.6f, y, warning.c_str(), 0xf03030ffu);
}

} // namespace wrally::ui
