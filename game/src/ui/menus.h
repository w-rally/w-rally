#pragma once
// The game's menus (wrally::ui), in Dear ImGui - each a function drawing one screen centred in the window, called
// between ImGuiLayer::NewFrame and Render, returning what was chosen. The screens hold no state of their own beyond
// ImGui's; what they edit is the caller's (Settings, the debug switches).
//
//   MainMenu     Quick rally, Settings, Exit
//   QuickRally   stage, car, setup, damage, weather, road - Start / Back (the choice kept in Settings)
//   SettingsUi   the content's folders, the base physics, graphics, sound; Extract the slot cars' physics
//   PauseMenu    the in-stage menu (Esc): Resume, Restart stage, Settings, Debug, Exit to main menu
//   DebugMenu    what the keys switch (the renderer's, the cameras, the sound)

#include <string>
#include <vector>

#include "app/settings.h"
#include "assets/catalog.h"

namespace wrally::ui {

class ImGuiLayer;

// The menus' backdrop when no stage is drawn behind them.
void DrawBackdrop(int width, int height);

enum class MainChoice { None, QuickRally, Settings, Exit };
// `note`: shown under the buttons ("" none) - what is missing for Quick rally.
MainChoice MainMenu(const ImGuiLayer& ui, int width, int height, const std::string& note);

enum class QuickChoice { None, Start, Back };
// `s`'s quick fields (stage, car, setup, damage, weather, road) are the choice, kept between runs.
QuickChoice QuickRally(const ImGuiLayer& ui, const assets::Catalog& catalog, app::Settings& s, int width, int height);

enum class SettingsChoice { None, Back, Graphics, Rescan, Extract };
// Graphics: a graphics field changed (to apply now). `inStage`: the content's part is shown but not editable.
// `log`: the last Rescan / Extract's report.
SettingsChoice SettingsUi(const ImGuiLayer& ui, app::Settings& s, bool inStage, const std::string& log, int width, int height);

enum class PauseChoice { None, Resume, Restart, Settings, Debug, ExitToMenu };
PauseChoice PauseMenu(const ImGuiLayer& ui, int width, int height);

// The switches the keys work, for the debug menu to work too.
struct DebugSwitches {
    bool taa = true, ao = true, shadows = true, rayTracing = true;
    bool readout = false;
    int debugView = 0;
    int camera = 0, cameras = 4;
    float chaseDistance = 5.2f;
    float orbitDegrees = 0;
    bool muted = false, antiLagIdle = false;
};
enum class DebugChoice { None, Back, Changed };
// `views`: the renderer's debug views' names (0 off).
DebugChoice DebugMenu(const ImGuiLayer& ui, DebugSwitches& d, const std::vector<std::string>& views, int width, int height);

} // namespace wrally::ui
