#pragma once
// The physics' objects and their frames, natively (M6 step 2; the classic CEntity / CFrame family, core/frame.cpp).
//
// An entity has two frames: its own (the place its points live in, moved into its parent's by the entity's transforms)
// and an older one (the same, but through the pose saved at the step's start while the entity is awake). A collision
// event has an alias frame (moves nothing). Points, directions and planes move between any two frames by walking the
// frame tree (FrameMove) - each level's move is the entity's, by kind.
//
// The classic code's quirks are kept: a walk down into a deeper frame moves only the first point through the levels
// above it (the original pushes n = 1 there). Moves no classic run reaches - the vtable slots without ported code - stop
// the program with their name (Unreached) rather than guess.
#include <array>
#include <cstdint>
#include <vector>

#include "native/rigid_body.h"
#include "native/unreached.h"
#include "native/vcu.h"

namespace phys::native {

class Entity;
struct CollisionMesh;   // native/collision_mesh.h
struct CollisionHit;    // native/event.h
class EventList;        // native/event.h
struct MaterialBlend;   // native/event.h
struct ContactGroup;    // native/solver.h
struct PushGroup;       // native/solver.h
class World;            // native/world.h

// What a contact hands a body with its impulse (vslot 74's arguments past the point and the impulse).
struct ImpulseContact {
    uint32_t surfaceA = 0, surfaceB = 0;         // the surface cells at the contact (the body's own or the other's)
    const MaterialBlend* materials = nullptr;    // the other side's
    float normal = 0;                            // j, the normal impulse
    float slide = 0;                             // |t|, the slide speed
    const Vec4* tangent = nullptr;               // t, the slide velocity (negated for the event's other side)
};

// What a solid's damage handler hears (the game's CCarDamageHandler): an impulse's hit (its vslot 1, DamageHandlerHit) and
// a resting contact's push (vslot 2, DamageHandlerImpact).
struct ImpulseHit {
    uint32_t surfaceA = 0, surfaceB = 0;
    uint32_t material = 0;   // the other side's (the first of its blend when 0.5 > the weight, else the second)
    float normal = 0, slide = 0;
    Vec4 pointInBody{}, point{};
    const Vec4* tangent = nullptr;
};
struct ContactImpact {
    uint32_t surfaceA = 0, surfaceB = 0;   // the car's surface cells under the point (CarSurfaceAt)
    uint32_t material = 0;                 // the record's
    float force = 0, slide = 0;            // |the contact force|; the slide speed (the approach speed without a slide)
    Vec4 bodyPoint{}, point{};             // the contact point in the body's frame and in the world
    const Vec4* direction = nullptr;       // the slide's direction (unnormalised without a slide)
};
class DamageListener {
public:
    virtual ~DamageListener() = default;
    virtual void Hit(const ImpulseHit& hit) = 0;
    virtual void Impact(const ContactImpact& impact) = 0;
};

// What a walk moves: points (the frames' vslots 1 / 2), directions (3 / 4) or planes (5 / 6).
enum class Move { Points, Dirs, Planes };

struct Frame {
    enum class Kind { Entity, Older, Alias };
    Kind kind = Kind::Entity;
    const Frame* parent = nullptr;
    uint32_t depth = 0;            // 0: the world
    const Entity* owner = nullptr;

    void Up(Vec4* p, uint32_t n, Move m) const;     // into the parent's frame
    void Down(Vec4* p, uint32_t n, Move m) const;   // out of it
};

// A frame point: a point and the frame it is in (the classic {vec4 *, entity}).
struct FramePoint {
    const Vec4* p = nullptr;
    const Frame* frame = nullptr;
};

// A mesh in a frame (the classic {mesh *, frame} record a detector hands the pair tests: a body's mesh where it was - built
// into its older frame - or where it is).
struct MeshInFrame {
    const CollisionMesh* mesh = nullptr;
    const Frame* frame = nullptr;
};


class Entity {
public:
    virtual ~Entity() = default;

    Frame frame{Frame::Kind::Entity}, olderFrame{Frame::Kind::Older};
    uint32_t id = 0;
    uint32_t flags = 0;   // EntityFlags (model/entity.h)
    Entity* parent = nullptr;
    std::vector<Entity*> children;          // the classic children list's order
    bool hasExclusions = false;             // a child array (EntityChildArray: the entities it does not collide with)
    std::vector<Entity*> exclusions;
    std::vector<Entity*>* cell = nullptr;   // its object grid cell (the world re-files a moved solid without a parent)

    virtual bool Awake() const { return false; }   // simulated (vslot 32)
    virtual bool Static() const { return false; }  // one of the world's static objects (vslot 34)
    virtual bool VerySoft() const { return false; }   // the wheels' rays pass it (vslot 85)
    // The pose changed (vslot 16, EntityVslot16): every awake descendant's, depth first - each through its own, so the
    // deeper ones come again, as the original's walk (the updates are idempotent).
    virtual void Notify();
    // The step's working data reset (vslot 21; a plain entity's scratch is empty).
    virtual void ResetStep() {}
    // A tick's slots (the world's PhysicsStep and BodyPhases): a plain entity hands each to its awake children (vslots 8,
    // 23, 25, 22; ChildrenVslot*), its step updates its pose's dependants first (BodyVslot27).
    virtual void TickBegin(World& w, float dt);   // vslot 8
    virtual void PrepareForces(World& w);         // vslot 23 (a solid's gravity, vslot 24, after the children's)
    virtual void Forces(World& w);                // vslot 25
    virtual void Step(World& w, float dt);        // vslot 27
    virtual void StepChildren(World& w, float dt);   // vslot 22
    // A held body's tick (vslot 38, the world's held list; a plain solid's 0x51a7c0 is not ported - no run holds one).
    virtual void HeldTick(World&, float) { Unreached("vslot 38 (0x51a7c0) of a held body"); }
    // The step's start saved (vslot 18): the awake children's (ChildrenVslot18).
    virtual void SaveState()
    {
        for (Entity* c : children)
            if (c->Awake()) c->SaveState();
    }
    virtual CollisionMesh* Mesh() const { return nullptr; }   // vslot 70
    virtual bool Collidable() const
    {
        Unreached("CEntity vslot 68");
        return false;
    }
    // The segment between two frame points as a contact point and plane (vslot 60: BrakeWallContact's; a body's - 0x4e2dd0
    // - and the ground's are not ported).
    virtual bool SegmentPoint(const FramePoint&, const FramePoint&, Vec4*, Vec4*, uint32_t*)
    {
        Unreached("vslot 60");
        return false;
    }
    // The segment between two frame points against the entity (vslot 58): its contact record and true on a hit.
    virtual bool SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool enabled);

    // What the collision detectors ask of an entity.
    virtual void WorldBox(Vec4&, Vec4&) const { Unreached("vslot 71"); }   // its box in the world (vslot 72 forwards to 71)
    // Does the box between two frame points touch it (vslot 51)?
    virtual bool BoxTest(const FramePoint&, const FramePoint&)
    {
        Unreached("vslot 51");
        return false;
    }
    virtual bool Detectable() const { return false; }    // vslot 67: it starts a broad phase (a body with a mesh)
    virtual bool EdgeCapable() const { return false; }   // vslot 69: the edge sweep may run against it
    // The pair tests with it as the moving side (vslots 52, 54, 55; physicsFlags the config's Flags): none for a plain
    // entity.
    virtual bool Penetrate(const MeshInFrame&, Entity&, EventList&, uint32_t) { return false; }
    virtual bool Sweep(const MeshInFrame&, const MeshInFrame&, Entity&, EventList&) { return false; }
    virtual bool EdgeSweep(const MeshInFrame&, const MeshInFrame&, Entity&, EventList&) { return false; }

    // What the contact solver asks of an entity (vslots 79, 80, 78, 74): one that does not move has no mass, no velocity,
    // no response and takes no impulse (0x519ec0, BodyZeroVelocity, 0x519e10, 0x519dd0).
    virtual float InverseMass() const { return 0.0f; }
    virtual void VelocityAt(const Vec4&, Vec4& out) const { num::Vec4Zero(out); }   // at a point in the world
    // The response terms of a contact at p with the normal n and the other direction d, all in `from` (an event's frame).
    virtual void ImpulseResponse(const Frame&, const Vec4&, const Vec4&, const Vec4&, bool, float*, float*) const {}
    virtual void ApplyImpulse(const Vec4&, const Vec4&, const ImpulseContact&) {}   // J at p, both in the world
    virtual class Solid* AsSolid() { return nullptr; }

    // The moves into / out of the parent's frame (vslots 39 .. 48): the current pose, the saved one, the directions, the
    // planes. A plain entity moves nothing.
    virtual void PointsUp(Vec4*, uint32_t) const {}
    virtual void PointsDown(Vec4*, uint32_t) const {}
    virtual void SavedPointsUp(Vec4*, uint32_t) const {}
    virtual void SavedPointsDown(Vec4*, uint32_t) const {}
    virtual void DirsUp(Vec4*, uint32_t) const {}
    virtual void DirsDown(Vec4*, uint32_t) const {}
    virtual void SavedDirsUp(Vec4*, uint32_t) const { Unreached("CEntity vslot 45"); }
    virtual void SavedDirsDown(Vec4*, uint32_t) const { Unreached("CEntity vslot 46"); }
    virtual void PlanesUp(Vec4*, uint32_t) const { Unreached("CEntity vslot 47"); }
    virtual void PlanesDown(Vec4*, uint32_t) const { Unreached("CEntity vslot 48"); }
};

// A body (CBody): an entity with a collision mesh and a world box.
class Body : public Entity {
public:
    CollisionMesh* mesh = nullptr;         // vslot 70 (0: none; a query's hit leaves its surface bytes in it)
    Vec4 boundsMin{}, boundsMax{};         // the world box (BoundsUpdate; vslots 71 / 72)
    CollisionMesh* Mesh() const override { return mesh; }
    void Notify() override;                // BodyVslot16: the world box anew (BoundsUpdate), then the entity's
    bool Collidable() const override;      // vslot 68: a mesh with faces
    // BodySegmentContact (0x4e2f50) through BodySegmentQuery (vslot 61, 0x4e2e90): a into the older frame, b into the
    // current one, the mesh's segment hit, its point and plane into the world, the mesh's surface bytes consumed.
    bool SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool enabled) override;
    void WorldBox(Vec4& lo, Vec4& hi) const override { lo = boundsMin, hi = boundsMax; }   // BodyBounds (vslot 71)
    bool BoxTest(const FramePoint& lo, const FramePoint& hi) override;                     // BodyBoxOverlap (0x4e1230)
    bool Detectable() const override { return mesh != nullptr; }                           // BodyCollidable (vslot 67)
    bool EdgeCapable() const override { return mesh != nullptr; }                          // CarHasMesh (vslot 69)
    // CarPenetrate (vslot 52), BodySweepContacts (54), BodyEdgeSweep (55): native/contacts.h.
    bool Penetrate(const MeshInFrame& rec, Entity& other, EventList& list, uint32_t physicsFlags) override;
    bool Sweep(const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list) override;
    bool EdgeSweep(const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list) override;
};

// A rigid body in the world (CSolid and its kin: the car, its wheels, hinged and mounted parts, the stage's objects).
class Solid : public Body {
public:
    RigidBody body;
    bool awake = false;   // simulated: in the world's stepping list
    bool held = false;    // held (a hinge): in the held list

    DamageListener* damage = nullptr;   // the damage handler (none: no one hears the impacts)
    World* world = nullptr;   // the world it is in (a part torn off is filed there)
    // The step's contacts and push-outs (the penetration pass; body.inContactGroup follows contactGroup).
    ContactGroup* contactGroup = nullptr;
    PushGroup* pushGroup = nullptr;

    bool Awake() const override { return awake; }
    Solid* AsSolid() override { return this; }
    bool VerySoft() const override { return body.def->verySoft; }   // SolidDefFlag1ec
    void Notify() override;      // SolidVslot16: the rotation from the orientation, then the body's
    void ResetStep() override;   // SolidVslot21: SolidScratchReset
    void PrepareForces(World& w) override;   // SolidVslot23: the children's, then SolidGravity
    void Forces(World& w) override;          // SolidForces: the children's, the resting contacts, the step's accelerations
    void Step(World& w, float dt) override;  // SolidStep: integrated, re-filed, the children stepped, the sleep check
    void SaveState() override   // SolidSaveState: the awake children's, then its own
    {
        Entity::SaveState();
        body.SaveState();
    }
    // Woken (SolidWake: its state, the step's start saved; the world then re-files it and tells the game).
    void Wake()
    {
        awake = true, held = false;
        contactGroup = nullptr;
        body.inContactGroup = false;
        SaveState();
    }
    // The damping objects' push and drag on the solid (SolidDampingContacts, 0x52c220; a car's forces).
    void DampingContacts(World& w);
    float InverseMass() const override { return body.def->inverseMass; }   // SolidMassGetter
    void VelocityAt(const Vec4& p, Vec4& out) const override;              // SolidVelocityAt: v + w x r (and the parent's)
    void ImpulseResponse(const Frame& from, const Vec4& p, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const override;
    void ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c) override;   // SolidApplyImpulse
    void PointsUp(Vec4* p, uint32_t n) const override { body.TransformPoints4(p, n); }
    void PointsDown(Vec4* p, uint32_t n) const override { body.WorldToLocal4(p, n); }
    void SavedPointsUp(Vec4* p, uint32_t n) const override { body.LocalToWorld4(p, n); }
    void SavedPointsDown(Vec4* p, uint32_t n) const override { body.WorldToBody4(p, n); }
    void DirsUp(Vec4* p, uint32_t n) const override { body.RotateT4(p, n); }
    void DirsDown(Vec4* p, uint32_t n) const override { body.Rotate3(p, n); }
    void SavedDirsUp(Vec4* p, uint32_t n) const override { body.RotateDirs4(p, n); }
    void SavedDirsDown(Vec4* p, uint32_t n) const override { body.WorldToBodyDirs4(p, n); }
    void PlanesUp(Vec4* p, uint32_t n) const override { body.TransformPlanes4(p, n); }
    void PlanesDown(Vec4*, uint32_t) const override { Unreached("CSolid vslot 48"); }
};

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
    // towards (0, 0, 0.35), the mesh's surface bytes consumed; 0 without a hit.
    void SurfaceAt(const Vec4& pb, uint32_t& surfaceA, uint32_t& surfaceB) const;
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

// The walk between two frames, generic over a level's move (ops.Up(frame, n) / ops.Down(frame, n)): same depth - t's up,
// the parents, a's down; t deeper - up and on with t's parent; a deeper - the walk with a's parent and n = 1 (the
// original's push 1: only the first item goes through the levels above), then a's down with all n.
namespace walk {
template <class Ops> void Level(const Frame* t, const Frame* a, uint32_t n, const Ops& o)
{
    if (t == a) return;
    o.Up(*t, n);
    Level(t->parent, a->parent, n, o);
    o.Down(*a, n);
}
template <class Ops> void Deeper(const Frame* t, const Frame* a, uint32_t n, const Ops& o)
{
    if (t->depth > a->depth) {
        o.Up(*t, n);
        Deeper(t->parent, a, n, o);
        return;
    }
    Level(t, a, n, o);
}
template <class Ops> void Shallower(const Frame* t, const Frame* a, uint32_t n, const Ops& o)
{
    if (t->depth < a->depth) {
        Shallower(t, a->parent, 1, o);
        o.Down(*a, n);
        return;
    }
    Level(t, a, n, o);
}
template <class Ops> void Move(const Frame& t, const Frame& a, uint32_t n, const Ops& o)
{
    if (&a == &t) return;
    if (t.depth > a.depth) Deeper(&t, &a, n, o);
    else if (t.depth < a.depth) Shallower(&t, &a, n, o);
    else Level(&t, &a, n, o);
}
} // namespace walk

// The world: the root of a frame's tree.
const Frame& WorldOf(const Frame& f);

// The points (directions, planes) p moved from frame t into frame a (0x51b320 / 0x51b370 / 0x51b3c0).
Vec4* FrameMove(const Frame& t, const Frame& a, Vec4* p, uint32_t n, Move m);
// out = src, then moved (0x51b4c0 / 0x51b560 / 0x51b600).
Vec4* FrameMoveCopy(const Frame& t, const Vec4& src, const Frame& a, Vec4& out, Move m);
// n of them (0x51b460 / 0x51b500).
Vec4* FrameMoveCopyN(const Frame& t, const Vec4* src, const Frame& a, Vec4* dst, uint32_t n, Move m);
// A frame point moved into frame `to` (0x51b6c0).
Vec4* FramePointMove(const FramePoint& fp, Vec4& out, const Frame& to);

} // namespace phys::native
