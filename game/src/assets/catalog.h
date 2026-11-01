#pragma once
// wrally::assets::Catalog - what the menus offer (docs/design.md 6.1), from an RBR folder and the .ngg folders:
//
//   stages   every .ngg in the .ngg folders, for each stage of its map in Maps (track-41.ngg: track-41_M, ...; a
//            track-41_M.ngg that one), with the stage's weathers - the TrackSettings.ini sections for its map and time
//            of day (41M_crisp_clear, 41O_M_heavyrain_heavycloud, ...), or the .ngg's own presets when it has some
//            (rbrtm --import-weather writes them under those names); then every other stage in Maps (an .lbs and its
//            .trk), imported when it is driven (legacy_stage.h), its weathers TrackSettings.ini's, drawn - except a
//            protected (encrypted) one, which is never imported and so not offered (protectedStages)
//   cars     EVERY car folder in Cars (one with an .sgc), not only the eight Cars.ini slots. Its physics, in order:
//              - its own: Cars/<folder>/physics/ (common.lsp, <surface>.lsp, r_<surface>.lsp, setups/)
//              - a slot car's: its physics.rbz folder (Car00 c_xsara ... Car07 s_i2000, the game's table)
//              - else the base physics (Settings::basePhysics), so any car with an .sgc drives
//            its engine sound: an .eng in its folder, else its slot's (audio/Cars/Cars.ini), else the default
//   setups   per car: its own physics' setups/*.lsp, and a slot car's saved ones (SavedGames/<slot>slot<n>_<name>.lsp)
//
// ExtractSlotPhysics copies the slot cars' physics out of physics.rbz into their folders (Cars/<folder>/physics/), so
// every car's physics is loose files of its own; a car that has some already is left alone.

#include <string>
#include <vector>

#include "app/settings.h"
#include "assets/car_register.h"
#include "assets/track_register.h"

namespace wrally::assets {

struct Stage {
    std::string key;                    // ngg path '|' stage: what Settings keeps
    std::string ngg;                    // the .ngg file, or the .lbs of an imported stage
    std::string stage;                  // track-41_M
    int map = -1;
    std::string title;                  // for the menu
    // Its weathers: the .ngg's own presets (NGGW) when it carries some - what the sky is drawn from - else the
    // TrackSettings.ini names for its map and time of day, which then set only the road's wetness (Road: from the
    // weather); the sky stays the .ngg's own.
    std::vector<std::string> weathers;
    bool weathersDrawn = false;         // the weathers are the .ngg's presets
    bool imported = false;              // no .ngg: the .lbs is imported when the stage loads
};

struct Car {
    std::string folder;                 // Cars/<folder>
    std::string name;                   // Cars.ini's CarName, else the folder's name
    std::string sgc;                    // the model (a full path)
    std::string physics;                // a physics.rbz folder name, or Cars/<folder>/physics/ (root-relative)
    std::string physicsFrom;            // "its own", "slot N", "base (s_i2003)" - for the menu
    int slot = -1;                      // its Cars.ini slot (0..7), -1 none
    std::string engine;                 // its engine sound: an .eng path or an RBR name ("" the default)
};

struct Setup {
    std::string name;                   // for the menu
    std::string path;                   // root-relative (SavedGames/..., Cars/.../setups/...)
};

class Catalog {
public:
    // Returns "" or what is missing (no RBR folder, no stages...).
    std::string Scan(const app::Settings& s);
    std::vector<Stage> stages;
    std::vector<std::string> protectedStages;   // encrypted, so not offered (no .ngg draws them, none is imported)
    std::vector<Car> cars;
    // What W-Rally knows of them (assets/car_register.h, track_register.h), built with them: a car's friendly name and
    // its car.json's physics are the catalog's car's too, a track's name its stages' titles.
    CarRegister carRegister;
    TrackRegister trackRegister;
    std::vector<Setup> SetupsFor(const Car& car) const;
    const Stage* FindStage(const std::string& key) const;
    const Car* FindCar(const std::string& folder) const;

private:
    std::string m_root;
};

// The physics.rbz folder of RBR's car slot 0..7 ("" none).
const char* SlotPhysics(int slot);

// The physics of the slot cars without their own copied out of physics.rbz into Cars/<folder>/physics/. Returns what
// it did, a line a car.
std::string ExtractSlotPhysics(const std::string& rbrPath, const Catalog& catalog);

// The weather of a preset's name for the physics: the road 0 dry, 1 damp, 2 wet (rain).
int RoadOfWeather(const std::string& preset);

} // namespace wrally::assets
