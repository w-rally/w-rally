#pragma once
// A wheel of the car, natively (M6 step 2; the classic CWheel, core/wheel.cpp): a solid hung on its suspension, with its
// tyre and its spring-damper. Now: the suspension's kinematics (SuspensionUpdate: the top mount as the damage deformed it,
// the axle's inclination and the bent rim's wobble, the wishbone swinging about its joint to reach the strut, the steering
// rod turning the knuckle - the hub's pose and the axes the wheel's forces use); the wheel's step, the ground probe and the
// car's links wait.
#include <cstdint>

#include "native/car_solids.h"
#include "native/stage_collision.h"
#include "native/suspension.h"
#include "native/tyre.h"

namespace phys::native {

// CWheel::CDef's suspension geometry: the .lsp's values and the points and axes the setup put in the car's frame.
struct WheelDef {
    bool isSteered = false;
    float steeringRodLength = 0, wishboneLength = 0;   // the def's
    float steeringSign = 0, wishboneSign = 0;
    float strutDimX = 0, strutDimY = 0;                // the strut's offset and height (the setup's ride height in it)
    float springOffset = 0, driveAxleMountOffset = 0;
    uint32_t antiRollBarId = 0, antiRollBarSlot = 0;
    Vec4 strutRodMount{}, strutHubPoint{};             // the strut's frame
    Vec4 jointMount{}, jointAxis{}, topMount{}, steeringMount{}, steeringAxis{}, driveAxleJointPos{};   // the car's frame
    Vec4 topMountDeformUpAxis{}, topMountDeformSideAxis{};
    float axisInclination = 0;                         // the setup's WheelAxisInclination
    float topMountMaxDeformSide = 0, topMountMaxDeformUp = 0;
    float wearLoadStart = 0, wearLoadFull = 0;         // the loads that start / complete the top mount's wear
    float inclinationPerDamage = 0;
    float steerReduceStart = 0, steerReduceFull = 0;   // the side loads that knock the inclination out
};

// A wheel's damage (the car damage handler's record).
struct WheelDamage {
    float topMountUp = 0, topMountSide = 0;   // the top mount pushed up / sideways (0 .. 1)
    float rodBend = 0;                        // the steering rod bent (0.015 m shorter per unit)
    float controlArm = 0;
    float inclination = 0;                    // the axis inclination knocked out (-1 .. 1)
    float sectorsA[4] = {}, sectorsB[4] = {}; // per quarter of the tyre
    float loadFactor = 0;                     // how the loads wear the suspension
};

// The suspension's state: its own pose (a solid state's: the rotation updated with the wheel's) and the geometry's.
struct WheelSuspension {
    RigidBody pose;            // (only its orientation, scale and rotation)
    Vec4 topMount{};           // the def's, deformed by the damage
    float spinAngle = 0;       // radians, wrapped by 20 pi
    float travel = 0, travelRate = 0;
    float steeringRodLength = 0, wishboneLength = 0;
    Vec4 axle{};               // the spin axis: axisRow0 with the bent rim's wobble
    Vec4 axisRow0{}, axisRow1{};   // (cos a, 0, sin a), (-sin a, 0, cos a): the axle's inclination
};

// One of the wheel's three ground rays (the rim's two edges and the centre: the classic WheelRay).
struct WheelRay {
    uint32_t hit = 0;              // the ray hit the ground or an object's mesh
    uint32_t valid = 0;            // a usable contact (SurfaceContact)
    GroundHit ground{};            // the hit
    float pointW = 0;              // (the classic contact's word after the point: an object mesh hit's fourth lane)
    uint32_t after = 0;            // (the word after the classic contact)
    float lateral = 0;             // its offset across the tyre (-0.1, 0, 0.1)
    float surfaceOffset = 0;       // the surface below the ray's ground: the ground offset and the bumps (SurfaceBlend)
    float soilOffset = 0;          // the soil's bottom
    float gain = 0;
    float unknown060 = 0;
    float depth = 0;               // how far down the ground is
};

// The wheel's working values for its contact and its forces (the classic WheelScratch's names).
struct WheelContact {
    Vec4 rimA{}, hub{}, rimB{};    // the rays' origins: the hub and the rim's edges along the axle
    Vec4 hubAxisWorld{}, down{}, rollDir{};   // the rays along down; rollDir = hubAxisWorld x down
    float groundDistance = 0;      // from the hub along down
    TyreSurface surface;           // the surface's values at the contact, blended (WheelSurfaceBlend)
    uint32_t surfaceA = 0, surfaceB = 0;
    float surfaceBlend = 0;
    WheelRay rayCentre, rayA, rayB;
    float sectorBlendA = 0, sectorBlendB = 0;   // the damage's sectors at the spin angle (the outer rays' softness)
    float quadDepth = 0;           // a quad object's depth into the tyre
    Entity* movingGround = nullptr;   // a solid the centre ray hit: the contact relative to its velocity
    Vec4 centreRayFrom{}, centreRayTo{}, centreRayDir{}, contactPoint{};   // for display
    Vec4 rimBShown{}, rayBHit{}, rimAShown{}, rayAHit{};
    float spinRate = 0;            // rad/s, the car's drive slot's
    Vec4 contactVelocity{};        // the ground point's velocity (the car's v + w x r, + the suspension's rate)
    Vec4 carSpin{};                // the car's angular velocity in its frame, + the steering's turn about linkAxis
    Vec4 force{}, torque{};        // for the car (its accumulators, PrepareForces)
    uint32_t flushed = 0;          // (cleared with them)
    Vec4 pushOffset{};             // (WheelContactPush)
    float shownRollBar = 0, shownSpring = 0, shownDamper = 0, shownLateral = 0;   // the step's forces, for display
    Vec4 springAxis{}, linkAxis{}, linkPivot{};   // the strut's direction, the car's frame
    float inverseArmA = 0, inverseArmB = 0;       // 0 unless both arms are longer than 0.001
    Vec4 driveAxleDir{}, hubAxis{};               // the drive axle's direction, the hub's axis (the car's frame)
    Vec4 debugAxes[3] = {};                       // the hub's axes (RENDER_SUSPENSION_GEOMETRY)
    Vec4 strutTop{}, strutVector{};
};

class Wheel : public Solid {
public:
    const WheelDef* wheelDef = nullptr;
    Car* car = nullptr;        // the car it is on
    uint32_t slot = 0;         // its index among the car's wheels (lf rf lb rb)
    uint32_t onGround = 0;
    float shownSpinRate = 0;
    WheelSuspension suspension;
    WheelDamage* damage = nullptr;
    WheelContact contact;
    uint32_t damageChanged = 0;
    Tyre tyre;
    SpringDamper springDamper;

    // The tick's start (WheelVslot8, 0x539090): its velocities zeroed, its pose's dependants, its children's.
    void TickBegin(World& w, float dt) override;
    // Its force and torque from the last step handed to the car (WheelFlushForces, vslot 23: no gravity of its own).
    void PrepareForces(World& w) override;
    // (Its forces and its step are a solid's: SolidForces, WheelSolidStep's SolidStep.)
    void StepChildren(World& w, float dt) override { SuspensionStep(w, dt); }   // WheelVslot22: vslot 96
    // The wheel's step (WheelStep, vslot 96 at 0x53dd30): the travel clamped, the suspension's kinematics, the rays' frame
    // from the car, the ground probe; in contact the tyre's step and the rays' push, their forces into the car's frame; the
    // spring-damper and the roll bar, the new travel and its rate, the forces and torques for the car, the steering's
    // torque, the damage the loads do, the drive slot's torques, the roll bar's travel, the spin angle, the tyre's heat.
    void SuspensionStep(World& w, float dt);
    // The car's spin as the wheel turns with it (WheelBodySpin, 0x53aef0) and the gyroscopic torque of its spin
    // (WheelSpinCross, 0x53afb0).
    void BodySpin();
    void SpinCross(Vec4& out);
    // The rays' push against the ground they sank into (WheelContactForces, 0x5397a0; WheelContactPush, 0x5394d0).
    bool ContactForces(Vec4& out);
    void Notify() override   // WheelVslot16: the solid's, then the suspension state's rotation
    {
        Solid::Notify();
        suspension.pose.UpdateRotation();
    }

    // The suspension's kinematics at the travel a1 (WheelSuspensionUpdate, 0x53b900; the car's steering angle for a steered
    // wheel): the hub's pose and the contact's axes (SuspensionKinematics), then Notify.
    void SuspensionUpdate(float a1, float steeringAngle, bool renderGeometry)
    {
        SuspensionKinematics(a1, steeringAngle, renderGeometry);
        Notify();
    }
    void SuspensionKinematics(float a1, float steeringAngle, bool renderGeometry);
    // The top mount as the damage deformed it (TwoAxisMulAdd, 0x53ab90).
    void TopMountDeform();
    // The axle from the inclination rows and the bent rim's wobble (AngleOffset, 0x53a9e0).
    void AxleWobble();
    // The inclination rows from the def and the damage (WheelSteerRows, 0x53ab00).
    void SteerRows();
    // A side load x knocking the inclination out (WheelSteerReduce, 0x53ac50).
    void SteerReduce(float x);
    // The ground under the wheel (WheelGroundProbe, 0x53d520): three rays along down from the hub and the rim's edges against
    // the stage's ground and the objects' meshes near the hub; false without a usable contact, else the contact point and the
    // ground's normal (w: its plane), the distance, the surface blended.
    bool GroundProbe(World& w, Vec4& point, Vec4& normal);
    // The rays' origins at the rim's edges (MirrorOffsets, 0x539180): base -/+ half the carcass's width along dir.
    void MirrorOffsets(const Vec4& dir, const Vec4& base, Vec4& o1, Vec4& o2);
    // The surface under the patch: the materials a and b blended by u through the tyre's pair table (WheelSurfaceBlend, 0x539900).
    void SurfaceBlend(const World& w, uint32_t a, uint32_t b, float u);
    // A ray's ground: its surface's offsets (SurfaceBlend, 0x53d230) and its depth to the soil's bottom, valid within lo,
    // capped at hi (SurfaceContact, 0x53d460).
    void RaySurface(const World& w, WheelRay& r, float lo, float hi) const;

private:
    uint32_t RingSector(uint32_t* out, int32_t n) const;
    void RingTableBlend();
    void ContactAxis(const WheelRay& a, const WheelRay& b, Vec4& out) const;   // WheelContactAxis (0x539290)
    void ContactPush(WheelRay& r, const float* p, Vec4& out);
};

} // namespace phys::native
