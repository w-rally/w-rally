#pragma once
// wrally::physics::Physics - the game's side of a physics module (docs/design.md 2.4): finds the module DLL, initialises
// it, and turns its function table (wrally/iface/physics.h) into the calls the game makes - the same shape as the
// physics library phys-drive linked (plugin-rbr's phys::sim::Physics), so the game did not change with the boundary.
//
// Records: the module's RecordSink lands here and is emitted on the game's signals::Bus (signals/convert.h). CarInfo and
// EngineInfo come once, at the load; Connect replays them, as the library did when connected after its load.
//
// Paths: the game's, under Config::root (the RBR folder) - handed to the module relative to it, as its VFS paths.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wrally/modules/module_host.h"

namespace signals {
class Bus;
}

namespace wrally::physics {

struct Config {
    std::string root;                                      // the game data root: the VFS's (the RBR folder)
    std::string rbz;                                       // physics.rbz
    std::string trk, col, mat, fnc, lbs;                   // the stage's files (Maps/track-<n>_<M|O>.*)
    std::string sgc;                                       // the car's model: its collision meshes
    std::string car = "s_i2003";                           // the car's physics folder in physics.rbz
    std::string surface = "tarmac";                        // tarmac, gravel or snow
    std::string tyres = "michelin";                        // the tyre brand
    std::string setup;                                     // a setup member in physics.rbz ("" the surface's default)
    std::string overrides;                                 // WRally's car data: a FILE, read here ("" none)
    uint32_t tyreIndex = 0;                                // TyreTypeNN
    uint32_t settings = 0x217;                             // the game's options word (data/rec.h)
    uint32_t aids[4] = {};                                 // AutoGears, GearGuard, NeutralLock, ClutchHelp
    uint32_t weather = 0;                                  // the .mat's weather
    int32_t wetness = -1;                                  // 0 dry, 1 damp, 2 wet; -1 weather / 3
    bool start = false;                                    // start here instead of the .lbs's:
    float startPosition[3] = {}, startHeading = 0;         // physics axes; the .lbs's angle (radians)
};

// A control record: 8 words, wrally/iface Controls.
struct Control {
    float throttle = 0, brake = 0, handbrake = 0, clutch = 0, steering = 0;
    uint32_t flags = 0, gear = 0, reserved = 0;
};
static_assert(sizeof(Control) == 32, "a control record is 8 words");

struct Pose {
    float position[3];
    float rotation[4];   // (x, y, z, w)
    float velocity[3];
};

// A stage solid the physics moves; `id` is the .trk instance's (the .ngg's NGGK key).
struct Body {
    int32_t id;
    float startPosition[3], startRotation[4];
    float position[3], rotation[4];
};

// In the car's frame: the hub, the knuckle's rotation (not the spin), the spin angle.
struct Wheel {
    float position[3];
    float rotation[4];
    float spin;
};

// A part of the car that can come loose (iface::PartState): `name` the car model's ("door_l", "bonnet", "bumper_f"...).
// On the car its pose is in the car's frame, off it the world's; rest*: in the car's frame at the load.
struct Part {
    char name[16];
    uint32_t state;      // iface::kPart*: 0 on, 1 loose, 2 off
    bool inCar;
    float position[3], rotation[4];
    float restPosition[3], restRotation[4];
};

struct Dashboard {
    float speed;   // m/s
    float rpm;
    int gear;      // -1 reverse, 0 neutral, 1..
};

struct Box {
    float lo[3], hi[3];
};

class Physics {
public:
    Physics();
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // The module with id `moduleId` ("rbr") among the physics DLLs in `modulesDir`, its world loaded. Returns "" or
    // what is wrong (then it is unusable).
    std::string Open(const std::filesystem::path& modulesDir, const std::string& moduleId, const Config& c);

    void Apply(const Control& c);
    void ApplyWords(const uint32_t words[8]);
    // Returns false on a fault (Error()).
    bool Step(uint32_t n = 1);
    Pose CarPose() const;
    Dashboard Dash() const;
    void Wheels(Wheel out[4]) const;
    // The car's records on `bus` from now on (nullptr: none).
    void Connect(signals::Bus* bus);
    void CenterOfGravity(float out[3]) const;
    std::vector<Box> Water() const;
    const std::vector<Body>& Bodies();
    // The car's parts that can come loose, as they are now.
    void Parts(std::vector<Part>& out) const;
    // The 13 words a recording's pose check holds: quaternion, position, angular velocity, velocity.
    void PoseWords(uint32_t out[13]) const;
    float StageDistance() const;
    float Dt() const;
    uint32_t Tick() const;
    const std::string& Error() const { return m_error; }
    // What the module logged while loading (its Info lines), one per line.
    const std::string& Log() const { return m_log; }
    // The module's ModuleInfo name and id, for the log.
    std::string ModuleName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    std::string m_error, m_log;
    std::vector<Body> m_bodies;
};

} // namespace wrally::physics
