#include "native/setup.h"

#include <cstring>

#include "num/f32.h"
#include "num/trig.h"

namespace phys::native {

using num::F32;

void SetupFromOptions(CarSetup& s, CarSetupOptions& o)
{
    for (GearChoice& g : o.drive.gearLists)   // 0x546f70: every list's ratio Gear / Pinion (0 for a 0 pinion)
        g.ratio = g.pinion ? static_cast<float>(g.gear) / static_cast<float>(g.pinion) : 0.0f;
    DriveSetup& d = s.drive;
    for (uint32_t k = 0; k < o.drive.gears; ++k) d.gears[k] = o.drive.gearLists[d.gearIds[k]];
    d.finalDrive = o.drive.gearLists[d.finalDriveId];
    d.dropGear = o.drive.gearLists[d.dropGearId];
    // (The brakes the setup names are copied into it too, but no setup function reads them.)
    for (uint32_t w = 0; w < 4; ++w)   // each wheel's top mount slot names its position
        s.wheels[w].topMountPosition = o.wheels[w].topMounts[static_cast<int32_t>(s.wheels[w].topMountSlot)];
}

void SetupAids(CarSetup& s, const uint32_t aids[4])
{
    VcuSetup& v = s.controlUnit;
    v.autoGears = aids[0], v.gearGuard = aids[1], v.neutralLock = aids[2], v.clutchHelp = aids[3];
}

void SetupTyres(CarSetup& s, const CarSetupOptions& o, uint32_t tyreType)
{
    for (TyreSetup& t : s.tyres) t.type = &o.tyreTypes[tyreType];
}

namespace {

// 0x508770: the gears, the final drive over the drop gear, the footbrake pressure and its front / rear shares.
void DriveApply(DriveDef& D, const DriveSetup& L)
{
    for (uint32_t j = 0; j < 3; ++j) D.diffs[j].maxTorque = L.diffMaxTorque[j];
    for (uint32_t k = 0; k < 10; ++k) D.gears[k] = {L.gears[k].ratio, L.gears[k].torqueMin, L.gears[k].torqueMax};
    D.finalRatio = (F32(L.finalDrive.ratio) / F32(L.dropGear.ratio)).f();
    const float front = L.maxBrakePressureFront, rear = L.maxBrakePressureRear;
    D.maxFootbrakePressure = (F32(rear) + F32(front)).f();
    float a = 0.0f, b = 0.0f;
    if (!(D.maxFootbrakePressure == 0.0f)) {   // (ucomiss: unordered counts as nonzero)
        const F32 q = F32(1.0f) / F32(D.maxFootbrakePressure);
        a = (q * F32(front)).f();
        b = (q * F32(rear)).f();
    }
    D.wheels[0].brakeRatio = a, D.wheels[1].brakeRatio = a;
    D.wheels[2].brakeRatio = b, D.wheels[3].brakeRatio = b;
}

// 0x537c90: the differentials' maps and the driving aids (the clutch help always with the automatic gearbox).
void VcuApply(CarRootDef& car, const VcuSetup& L)
{
    for (uint32_t m = 0; m < 4; ++m)
        for (uint32_t i = 0; i < 11; ++i) {
            DiffMap& d = car.controlUnit.maps[m];
            d.throttle[i] = L.maps[m].throttle[i], d.brake[i] = L.maps[m].brake[i];
            d.speed[i] = {L.maps[m].velocity[i], L.maps[m].factor[i]};
        }
    car.autoGears = L.autoGears, car.gearGuard = L.gearGuard, car.neutralLock = L.neutralLock;
    car.twoPedals = L.autoGears;
    car.clutchHelp = L.autoGears ? 1u : L.clutchHelp;
}

// 0x52dbd0
void SpringDamperApply(SpringDamperDef& d, const SpringDamperSetup& l)
{
    d.springLength = l.springLength, d.springStiffness = l.springStiffness;
    d.helperSpringMinLength = l.helperSpringMinLength, d.helperSpringLength = l.helperSpringLength;
    d.helperSpringStiffness = l.helperSpringStiffness;
    d.dampingBump = l.dampingBump, d.highSpeedDampingBump = l.dampingBumpHighSpeed;
    d.dampingRebound = l.dampingRebound, d.highSpeedBreakBump = l.bumpHighSpeedBreak;
}

// 0x534760: the tyre type into the tyre's def and its starting state.
void TyreApply(WheelPartDef& w, const TyreSetup& L)
{
    TyreDef& D = w.tyre;
    const TyreType& T = *L.type;
    w.tyreType = T.type;
    D.hubOffset = T.hubOffset, D.carcassWidth = T.carcassWidth, D.carcassRadius = T.carcassRadius, D.treadDepth = T.treadDepth;
    D.treadWidth = T.treadWidth, D.carcassThickness = T.carcassThickness, D.treadCoverage = T.treadCoverage;
    D.sideWallHeight = T.sideWallHeight, D.pressureStiffness = T.pressureRatio;
    D.carcassStiffnessLateral = T.carcassStiffnessLateral, D.carcassStiffnessTorsional = T.carcassStiffnessTorsional;
    TyreState& St = w.tyreStart;
    St.treadElasticity = T.treadElasticity, St.carcassStiffnessVertical = T.carcassStiffnessVertical;
    St.pressure = L.pressure;
    const uint32_t radius = 0x3ea147aeu;   // 0.315
    std::memcpy(&St.rollingRadius, &radius, 4);
    St.workBlend = D.treadCoverage;
    std::memcpy(D.rubberFriction, T.rubberFriction, sizeof D.rubberFriction);
    std::memcpy(D.rubberWear, T.rubberWear, sizeof D.rubberWear);
    std::memcpy(D.cornerStiffness, T.cornerStiffness, sizeof D.cornerStiffness);
    std::memcpy(D.tractionStiffness, T.tractionStiffness, sizeof D.tractionStiffness);
    std::memcpy(D.cornerFriction, T.cornerFriction, sizeof D.cornerFriction);
    std::memcpy(D.tractionFriction, T.tractionFriction, sizeof D.tractionFriction);
    std::memcpy(D.peakSlipCorner, T.peakSlipCorner, sizeof D.peakSlipCorner);
    std::memcpy(D.peakSlipTraction, T.peakSlipTraction, sizeof D.peakSlipTraction);
    std::memcpy(D.stiffnessFactor, T.stiffnessFactor, sizeof D.stiffnessFactor);
    D.wetnessDesign = T.wetnessDesign, D.wetnessWearFactor = T.wetnessWearFactor;
    std::memcpy(D.surfaceFriction, T.surfaceFriction, sizeof D.surfaceFriction);
    std::memcpy(D.wetSlope, T.wetSlope, sizeof D.wetSlope);
    std::memcpy(D.wearFactor, T.wearFactor, sizeof D.wearFactor);
}

// 0x53cf30: the spring-damper, the tyre, the steering rod, the top mount, the strut's platform and the axle's inclination.
void WheelApply(WheelPartDef& w, const WheelSetup& L, const SpringDamperSetup& sd, const TyreSetup& t)
{
    SpringDamperApply(w.springDamper, sd);
    TyreApply(w, t);
    WheelDef& d = w.wheel;
    d.steeringRodLength = L.steeringRodLength;
    w.topMountVF = L.topMountPosition;
    d.springOffset = L.strutPlatformHeight;
    d.strutDimY = (F32(w.strutDimZ) + F32(L.strutPlatformHeight)).f();   // the strut's height, the platform's added
    d.axisInclination = L.wheelAxisInclination;   // (the def's row (cos a, 0, sin a): not read natively)
}

} // namespace

std::string ApplySetup(const CarSetup& s, Defs& defs)
{
    const int root = KindOf("CAR_ROOT");
    CarRootDef* const car = root >= 0 ? defs.cars[root].get() : nullptr;
    if (!car) return "no CAR_ROOT def";
    DriveApply(car->drive, s.drive);
    VcuApply(*car, s.controlUnit);
    static const char* const kWheels[4] = {"CAR_WHEEL_LF", "CAR_WHEEL_RF", "CAR_WHEEL_LB", "CAR_WHEEL_RB"};
    for (uint32_t k = 0; k < 4; ++k) {
        WheelPartDef* const w = defs.wheels[KindOf(kWheels[k])].get();
        if (!w) return std::string("no ") + kWheels[k] + " def";
        if (!s.tyres[k].type) return "no tyre type";
        WheelApply(*w, s.wheels[k], s.springDampers[k], s.tyres[k]);
    }
    car->car.maxSteeringLock = s.car.maxSteeringLock;   // the Car list into the car's def
    car->car.antiRollBarStiffness[0] = s.car.frontRollBarStiffness;
    car->car.antiRollBarStiffness[1] = s.car.rearRollBarStiffness;
    return "";
}

} // namespace phys::native
