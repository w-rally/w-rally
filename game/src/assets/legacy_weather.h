#pragma once
// An RBR stage's weathers as .ngg presets (wrally::assets; docs/design.md phase 8 step 3, NGG design 5.0q): every
// Maps/TrackSettings.ini section of the stage's map, translated into NGGW records for the legacy importer to write.
//
// TrackSettings.ini's numbers are art knobs for RBR's 2004 shader (Turbidity -2.8..9, a Mie phase g of 1.0), not
// physics, so they are not copied. What carries over:
//   - the name, as the identity: the game and the menu select a preset by its section's name
//   - SunDir (the direction light travels) and the ground's reflectance, physical already
//   - the condition in the name, translated once into our units: hazy -> turbidity 3; partcloud / lightcloud /
//     heavycloud -> cloud cover 0.35 / 0.6 / 0.95; lightfog / heavyfog -> visibility from FogEnd (else 1500 / 300 m)
//     under a 150 m layer; light/heavy rain and snow -> rain or snow 0.4 / 1 with their haze and visibility
// and two repairs RBR's file needs: decimal commas ("0,64") read as points, and a preset whose sun is at or near the
// horizon (48 of them, overcast ones mostly) given a low sun on the same bearing, or a physical sky draws it as night. Every
// RBR stage is daytime: M morning, N noon, E evening.

#include <string>
#include <vector>

#include "ngg_format.h"

namespace wrally::assets {

// The presets of the stage `stage` (its file's stem, "track-41_M") from `iniPath`, the default first: the stage's own
// time of day, crisp and clear if it has that, else its first section of that time of day, else the map's first.
// Returns a line for the import's log; `out` is left empty when the file or the map's sections are missing.
std::string LegacyWeather(const std::string& iniPath, const std::string& stage, std::vector<ngg::WeatherRecord>& out);

} // namespace wrally::assets
