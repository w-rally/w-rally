#pragma once
// The pair tests, natively (M6 step 2; the classic CBody / CCar vslots 52 .. 55, core/contact.cpp): a body's mesh against
// what it may touch, each contact found an event in the list. Now: a body against the stage's ground (GroundBodyContacts).
#include <vector>

#include "native/collision_mesh.h"
#include "native/event.h"
#include "native/stage_collision.h"

namespace phys::native {

// The stage's ground (CGround): an entity whose shape is the stage's static collision.
class Ground : public Body {
public:
    bool Static() const override { return true; }
    void Notify() override { Entity::Notify(); }   // EntityVslot16: no world box (+0x80 is not one)
    const StageCollision* collision = nullptr;
    const MaterialMaps* maps = nullptr;

    bool Collidable() const override { return true; }   // vslot 68 (0x51bb70)
    void WorldBox(Vec4&, Vec4&) const override { Unreached("CGround vslot 71"); }
    bool BoxTest(const FramePoint&, const FramePoint&) override { return true; }   // GroundBoxTrue (vslot 51)
    bool Detectable() const override { return false; }                              // vslot 67 (0x519d40)
    bool EdgeCapable() const override { return true; }                              // vslot 69 (0x51bb80)
    bool Penetrate(const MeshInFrame&, Entity&, EventList&, uint32_t) override { return false; }   // vslots 52, 54, 55: trivial
    bool Sweep(const MeshInFrame&, const MeshInFrame&, Entity&, EventList&) override { return false; }
    bool EdgeSweep(const MeshInFrame&, const MeshInFrame&, Entity&, EventList&) override { return false; }
    // GroundSegmentQuery (vslot 58, 0x51c6f0): the segment between two frame points (moved into the world) against the
    // stage: the contact record {point, normal and plane, the two materials and the blend, time, the ground}.
    bool SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool enabled) override;
};

// 0x51bd30: the body's mesh against the track (its world box's leaves; per vertex the segment from the mesh's centre, a
// car's edges both ways, else the centre as a point in the ground). Returns whether any event was made.
bool GroundBodyContacts(Ground& g, Body& body, EventList& list);

// 0x4e3070 (vslot 54): the body's mesh swept from where it was (a) to where it is (b) against an entity with a segment
// query (the ground, a brake wall, a body): per vertex the other's SegmentContact, a hit an event 0 at the hit with the
// vertex's surface. Returns whether any event was made.
bool BodySweepContacts(Body& body, const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list);

// 0x4e2850 (vslot 55): the body's edges swept from where they were (a) to where they are (b) against the other's mesh,
// within the overlap of the swept box and the other's: per edge through that box, the other mesh's EdgesSweep - a hit an
// event 1 with its time, the body mesh's surface bytes (consumed). Returns whether any edge hit.
bool BodyEdgeSweep(Body& body, const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list);

// 0x4e1eb0 (vslot 52): the car's mesh (rec: where it is) penetrating another entity - the ground (GroundBodyContacts),
// an entity without a mesh or a brake wall (its SegmentPoint from the centre to each vertex), else the other's mesh: with
// FACE_PENETR the vertices inside it (an event 4 each), and when none, with EDGE_PENETR the edges through it (events 5).
// physicsFlags: the physics config's Flags (model/physics.h). Returns whether any event was made.
bool CarPenetrate(Body& car, const MeshInFrame& rec, Entity& other, EventList& list, uint32_t physicsFlags);

} // namespace phys::native
