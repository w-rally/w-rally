#pragma once
// A rigid body in the world, natively (CSolid; library rbr-world): an entity's body with its state, stepped by the world,
// and what its damage handler hears. The base classes are native/entity.h, the car's solids native/car_solids.h.
#include <cstdint>

#include "native/entity.h"
#include "native/rigid_body.h"

namespace phys::native {

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

    // The car's parts in a solid's own code - where the classic casts a kCar-flagged solid to CCar - as slots the car
    // fills (native/car_solids.h), so the world and the solver need not know the car: its parameter along the drive line
    // after the integration (SolidIntegrate's, a car without a parent that moved), and its surface cells under a point in
    // its frame (CarSurfaceAt; false: not a car).
    virtual void DriveLineStep(World&) {}
    virtual bool SurfaceAt(const Vec4&, uint32_t&, uint32_t&) const { return false; }
};

} // namespace phys::native
