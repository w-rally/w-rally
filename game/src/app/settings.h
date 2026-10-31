#pragma once
// WRally's settings (wrally::app): wrally.ini beside the exe, `name = value` lines - where the content is, the graphics
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
    std::string nggFolders;             // folders of stage .ngg files, ';' between them ("": <rbr>/Ngg)
    std::string basePhysics = "s_i2003";   // the physics of a car without its own (a physics.rbz folder)

    // Graphics
    uint32_t quality = 1;               // 0 low, 1 medium, 2 high
    bool rayTracing = true, taa = true, shadows = true, vsync = true;

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

    // The lamps each car runs (NGG design 5.4 N3b; in the setup editor once there is one): per car folder its ramp (a
    // CarLamps layout's name, "" its first) and the ramp's aim - each pod further out, degrees; up, degrees. wrally.ini:
    // lamps_<folder> = <ramp>|<spread>|<pitch>.
    struct Lamps {
        std::string layout;
        float spread = 0.0f, pitch = 0.0f;
    };
    std::map<std::string, Lamps> lamps;

    // The ';'-separated .ngg folders, or <rbr>/Ngg.
    std::string NggFolders() const;
};

std::filesystem::path SettingsPath();   // wrally.ini beside the exe
Settings LoadSettings();
// The date Quick rally's times of day are on: an autumn day, the clocks still on summer time (dusk about 18:30 at 50 N).
inline const char* kQuickDate = "2026-10-15";
inline const char* kQuickUtcOffset = "2";
// Returns "" or what went wrong.
std::string SaveSettings(const Settings& s);

} // namespace wrally::app
