#pragma once
// A rigid body, natively (M6 step 2; the classic CSolid, core/solid.cpp): the car's chassis, its wheels and parts, the
// stage's movable objects. Its mass properties and sleep rules (RigidBodyDef), what it is now (the orientation, position,
// velocities and their accelerations) and the step's working data (the state saved at the step's start, the forces and
// torques gathered, the accelerations from them).
//
// Only the body's own kinematics live here. What the world does around a body's step - its parent's frame, re-filing it
// in the object grid, the car's distance along the drive line, the children, the signals - is the world's (it calls the
// steps here in the classic order). The arithmetic is the classic code's, bit for bit (tools/native_check.cpp).
#include <cstdint>

#include "num/math.h"
#include "num/x87.h"

namespace phys::native {

using num::Mat33;
using num::Vec4;

// CSolid::CDef's values (the .lsp's).
struct RigidBodyDef {
    Vec4 localInverseInertia;          // vecLocalInverseInertiaDiagonal (the body's axes)
    float inverseMass = 0;             // InverseMass
    float angularThresholdVelocity = 0, linearThresholdVelocity = 0;   // below both: slow (SlowTest)
    bool sleepable = false;            // Sleepable
    float sleepDelay = 0;              // seconds slow (and in contact) before it sleeps
    float linearSleepVelocity = 0, angularSleepVelocity = 0;
    bool verySoft = false;             // VerySoft: the wheels' rays pass it
};

class RigidBody {
public:
    const RigidBodyDef* def = nullptr;

    // What the body is now.
    Vec4 orientation;              // a quaternion (x, y, z, w)
    Mat33 rotation;                // from it (UpdateRotation), rows; scaled by `scale`
    float rotationTail = 0;        // the float after the matrix: a 4-wide load of its last row reads it (RotateT4's w)
    Vec4 position;                 // in the parent's frame (the world's for most)
    float scale[3] = {1, 1, 1};    // row scales of `rotation`
    float scaleOn = 0;             // 1 when a scale is not exactly 1
    Vec4 angularVelocity;          // rad/s, the world's axes
    Vec4 velocity;                 // m/s
    Vec4 previousAngularVelocity, previousVelocity;              // the last step's
    Vec4 measuredAcceleration, measuredAngularAcceleration;      // (v - v') / dt
    Vec4 acceleration, angularAcceleration;                      // measured, smoothed (0.2 old + 0.8 new)
    float sleepTimer = 0;          // seconds slow (SleepCheck)
    bool inContactGroup = false;   // a body sleeps only in a contact group

    // The step's working data.
    float accumulateScale = 1;     // the force and torque accumulators' factor
    float blend = 0;               // how far the state has been stepped (BlendState / AdvanceRest; 1 done)
    Vec4 savedOrientation, savedPosition, savedVelocity, savedAngularVelocity;   // the state at the step's start
    Vec4 torque, force;            // gathered, the body's axes
    Vec4 stepAngularAcceleration, stepAcceleration;   // from them, the world's axes
    bool backwards = false;        // the last step went backwards (dt < 0)

    // Points and vectors between the body's frame and its parent's.
    Vec4* Rotate3(Vec4* v, uint32_t n) const;            // by `rotation` (x, y, z)
    Vec4* RotateT4(Vec4* v, uint32_t n) const;           // by `rotation`'s rows loaded 4 wide (w: rotationTail's lane)
    Vec4* TransformPoints4(Vec4* v, uint32_t n) const;   // rotated, plus the position
    Vec4* TransformPlanes4(Vec4* v, uint32_t n) const;   // planes (normal, d) into the parent's frame
    Vec4* WorldToLocal4(Vec4* v, uint32_t n) const;      // less the position, then rotated
    // The same through the orientation saved at the step's start (the matrix rebuilt per point, as the original).
    Vec4* RotateDirs4(Vec4* v, uint32_t n) const;
    Vec4* LocalToWorld4(Vec4* v, uint32_t n) const;
    Vec4* WorldToBody4(Vec4* v, uint32_t n) const;
    Vec4* WorldToBodyDirs4(Vec4* v, uint32_t n) const;

    void PointVelocity(const Vec4& p, Vec4& out) const;  // the velocity of the body's point at p
    void PointAccel(const Vec4& p, Vec4& out) const;     // its acceleration (the smoothed ones)
    bool SlowTest() const;                               // below both threshold velocities
    // A contact's response terms (r the lever, n the normal, d the other direction).
    num::X87 AngularResponse(const Vec4& r, const Vec4& n, const float* d) const;
    void ImpulseResponse(const Vec4& r, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const;

    void AccumulateTorque(const Vec4& v);                // the body's axes, scaled by accumulateScale
    void AccumulateForce(const Vec4& v);
    void SaveState();                                    // the step's start (after the children's)
    void UpdateRotation();                               // `rotation` from the orientation and the scale

    // One integration step of the pose (dt == 0 or NaN: nothing, false). The world then re-files the body.
    bool Integrate(float dt, bool translate, bool rotate);
    // The body's step: the velocities from the step's accelerations, a still car frozen (freezable), Integrate, the
    // accelerations measured and smoothed. The world then steps the children and calls SleepCheck.
    void Step(float dt, bool freezable);
    // Slow long enough in a contact group: the velocities zeroed and true (the world puts the body to sleep).
    bool SleepCheck(float dt);
    // The state blended back toward the saved one at a contact's time f, and the rest of a partial step: false when
    // there is nothing to do; else the world re-files the body.
    bool BlendState(float f);
    bool AdvanceRest(float dt);
    // The velocity into n cancelled (the brake wall), limited as the original.
    void CancelNormalVelocity(const Vec4& n, float a, float cap, float offset, float k);
};

} // namespace phys::native
