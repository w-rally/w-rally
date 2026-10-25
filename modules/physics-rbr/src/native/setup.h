#pragma once
// The car's setup, natively (M6 step 2, the load; the classic core/setup.cpp): the complete car setup the game sends
// the car (a setup file, Physics/<car>/setups/*.lsp) made on the setup screen from the car's options (r_<surface>.lsp
// and the tyre brand's tyres.lsp: the gear lists, the top mount slots, the tyre types), then applied to the car's defs
// by its parts' setup functions - the drive's gears, final drive and brake shares, the control unit's differential maps
// and driving aids, each wheel's geometry, spring-damper and tyre, the car's steering lock and roll bars.
//
// The containers are bound from the files through generated key tables (native/def_keys.inc), as the defs are.
#include <cstdint>
#include <string>

#include "num/math.h"
#include "native/defs.h"

namespace phys::native {

// A gear (an option's gear list, the setup's choice of one): its ratio Gear / Pinion and its torque limits.
struct GearChoice {
    float ratio = 0;
    int32_t pinion = 0, gear = 0;
    float torqueMin = 0, torqueMax = 0;
};

// A tyre type (the options' TyreType00 .. 06, from tyres.lsp).
struct TyreType {
    uint32_t type = 0;
    float rimRadius = 0, rimInnerRadius = 0, hubOffset = 0;
    float carcassStiffnessLateral = 0, carcassStiffnessTorsional = 0, carcassStiffnessVertical = 0;
    float treadElasticity = 0, pressureRatio = 0;
    float carcassRadius = 0, treadDepth = 0, carcassThickness = 0, treadWidth = 0, treadCoverage = 0, carcassWidth = 0;
    float sideWallHeight = 0;
    float surfaceFriction[6] = {}, wetnessDesign = 0, wetSlope[6] = {};
    float stiffnessFactor[4] = {};
    float rubberFriction[8] = {}, rubberWear[8] = {};
    float cornerStiffness[8] = {}, tractionStiffness[8] = {}, cornerFriction[8] = {}, tractionFriction[8] = {};
    float peakSlipCorner[8] = {}, peakSlipTraction[8] = {};
    float wearFactor[6] = {}, wetnessWearFactor = 0;
};

struct CarSetupCar {
    float maxSteeringLock = 0, frontRollBarStiffness = 0, rearRollBarStiffness = 0;
};
struct DriveSetup {
    float diffMaxTorque[3] = {};    // the centre's, the front's, the rear's
    GearChoice gears[10];           // the setup screen's: the options' lists GearId0.. name
    GearChoice finalDrive, dropGear;
    uint32_t gearIds[10] = {}, finalDriveId = 0, dropGearId = 0;
    float maxBrakePressureFront = 0, maxBrakePressureRear = 0;
};
// A differential's maps (the centre's, the centre's left-foot braking, the front's, the rear's).
struct VcuSetupMap {
    float throttle[11] = {}, brake[11] = {}, velocity[11] = {}, factor[11] = {};
};
struct VcuSetup {
    uint32_t autoGears = 0, gearGuard = 0, neutralLock = 0, clutchHelp = 0;   // the driving aids (the game's options)
    float centerDiffHandbrakeRelease = 0, leftFootBrakeThreshold = 0;
    VcuSetupMap maps[4];
};
struct WheelSetup {
    float topMountSlot = 0;         // the options' slot by its integer part
    Vec4 topMountPosition{};        // the slot's position (the setup screen's)
    float steeringRodLength = 0, strutPlatformHeight = 0, wheelAxisInclination = 0;
};
struct SpringDamperSetup {
    float springLength = 0, springStiffness = 0, helperSpringMinLength = 0, helperSpringLength = 0, helperSpringStiffness = 0;
    float dampingBump = 0, dampingRebound = 0, bumpHighSpeedBreak = 0, dampingBumpHighSpeed = 0;
};
struct TyreSetup {
    float pressure = 0;
    const TyreType* type = nullptr; // the tyre type chosen (the game's option)
};

// CCompleteCarSetup (the setup file's): the wheels LF, RF, LB, RB.
struct CarSetup {
    CarSetupCar car;
    DriveSetup drive;
    VcuSetup controlUnit;
    WheelSetup wheels[4];
    SpringDamperSetup springDampers[4];
    TyreSetup tyres[4];
};

struct WheelOptions {
    Vec4 topMounts[8];              // the top mount slots
};

struct DriveOptions {
    GearChoice gearLists[32];
    uint32_t gears = 0;             // how many gears the setup has
};

// CCompleteCarSetupOptions (r_<surface>.lsp and tyres.lsp): what the setup screen offers.
struct CarSetupOptions {
    DriveOptions drive;
    WheelOptions wheels[4];
    TyreType tyreTypes[7];
};

// The files into the containers (a file's first list; the options: r_<surface>.lsp, then tyres.lsp). "" or what is wrong.
std::string LoadSetup(const std::string& text, CarSetup& out, DefsReport& report);
std::string LoadSetupOptions(const std::string& text, CarSetupOptions& out, DefsReport& report);
void Defaults(CarSetup& s);
void Defaults(CarSetupOptions& o);

// The setup screen's derivations (SetupFromOptions): the gear lists' ratios, the setup's gears from the lists its ids
// name, each wheel's top mount position from its slot; then the game's driving aids (SetupAids: auto gears, gear guard,
// neutral lock, clutch help) and the tyre type (all four: the options' type `tyreType`).
void SetupFromOptions(CarSetup& s, CarSetupOptions& o);
void SetupAids(CarSetup& s, const uint32_t aids[4]);
void SetupTyres(CarSetup& s, const CarSetupOptions& o, uint32_t tyreType);

// The setup applied to the car's defs (CarApplySetup: the drive's, the control unit's, each wheel's - its spring-damper's
// and tyre's - and the car's steering lock and roll bars): CAR_ROOT's def and the wheels' (CAR_WHEEL_LF .. RB).
// Returns "" or what is missing.
std::string ApplySetup(const CarSetup& s, Defs& defs);

} // namespace phys::native
