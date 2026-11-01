#pragma once
// The HUD (docs/design.md 6.5, wrally::ui), drawn with Dear ImGui on its foreground draw list - the renderer's UI pass -
// to the W-Rally design system's HUD (its StageTimer, PaceNote and Speedometer): plates on the HUD scrim, cut at the
// livery's slant, inside a 48 px safe area (at 1080p, scaled with the window). The stage timer at the top left, the
// co-driver's call at the top centre, the shift lights, speed and gear at the bottom right; the countdown and the finish
// in the middle; the renderer's readout (F1) and warnings under the timer. Between ImGuiLayer::NewFrame and Render.

#include <string>
#include <vector>

#include "physics/physics.h"
#include "rules/stage.h"
#include "ui/imgui_layer.h"

namespace wrally::ui {

// Speed, gear and the shift lights: `rpmLimit` the engine's rev limiter (0 unknown: 8000), the lights lit from 60 % of
// it, the last two in the signal colour, the whole bar so at the limiter.
void DrawDashboard(const ImGuiLayer& ui, const physics::Dashboard& d, float rpmLimit, int width, int height);
// The stage time on its plate (the last split under it, flashed once as it is crossed; a false start's penalty), the
// countdown's digits and GO big in the middle, the finish's time on a plate there.
void DrawStageClock(const ImGuiLayer& ui, const rules::StageClock& clock, int width, int height);
// The co-driver's calls at the top centre ("": none), as CoDriver::Upcoming words them: a corner's grade (L1 .. R7) in
// the signal block with its arrow (the warning colour for a don't-cut, caution or water call), the rest on a plate, its
// distances dimmer; the next call under it, smaller and dimmer.
void DrawPacenotes(const ImGuiLayer& ui, const std::string& current, const std::string& next, int width, int height);
// The readout's lines under the stage timer; then `warning` in red under them ("" none). Off: the warning alone.
void DrawReadout(const ImGuiLayer& ui, bool on, const std::vector<std::string>& lines, const std::string& warning, int height);

} // namespace wrally::ui
