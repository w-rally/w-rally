#pragma once
// The engine, natively (M6 step 2; the classic CEngine, core/engine.cpp): a gas-dynamic model over the four-stroke cycle
// of 256 steps. Each cylinder's work is a curve over its phase, blended by its combustion; at its intake it takes a
// charge (the volumetric efficiency by the crank speed, the throttle's flow, the intake's pressure and temperature) and
// fires or misfires (the spark, the rev limiter, the head gasket). With a turbocharger the intake and exhaust are plenums
// with the compressor and turbine between them (their maps by the shaft's speed), the wastegate and the exhaust bypass.
// The drag (friction, the cylinders' wear, a hot block), the heat into the block and the coolant (the radiator cooled by
// the air through the car), the oil's wear and random misfires complete it.
//
// The drive steps it once a tick (Step: the crank speed in, the torque out). The classic code's arithmetic is kept bit
// for bit; the tables are the def's.
#include <cstdint>
#include <vector>

#include "num/math.h"
#include "num/x87.h"

namespace phys::native {

using num::Vec4;

// A point of a 16-point curve by the crank speed (rad/s).
struct CurvePoint {
    float x = 0, y = 0;
};

// A turbo map (the compressor's, the turbine's): curves by the shaft's speed (key), each points {key, y, z} by the
// pressure ratio; all of a map's curves have the same number of points.
struct GasCurvePoint {
    float key = 0, y = 0, z = 0;
};
struct GasCurve {
    float key = 0;
    std::vector<GasCurvePoint> points;
};
struct GasMap {
    std::vector<GasCurve> curves;   // at least two
};

// A cylinder's def.
struct CylinderDef {
    uint32_t phaseOffset = 0;       // its firing offset in the cycle
    float stiffness = 0, stiffnessWorn = 0;   // its stage of the series stiffness, new and worn
    float dragLimit = 0;            // the friction drag fades in above it
};

// CEngine::CDef (Turbo and the VE curve from the .lsp, the rest the game's constants).
struct EngineDef {
    float turboInertia = 0;         // the turbo shaft's
    float wastegateOpen = 0, wastegateFull = 0, wastegateFlow = 0;   // the intake's pressure over the charge pressure
    float exhaustBypassFlow = 0;    // the exhaust side's blow-off above the boost target
    GasMap compressorMap, turbineMap;
    float turboFriction = 0, turboFrictionQuadratic = 0;
    float intercoolerStiffness = 0, intercoolerStiffnessWorn = 0;
    CylinderDef cylinders[4];
    float cycle[2048] = {};         // the work curve: 256 steps x 8 coefficients; the last step's first three are the charge blends' factors
    float chargeCurves[18] = {};    // three quadratics in the charge density
    CurvePoint ve[16];              // the volumetric efficiency
    CurvePoint boostTarget[16];     // the charge pressure (the exhaust bypass opens above it)
    float cylinderVolume = 0;       // m^3
    float flowCurve[17] = {};       // the intake's flow factor over 0..2
    uint32_t cylinderCount = 0;
    float intakeArea = 0, intakeFlow = 0;
    uint32_t turbo = 0;             // the turbocharger and the plenums simulated
    float idleThrottle = 0;         // the eased throttle's minimum
    float bleedFlow = 0;            // the damaged intake's leak
    float block[2] = {};            // mass, specific heat
    float coolantTransfer = 0, coolantTransferArea = 0;   // block to coolant
    float coolant[4] = {};          // the fluid: specific heat, latent heat, -, boiling point
    float engineCoolantCapacity = 0;
    float circulationTemperature = 0, circulationRate = 0;   // the engine side circulates above it
    float radiatorCapacity = 0;
    float leakLevels[2] = {};       // the damage-blended level above which the radiator leaks (also its fill range)
    float radiatorTransfer = 0, radiatorBase = 0, radiatorAirFactor = 0;
    Vec4 radiatorAirAxis{};         // the air flow through it: the car's velocity in its frame along this
    float wearScale = 0;
    float frictionFactor = 0;       // the friction per crank speed
    float dragFade = 0, hotDragFactor = 0, hotTemperature = 0;
};

// A cylinder's damage.
struct CylinderDamage {
    float headGasket = 0;           // its wear (a worn one misfires and fails)
    float stiffnessWear = 0;        // new to worn
    float spark = 0;                // non-zero: it can fire
    float dragWear = 0;
};

// The engine's damage (the car damage handler's record).
struct EngineDamage {
    uint32_t* random = nullptr;     // the handler's random seed
    uint32_t enabled = 0;           // the head gaskets wear
    CylinderDamage cylinders[4];
    uint32_t waterPump = 0;         // the coolant circulates
    float radiator = 0;             // its damage: the coolant leaks
    float oil = 0;                  // the oil left
    float oilPump = 0, oilCooler = 0;
    float intakeLeak = 0;
    float turboWear = 0;
    uint32_t headGasket = 0;        // intact: the engine runs
};

// A cylinder's state; its floats are the work curve's arguments (EntryEval's p[0..11]) - the layout is kept.
struct Cylinder {
    uint32_t phase = 0;             // its step in the cycle
    float work = 0;                 // the cycle's work up to its phase
    float blend[4] = {};            // the combustion: [0] 1 not burning, [1..3] the weights
    float chargeMass = 0;
    float burn = 0;                 // the charge's quadratics blended
    float unknown20 = 0;
    float chargePressure = 0, exhaustPressure = 0, ambientPressure = 0;
    float stiffness = 0, stiffnessShare = 0;   // its stage of the series stiffness
};

// The engine-noise signal's record (CSigEngineNoise).
struct EngineNoise {
    float speed = 0, throttle = 0, turboSpeed = 0, boost = 0, misfire = 0;
    float coolantLeak = 0, wastegateLoss = 0, wastegateOpening = 0;
    uint32_t fired[4] = {};         // each cylinder this cycle
};

// CEngine::CState.
struct EngineState {
    float misfireLoad = 0, misfireTimer = 0, misfireNext = 0;   // what builds a random misfire
    float radiatorCoolant[2] = {};  // amount, temperature
    float engineCoolant[2] = {};
    EngineNoise noise;              // (also the state's: the boost, the misfire, the leak, the wastegate, the cylinders fired)
    float turboSpeed = 0;           // rad/s
    float intake[4] = {};           // the plenum after the compressor: amount, temperature, pressure, volume
    float exhaust[4] = {};          // before the turbine
    float intercoolerStiffness = 0, intercoolerStiffnessShare = 0;
    Cylinder cylinders[4];
    uint32_t sparks[4] = {};        // each cylinder may fire (the control unit's rev limiter)
    float crankAngle = 0;           // 0..4 pi
    uint32_t step = 0, previousStep = 0;   // of 256 over the cycle
    float crankSpeed = 0;           // rad/s
    float smoothedSpeed = 0;        // 0.3 new + 0.7 old
    float intakeTemperature = 0, intakePressure = 0;
    float chargePressure = 0;       // after the throttle
    float spentChargeTemperature = 0, chargeDensity = 0;
    float compressorFlow = 0, turbineFlow = 0;   // filtered
    float torque = 0, power = 0;    // filtered
    uint32_t tics = 0;
    float heat = 0;                 // from combustion and friction since the heat step
    float blockTemperature = 0;     // K
    float friction = 0;             // |frictionFactor * smoothedSpeed|
    float wearLoad = 0;
    float stiffness = 0;            // the stages in series
    float dragTorque = 0;
};

// What the engine needs from its car and the game.
struct EngineHooks {
    void (*bodyVelocity)(void* ctx, Vec4& out) = nullptr;           // the car's velocity in its frame
    void (*noise)(void* ctx, const EngineNoise& record) = nullptr;  // the engine-noise signal
    void (*telemetry)(void* ctx, float torque, float power) = nullptr;   // the recorders (RENDER_ENGINE_TORQUE)
    void* ctx = nullptr;
    float* gasketFactors = nullptr; // the game's per-cylinder factors the head gaskets scale (0x7ca668)
    bool torqueTelemetry = false;   // the config's RENDER_ENGINE_TORQUE
};

class Engine {
public:
    const EngineDef* def = nullptr;
    EngineState* state = nullptr;
    EngineDamage* damage = nullptr;
    EngineHooks hooks;

    float throttle = 0;             // the control unit's
    float easedThrottle = 0;        // cosine-eased above idleThrottle
    float stepAngle = 0;            // the crank's turn this tick
    float coolantPumped = 0;
    float radiatorHeatRate = 0, combustionHeatRate = 0, coolantHeatRate = 0;   // per second
    float friction = 0;
    float misfire = 0;
    float boilOffRate = 0;

    // The tick (EngineStep, 0x518e20): the crank speed w in, the torque out.
    num::X87 Step(float dt, float w);

private:
    float Random();
    void CosineEase();
    num::X87 RpmBandFactor() const;
    void BlendedQuadratic(float x, Cylinder& c) const;
    num::X87 EntryEval(uint32_t index, const Cylinder& c) const;
    void CyclePhase(float da, int32_t* steps, float* angle);
    void RandomEvent(float dt);
    num::X87 DragTorque(float k);
    num::X87 AbsSpeedTerm();
    void SeriesStiffness();
    void GasFlow(float x0, float x, float s, const GasMap& map, float k, float* P, float* out);
    void GasOutflow(float x0, float x, const GasMap& map, float k, float* P, float* out);
    void HeadGasketWear(uint32_t i);
    void CylinderCharge(uint32_t i);
    void GasStep(float dt);
    void WearStep(float k);
    void CoolantStep(float dt);
    num::X87 CylinderAdvance(uint32_t i, uint32_t d);
    void HeatStep(float dt);
};

} // namespace phys::native
