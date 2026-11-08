#pragma once
// wrally::assets::Catalog - what the menus offer (docs/design.md 6.1), from the content layers (wrally::vfs,
// docs/content-layers.md: the downloaded folder, W-Rally's content, the RBR folder mapped) and the .ngg folders:
//
//   stages   every track's stages (its newest version: the item's track-<n>_<M|E|N|O>.trk), each drawn by, best first:
//            an .ngg of the track's own (<stage>.ngg, <id>.ngg, track-<n>.ngg in its folder), an .ngg in the .ngg
//            folders (development: track-41.ngg for every stage of map 41, track-41_M.ngg for that one), else its .lbs
//            imported when it is driven (legacy_stage.h) - except a protected (encrypted) one, which is never imported
//            and so not offered (protectedStages). Its weathers: the TrackSettings.ini sections for its map and time of
//            day (41M_crisp_clear, 41O_M_heavyrain_heavycloud, ...), or the .ngg's own presets when it has some (rbrtm
//            --import-weather writes them under those names); an imported stage's are drawn.
//   cars     every car (its newest version: one with an .sgc), not only the eight Cars.ini slots. Its physics: every
//            variant it has (cars/<id>/physics/<variant>/, the newest version of each: "rbr" its own loose physics/,
//            else its slot's folder in physics.rbz, as the VFS maps them; others downloaded or W-Rally's), each with
//            the modules that read its format (physicsModules); the default car.yaml's `physics` (a variant's id).
//            None a module drives: not usable (no stand-in: a car drives only as itself). Its engine sound: an .eng in
//            its folder, else its slot's (audio/Cars/Cars.ini), else the default.
//   setups   per car and physics: the physics' setups/*.lsp, and a slot car's saved ones
//            (rbr/SavedGames/<slot>slot<n>_<name>.lsp)
//
// The files the game reads itself (a stage's .ngg or .lbs, a car's .sgc and .eng) are disk paths, resolved through
// the item's versions; what the physics reads (a car's physics, a setup) are VFS paths.

#include <memory>
#include <string>
#include <vector>

#include "app/settings.h"
#include "assets/car_model.h"
#include "assets/car_register.h"
#include "assets/track_register.h"
#include "wrally/vfs/vfs.h"

namespace wrally::assets {

struct Stage {
    std::string key;                    // ngg path '|' stage: what Settings keeps
    std::string ngg;                    // the .ngg file, or the .lbs of an imported stage
    std::string stage;                  // track-41_M: its gameplay files, and the name its .ngg is asked for it by
    int map = -1;
    std::string track, version;         // its track item: id and version (fr_joux_verte, rbr)
    std::string trackStage;             // its stage of the track (rbr-tracks.yaml: "joux_verte_ii"), "" none
    int trackOrder = 0;                 // its place among the track's stages (the table's order, its main one first)
    // The RBR stages it is, itself first: a map's times of day (_M _N _E _O) that its .ngg made one stage (NGGA) - they
    // drive the same, and W-Rally makes the time of day as weather. One for a stage no .ngg collapsed.
    std::vector<std::string> times;
    std::string source;                 // where its .ngg (or .lbs) is: "downloaded", "w-rally", "rbr", "ngg folder"
    std::string title;                  // for the menu
    // Its weathers: the .ngg's own presets (NGGW) when it carries some - what the sky is drawn from - else the
    // TrackSettings.ini names for its map and time of day, which then set only the road's wetness (Road: from the
    // weather); the sky stays the .ngg's own.
    std::vector<std::string> weathers;
    bool weathersDrawn = false;         // the weathers are the .ngg's presets
    bool imported = false;              // no .ngg: the .lbs is imported when the stage loads
};

// A physics module as the catalog needs it (physics::ListModules): what it is and the car physics formats it reads.
struct PhysicsModule {
    std::string id, name;               // "rbr", "RBR original"
    uint32_t simVersion = 0;
    struct Format {
        std::string name;               // "rbr-lsp"
        uint32_t minVersion = 0, maxVersion = 0;
    };
    std::vector<Format> formats;
    bool Reads(const std::string& format, uint32_t version) const;
};

// One of a car's physics (docs/content-layers.md 4): cars/<car>/physics/<id>/<version>/ and its physics.yaml.
struct PhysicsVariant {
    std::string id, version;            // "rbr", "rbr"
    std::string name;                   // physics.yaml's ("RBR original"), else the id
    std::string folder;                 // the VFS folder ("cars/s_i2003/physics/rbr/rbr/")
    std::string base;                   // its physics base's VFS folder ("physics/rbr/rbr/"), "" none found
    std::string format;                 // "rbr-lsp"
    uint32_t formatVersion = 0;
    std::string source;                 // where its files are: "downloaded", "w-rally", "rbr"
    std::vector<std::string> modules;   // the installed physics modules that can drive it (ids, best first)
};

struct Car {
    std::string id, version;            // its item: s_i2003, 1.1.0
    std::string folder;                 // an RBR car's folder (Cars/<folder>), else its id: what Settings keeps
    std::string source;                 // where its model is: "downloaded", "w-rally", "rbr"
    std::string name;                   // Cars.ini's CarName, else the folder's name
    std::string sgc;                    // the model (a full path)
    // Its physics: every variant, the default's first (car.yaml's `physics`, else "rbr", else the first one a module
    // drives); `physics` the default's folder ("" none), `physicsFrom` where it comes from - for the menu.
    std::vector<PhysicsVariant> variants;
    std::string physics;
    std::string physicsFrom;            // "its own", "slot N", "car.yaml (...)" - for the menu
    int slot = -1;                      // its Cars.ini slot (0..7), -1 none
    std::string engine;                 // its engine sound: an .eng path or an RBR name ("" the default)
};

struct Setup {
    std::string name;                   // for the menu
    std::string path;                   // a VFS path (rbr/SavedGames/..., cars/.../setups/...)
};

class Catalog {
public:
    // Returns "" or what is missing (no stages...). The layers made from the settings (app::MakeVfs), or given.
    std::string Scan(const app::Settings& s);
    std::string Scan(const app::Settings& s, std::shared_ptr<const vfs::Vfs> content);
    std::vector<Stage> stages;
    std::vector<std::string> protectedStages;   // encrypted, so not offered (no .ngg draws them, none is imported)
    std::vector<Car> cars;
    // What W-Rally knows of them (assets/car_register.h, track_register.h), built with them: a car's friendly name and
    // its car.yaml's physics are the catalog's car's too, a track's name its stages' titles.
    CarRegister carRegister;
    TrackRegister trackRegister;
    // The physics modules installed (set before Scan: the variants' modules come from them).
    std::vector<PhysicsModule> physicsModules;
    // The car's physics as chosen: Settings' physics_<car id> when a module drives it, else the car's default when one
    // does; null: none any module drives - the car is not usable.
    const PhysicsVariant* PhysicsFor(const Car& car, const app::Settings& s) const;
    // A module drives one of its physics at least.
    static bool Drivable(const Car& car);
    // A physics's setups (default: the car's default physics').
    std::vector<Setup> SetupsFor(const Car& car, const PhysicsVariant* physics = nullptr) const;
    const Stage* FindStage(const std::string& key) const;
    const Car* FindCar(const std::string& folder) const;   // by its folder or its id
    // The layers it was scanned from (null before a Scan).
    const std::shared_ptr<const vfs::Vfs>& Content() const { return m_content; }

private:
    std::shared_ptr<const vfs::Vfs> m_content;
};

// A car's files through its item (`ref`: cars/<id>/<version>, through its base chain), for ReadCarModel; empty when
// the item is not in `content`.
CarFiles CarFilesOf(std::shared_ptr<const vfs::Vfs> content, const vfs::ItemRef& ref);

// The physics.rbz folder of RBR's car slot 0..7 ("" none).
const char* SlotPhysics(int slot);

// The weather of a preset's name for the physics: the road 0 dry, 1 damp, 2 wet (rain).
int RoadOfWeather(const std::string& preset);

} // namespace wrally::assets
