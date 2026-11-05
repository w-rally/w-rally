#pragma once
// A stage's world loaded natively from the game's files and ready to run (M6 step 2): the defs (physics.lsp, the car's
// common and surface .lsp), the stage (the ground, the .col, the .trk's meshes and objects, the surface maps), the car
// (its damage handler, the .sgc's collision meshes, its records at the .lbs start), the setup (the setup file, its
// options and the tyres; the setup screen's derivations, the aids, the tyre type) and its finish, the drive line and the
// fences, in the game's order (native/load.h's WorldLoader) - then stepped one tick at a time with the car's controls.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data/rec.h"
#include "data/trkworld.h"
#include "native/defs.h"
#include "native/load.h"
#include "native/setup.h"

namespace phys::native {

// What the load reads: the files and the game's options.
struct WorldFiles {
    std::string base;                             // the format's shared files: a folder ending in / (physics.lsp, <brand>/tyres.lsp)
    std::string trk, col, mat, fnc, lbs;          // the stage's (Maps/track-<n>_<M|O>.*)
    std::string sgc;                              // the car's model: its collision meshes
    std::string car;                              // the car's physics: a folder of loose files, ending in /
    std::string surface = "tarmac";               // its <surface>.lsp, setup and options
    std::string tyres = "michelin";               // the tyre brand (<base><brand>/tyres.lsp)
    std::string setup;                            // a setup file; default the car's setups/d_<surface>.lsp
    uint32_t tyreIndex = 0;                       // the tyre type (TyreTypeNN of the options)
    uint32_t settings = 0x217;                    // the game's options word (bit 3 the game mode, 0x10 a damage level in bits 8..15)
    uint32_t aids[4] = {};                        // AutoGears, GearGuard, NeutralLock, ClutchHelp
    uint32_t weather = 0;                         // the surface maps' weather
    int32_t wetness = -1;                         // 0 dry, 1 damp, 2 wet; -1: weather / 3
    bool start = false;                           // the car's start here instead of the .lbs's
    float startPosition[3] = {}, startHeading = 0;   // the physics' axes; the heading as the .lbs's angle
};

// What a load made and keeps: the defs (the loader's entities point into them), the loader, the records' data.
struct LoadedWorld {
    Defs defs;
    std::unique_ptr<WorldLoader> loader;
    data::TrkWorld trk;
    std::vector<uint8_t> driveLine;                // the .trk's block (3,20)
    std::vector<MeshRecord> baked;                 // the .trk's baked meshes' records (views of trk's)
    std::vector<data::RecMesh> carMeshes;
    CarSetup setup;
    CarSetupOptions options;
    // The paths the stage's records took, and what was refused.
    uint32_t meshes = 0, baked0 = 0, renamed = 0, rotated = 0, slot0 = 0, refused = 0;
    std::string colError;                          // the .col not loaded ("": loaded) - the game's world has no stage collision then
    std::string firstRefused;                      // the first stage record refused, why
    struct StageSolid {
        Solid* solid;
        int32_t id;                                // the .trk instance's (a renderer's object of it has the same)
    };
    std::vector<StageSolid> stageSolids;           // in the order the stage made them
};

// The load up to the car's records (the defs, the stage, the car's damage handler, meshes and records), before the
// setup. Returns "" or what is wrong.
std::string LoadToCar(const WorldFiles& f, LoadedWorld& w);
// The rest: the setup into the defs, the car's finish, the drive line, the fences, the world readied to run.
std::string FinishLoad(const WorldFiles& f, LoadedWorld& w);
// Both.
std::unique_ptr<LoadedWorld> LoadWorld(const WorldFiles& f, std::string& error);

} // namespace phys::native
