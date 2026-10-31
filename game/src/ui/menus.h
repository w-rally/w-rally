#pragma once
// The game's menus (wrally::ui), in Dear ImGui - each a function drawing one screen centred in the window, called
// between ImGuiLayer::NewFrame and Render, returning what was chosen. The screens hold no state of their own beyond
// ImGui's and Controls' wait for a key; what they edit is the caller's (Settings, the debug switches).
//
//   MainMenu     Quick rally, Settings, Exit (Season, Marketplace: not there yet): the whole window, drawn to the WRally design -
//                W / S or the arrows slide the selection, Enter or Space picks it
//   QuickRally   stage, car, setup, damage, weather, road - Start / Back (the choice kept in Settings)
//   SettingsUi   tabs: the content (folders, base physics, Extract the slot cars' physics), graphics, sound, controls -
//                the whole window, drawn to the WRally design: Q / E the tabs, W / S the rows, A / D the values
//   PauseMenu    the in-stage menu (Esc): Resume, Restart stage, Settings, Debug, Exit to main menu
//   DebugMenu    what the keys switch (the renderer's, the cameras, the sound), the sound mix
//   MixPanel     the sound mix beside the stage while driving (the Debug menu turns it on)

#include <string>
#include <vector>

#include "app/settings.h"
#include "assets/catalog.h"
#include "sound/mix_edit.h"

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
// The screen has tabs: Content, Graphics, Sound, Controls (the keys). `keyDown`: the key pressed this frame
// (Window::TakeKeyDown), which Controls binds while it waits for one.
SettingsChoice SettingsUi(const ImGuiLayer& ui, app::Settings& s, bool inStage, const std::string& log, UINT keyDown,
                          int width, int height);
// Esc on the screen is Back. Whether Controls is waiting for a key or a text field has the keyboard: Esc is then
// theirs (no change), not the menu's.
bool CapturingKey();

enum class PauseChoice { None, Resume, Restart, Settings, Debug, ExitToMenu };
PauseChoice PauseMenu(const ImGuiLayer& ui, int width, int height);

// The switches the keys work, for the debug menu to work too.
struct DebugSwitches {
    bool taa = true, ao = true, shadows = true, rayTracing = true;
    bool readout = false;
    int debugView = 0;
    int legacyProfile = -1;   // an imported .lbs stage's card lighting (0 off, 1 A, 2 B); -1 not an imported stage
    int camera = 0, cameras = 4;
    float chaseDistance = 5.2f;
    float orbitDegrees = 0;
    bool muted = false, antiLagIdle = false;
    bool mixPanel = false;                       // the mix panel up while driving
    std::vector<sound::MixValue>* mix = nullptr;   // the sound mix's values (none: no sound)
    std::string mixStatus;                       // the last save / reload's report
};
// What the mix's controls did: a value changed (to the sound now), Save to the mix file, Reload it, the Defaults.
enum class MixChoice { None, Changed, Save, Reload, Defaults };
enum class DebugChoice { None, Back, Changed };
// `views`: the renderer's debug views' names (0 off). `keys`: for the labels (each switch with its key).
// `mixDone`: what its sound mix's controls did.
DebugChoice DebugMenu(const ImGuiLayer& ui, DebugSwitches& d, const std::vector<std::string>& views, const input::Bindings& keys,
                      int width, int height, MixChoice& mixDone);
// The mix's controls in a panel at the window's right, over the stage while it runs (no keyboard navigation: the keys
// drive). Its close box clears `open`.
MixChoice MixPanel(std::vector<sound::MixValue>& values, const std::string& status, bool& open, int width, int height);

} // namespace wrally::ui
