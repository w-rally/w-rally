#pragma once
// The game (wrally::app): the window, the renderer and ImGui for the whole run; the menus (ui/menus.h) and the stages on
// them. A stage: the modules loaded (physics through the session, sound), the frame loop - the keys read, the session
// advanced, the frame between its last two steps drawn with the HUD - and Esc's menu (Resume, Restart stage, Settings,
// Debug, Exit to main menu).

#include <string>
#include <vector>

#include "app/options.h"

namespace wrally::app {

// Runs the game until it is left; returns the exit code. A command line (app/options.h) runs that stage at once (its
// end - the window closed, a replay's end, --exit-at - the game's too; Esc's "Exit to main menu" goes to the menus);
// none, the main menu.
int RunGame(const std::vector<std::string>& args);

} // namespace wrally::app
