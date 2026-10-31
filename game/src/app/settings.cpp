#include "app/settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "app/options.h"
#include "app/paths.h"

namespace wrally::app {

namespace {

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The keys, bound to the fields once - read and written from the same list.
template <class F> void Fields(Settings& s, F&& f)
{
    f("rbr_path", s.rbrPath);
    f("ngg_folders", s.nggFolders);
    f("base_physics", s.basePhysics);
    f("quality", s.quality);
    f("ray_tracing", s.rayTracing);
    f("taa", s.taa);
    f("shadows", s.shadows);
    f("vsync", s.vsync);
    f("sound", s.sound);
    f("codriver", s.codriver);
    f("codriver_volume", s.codriverVolume);
    f("codriver_lead", s.codriverLead);
    f("codriver_replay", s.codriverReplay);
    f("hud_pacenote_next", s.pacenoteNext);
    f("dev_engine", s.devEngine);
    f("dev_als_bangs", s.devBangs);
    f("dev_als_idle_bangs", s.devIdleBangs);
    f("quick_stage", s.stage);
    f("quick_car", s.car);
    f("quick_setup", s.setup);
    f("quick_damage", s.damage);
    f("quick_weather", s.weather);
    f("quick_road", s.road);
    f("quick_time", s.time);
    f("quick_weather_custom", s.weatherCustom);
    f("quick_season", s.season);
    f("quick_clouds", s.clouds);
    f("quick_fog", s.fog);
    f("quick_rain", s.rain);
}

void Set(std::string& v, const std::string& text) { v = text; }
void Set(uint32_t& v, const std::string& text) { v = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10)); }
void Set(bool& v, const std::string& text) { v = text == "1" || text == "true" || text == "on"; }
void Set(float& v, const std::string& text) { v = static_cast<float>(std::atof(text.c_str())); }
std::string Text(const std::string& v) { return v; }
std::string Text(uint32_t v) { return std::to_string(v); }
std::string Text(bool v) { return v ? "1" : "0"; }
std::string Text(float v)
{
    char t[32];
    std::snprintf(t, sizeof t, "%g", v);
    return t;
}

} // namespace

std::string Settings::NggFolders() const
{
    if (!nggFolders.empty()) return nggFolders;
    return rbrPath.empty() ? "" : rbrPath + "\\Ngg";
}

std::filesystem::path SettingsPath() { return UserDir() / "W-Rally.ini"; }

Settings LoadSettings()
{
    Settings s;
    // Unset, the RBR folder is the exe's own when W-Rally sits in one.
    if (std::filesystem::exists(ExeDir() / "RichardBurnsRally_SSE.exe")) s.rbrPath = ExeDir().string();
    // The installer's install.ini first (the RBR folder chosen at install), then the user's own W-Rally.ini over it
    // (none yet: the wrally.ini of before the rename, written as W-Rally.ini at the next save).
    std::error_code e;
    const std::filesystem::path own = std::filesystem::exists(SettingsPath(), e) ? SettingsPath() : UserDir() / "wrally.ini";
    for (const std::filesystem::path& path : {InstallIni(), own}) {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (const size_t hash = line.find('#'); hash != std::string::npos && line.find_first_not_of(" \t") == hash) continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            Fields(s, [&](const char* name, auto& field) {
                if (key == name) Set(field, value);
            });
            for (int a = 0; a < input::kActions; ++a)
                if (key == std::string("key_") + input::Info(static_cast<input::Action>(a)).id)
                    s.keys.scan[a] = static_cast<UINT>(std::strtoul(value.c_str(), nullptr, 0));
            if (key.rfind("lamps_", 0) == 0) {   // lamps_<folder> = <ramp>|<spread>|<pitch>
                Settings::Lamps& l = s.lamps[key.substr(6)];
                const size_t a = value.find('|'), b = a == std::string::npos ? a : value.find('|', a + 1);
                l.layout = Trim(value.substr(0, a));
                if (a != std::string::npos) l.spread = static_cast<float>(std::atof(value.c_str() + a + 1));
                if (b != std::string::npos) l.pitch = static_cast<float>(std::atof(value.c_str() + b + 1));
            }
        }
    }
    return s;
}

std::string SaveSettings(const Settings& in)
{
    Settings s = in;
    std::ostringstream out;
    out << "# W-Rally's settings - written by the game (the Settings screen, Quick rally); edits by hand are kept.\n";
    Fields(s, [&](const char* name, auto& field) { out << name << " = " << Text(field) << "\n"; });
    for (int a = 0; a < input::kActions; ++a) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%#x", s.keys.scan[a]);
        out << "key_" << input::Info(static_cast<input::Action>(a)).id << " = " << (s.keys.scan[a] ? hex : "0") << "   # "
            << input::KeyName(s.keys.scan[a]) << "\n";
    }
    for (const auto& [car, l] : s.lamps) out << "lamps_" << car << " = " << l.layout << "|" << Text(l.spread) << "|" << Text(l.pitch) << "\n";
    std::ofstream f(SettingsPath(), std::ios::binary);
    if (!f) return "cannot write " + SettingsPath().string();
    f << out.str();
    return f ? "" : "cannot write " + SettingsPath().string();
}

} // namespace wrally::app
