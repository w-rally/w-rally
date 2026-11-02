#pragma once
// wrally::assets - a stage's splash picture, for the loading screen's stripe. RBR shows it as its loading screen: a
// 1024 x 1024 .dds stretched over a 4:3 screen, a photo of the stage on its right, torn paper round it (the menus' own
// art left of it, a title or a credit above and below on some). A community track names its own (Tracks.ini's
// SplashScreen, TrackInfo::splash); the original stages' are packed in Textures/splash.dat (load_NN.dds - which NN is
// which stage is RBR's own table, not in its files: a track's splash in track-<n>.json says it).

#include <string>

#include "formats/dds_image.h"

namespace wrally::assets {

// The photo of the splash at `path` (relative to the RBR folder: "Textures\Splash\track-90.dds"): read loose, else from
// Textures/splash.dat by its file name; cropped to the photo. `aspect`: its width over its height as RBR shows it (the
// .dds stretched to 4:3). An http(s) URL (TrackInfo::splash from the server) is not fetched yet. "" or what is wrong.
std::string ReadSplash(const std::string& rbrPath, const std::string& path, formats::Image& out, float& aspect);

} // namespace wrally::assets
