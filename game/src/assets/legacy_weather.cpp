#include "assets/legacy_weather.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "formats/track_settings.h"
#include "rules/stage.h"

namespace wrally::assets {

namespace {

using formats::TrackSettingsSection;

// Some presets were written on a decimal-comma locale ("SunDir = 0,64 -0,42 0,63"): a C parser stops at the comma and
// reads 0. No value in the file uses a comma otherwise.
std::string Dotted(std::string v)
{
    for (char& c : v)
        if (c == ',') c = '.';
    return v;
}

float Number(const TrackSettingsSection& s, const char* key, float fallback)
{
    const auto it = s.values.find(key);
    if (it == s.values.end()) return fallback;
    const std::string v = Dotted(it->second);
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);   // tolerates "85.0 <<< comment"
    return end == v.c_str() ? fallback : static_cast<float>(d);
}

// A whole underscore-separated word of the name: "lightrain" must not match "rain".
bool HasWord(const std::string& name, const char* word)
{
    for (size_t start = 0; start <= name.size();) {
        size_t end = name.find('_', start);
        if (end == std::string::npos) end = name.size();
        if (!_stricmp(name.substr(start, end - start).c_str(), word)) return true;
        start = end + 1;
    }
    return false;
}

// The time of day a section lights: "41M_..." M, "108O_E_..." E.
char TimeOfDay(const std::string& name)
{
    const size_t us = name.find('_');
    if (us == std::string::npos || us == 0) return 'M';
    const std::string rest = name.substr(us + 1);
    return static_cast<char>(std::toupper(static_cast<unsigned char>(rest.size() >= 2 && rest[1] == '_' ? rest[0] : name[us - 1])));
}

ngg::WeatherRecord Translate(const TrackSettingsSection& s, bool& mirrored, bool& truncated)
{
    ngg::WeatherRecord w = ngg::DefaultWeatherRecord();
    truncated = s.name.size() >= sizeof w.name;
    std::snprintf(w.name, sizeof w.name, "%s", s.name.c_str());

    // Physical already: the sun and the ground.
    if (const auto it = s.values.find("sundir"); it != s.values.end()) {
        float d[3];
        if (std::sscanf(Dotted(it->second).c_str(), "%f %f %f", &d[0], &d[1], &d[2]) == 3)
            for (int k = 0; k < 3; ++k) w.sunDirection[k] = d[k];
    }
    // A daytime sun at or below the horizon (y >= 0: the light travels upwards). RBR's hand-set ambient made the sun
    // irrelevant on those (overcast ones mostly); a physical sky draws them as night. A low sun on the same bearing,
    // at least ~10 degrees up, is what morning or evening means. Night keeps its own.
    mirrored = false;
    if (TimeOfDay(s.name) != 'N' && w.sunDirection[1] > -0.05f) {
        w.sunDirection[1] = -std::max(std::fabs(w.sunDirection[1]), 0.17f);
        mirrored = true;
    }
    const float len = std::sqrt(w.sunDirection[0] * w.sunDirection[0] + w.sunDirection[1] * w.sunDirection[1] +
                                w.sunDirection[2] * w.sunDirection[2]);
    if (len > 1e-6f)
        for (float& c : w.sunDirection) c /= len;
    w.groundColour[0] = Number(s, "terrain_reflectance_red", w.groundColour[0]);
    w.groundColour[1] = Number(s, "terrain_reflectance_green", w.groundColour[1]);
    w.groundColour[2] = Number(s, "terrain_reflectance_blue", w.groundColour[2]);
    if (w.groundColour[0] + w.groundColour[1] + w.groundColour[2] < 0.003f) {   // 0 in the file: unset, not black
        const ngg::WeatherRecord d = ngg::DefaultWeatherRecord();
        for (int k = 0; k < 3; ++k) w.groundColour[k] = d.groundColour[k];
    }
    if (const float albedo = Number(s, "terrain_reflectance", 0.0f); albedo > 0.001f) w.groundAlbedo = albedo;

    // The condition in the name, in our units: the translation's choices, made once (NGG design 5.0q).
    const std::string& n = s.name;
    if (HasWord(n, "hazy")) w.turbidity = 3.0f;
    if (HasWord(n, "lightrain")) w.rain = 0.4f, w.turbidity = 3.0f, w.fogVisibility = 6000.0f, w.fogHeight = 600.0f;
    if (HasWord(n, "heavyrain")) w.rain = 1.0f, w.turbidity = 4.0f, w.fogVisibility = 2500.0f, w.fogHeight = 600.0f;
    if (HasWord(n, "lightsnow")) w.snow = 0.4f, w.turbidity = 3.0f, w.fogVisibility = 3000.0f, w.fogHeight = 600.0f;
    if (HasWord(n, "heavysnow")) w.snow = 1.0f, w.turbidity = 4.0f, w.fogVisibility = 800.0f, w.fogHeight = 600.0f;
    if (HasWord(n, "partcloud")) w.cloudCover = 0.35f;
    else if (HasWord(n, "lightcloud") || HasWord(n, "ligthcloud")) w.cloudCover = 0.6f;   // one preset is misspelt
    else if (HasWord(n, "heavycloud")) w.cloudCover = 0.95f;
    // Fog: RBR's linear fog is opaque at FogEnd, which is what visibility means - when the preset uses it.
    const auto useFog = s.values.find("usefog");
    const bool fogOn = useFog != s.values.end() && !_stricmp(useFog->second.c_str(), "true");
    const float fogEnd = Number(s, "fogend", 0.0f);
    for (const auto& [word, vis] : {std::make_pair("lightfog", 1500.0f), std::make_pair("heavyfog", 300.0f)})
        if (HasWord(n, word)) {
            w.fogVisibility = fogOn && fogEnd > 50.0f ? fogEnd : vis;
            w.fogHeight = 150.0f;
            w.turbidity = 2.0f;
        }
    return w;
}

} // namespace

std::string LegacyWeather(const std::string& iniPath, const std::string& stage, std::vector<ngg::WeatherRecord>& out)
{
    out.clear();
    const int map = rules::StageMap(stage);
    if (map < 0) return "weather: " + stage + " is not a track-<n> stage - no presets\n";
    std::vector<TrackSettingsSection> sections;
    if (!formats::ReadTrackSettings(iniPath, sections)) return "weather: no " + iniPath + " - the default sky\n";

    // The map's sections: "41..." but not "410...".
    const std::string prefix = formats::TrackSettingsMap(map);
    const size_t us = stage.find_last_of('_');
    const char tod = us != std::string::npos && us + 1 < stage.size()
                         ? static_cast<char>(std::toupper(static_cast<unsigned char>(stage[us + 1])))
                         : 'M';
    const std::string mine = prefix + tod + "_", plain = mine + "crisp_clear";
    size_t mirrored = 0, truncated = 0, def = SIZE_MAX, firstMine = SIZE_MAX;
    for (const TrackSettingsSection& s : sections) {
        if (s.name.compare(0, prefix.size(), prefix) || s.name.size() <= prefix.size() ||
            std::isdigit(static_cast<unsigned char>(s.name[prefix.size()])))
            continue;
        bool m = false, t = false;
        out.push_back(Translate(s, m, t));
        mirrored += m;
        truncated += t;
        if (def == SIZE_MAX && !_stricmp(s.name.c_str(), plain.c_str())) def = out.size() - 1;
        if (firstMine == SIZE_MAX && !_strnicmp(s.name.c_str(), mine.c_str(), mine.size())) firstMine = out.size() - 1;
    }
    if (out.empty()) return "weather: TrackSettings.ini has no sections for map " + prefix + " - the default sky\n";
    const size_t first = def != SIZE_MAX ? def : firstMine != SIZE_MAX ? firstMine : 0;
    if (first) std::rotate(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(first), out.begin() + static_cast<std::ptrdiff_t>(first) + 1);

    char line[256];
    std::snprintf(line, sizeof line, "weather: %zu presets from TrackSettings.ini, default %s (%zu daytime suns below "
                  "the horizon raised, %zu names cut to 63 bytes)\n", out.size(), out[0].name, mirrored, truncated);
    return line;
}

} // namespace wrally::assets
