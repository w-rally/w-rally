#pragma once
// The HUD (docs/design.md 6.5, wrally::ui), drawn with Dear ImGui on its foreground draw list - the renderer's UI pass:
// the dashboard at the bottom right, the stage clock (the countdown big in the middle, the stage time and splits at the
// top right), the renderer's readout (F1) and warnings at the top left. Between ImGuiLayer::NewFrame and Render.

#include <string>
#include <vector>

#include "physics/physics.h"
#include "rules/stage.h"
#include "ui/imgui_layer.h"

namespace wrally::ui {

void DrawDashboard(const ImGuiLayer& ui, const physics::Dashboard& d, int width, int height);
void DrawStageClock(const ImGuiLayer& ui, const rules::StageClock& clock, int width, int height);
// The readout's lines at the top left; then `warning` in red under them ("" none). Off: the warning alone.
void DrawReadout(const ImGuiLayer& ui, bool on, const std::vector<std::string>& lines, const std::string& warning, int height);

} // namespace wrally::ui
