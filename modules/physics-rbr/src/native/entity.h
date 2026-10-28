#pragma once
// The physics' objects and their frames, natively (M6 step 2; the classic CEntity / CFrame family, core/frame.cpp): the
// base of the object model, library rbr-collision - the collision code needs nothing more. Solid is native/solid.h, the
// car's solids native/car_solids.h.
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

#include "num/math.h"
#include "native/unreached.h"

namespace phys::native {

class Entity;
struct CollisionMesh;   // native/collision_mesh.h
struct CollisionHit;    // native/event.h
class EventList;        // native/event.h
struct MaterialBlend;   // native/event.h
struct ContactGroup;    // native/solver.h
struct PushGroup;       // native/solver.h
class World;            // native/world.h
class Solid;            // native/solid.h

using num::Mat33;
using num::Vec4;

// What a contact hands a body with its impulse (vslot 74's arguments past the point and the impulse).
struct ImpulseContact {
    uint32_t surfaceA = 0, surfaceB = 0;         // the surface cells at the contact (the body's own or the other's)
    const MaterialBlend* materials = nullptr;    // the other side's
    float normal = 0;                            // j, the normal impulse
    float slide = 0;                             // |t|, the slide speed
    const Vec4* tangent = nullptr;               // t, the slide velocity (negated for the event's other side)
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
