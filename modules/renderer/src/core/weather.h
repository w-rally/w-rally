// ---------------------------------------------------------------------------
// Weather presets - OUR format (design §5.1).
//
// RBR's TrackSettings.ini was the obvious source and the wrong destination. Its
// 757 sections do name the weather - "<map><variant>_<haze>_<cloud>", e.g.
// 108O_M_heavyrain_heavycloud - but the numbers inside are art knobs for a 2004
// shader, not physics: Turbidity ranges -2.8..9 with a median of 0, the Mie
// phase "Greenstein_Value" is 1.0 (a delta function) in most presets, and
// track-108's clear sky has Rayleigh_Multiplier 0.025. Feeding those into a
// physical atmosphere gives nonsense; being bound by their shape would cap the
// renderer at what a 2004 shader needed (design §2.1's rule: replace, do not
// extend).
//
// So a preset here holds PHYSICAL quantities only - visibility in metres, cloud
// cover as a fraction, turbidity in our model's units. It has two forms: the
// versioned JSON below, for projects and the editor (Wallaby's sidecar, the
// flat API, --export-weather), and binary records in the .ngg's NGGW chunk
// (format/ngg_format.h), which is what the runtime loads. The legacy file is an IMPORT source (game/src/assets/legacy_weather.*): its
// category words and fog distances are translated once, the rest is left
// behind, and the import is recorded in `source` so a preset can always be
// traced back.
//
//   {
//     "format": "ngg-weather", "version": 1,
//     "name": "108O_M_heavyfog_clear",
//     "source": "TrackSettings.ini [108O_M_heavyfog_clear]",
//     "sun":           { "direction": [x, y, z], "intensity": 22 },
//     "atmosphere":    { "turbidity": 1, "groundAlbedo": 0.12, "groundColour": [r, g, b] },
//     "clouds":        { "cover": 0.95 },
//     "fog":           { "visibility": 300, "height": 150 },
//     "precipitation": { "rain": 0, "snow": 0 },
//     "exposure":      { "value": 0.8 },
//     "wind":          { "direction": 90, "speed": 6, "gustiness": 0.4 }
//   }
//
// sun.direction is the direction light TRAVELS (from the sun), like SkyParams.
// wind.direction is where it blows TOWARDS, degrees from +z towards +x; speed m/s at 10 m; gustiness 0..1. Missing: calm.
// fog.visibility is meteorological: the distance at which contrast falls to
// 2 % (Koschmieder: extinction = 3.912 / visibility); 0 means no fog layer.
// Unknown keys are ignored and missing ones keep their defaults, so the
// format can grow without breaking older presets.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "astronomy.h"
#include "sky.h"

namespace ngg {
namespace core {

struct WeatherPreset
{
    std::string name;
    std::string source;          // provenance: where it was imported or authored
    SkyParams   sky;             // sun, turbidity, ground, exposure
    float       cloudCover = 0.0f;      // 0 clear .. 1 overcast
    float       fogVisibility = 0.0f;   // metres; 0 = none
    float       fogHeight = 150.0f;     // scale height of the fog layer, metres
    float       rain = 0.0f;            // 0 dry .. 1 heavy - carried, not yet rendered
    float       snow = 0.0f;            // 0 none .. 1 heavy - carried, not yet rendered
    // The wind (NGG design §2.6): it sways the instanced models that carry it (ngg_format.h kModelWind).
    float       windDirectionDeg = 0.0f;   // where it blows TOWARDS: degrees from +z towards +x
    float       windSpeed = 0.0f;          // m/s at 10 m; 0 calm
    float       gustiness = 0.0f;          // 0..1
    // The time and place (NGG design §5.4). When set, the sun follows from them (ApplyWeather) and sky.sunDir is
    // not used.
    bool        timeSet = false;
    SkyTime     time;
};

// The .ngg's NGGW chunk (design §2.5): binary records. Parse returns false on a
// payload it does not understand (logged) and leaves `out` empty; Pack writes
// the presets in order, the first being the default.
bool ParseWeatherChunk(const void* payload, size_t bytes, std::vector<WeatherPreset>& out);
std::vector<uint8_t> PackWeatherChunk(const std::vector<WeatherPreset>& presets);

bool        LoadWeather(const wchar_t* path, WeatherPreset& out);
bool        SaveWeather(const wchar_t* path, const WeatherPreset& in);
std::string WeatherToJson(const WeatherPreset& in);
bool        WeatherFromJson(const char* text, WeatherPreset& out);

// The preset's weather, applied onto SkyParams (the renderer's input). A preset with its time set places the sun
// astronomically (ApplySkyTime).
void ApplyWeather(const WeatherPreset& w, SkyParams& sky);

// The sun where a date, a time and a place put it: sky.sunDir becomes the direction its light travels. Returns the
// bodies, for what else follows from them (the moon, the stars).
SkyBodies ApplySkyTime(const SkyTime& t, SkyParams& sky);

} // namespace core
} // namespace ngg
