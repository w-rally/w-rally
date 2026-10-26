#include "app/settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "app/options.h"

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
    f("quick_stage", s.stage);
    f("quick_car", s.car);
    f("quick_setup", s.setup);
    f("quick_damage", s.damage);
    f("quick_weather", s.weather);
    f("quick_road", s.road);
}

void Set(std::string& v, const std::string& text) { v = text; }
void Set(uint32_t& v, const std::string& text) { v = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10)); }
void Set(bool& v, const std::string& text) { v = text == "1" || text == "true" || text == "on"; }
std::string Text(const std::string& v) { return v; }
std::string Text(uint32_t v) { return std::to_string(v); }
std::string Text(bool v) { return v ? "1" : "0"; }

} // namespace

std::string Settings::NggFolders() const
{
    if (!nggFolders.empty()) return nggFolders;
    return rbrPath.empty() ? "" : rbrPath + "\\Ngg";
}

std::filesystem::path SettingsPath() { return ExeDir() / "wrally.ini"; }

Settings LoadSettings()
{
    Settings s;
    // Unset, the RBR folder is the exe's own when WRally sits in one.
    if (std::filesystem::exists(ExeDir() / "RichardBurnsRally_SSE.exe")) s.rbrPath = ExeDir().string();
    std::ifstream f(SettingsPath());
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
    }
    return s;
}

std::string SaveSettings(const Settings& in)
{
    Settings s = in;
    std::ostringstream out;
    out << "# WRally's settings - written by the game (the Settings screen, Quick rally); edits by hand are kept.\n";
    Fields(s, [&](const char* name, auto& field) { out << name << " = " << Text(field) << "\n"; });
    for (int a = 0; a < input::kActions; ++a) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%#x", s.keys.scan[a]);
        out << "key_" << input::Info(static_cast<input::Action>(a)).id << " = " << (s.keys.scan[a] ? hex : "0") << "   # "
            << input::KeyName(s.keys.scan[a]) << "\n";
    }
    std::ofstream f(SettingsPath(), std::ios::binary);
    if (!f) return "cannot write " + SettingsPath().string();
    f << out.str();
    return f ? "" : "cannot write " + SettingsPath().string();
}

} // namespace wrally::app
