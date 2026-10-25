#pragma once
// A wheel's tyre, natively (M6 step 2; the classic CTyre, core/tyre.cpp): the contact patch from the carcass's
// deflection (and the ground's own give), the slip and its force curves, the friction circle, the forces and torques on
// the wheel, the heat of the tread, carcass and gas, and the tread's wear.
//
// The tyre is stepped once a tick by its wheel (Step: the wheel hands it the contact's frame and velocity, the surface
// blend under it and the hub's height); between steps it keeps its state (the temperatures, the gas, the carcass's
// deflection) and a few smoothed values. The classic code's arithmetic is kept bit for bit (the x87 parts too); the
// tables are the def's. The records it fills (TyreRecord, TireParticles) are the game's - their layout is kept.
#include <cstdint>

#include "num/math.h"
#include "num/x87.h"
#include "native/records.h"

namespace phys::native {

using num::Bracket;
using num::Vec4;

// The tyre's definition (CTyre::CDef; the .lsp's and the tyre type's values, the thermal constants and the tables).
struct TyreDef {
    uint32_t debugId = 0;
    float pressureStiffness = 0, referenceFriction = 0;
    float carcassStiffnessLateral = 0, carcassStiffnessTorsional = 0;
    float hubOffset = 0, carcassRadius = 0, treadDepth = 0, carcassThickness = 0;
    float treadWidth = 0, treadCoverage = 0, carcassWidth = 0, sideWallHeight = 0;
    float treadHeatCapacity[3] = {}, carcassHeatCapacity[3] = {}, gasHeatCapacity[3] = {};   // AddDivProduct's {c, m, -}
    float pressureBreaks[4] = {}, stiffnessFactor[4] = {};
    float workBreaks[6] = {}, heatByWork[6] = {};
    float temperatureBreaks[8] = {}, rubberFriction[8] = {}, rubberWear[8] = {};
    float loadBreaks[8] = {}, cornerStiffness[8] = {}, tractionStiffness[8] = {}, cornerFriction[8] = {}, tractionFriction[8] = {};
    float peakSlipCorner[8] = {}, peakSlipTraction[8] = {};
    float surfaceFriction[6] = {}, wetnessDesign = 0, wetSlope[6] = {}, wearFactor[6] = {}, wetnessWearFactor = 0;
};

// The surface under the patch: the wheel's material blend (the values CopyXyzFields takes), its frictions then set by the
// ground's give and the tyre's grip.
struct TyreSurface {
    float kineticFriction = 0, staticFriction = 0, unknown02c = 0, solidGroundOffset = 0, unknown01c = 0;
    float soilDensity = 0, sinkFactor = 0, soilThickness = 0, soilParticleMass = 0, rollingResistance = 0;
    float staticSoilFriction = 0, kineticSoilFriction = 0, waterFactor = 0;
};

// What lasts between steps (the wheel's tyre block).
struct TyreState {
    uint32_t stepped = 0;           // stepped in contact
    uint32_t element = 0;           // the element being heated (HeatTick: & 7)
    uint32_t started = 0;
    Vec4 restPoint{};               // the contact point out of contact
    float deflectionLateral = 0, deflectionTorsional = 0;   // the carcass's
    float gap = 0;                  // above the ground (GapAboveRest)
    float rollingRadius = 0;
    float carcassStiffnessVertical = 0, treadElasticity = 0;   // the tyre type's
    float workBlend = 0;
    uint32_t frictionLatched = 0;   // the friction circle was exceeded
    float gasMass = 0, gasTemperature = 0, pressure = 0, gasVolume = 0;
    uint32_t regimeA = 0, regimeB = 0;   // the contact solver's regime
    Vec4 heading{};                 // the patch's heading (AlignDirection)
    Bracket pressureBracket{}, loadBracket{}, treadTemperatureBracket{}, carcassTemperatureBracket{}, workBracket{};
    float elementTemperatures[24] = {};   // K, per element (8) {inner, tread, outer}
    float carcassTemperature = 0, innerTemperature = 0;
    uint32_t contactElement = 0;    // the element on the ground
};

// One step's inputs from the wheel (the classic TyreStepArgs).
struct TyreStepInput {
    float spinRate = 0;             // rad/s
    float dt = 0;
    float hubHeight = 0;            // the hub above the ground
    float quadDepth = 0;            // a quad object's depth into it
    const TyreSurface* surface = nullptr;   // the material blend under the patch
    const float* contact = nullptr; // the surface entries {a, b, blend} (the record's)
    Vec4 normal{}, axle{};          // the ground's normal and the hub's axis (the patch's frame)
    Vec4 velocity{};                // the patch's velocity
    Vec4 point{};                   // the contact point
    float spinAngle = 0;            // the wheel's (the element on the ground)
    bool renderMaterial = false;    // the config's RENDER_MATERIAL (the contact's surface entries kept)
};

class Tyre {
public:
    const TyreDef* def = nullptr;
    TyreState* state = nullptr;
    float* wear = nullptr;          // per element (the damage handler's)

    // The patch's frame and the step's working values (the classic CTyre's names).
    Vec4 contactPoint{}, lateral{}, forward{}, normal{};
    TyreSurface surface;
    float lateralGripScale = 0, heightRatio = 0, contactStiffness = 0, contactLength = 0, specificWork = 0;
    float load = 0, loadSolved = 0, loadGround = 0, workPerLoad = 0, groundShare = 0;
    float solver[7] = {};
    float work = 0, workLateral = 0, workTorsional = 0, workSlip = 0, workBrake = 0, workDrive = 0;
    float slipSpeed = 0, slipSpeed2 = 0, velocityLateral = 0, velocityForward = 0, velocityNormal = 0, slipVelocity = 0;
    float spinRate = 0, slipRatio = 0, unknown380 = 0, unknown384 = 0, slipCurve = 0;
    float cornerStiffness = 0, tractionStiffness = 0, stiffnessLateral = 0, stiffnessTorsional = 0;
    float forceLateral = 0, forceForward = 0, torqueAlign = 0;
    float hubHeight = 0, normalDotAxle = 0, camberCos = 0, pressureFactor = 0, rollingTorque = 0, radius = 0;
    uint32_t element = 0;
    float heatFactor = 0, rollingResistanceTorque = 0, unknown3cc = 0, unknown3d0 = 0, unknown3d4 = 0, quadDepth = 0;
    float gripBlend = 0, slipCurveLong = 0, slipCurveLat = 0, deflectionRateLateral = 0, deflectionRateTorsional = 0;
    float slideBlend = 0, slideMarkLong = 0, slideMarkLat = 0, slideLateral = 0, slideTorsional = 0, wearRate = 0;
    TyreRecord record{};     // for the game's tyre effects
    uint32_t inContact = 0, elementShown = 0;
    Vec4 contactShown{}, forceShown{};
    float workShown = 0, slipVelocityShown = 0, velocityForwardShown = 0, grip = 0, forceLong = 0, forceLat = 0;
    float contactSurface[3] = {};
    float heatIn = 0, heatPower = 0, heatShare = 0, heatExchange = 0, heatLoss = 0;
    float workSmoothed = 0, camberSmoothed = 0, patchSmoothed = 0;

    // One step (TyreStep, 0x5360c0): the patch, its forces F (set) and torque T (added to), the aligning and rolling
    // torques (align: their sum, drag: the slide's), the particles record.
    void Step(const TyreStepInput& in, Vec4& F, Vec4& T, float& align, float& drag, TireParticles& particles);
    // The heat's slow part (TyreHeatTick, 0x5358c0: the carcass, the gas and one tread element, then the next element).
    void HeatTick(float dt);
    // The gap above the ground at the hub height x (GapAboveRest, 0x533490).
    void GapAboveRest(float x);
    // The surface frictions of the tyre's pair table for the surface types i and j at the wetnesses x and y (PairTableScale,
    // 0x534e70): the def's, by the wet and the worn element on the ground.
    void PairTableScale(int32_t i, int32_t j, float x, float y, float* p1, float* p2) const;

private:
    bool AxisFrame(const Vec4& a, const Vec4& b);
    void AxisProject(const Vec4& v, float spin);
    bool ContactGeometry(float spinAngle, float* out);
    void ContactSolve(float* S, float* O, const float* P, uint32_t depth, int regime);
    void ContactDispatch(float* S, float* O, const float* P, uint32_t a, uint32_t b, uint32_t depth);
    void ForceWork();
    void RangeBlend();
    void ForceStep();
    void Force(float* out, Vec4* zero, float* roll, float* b1, float* b2);
    void CoefficientsTo(float a1, float a2, float a4, float a5, float* const* o) const;
    void UnitAbs2(float a, float b, float c, float* o1, float* o2, float* o3) const;
    void FrictionStep();
    void FrictionLimit(float load, float muLatched, float mu, float a, float b, float* o1, float* o2);
    void ApplyForces(float k, Vec4& F, Vec4& T);
    num::X87 AlignDirection(float k);
    num::X87 SignedProduct() const;
    void HeatInput(float a, float b, float k);
    void HeatBalance(float x);
    void HeatExchange(int32_t i, float x);
    void OutputRecord(TireParticles& P);
    void WheelRecord(const Vec4& a, const float* b, float x);
};

} // namespace phys::native
