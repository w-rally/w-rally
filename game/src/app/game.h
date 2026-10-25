#pragma once
// The game (wrally::app): the modules loaded (physics through the session, sound, renderer), the window, and the frame
// loop - the keys read, the session advanced, the frame between its last two steps drawn with the HUD.

#include "app/options.h"

namespace wrally::app {

// Runs the game until the window closes; returns the exit code.
int RunGame(const Options& o);

} // namespace wrally::app
