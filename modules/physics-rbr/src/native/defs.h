#pragma once
// The defs, natively (M6 step 2, the load; the classic def manager, oracle/worldbuild.h): what the .lsp files say about
// the physics - physics.lsp's PhysicsEngine (the config, the materials, the defs of the stage's objects) and, later, the
// car's files - read straight into the native defs, without the game's def objects.
//
// A def's keys are bound through a key table generated from the game's dicts and its constructor's words
// (re/py/native_defs.py -> native/def_keys.inc): every key the game knows is known here (the ones the native physics
// does not use are read and dropped), each kept one stores into its native member, and the defaults are the
// constructor's. Each def is one kind's (the game's 50 kinds: GROUND, the stage objects' GENERIC .. WOODEN_FENCE_POLE,
// the car's parts, CAR_ROOT); a file's list for a kind lays its keys over what the kind has so far.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "num/math.h"
#include "native/drive.h"
#include "native/engine.h"
#include "native/entity.h"
#include "native/rigid_body.h"
#include "native/solver.h"
#include "native/suspension.h"
#include "native/tyre.h"
#include "native/vcu.h"
#include "native/wheel.h"

namespace phys::data {
struct LspNode;
}

namespace phys::native {

// A def's key table (generated, def_keys.inc).
namespace defkeys {
enum class Tweak : uint8_t {
    Real,       // Tweak::CReal: one float
    Unsigned,   // CUnsigned / CInteger: one integer (signed in the file: Left -16384)
    Vector,     // CVector: three floats (w left)
    Flags,      // a flags class: "+NAME -NAME ..." over its bits
    Def,        // a nested def ("Key (sub-keys ...)")
};
struct Table;
struct Key {
    const char* name;
    Tweak tweak;
    uint32_t offset = 0;                                                    // its Tweak in the game's def (for the checks)
    void (*set)(void* def, const float* v, const uint32_t* u) = nullptr;   // a value's (null: not kept)
    const void* (*get)(const void* def) = nullptr;                          // the kept member (a flags key's word too)
    uint8_t bytes = 0;                                                      // its size (1: a bool)
    uint32_t* (*bits)(void* def) = nullptr;                                 // a flags key's word (null: not kept)
    const char* const* flagNames = nullptr;
    uint32_t flagCount = 0;
    const Table* sub = nullptr;                                             // a nested def's keys
    void* (*subDef)(void* def) = nullptr;                                   // ... and where it is (null: not kept)
};
struct Table {
    const Key* keys;
    uint32_t count;
    const Key* Find(const std::string& name) const;   // the last one so named (a class's own over its parent's)
};
} // namespace defkeys

// physics.lsp's PhysicsEngine: what the world reads of it.
struct PhysicsConfig {
    float maxTimeStep = 0;        // MaxTimeStep: the step (0.007 s); also the resting contacts' friction factor (ContactSum
                                  // reads this slot)
    Vec4 gravity{};               // vecGravity
    uint32_t flags = 0;           // Flags (PhysicsFlags: model/physics.h)
    uint32_t debugFlags = 0;      // DbgFlags
    float brakeWallBuffer = 0, brakeWallFactor = 0;
    uint32_t forceMaterial = 0;   // ForceMaterial: every contact's material with the flag FORCE_MATERIAL
    MaterialTable materials;      // MaterialManager: by id (the key: its name)
};

// A stage object's def (the Solid class: kinds GENERIC .. WOODEN_FENCE_POLE).
struct SolidDef {
    RigidBodyDef body;
};

// A wheel's def (the Wheel class: CAR_WHEEL_LF .. RB): the body, the suspension's geometry, its spring-damper and
// tyre. The geometry as the files give it (the _VF points in the car body's frame, the strut's); the wheel's def proper
// takes them into the car's frame when the car is built.
struct WheelPartDef {
    RigidBodyDef body;
    WheelDef wheel;
    SpringDamperDef springDamper;
    TyreDef tyre;
    bool doSuspensionSweep = false;
    Vec4 jointMountVF{}, jointAxisVF{}, topMountVF{}, steeringMountVF{}, steeringAxisVF{}, driveAxleJointPosVF{};
    Vec4 topMountDeformUpAxisRF{}, topMountDeformSideAxisRF{};
    Vec4 strutHubAxis{};
    float strutDimZ = 0;
    // The setup's: the tyre type's number and the tyre's starting state (the type's vertical stiffness and tread
    // elasticity, the pressure).
    uint32_t tyreType = 0;
    TyreState tyreStart;
};

// The car's def (CAR_ROOT): the body, the car's own values and its systems' defs (most of whose values the setup gives).
struct CarRootDef {
    RigidBodyDef body;
    CarDef car;
    VcuDef controlUnit;
    DriveDef drive;
    EngineDef engine;
    // The setup's driving aids (the control unit's; the clutch help always with the automatic gearbox).
    uint32_t autoGears = 0, gearGuard = 0, neutralLock = 0, twoPedals = 0, clutchHelp = 0;
    float maxFFTorque = 0;         // the force feedback's
    uint32_t subTics = 0, setupSpec = 0, carId = 0;
};

// The car body's def (CAR_BODY): the centre of gravity the car's frame starts from.
struct CarBodyDef {
    Vec4 centerOfGravity{};
};

// A hinged part's def (the doors, the bonnet, the exhaust pipe).
struct HingedPartDef {
    RigidBodyDef body;
    HingeDef hinge;
    Vec4 zeroAxisRF{}, zeroAxisBF{};
    uint32_t unlocked = 0, bodyId = 0, breakChain = 0;
};

// A mounted part's def (the bumpers, the wing).
struct MountedPartDef {
    RigidBodyDef body;
    Vec4 basePoint{};              // vecBasePoint_PRF
    uint32_t bodyId = 0;
};

// The kinds (the game's name table, 0x7cb9e8).
constexpr uint32_t kKinds = 50;
extern const char* const kKindNames[kKinds];
int KindOf(const std::string& name);   // -1: none so named

struct Defs {
    PhysicsConfig physics;
    // By kind, of the class a file gave it (one of these).
    std::array<std::unique_ptr<SolidDef>, kKinds> solids;   // GENERIC .. WOODEN_FENCE_POLE (1 .. 32): every one (a kind no
                                                            // file names keeps the default)
    std::array<std::unique_ptr<WheelPartDef>, kKinds> wheels;
    std::array<std::unique_ptr<CarRootDef>, kKinds> cars;
    std::array<std::unique_ptr<CarBodyDef>, kKinds> carBodies;
    std::array<std::unique_ptr<HingedPartDef>, kKinds> hinged;
    std::array<std::unique_ptr<MountedPartDef>, kKinds> mounted;
    std::string classOf[kKinds];                             // (the class of each kind's def)

    // Kind k's def and its keys (null: none of that class).
    void* Of(uint32_t k, const defkeys::Table** keys) const;
};

struct DefsReport {
    uint32_t values = 0;                 // values set
    std::vector<std::string> unbound;    // keys no table knows ("class key")
    uint32_t dropped = 0;                // known keys the native defs do not keep
};

// physics.lsp into the defs (the config and its DefManager's classes). Returns "" or what is wrong.
std::string LoadPhysicsLsp(const std::string& text, Defs& out, DefsReport& report);
// A car's def file (its common.lsp, its surface's .lsp: their DefManager lists) over the defs, after physics.lsp.
std::string LoadDefsLsp(const std::string& text, Defs& out, DefsReport& report);

namespace defkeys {
extern const Table& kPhysicsConfigKeys;
}

} // namespace phys::native
