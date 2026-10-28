#pragma once
// The contact solver's impulse pass, natively (M6 step 2; the classic CollisionEventsFlag / CollisionEventsSolve,
// core/event.cpp, core/contact.cpp): each collision event the detectors found turned into an impulse on its two bodies.
//
// An event's two sides first get their materials (an id of 0 takes the body's mesh material) and its flags: the sides
// that move (a mass) and whether a side's material lets bodies pass through. The solve then walks the events newest first:
// an event after the bodies' step is skipped; an asleep body with a mass is woken; the approach speed along the contact's
// normal gives the normal impulse - restitution and friction blended between the material's high and low values by the
// impact speed - and the slide along the contact plane a friction impulse limited by it. Both go to the two bodies at the
// contact point, the other side's negated.
//
// The penetration pass (CollisionEventsDepenetrate) walks the same events: a contact deeper than a margin pushes its moving
// sides out along the normal (shared by their inverse masses when both move) - each body's push-outs gathered in a push group
// and resolved together, the deepest along their mean - and the contacts of a body that moves against a fixed side go into
// its contact group, which the body's next forces turn into resting contact forces.
#include <array>
#include <cstdint>
#include <functional>

#include "native/solid.h"
#include "native/event.h"

namespace phys::native {

// A surface material (the classic Material's values): the collisions' (CollMy*, e*, flags) and the ground's under a tyre
// (the soil, its bumps, the water, the rolling resistance; the tyre's pair table index).
struct SurfaceMaterial {
    float frictionHigh = 0, frictionLow = 0;         // the impact friction at a hard / a soft impact
    float restitutionHigh = 0, restitutionLow = 0;   // the restitution likewise
    uint32_t flags = 0;                              // MaterialFlags (model/material.h)
    float solidGroundOffset = 0, unknown01c = 0, unknown02c = 0;
    float staticFriction = 0, kineticFriction = 0, staticSoilFriction = 0, kineticSoilFriction = 0;
    float sinkFactor = 0, soilDensity = 0, soilParticleMass = 0, soilThickness = 0;
    float bumpAmplitude = 0, bumpFrequency = 0;     // the surface's waviness (0: flat)
    float waterFactor = 0, rollingResistance = 0;
    uint32_t type = 0;                              // the tyre's pair table index
};

// The physics' 256 materials by id; an undefined one reads as material 0 (FlaggedEntry).
class MaterialTable {
public:
    std::array<SurfaceMaterial, 256> entries{};
    const SurfaceMaterial& operator[](uint32_t id) const;
};

// The materials of an event's two sides (CollisionEventMaterials, 0x4f4a20) and its pass-through flags
// (CollisionEventMaterialFlags, 0x4f4ac0).
void EventMaterials(CollisionEvent& ev);
void EventMaterialFlags(CollisionEvent& ev, const MaterialTable& materials);
// Each event's materials and flags, and which of its sides move (CollisionEventsFlag, 0x4fbc20).
void EventsFlag(EventList& list, const MaterialTable& materials);

// One contact of a body's group (the classic ContactRecord): what ContactGroupAddEvent keeps of an event.
struct ContactRecord {
    bool soft = false;                       // a pass-through material's soft contact (the spring below)
    uint32_t material = 0;                   // the material id (the event's)
    uint32_t surfaceB = 0, surfaceA = 0;     // the side's surface cells
    float softStiffness = 0, softLimit = 0;  // soft: the material's restitution low / high as the spring's stiffness and cap
    float frictionHigh = 0, frictionLow = 0;
    Vec4 point{};                            // the other side's point, in the world
    Vec4 normal{};                           // towards the body
    Vec4 bodyPoint{};                        // the body's point, in its frame
    Vec4 worldPoint{};                       // the body's point, in the world
};

// A body's contacts (at most 8).
struct ContactGroup {
    Solid* body = nullptr;
    uint32_t count = 0;
    std::array<ContactRecord, 8> records;
};

// A body's push-outs (at most 8), resolved at the pass's end.
struct PushGroup {
    Solid* body = nullptr;
    uint32_t count = 0;
    std::array<Vec4, 8> points;
};

// The step's contact and push groups (the classic CCollisionList's pools: 64 of each).
class ContactGroups {
public:
    std::array<ContactGroup, 64> groups;
    uint32_t groupCount = 0;
    std::array<PushGroup, 64> pushes;
    uint32_t pushCount = 0;

    // An event into the body's contact group (ContactGroupAddEvent, 0x4fbd30): side true the event's body, false its other
    // (the plane turned round). False when the pool or the group is full.
    bool AddEvent(Solid& body, const CollisionEvent& ev, bool side, const MaterialTable& materials);
    // A push-out into the body's push group (ContactGroupAddPoint, 0x4fbfc0).
    bool AddPoint(Solid& body, const Vec4& point);
    // The penetration pass over the list (CollisionEventsDepenetrate, 0x4fc590), then every push group resolved.
    void Depenetrate(EventList& list, const MaterialTable& materials);
    // The contact groups forgotten (ContactGroupsClear, 0x4fb7f0).
    void Clear();
};

// One contact record's resting force on the body (ContactResponse, 0x52b9f0): false when it does not push (not below the
// surface, or no impulse). The force and torque come in the body's axes, the impulse and the depth as computed; weight
// scales the spring of a hard contact (1 a solid's forces, 3 a hinge's), frictionScale is the config's.
bool ContactResponse(Solid& body, const ContactRecord& rec, float weight, float frictionScale, Vec4& force, Vec4& torque, float& impulse,
                     float& depth);
// The body's contact group's forces and torques summed (ContactSum, 0x52c510): false without a group or a response.
bool ContactSum(Solid& body, float weight, float frictionScale, Vec4& force, Vec4& torque);

// A push group resolved (ContactGroupResolve, 0x4fb890): the body moved along the points' mean by the deepest point along
// it (at most 0.03 m).
void ResolvePushGroup(PushGroup& group);

class ImpulseSolver {
public:
    const MaterialTable* materials = nullptr;
    std::function<void(Solid&)> woken;   // the world's part of a wake (the re-filing, the game's event)

    // The relative velocity at the contact (CollisionEventRelVel, 0x4f40f0): the body's minus the other's.
    void RelativeVelocity(const CollisionEvent& ev, Vec4& out) const;
    // Both sides' response terms for the directions v0 / v1 (CollisionEventResponse, 0x4f4390).
    void Response(const CollisionEvent& ev, const Vec4& v0, const Vec4& v1, bool cross, float* self, float* other) const;
    // The impulse J (j the normal one, L the slide speed, K the slide) to both sides (CollisionEventApply, 0x4f4dc0).
    void Apply(const CollisionEvent& ev, float j, float L, const Vec4& J, const Vec4& K) const;
    // One event's impulse (CollisionEventSolve, 0x4fc1a0): the normal impulse j (0: none).
    float Solve(CollisionEvent& ev) const;
    // The list's events, newest first (CollisionEventsSolve, 0x4fca20).
    void SolveAll(EventList& list) const;
};

} // namespace phys::native
