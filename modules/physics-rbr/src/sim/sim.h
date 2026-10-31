#pragma once
// The physics as a library (M5): a stage and a car loaded from the game's files - physics.rbz, the stage's .trk / .col /
// .mat / .fnc / .lbs, the car's .sgc - and stepped with control records, natively in 64 bits.
//
// W-Rally has the native implementation only (NativeSim, sim/native_sim.h). plugin-rbr's classic Sim - the game's code
// in a 4 GB arena, from the exe's image - stays there as the oracle the native port is checked against
// (docs/design.md 0); Config's exe and heap fields are its and unused here. The stage clock and the stage helpers
// are the game's (game/src/rules/stage.h).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace signals {
class Bus;
}

namespace phys::sim {

struct Config {
    std::string exe;            // empty: the exe's image from core/tables/exeimage.inc; else RichardBurnsRally_SSE.exe,
                                // its image mapped from the file (data only, nothing of it runs) - for PHYS_SIM_TOUCH
    std::string rbz;            // physics.rbz
    std::string trk, col, mat, fnc, lbs;   // the stage's files (Maps/track-<n>_<M|O>.*)
    std::string sgc;            // the car's model (Cars/<car>/<car>.sgc): its collision meshes
    std::string car = "s_i2003";            // the car's physics: a folder in physics.rbz, or a folder path ending in /
    std::string surface = "tarmac";         // tarmac, gravel or snow: the car's <surface>.lsp, its setup and options
    std::string tyres = "michelin";         // the tyre brand (Physics/<brand>/tyres.lsp; the game: pirelli for cars 5, 7)
    std::string setup;                      // a setup: a physics.rbz member (Physics/...) or a file; default the car's setups/d_<surface>.lsp
    // Our own values on top of RBR's car (W-Rally's car data, cars/<car>.ini; "" none): `Name = value` lines, written
    // into the car's defs once they are built - for what RBR's files cannot set or set otherwise than the real car.
    // Names (CarOverride): Engine.IdleThrottle (the eased throttle's minimum: the idle's revs).
    // The TEXT (the file read by the caller: W-Rally's car data is not in the game's VFS), `overridesName` what messages
    // call it.
    std::string overrides, overridesName = "overrides";
    uint32_t tyreIndex = 0;                 // the tyre type (TyreTypeNN of the options)
    uint32_t settings = 0x217;              // the game's options as data/rec.h's settings word
    uint32_t aids[4] = {};                  // AutoGears, GearGuard, NeutralLock, ClutchHelp
    uint32_t weather = 0;                   // the surface maps' weather (the .mat's: dry, damp, wet x new, normal, worn)
    int32_t wetness = -1;                   // 0 dry, 1 damp, 2 wet (the water loads only when wet); -1: weather / 3
    std::vector<std::array<uint32_t, 3>> anims;   // the animation objects' pick (positions; data/rec.h 'A'); none: no spectators
    bool start = false;                     // the car's start here instead of the .lbs's (2,11):
    float startPosition[3] = {}, startHeading = 0;   // the physics' axes; the heading as the .lbs's angle (radians)
    uint32_t heap = 0x30000000;             // where the load's heap sits in the arena (the oracle's layout)
};

// A control record (CSigCarControl, the first 8 words; data/rec.h 'C'): throttle, brake, handbrake, clutch (0..1),
// steering (-1 .. 1, left negative), flags (bit 0 gear up, bit 1 gear down, bit 2 the gear in `gear`), gear, 0.
struct Control {
    float throttle = 0, brake = 0, handbrake = 0, clutch = 0, steering = 0;
    uint32_t flags = 0, gear = 0, reserved = 0;
};
static_assert(sizeof(Control) == 32, "a control record is 8 words");

// The car's state for a renderer (the physics' frame: z up).
struct Pose {
    float position[3];      // the car body +0x140
    float rotation[4];      // a quaternion (x, y, z, w) +0x100
    float velocity[3];      // +0x1c0
};

// A stage object the physics moves (a solid of the stage's: cones, signs, fences ...), as a renderer follows it: its
// pose at the load and now (the physics' frame), and its collision mesh's bounds in its own frame.
struct Body {
    int32_t id;   // the .trk instance's id: the .ngg objects keyed with it (NGGK, ngg_object_key) draw this solid
    float startPosition[3], startRotation[4];
    float position[3], rotation[4];
    float lo[3], hi[3];
    bool hasMesh;
};

// What a dashboard shows.
// A wheel (CWheel, car +0x470 + 4 k; k = 0 lf, 1 rf, 2 lb, 3 rb) in the car's frame (CarPose's, the centre of gravity):
// the hub +0x140, the knuckle's rotation +0x100 (steering, camber, toe; the right wheels turned half about z; not the
// spin), and the spin angle +0x5ac (radians, about the knuckle's x, wrapped).
struct Wheel {
    float position[3];
    float rotation[4];
    float spin;
};

// A part of the car that can come loose (CHingedRigidBody: the doors, the bonnet, the exhaust pipe; CMountedRigidBody:
// the bumpers, the wing): `name` is its collision mesh's in the .sgc (c_e_<name>). On the car its pose is in the car's
// frame (CarPose's: a door swings on its hinge, a loose bumper hangs on its base point); off, it is a solid of the
// world's and its pose the world's. `rest*`: its pose in the car's frame at the load.
struct Part {
    std::string name;
    uint32_t state = 0;     // 0 on, 1 loose (a mounted part hanging on its base point), 2 off
    bool inCar = true;      // the pose in the car's frame (else the world's)
    float position[3]{}, rotation[4]{};
    float restPosition[3]{}, restRotation[4]{};
};

struct Dashboard {
    float speed;    // m/s, the car body's velocity
    float rpm;      // the engine: E +0x45c (rad/s) * 60 / 2 pi, E = *(car + 0x4b0 + 8)
    int gear;       // -1 reverse, 0 neutral, 1.. forward (the drive's gear index E +0x54c - 1)
};

// A stage's physics as a library user drives it: the classic one (Sim, the game's code in its arena) or the native one
// (NativeSim, sim/native_sim.h: native/run.h's world). The same calls, the same results bit for bit.
class Physics {
public:
    virtual ~Physics() = default;
    // Loads the world. Returns "" or what is wrong (then it is unusable).
    virtual std::string Open(const Config& c) = 0;
    // A control record to the car (CarControlInput), as the game sends one before its steps.
    virtual void Apply(const Control& c) = 0;
    virtual void ApplyWords(const uint32_t words[8]) = 0;
    // StepForward(n) - the game calls it with 1 per physics tick. Returns false on a fault (Error()).
    virtual bool Step(uint32_t n = 1) = 0;
    virtual Pose CarPose() const = 0;
    virtual Dashboard Dash() const = 0;
    virtual void Wheels(Wheel out[4]) const = 0;
    // The car's records (signals/car.h) on `bus` as car `car` from now on: CarInfo and EngineInfo at once, the ...Tick
    // records and StepEnd after every Step (one set per call: Step(1) for them tick by tick), the events within a step
    // (GearChange, Collision; the classic one: every classic emit as a ClassicSignal too). Reading only. nullptr: none.
    virtual void Connect(signals::Bus* bus, uint32_t car = 0) = 0;
    // The car body's centre of gravity in the model's frame (the .sgc's, the collision meshes', physics axes): the
    // pose's origin; a model point v sits at v - CenterOfGravity in the car's frame.
    virtual void CenterOfGravity(float out[3]) const = 0;
    // The stage's water and river quads as loaded: their bounds (physics axes).
    struct Box {
        float lo[3], hi[3];
    };
    virtual std::vector<Box> Water() const = 0;
    // The stage's solids (Body), in the order the stage created them; Bodies() refreshes their current poses.
    virtual const std::vector<Body>& Bodies() = 0;
    // The car's parts that can come loose (Part), as they are now.
    virtual std::vector<Part> Parts() const = 0;
    // The 13 words the probe's POSE line holds (quaternion, position, angular velocity, velocity), for bit-exact checks.
    virtual void PoseWords(uint32_t out[13]) const = 0;
    // The car's distance along the stage's drive line, as the stage timing measures it.
    virtual float StageDistance() const = 0;
    virtual float Dt() const = 0;          // the physics' time step
    virtual uint32_t Tick() const = 0;     // the world tick
    virtual const std::string& Error() const = 0;
    virtual std::string Log() const = 0;
};

} // namespace phys::sim
