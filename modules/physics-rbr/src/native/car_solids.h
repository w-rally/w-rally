#pragma once
// The car's solids, natively (CCar, its passengers, its hinged and mounted parts, its body; library rbr-car). The wheel is
// native/wheel.h; the car's models (drive, engine, control unit) native/drive.h, engine.h, vcu.h.
#include <array>
#include <cstdint>

#include "native/solid.h"
#include "native/vcu.h"

namespace phys::native {

// (A wheel, CWheel: native/wheel.h.)

// The car (CCar): its impulses take the car's own surface under the point.
class MountedSolid;
class Wheel;

// A passenger (CPassenger: the driver, the co-driver): the car's acceleration with gravity felt in its frame, the torso
// held about the seat by springs and stops, the head following the torso.
struct Passenger {
    Vec4 felt{};   // in the car's frame
    Vec4 headVelocity{}, torsoVelocity{}, headPosition{}, torsoPosition{};
    // PassengerStep (0x523160): felt from the car's measured acceleration, then SpringFollowStep and SpringStopStep.
    void Step(World& w, const Solid& car, float dt);
};

// The car's def (CCar::CDef: the .lsp's steering and roll bar keys).
struct CarDef {
    float maxSteeringLock = 0, steeringRackRatio = 0;
    float antiRollBarStiffness[2] = {};   // front, rear
    float frontArea = 0, dragCoefficient = 0;   // the aerodynamic drag's
    float effWheelRadius = 0;                    // the distance rolled per wheel turn
    float dangerousAcceleration = 0;             // a speed change per second over it ends the rally
};

// An anti-roll bar: its two wheels' travels in (their AntiRollBarSlot), the torque out (the wheels read it back).
struct RollBar {
    float travel[2] = {};
    float torque = 0;
};

// The car's state (the classic car state's own fields; its systems' states are theirs).
struct CarState {
    RollBar rollBars[2];        // the front, the rear
    float steeringTorque = 0;   // the steered wheels' torque about their steering axes (the wheels add to it; 0 each step)
    float steeringFeedback = 0; // the torque through the rack, blended by the speed
    float steering = 0;         // the input, slewed towards the controls' at most 20 per second
    float steeringAngle = 0;    // steering MaxSteeringLock SteeringRackRatio
    float steeringRate = 0;     // its change per second
    float speed = 0;            // |velocity|
    float assistanceTime = 0;   // the outside assistance's countdown: the car held
    float brakeWall = 0;        // how far into a brake wall (0 off one): the speed drag
    Vec4 feltAcceleration{};    // the step's acceleration in the car's frame, smoothed (0.7 old + 0.3 new)
    float distanceRolled = 0;   // the drive's wheel turn times EffWheelRadius, summed
    float forwardSpeed = 0;     // along the car's -y
    float travelSpeed = 0;      // |velocity|
    Vec4 travelDirection{};     // of the velocity, negated (0 below 0.001 m/s)
    uint32_t overDangerousAcceleration = 0;   // a step's speed change over DangerousAcceleration
    float resetTimer = 0;       // seconds to the car put back on the drive line (0 none)
};

struct ControlInput;
class Car : public Solid {
public:
    const CarDef* carDef = nullptr;
    CarState carState;
    float controlSteering = 0;  // the controls' steering (the target)
    Drive drive;                // its def, state and damage the owner's
    Engine engine;
    Vcu controlUnit;
    uint32_t extraStep = 0;     // the classic's extra step (0x4e9360; never on in the recorded runs)
    uint32_t feedbackOn = 0;    // the g-g diagram's force feedback started
    std::array<MountedSolid*, 4> mounted{};   // the mounted parts (the exhaust)
    std::array<Wheel*, 4> wheels{};           // lf rf lb rb
    const Body* carBody = nullptr;            // the car body (the hinges' frame)
    Passenger driver;
    float driveLineDistance = 0;   // its parameter along the stage's drive line
    float brakeWallTime = 0;       // seconds against a brake wall
    float fewWheelsTime = 0;       // seconds with fewer than 3 wheels on the ground
    uint32_t meshPlaced = 0;    // its mesh moved onto the car once (CarOnceInit)
    // The tick's start (CarVslot8, 0x4e72d0): the mesh's one-time placement, then the children's.
    void TickBegin(World& w, float dt) override;
    // (Its force preparation is a solid's: CarVslot23 = SolidVslot23, the children's then its gravity.)
    // The car's forces (CarForces, CCar vslot 25): the aerodynamic drag, a brake wall's drag, the gyroscopic torque, the
    // solid's (its children's, the contacts', the damping objects'), the felt acceleration, the g-g feedback; a loose
    // mounted part's force.
    void Forces(World& w) override;
    void AeroDrag(World& w);    // CarAeroDrag (0x4e7dc0)
    void SpeedDrag(World& w);   // CarSpeedDrag (0x4e7ad0)
    // Held (CarResetTimer, vslot 38 at 0x4e6dd0): the reset timer counts down; at 0 the car put back on the drive line -
    // its point at the car's parameter, the ground under it (10 m up and down) or the point itself, facing back along the
    // line (PlaceAt) -, in neutral (GearSet 1), its state saved (vslot 18).
    void HeldTick(World& w, float dt) override;
    // The game's rules on the car (core/rules.h; the standalone game's: no replay playing, the options' assistance).
    // Retired (CarRetire, 0x4e8fa0: a damping contact of material 200 - deep water): outside assistance as allowed (1,
    // 0.5, 4), else (0, 30, 4). (Its CSigRetireCar is the game's.)
    void Retire(World& w);
    // Outside assistance (CarOutsideAssistance, 0x4e8df0): the reset timer t1, the assistance's countdown t2 (the car held
    // by it: the controls' own record); asked for: put back turned over when it was on its roof or side (fewWheelsTime > 1
    // s); then held (vslot 86), a control record of zeros, the steering feedback 0. (CSigOutsideAssistance the game's.)
    void OutsideAssistance(World& w, bool request, float t1, float t2);
    // May the car get outside assistance (0x4e7730)? out as the original's: held - 0, no; moving (> 1.5 m/s) - 4, no; the
    // game's flag - 0, yes; else by the ground under it: its blend flag - 1, no; upside down on its light flag - 2, no;
    // else 0, yes.
    bool AssistanceTest(World& w, uint32_t& out) const;
    // The car put at `at` facing `dir` (CarPlaceAt, vslot 97 at 0x4e5270) and its pose set there (CarSetPose, vslot 96 at
    // 0x4e9660: lifted so its body's origin is there, the accumulators and velocities zeroed, woken, its contact group let
    // go, the brake wall's time 0; the signal the game's).
    void PlaceAt(World& w, const Vec4& at, const Vec4& dir);
    void SetPose(World& w, const Vec4& p, const Vec4& q);
    uint32_t turnedOver = 0;           // to be put back turned over (CarPlaceAt)
    bool assistanceAllowed = false;    // the game's: outside assistance allowed (0x5665f0; the options' bit 1)
    bool assistanceFlag = false;       // the game's flag (0x5666b0; the options' bit 2): help without the ground's test
    // The car's integration (CarIntegrate, CCar vslot 27): the assistance's countdown, the driver, the solid's step (the
    // drive line's parameter), a brake wall, the hinged parts woken or let go, the loose parts stepped or let go, the stage
    // timing, the race state, the distance rolled, the speeds, the signals.
    void Step(World& w, float dt) override;
    // The car's step (CarStep, CCar vslot 22): the controls (the steering, the control unit, the drive and its engine),
    // the children's steps (the wheels'), then the roll bars and the steering feedback.
    void StepChildren(World& w, float dt) override;
    void StepControls(float dt);   // the part before the children
    void StepAfterChildren();      // and after (CarRollBars, CarSpeedBlend, the extra step)
    // A control record (CarControlInput, 0x4e91f0): held by the assistance a record of its own (the handbrake on); the
    // control unit's command, the drive's handbrake and starter, the controls' steering. Then the record's requests for
    // help: 0x20 - unless held, not turned over and held; 0x10 - outside assistance (1, 0.5, 4) when AssistanceTest
    // passes (else the game's request signal).
    void Control(const ControlInput& r);
    std::array<Solid*, 4> hinged{};   // the hinged parts by their def's index (0: none, or torn off)
    void ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c) override;   // CarApplyImpulse
    // The car's surface cells under a point in its frame (CarSurfaceAt): its mesh hit from 10 m out along the point
    // towards (0, 0, 0.35), the mesh's surface bytes consumed; 0 without a hit. (Always true: a car's.)
    bool SurfaceAt(const Vec4& pb, uint32_t& surfaceA, uint32_t& surfaceB) const override;
    // Its parameter along the stage's drive line, after the integration (SolidIntegrate's).
    void DriveLineStep(World& w) override;
};

// A car's part on a hinge or a mount (CHingedRigidBody, CMountedRigidBody): its own solid once free; attached, its contacts'
// response is its parent's (HingeImpulseResponse: the vectors in the part's frame handed on unchanged, as the original).
class PartSolid : public Solid {
public:
    Solid* car = nullptr;   // attached to (none: free)
    void ImpulseResponse(const Frame& from, const Vec4& p, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const override;
};

// A part's state in the car's damage (the classic HingedDamage: the lock and attachment flags the damage model and the
// collisions share).
struct PartDamage {
    uint32_t lock = 0;       // shut (a door's or the bonnet's lock)
    uint32_t attached = 0;   // on the car
    uint32_t mount = 0;      // (the exhaust's mount)
};

// A hinge spring's def (SpringN...: type 2 a spring).
struct HingeSpringDef {
    uint32_t type = 0;
    float pos = 0, stiff = 0, damp = 0, maxDisp = 0, span = 0;
};

// A hinged part's def (CHingedRigidBody::CDef: the .lsp's hinge keys and the bases built from them).
struct HingeDef {
    HingeSpringDef springs[2];
    Vec4 hingeAxisRF{}, hingePointRF{};   // in the car body's frame
    Vec4 hingePoint{};                    // the hinge point in the car's frame
    float faceArea = 0;                   // the air on it
    Vec4 faceNormalBF{}, hingeAxisBF{}, hingePointBF{}, anchorBF{};   // in its own frame
    float basisRF[9] = {}, basisBFT[9] = {};
    float maxAngle = 0, minAngle = 0, defaultAngle = 0, restitution = 0;
    float inertia = 0, springRate = 0, damping = 0;
};

// A hinged part (a door, the bonnet): attached, an impulse spins it about its hinge and its load along the axis goes into the
// car - above 2000 the part comes off. Its tick turns it about the hinge: the car's motion at the hinge, the air on its
// face, its springs, its friction and its contacts drive the spin; its limits stop it; its pose follows from the angle.
class HingedSolid : public PartSolid {
public:
    // The hinge's angular acceleration (HingeSolve, vslot 25); locked: nothing; off the car: a solid's forces.
    void Forces(World& w) override;
    // The spin and the angle, the limits, the pose (HingeIntegrate, vslot 27); locked: nothing; off the car: a solid's step.
    void Step(World& w, float dt) override;
    const HingeDef* hingeDef = nullptr;
    float angle = 0;                      // about the hinge axis
    float springPos[2] = {};              // the springs' rest angles (moved past MaxDisp)
    float bodyRows[9] = {}, rotation[9] = {}, hingeRows[9] = {};   // the frame setup's (rotation: in the car, rows)
    Vec4 axisWorld{};
    Vec4 pivotAcceleration{};             // the car's acceleration at the pivot, gravity included
    Vec4 axisCar{};                       // the car's angular acceleration in its frame
    float axisAcceleration = 0;           // its part along the hinge axis
    float angularAcceleration = 0;        // the hinge's
    PartDamage* damage = nullptr;
    Vec4 pivot{};             // the hinge point (p - pivot: the lever of an impulse)
    float spin = 0;           // rad/s about the hinge axis
    Vec4 hingeAxis{};         // the def's vecHingeAxis_RF (the car's frame)
    float inertia = 0;        // the def's InertiaHingeAxis
    bool breakChain = false;  // the def's BreakChain: no load into the car, never torn off
    void ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c) override;   // HingeApplyImpulse

private:
    friend class WorldLoader;   // (the load's frame setup)
    const Body* carBody() const;
    void RotationRows();
    num::X87 SpringTorque(uint32_t i);
    num::X87 SpringTorqueSum();
    void Limits();
    void WorldFrame(World& w);
    num::X87 InertialTorque(World& w);
    void FrameSetup(World& w);
};

// A mounted part (the exhaust): attached, it takes no impulse (MountedApplyImpulse).
class MountedSolid : public PartSolid {
public:
    uint32_t* stick = nullptr;   // the damage handler's record: 0 on, 1 loose (hanging on its base point), 2 off (none: on)
    uint32_t Stick() const { return stick ? *stick : 0; }
    Vec4 basePoint{};     // the def's: where it hangs, in the car's frame
    // Loose: a spring to its base point on the car (StickObjectForce, 0x521740: the car's acceleration there less gravity,
    // into the car's frame and negated, the spring and the damping) into its measured acceleration; free: a solid's forces.
    void StickForce(World& w);
    // Loose: stepped on its base point, within a small box about it (StickObjectStep, 0x521d90); free: a solid's step.
    void StickStep(World& w, float dt);
    uint32_t poseSent = 0;   // its first pose signalled
    void Forces(World& w) override { StickForce(w); }            // vslot 25: StickObjectForce
    void Step(World& w, float dt) override { StickStep(w, dt); }  // vslot 27: StickObjectStep
    void ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c) override
    {
        if (!car) Solid::ApplyImpulse(p, J, c);
    }
};

// The car's body (CCarBody): its frame the car's, offset by the centre of gravity (the model's origin to the car's).
class CarBody : public Body {
public:
    Vec4 centerOfGravity;   // the def's vecCenterOfGravity
    // Its state's own copy of the box (CarBodyStateBounds: the mesh's in the world; valid once computed).
    bool stateBoundsValid = false;
    Vec4 stateBoundsLo{}, stateBoundsHi{};
    void Notify() override;

    bool Awake() const override { return true; }
    void PointsUp(Vec4* p, uint32_t n) const override;   // less the centre of gravity
    void PointsDown(Vec4*, uint32_t) const override { Unreached("CCarBody vslot 40"); }
    void SavedPointsUp(Vec4*, uint32_t) const override { Unreached("CCarBody vslot 41"); }
    void SavedPointsDown(Vec4*, uint32_t) const override { Unreached("CCarBody vslot 42"); }
};

// The car's parts let go, on the world's ReleasePart (a part's pose and velocities into the world's, out of the car's
// children, woken and filed): a hinged part torn off its car (CarHingedDetach 0x4e58f0: each of the car's slots holding
// it emptied when the part is released; then EntityChildRemove 0x51a060: the car out of the part's exclusions); a hinged
// part released (HingedRelease 0x51fd00, then PartFree); a mounted part (StickRelease 0x522100: its orientation
// renormalised, then PartFree). The releases: false when the part is not on its car any more.
void DetachHinged(World& w, Car& car, HingedSolid& h);
bool ReleaseHinged(World& w, HingedSolid& h);
bool ReleaseStick(World& w, MountedSolid& m);

} // namespace phys::native
