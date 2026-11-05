// ---------------------------------------------------------------------------
// wrally/iface/physics.h - the Physics module interface (WPhysicsRBR.dll,
// WPhysics.dll, and whatever comes after).
//
// The shape is plugin-rbr/phys/sim/sim.h's Physics - the class the native RBR
// port (phys/native, NativeSim) already implements bit for bit against the
// game - turned into a function table: load a world from the game's files,
// apply a control record, step, read the state back. What changes:
//   - files arrive through HostServices::files (the game's VFS), not paths
//     opened by the module
//   - the world holds CARS, by index, so the interface does not have to change
//     when a physics can run more than one; a module that cannot says so in
//     its caps and refuses carCount > 1
//   - records (engine, drivetrain, wheels, collisions) are delivered to a C
//     callback instead of a signals::Bus: same structs (records.h), fixed
//     layout, so the game re-emits them on its own bus unchanged
//
// Conventions:
//   - PHYSICS AXES: z up, as RBR's physics (render is y up; the game swaps)
//   - SI units; quaternions (x, y, z, w)
//   - the physics owns its time step (dt) - RBR's is fixed by physics.lsp,
//     WPhysics' is set in its own configuration and may be much shorter. The
//     game steps whole ticks to keep real time and interpolates for the
//     renderer; nothing in the game assumes a particular rate.
//
// CONFIGURATION is the module's own business. At InitWInterface it reads
// PhysicsInput::config (a VFS path, typically config/physics/<id>.json; ""
// means none, run on defaults) in whatever format it likes - JSON for
// WPhysics. Per car, CarDesc::carData names the module's own car file; the
// game passes it through without reading it.
//
// DETERMINISM IS PART OF THE CONTRACT. The same module build (ModuleInfo::
// buildHash), the same files and the same control records give the same state
// bit for bit, on any machine the module runs on. Replays (.phr) are controls
// plus that hash, and online results are only comparable within it.
//
// THREADING: all calls on one World from one thread at a time (the game's sim
// thread). Separate Worlds are independent. The record sink is called on the
// thread that called step.
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/common.h"

namespace wrally::iface {

// 2: carParts (PartState: the doors, the bonnet, the bumpers... where the physics has them).
// 3: the game's content layers (docs/content-layers.md): a car's physics is always a folder of loose files
//    (CarDesc::physicsCar), the shared files a folder too (StageDesc::physicsBase) - no physics.rbz; a setup is always a
//    file. The module reports the physics formats it reads and its simulation version (PhysicsOutput).
constexpr uint32_t kPhysicsInterfaceVer = 3;

struct PhysicsModule;   // the module's instance
struct World;           // one stage with its cars

// PhysicsCaps (ModuleInfo::caps)
constexpr uint64_t kPhysicsCapMultiCar      = 1ull << 0;   // carCount > 1
constexpr uint64_t kPhysicsCapSaveState     = 1ull << 1;   // saveState / loadState
constexpr uint64_t kPhysicsCapRbrCars       = 1ull << 2;   // reads RBR's car physics (the "rbr-lsp" format)
constexpr uint64_t kPhysicsCapRbrStages     = 1ull << 3;   // reads .trk / .col / .mat / .fnc / .lbs
constexpr uint64_t kPhysicsCapSpectators    = 1ull << 4;   // animation objects (.lbs scripts)
constexpr uint64_t kPhysicsCapCarOverrides  = 1ull << 5;   // CarDesc::overrides (W-Rally's car data)
constexpr uint64_t kPhysicsCapExtendedCars  = 1ull << 6;   // its own car files (CarDesc::carData)

#pragma pack(push, 4)

enum class Surface : uint32_t { Tarmac = 0, Gravel = 1, Snow = 2 };

// The stage. VFS paths; "" for a file the stage does not have.
struct StageDesc {
    const char* trk;          // Maps/track-<n>_<M|E|N|O>.trk: collision objects, drive line
    const char* col;          //                         .col: static collision
    const char* mat;          //                         .mat: surface maps
    const char* fnc;          //                         .fnc: fences
    const char* lbs;          //                         .lbs: script objects
    const char* dls;          //                         .dls: splits and finish along the drive line
    const char* physicsBase;  // the car physics format's shared files: a VFS folder ending in '/' ("physics/rbr/rbr/":
                              // physics.lsp, <brand>/tyres.lsp); "" for a module that brings its own
    uint32_t    weather;      // the .mat's weather: dry, damp, wet x new, normal, worn
    int32_t     wetness;      // 0 dry, 1 damp, 2 wet; -1 from weather
    uint32_t    settings;     // RBR's options word (phys data/rec.h: game mode, damage level)
    uint32_t    flags;        // kStage*
};
constexpr uint32_t kStageSpectators = 1u << 0;   // load the animation objects (needs kPhysicsCapSpectators)

// One car. Strings are VFS paths or names, "" for the default or "not this kind".
// An RBR car fills physicsCar (kPhysicsCapRbrCars); a W-Rally car fills carData
// (the module's own file, e.g. cars/<car>/wphysics.json). A module refuses a car
// it has no data for with ErrNotSupported.
struct CarDesc {
    const char* physicsCar;   // RBR car physics: a VFS folder ending in '/' of loose files
                              // ("cars/s_i2003/physics/rbr/rbr/": common.lsp, <surface>.lsp, r_<surface>.lsp,
                              // setups/d_<surface>.lsp)
    const char* carData;      // the module's own car file (VFS path)
    const char* model;        // Cars/<car>/<car>.sgc: the collision meshes
    const char* setup;        // a setup: a VFS file ("rbr/SavedGames/...", the car physics' setups/...); "" the car's
                              // setups/d_<surface>.lsp
    const char* tyres;        // brand: "michelin" (<physicsBase><brand>/tyres.lsp)
    const char* overrides;    // W-Rally's car data: `Name = value` lines (text, not a path), "" none
    Surface     surface;
    uint32_t    tyreType;     // TyreType00..06
    uint32_t    aids[4];      // AutoGears, GearGuard, NeutralLock, ClutchHelp
    uint32_t    useStart;     // 1: the pose below instead of the stage's start (.lbs)
    float       startPosition[3];
    float       startHeading; // radians, as the .lbs's angle
};

// A control record - sim::Control, CSigCarControl's first 8 words.
constexpr uint32_t kControlGearUp   = 1u << 0;
constexpr uint32_t kControlGearDown = 1u << 1;
constexpr uint32_t kControlGearSet  = 1u << 2;   // the gear in `gear`
struct Controls {
    float    throttle, brake, handbrake, clutch;   // 0..1
    float    steering;                             // -1 left .. 1 right
    uint32_t flags;
    uint32_t gear;
    uint32_t reserved;
};
static_assert(sizeof(Controls) == 32, "a control record is 8 words");

// Records the physics emits during step: kind is a records.h kKind, `data`
// that record, valid only during the call. Unknown kinds are ignored by the
// game - which is how a newer module adds records without a version bump.
struct RecordSink {
    void* ctx;
    void (WRALLY_CALL* onRecord)(void* ctx, uint32_t kind, const void* data, uint32_t bytes);
};

struct WorldDesc {
    StageDesc      stage;
    const CarDesc* cars;
    uint32_t       carCount;
    RecordSink     sink;       // onRecord null: no records
};

// ------------------------------------------------------------------- state

struct WheelState {           // in the car's frame (its centre of gravity)
    float    position[3];
    float    rotation[4];     // the knuckle: steering, camber, toe - not the spin
    float    spin;            // radians about the knuckle's x, wrapped
    float    spinRate;        // rad/s
    float    travel;          // m, suspension
    uint32_t onGround;
    uint32_t surface;         // the .mat entry under it
    float    slipLongitudinal, slipLateral;   // m/s
};

struct CarState {
    float      position[3];   // the body's centre of gravity, world
    float      rotation[4];
    float      velocity[3];
    float      angularVelocity[3];
    WheelState wheels[4];     // lf, rf, lb, rb
    float      speed;         // m/s
    float      rpm;
    int32_t    gear;          // -1 reverse, 0 neutral, 1..
    float      steeringTorque;   // N m at the steering column: the wheel's force feedback
    float      stageDistance;    // m along the drive line, as stage timing measures it
    uint32_t   damaged;          // bit per part (doors, bonnet, wings ...) detached or broken
};

// A stage solid the physics moves. `id` is the .trk instance's - the renderer's
// StageObjectUpdate::key.
struct BodyState {
    int32_t id;
    float   startPosition[3], startRotation[4];
    float   position[3], rotation[4];
};

struct Box {
    float lo[3], hi[3];
};

// A part of the car that can come loose - a door, the bonnet, a bumper - as the physics has it. `name` is the car
// model's name for it (RBR: its .sgc collision mesh c_e_<name>: "door_l", "door_r", "bonnet", "exhaust", "bumper_f",
// "bumper_b", "wing"), so the game can find the render meshes that go with it. On the car (inCar) its pose is in the
// car's frame (CarState's: a door swinging on its hinge, a loose bumper hanging); off, it is a solid of the world's
// and its pose the world's. rest*: its pose in the car's frame at the load - where it sits undamaged.
constexpr uint32_t kPartOn = 0, kPartLoose = 1, kPartOff = 2;
struct PartState {
    char     name[16];
    uint32_t state;           // kPart*
    uint32_t inCar;           // 1: the pose in the car's frame; 0: the world's
    float    position[3], rotation[4];
    float    restPosition[3], restRotation[4];
};
static_assert(sizeof(PartState) == 80, "PartState layout");

// Outside assistance: put the car back on the road.
struct ResetDesc {
    uint32_t useDriveLine;    // 1: the nearest drive line point, facing along it
    float    position[3];     // else here
    float    heading;
};

#pragma pack(pop)

// ------------------------------------------------------------- the table
struct PhysicsApi {
    void        (WRALLY_CALL* destroy)(PhysicsModule*);
    const char* (WRALLY_CALL* lastError)(PhysicsModule*);       // never null

    // Loads everything (the slow part: files, meshes, defs). On failure *out
    // is null and lastError says what.
    Status   (WRALLY_CALL* createWorld)(PhysicsModule*, const WorldDesc*, World** out);
    void     (WRALLY_CALL* destroyWorld)(World*);
    const char* (WRALLY_CALL* worldError)(World*);              // never null

    float    (WRALLY_CALL* dt)(World*);                         // the tick, s
    uint32_t (WRALLY_CALL* tick)(World*);

    // Before each step, as the game sends its CSigCarControl before StepForward.
    Status (WRALLY_CALL* setControls)(World*, uint32_t car, const Controls*);
    // n ticks. ErrFault: the world is broken (worldError), destroy it.
    Status (WRALLY_CALL* step)(World*, uint32_t ticks);

    Status   (WRALLY_CALL* carState)(World*, uint32_t car, CarState* out);
    Status   (WRALLY_CALL* centerOfGravity)(World*, uint32_t car, float out[3]);   // in the model's frame
    // Stage solids, in the order the stage made them. Writes up to `capacity`,
    // returns how many there are.
    uint32_t (WRALLY_CALL* bodies)(World*, BodyState* out, uint32_t capacity);
    uint32_t (WRALLY_CALL* water)(World*, Box* out, uint32_t capacity);

    Status (WRALLY_CALL* resetCar)(World*, uint32_t car, const ResetDesc*);

    // kPhysicsCapSaveState: the whole world's state as bytes (restart from a
    // split, rewind, rollback). The blob is the module's format, valid only for
    // the same buildHash and the same WorldDesc.
    Status (WRALLY_CALL* saveState)(World*, Blob* out);
    void   (WRALLY_CALL* releaseState)(World*, Blob*);
    Status (WRALLY_CALL* loadState)(World*, const void* data, uint64_t bytes);

    // The car's parts that can come loose (PartState), as they are now. Writes up to `capacity`, returns how many
    // there are (0: a car without any).
    uint32_t (WRALLY_CALL* carParts)(World*, uint32_t car, PartState* out, uint32_t capacity);
};

struct PhysicsInput {
    StructHeader header;      // kind Physics, kPhysicsInterfaceVer
    HostServices host;
    const char*  config;      // the module's configuration file (VFS path), "" none
};

// A car physics format the module reads, and the range of its versions (physics.json's format, formatVersion).
struct PhysicsFormat {
    char     name[32];        // "rbr-lsp"
    uint32_t minVersion, maxVersion;
};

struct PhysicsOutput {
    StructHeader         header;
    ModuleInfo           info;
    const PhysicsApi*    api;
    PhysicsModule*       instance;
    const PhysicsFormat* formats;      // what it can drive; valid for the instance's lifetime
    uint32_t             formatCount;
    // Bumped only when the same files and controls give a different result - not on every release (buildHash is the
    // exact build). Replays and online results record it.
    uint32_t             simVersion;
};

} // namespace wrally::iface
