#pragma once
// W-Rally's settings (wrally::app): W-Rally.ini in the user folder (app/paths.h), `name = value` lines - where the content is, the graphics
// and the sound, and what Quick rally chose last time. Read at the start, written when the Settings screen or Quick
// rally changes them. A missing file or key is its default.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "input/bindings.h"

namespace wrally::app {

struct Settings {
    // Content
    std::string rbrPath;                // the RBR folder ("" not set: Settings asks for it)
    std::string downloadPath;           // the downloaded content's folder, %VARIABLES% kept ("" the default: DownloadDir)
    std::string nggFolders;             // folders of stage .ngg files, ';' between them ("": <rbr>/Ngg) - the .ini's alone, for development

    // Graphics
    bool fullscreen = true;             // the whole screen (borderless), else a window
    uint32_t quality = 1;               // 0 low, 1 medium, 2 high
    bool rayTracing = true, taa = true, shadows = true, vsync = true;
    uint32_t upscaling = 0;             // kUpscaleModes: 0 off (the renderer's TAA at the full size), 1.. FSR, 5.. DLSS

    // Sound
    bool sound = true;
    bool codriver = true;               // the pacenotes read out
    float codriverVolume = 1.0f;        // 0..1
    float codriverLead = 3.0f;          // seconds ahead at the speed: earlier (more) or later (less)
    bool codriverReplay = false;        // the pacenotes in replays too (normally the live game only)
    float pacenoteNext = 8.0f;          // the HUD: the next call shown this many seconds before it (live only)
    // Development sounds over every car's ("" none): an .eng path, folders of anti-lag bang .wavs (moving, standing)
    std::string devEngine, devBangs, devIdleBangs;

    // Controls: the keys (key_<action> = its scan code, hex)
    input::Bindings keys;

    // Quick rally's last choice
    std::string stage;                  // the stage's key (catalog::Stage::key)
    std::string car;                    // the car's folder (Cars/<folder>)
    std::string setup;                  // "" the default, else the setup's path
    uint32_t damage = 2;                // 0 off, 1 safe, 2 reduced, 3 realistic
    std::string weather;                // a weather preset's name ("" the stage's default)
    uint32_t road = 0;                  // 0 from the weather, 1 dry, 2 damp, 3 wet
    std::string time;                   // the time of day, "HH:MM" on kQuickDate ("" the weather's own sun)
    // Custom weather (Quick rally -> Weather -> Custom): over `weather`'s preset, its clouds, fog and rain these, and the
    // time of day on the season's date (QuickDate).
    bool weatherCustom = false;
    uint32_t season = 2;                // 0 spring, 1 summer, 2 autumn, 3 winter
    float clouds = 0.3f, fog = 0.0f, rain = 0.0f;   // 0..1 each: the sky's cover, how thick the fog, how hard it rains

    // The lamps each car runs (NGG design §5.5; in the setup editor once there is one): per car folder its ramp (a
    // CarLamps layout's name, "" its first) and the ramp's aim - each pod further out, degrees; up, degrees. W-Rally.ini:
    // lamps_<folder> = <ramp>|<spread>|<pitch>.
    struct Lamps {
        std::string layout;
        float spread = 0.0f, pitch = 0.0f;
    };
    std::map<std::string, Lamps> lamps;
    // The physics each car was last driven with (docs/content-layers.md 4): per car id its physics variant's id.
    // W-Rally.ini: physics_<car id> = <variant id>.
    std::map<std::string, std::string> physics;

    // The ';'-separated .ngg folders, or <rbr>/Ngg.
    std::string NggFolders() const;
};

// The upscaling modes (Settings::upscaling, W-Rally.ini's `upscaling`; PHYS_DRIVE_UPSCALE=<index or name> over it):
// the upscaler, and the render scale per axis it upscales from - FSR's own ratios (1.5x, 1.7x, 2x) and NVIDIA's for
// DLSS (DLAA 1x, 1.5x, 1.72x, 2x). A DLSS mode where DLSS cannot run (no RTX GPU) is the renderer's TAA, logged.
struct UpscaleMode {
    const char* name;                   // as PHYS_DRIVE_UPSCALE takes it
    const char* label;                  // the Settings screen's
    uint32_t upscaler;                  // iface::kUpscaler*
    float scale;
};
inline constexpr UpscaleMode kUpscaleModes[] = {
    {"off", "OFF (TAA)", 0, 1.0f},
    {"fsr-native", "FSR NATIVE AA", 1, 1.0f},
    {"fsr-quality", "FSR QUALITY", 1, 1.0f / 1.5f},
    {"fsr-balanced", "FSR BALANCED", 1, 1.0f / 1.7f},
    {"fsr-performance", "FSR PERFORMANCE", 1, 0.5f},
    {"dlaa", "NVIDIA DLAA", 2, 1.0f},
    {"dlss-quality", "NVIDIA DLSS QUALITY", 2, 1.0f / 1.5f},
    {"dlss-balanced", "NVIDIA DLSS BALANCED", 2, 0.58f},
    {"dlss-performance", "NVIDIA DLSS PERFORMANCE", 2, 0.5f},
};
inline constexpr uint32_t kUpscaleModeCount = sizeof kUpscaleModes / sizeof kUpscaleModes[0];

std::filesystem::path SettingsPath();   // W-Rally.ini in the user folder (app/paths.h: beside the exe unless installed)
Settings LoadSettings();
// The date Quick rally's times of day are on: an autumn day, the clocks still on summer time (dusk about 18:30 at 50 N).
inline const char* kQuickDate = "2026-10-15";
inline const char* kQuickUtcOffset = "2";
// A season's date for the custom weather (0 spring .. 3 winter; autumn kQuickDate) and its clocks' offset from UTC.
struct QuickDay {
    const char* date;
    const char* utcOffset;
};
inline QuickDay QuickDate(uint32_t season)
{
    static const QuickDay kDays[4] = {{"2026-04-15", "2"}, {"2026-07-15", "2"}, {"2026-10-15", "2"}, {"2026-01-15", "1"}};
    return kDays[season < 4 ? season : 2];
}
// Returns "" or what went wrong.
std::string SaveSettings(const Settings& s);

} // namespace wrally::app
