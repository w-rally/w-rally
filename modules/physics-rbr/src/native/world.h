#pragma once
// The world's collision detection, natively (M6 step 2; the classic CPhysics broad phase and the CL1 / CL2 detectors,
// core/collide.cpp, core/world.cpp, core/grid.cpp): which entities a moving body is tested against.
//
// The object grid files the stage's movable objects, the fence items and the water by where they are; a body's L1
// detector walks the grid cells around its world box and the world's static objects (the ground ...) and hands each
// candidate to an L2 detector, which filters it (collidable, CanCollide, the box test) and runs the pair tests with the
// meshes moved into each other's frames. The penetration detectors do the same for bodies that overlap now, the gigantic
// one for the world's static objects. The classic order is kept everywhere (the grid cells' lists, the world's objects, the
// children), so the events come out in the classic order.
#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

#include "native/brake_wall.h"
#include "native/collision_mesh.h"
#include "native/driveline.h"
#include "native/contacts.h"
#include "native/solid.h"
#include "native/event.h"
#include "native/solver.h"
#include "native/records.h"

namespace phys::native {

class Car;   // native/car_solids.h (the game's callbacks name it)

// The object grid (the classic ObjectGrid, model/grid.h): 100 x 100 m blocks of 10 x 10 cells; a cell holds its entities in
// the classic list's order (the first its head: a re-filed body goes first).
class ObjectGrid {
public:
    using Cell = std::vector<Entity*>;
    float originX = 0, originY = 0;
    int32_t rows = 0, columns = 0;
    std::vector<std::unique_ptr<std::array<Cell, 100>>> blocks;   // rows x columns, null until touched

    void Create(float x0, float y0, float w, float h);   // the grid's constructor (0x4e0550)
    Cell* CellAt(float x, float y);                      // GridCell (0x4e03b0): its block made when first touched
    // GridCellsAround (0x5240c0): the four cells at a box's corners (spread: around its middle, 4.7 m out), a repeat 0.
    void CellsAround(bool spread, const float* a, const float* b, Cell* out[4]);
};

// The stage's water (CCollisionMeshWater): in the grid, collidable, its box test always true, but it never makes a contact
// (its segment queries are trivial).
class Water : public Entity {
public:
    Vec4 boundsMin{}, boundsMax{};
    Vec4 quad[4] = {};   // its corners (the .col's)
    Vec4 plane{};        // (0, 0, 1, -corner 0's z): level
    // The segment o + d t against its quad (ObjectQuadHit, 0x4fac50): its plane and t (at least 0).
    bool QuadHit(const float* o, const float* d, Vec4* out, float* t) const;
    bool Collidable() const override { return true; }   // vslot 68 (0x4fb0b0)
    void WorldBox(Vec4& lo, Vec4& hi) const override { lo = boundsMin, hi = boundsMax; }   // OpenMeshBounds (vslot 71)
    bool BoxTest(const FramePoint&, const FramePoint&) override { return true; }           // vslot 51 (0x519e90)
    bool SegmentContact(const FramePoint&, const FramePoint&, CollisionHit&, bool) override { return false; }   // vslot 58
    bool SegmentPoint(const FramePoint&, const FramePoint&, Vec4*, Vec4*, uint32_t*) override { return false; }  // vslot 60
};

// A fence item (StickSystem::CDummyObject): its box makes the spectators react (`react`, the game's - none here) but never a
// contact.
class FenceDummy : public Entity {
public:
    float centre[3] = {}, halfSize[3] = {};   // x, z, y (the classic layout's)
    void (*react)(FenceDummy& d) = nullptr;
    bool Collidable() const override { return true; }   // vslot 68 (0x600c80)
    void WorldBox(Vec4& lo, Vec4& hi) const override;    // DummyBounds (vslot 71)
    bool BoxTest(const FramePoint& a, const FramePoint& b) override;   // StickDummyBoxTest (0x6009b0): always false
    bool SegmentContact(const FramePoint&, const FramePoint&, CollisionHit&, bool) override { return false; }
    bool SegmentPoint(const FramePoint&, const FramePoint&, Vec4*, Vec4*, uint32_t*) override { return false; }
};

// May `a` collide with `b` (CanCollide, 0x519f80): the parents' first, then the shorter exclusion list.
bool CanCollide(const Entity& a, const Entity& b);

// The pair of `a` moving against `b` (BodyPairCollide, vslot 53): the sweep with FACE_COLL; none found, the edge sweep with
// EDGE_COLL when b may have one. rec*: a's mesh where it was and is, in b's frames.
bool PairCollide(Entity& a, const MeshInFrame& recA, const MeshInFrame& recB, Entity& b, EventList& list, uint32_t physicsFlags);

class World {
public:
    const Frame* world = nullptr;
    ObjectGrid grid;
    // The world's entities by what they do (the classic state's lists, their order): the bodies that step, the held ones
    // (the hinges), the static objects (the ground, the brake wall ...) and the rest.
    std::vector<Entity*> awake, held, objects, resting;
    uint32_t physicsFlags = 0;      // the config's Flags (model/physics.h; 0x2000: the grid's spread lookup)
    uint32_t debugFlags = 0;        // the config's debug flags (RENDER_MATERIAL, RENDER_SUSPENSION_GEOMETRY ...: values kept for display)
    const MaterialTable* materials = nullptr;
    const StageCollision* collision = nullptr;   // the stage's ground and its surface maps (the wheels' rays)
    const MaterialMaps* maps = nullptr;
    bool forceMaterial = false;     // the config's FORCE_MATERIAL: every contact's material forceMaterialId
    uint32_t forceMaterialId = 0;
    Vec4 gravity{};                 // the config's vecGravity (the world's axes)
    float frictionScale = 0;        // the config's resting contacts' friction factor (ContactSum)
    EventList events;               // the step's collision events (the solver's list)
    ContactGroups groups;
    std::function<void(Solid&)> woken;   // the game's part of a wake (its body event)

    // The broad phases of one entity (CL1CollisionDetector / CL1PenetrationDetector vslot 1, 0x526ad0 / 0x525a20): its
    // events into `list`.
    void CollisionDetect(Entity& e, EventList& list);
    void PenetrationDetect(Entity& e, EventList& list);
    // Both over the awake bodies and their awake descendants (CollisionBroadPhase 0x526a70, PenetrationBroadPhase 0x526cc0),
    // into `events`.
    void CollisionBroadPhase();
    void PenetrationBroadPhase();

    // An entity into the list its state names (WorldFile, 0x51fa60: held, awake, static or resting; appended) and out of the
    // one it was in first (PhysicsRefile, 0x51fae0).
    void File(Entity& e);
    void Refile(Entity& e);
    // A solid without a parent re-filed in the grid cell under its position (first in it).
    void GridRefile(Entity& e);
    // A solid's state blended back to the fraction f of the step (SolidBlendState, 0x52cfd0) and the rest of a partial step
    // (SolidAdvanceRest, 0x52d110): re-filed and its pose's dependants updated (vslot 16).
    void BlendState(Solid& s, float f);
    void AdvanceRest(Solid& s, float dt);

    // The meshes of the entities whose world boxes the box centre +- ext touches (MeshListBoxQuery, 0x525bd0: but those with
    // excludeFlags and `exclude`; at most 8), moved into the world.
    struct MeshList {
        uint32_t count = 0;
        Entity* entities[8] = {};
        CollisionMesh meshes[8];
    };
    bool MeshListBoxQuery(const float* centre, const float* ext, uint32_t excludeFlags, const Entity* exclude, MeshList& list);
    // The segment's nearest hit among the list's meshes (MeshListSegmentQuery, 0x5246a0): the entity (null: none), the hit as a
    // ground contact (pointW: the point's fourth lane, as the classic record keeps it).
    Entity* MeshListSegmentQuery(const float* p0, const float* p1, const MeshList& list, GroundHit& out, float& pointW);
    // The segment a .. b against the quads of the grid's open meshes near a (GridQuadSegmentQuery, 0x4fad90).
    bool GridQuadSegmentQuery(const float* a, const float* b, Vec4* out, float* t);

    // The damping box (the classic *0x7c7040): a box in a solid's frame against the objects' shapes - bushes, water -,
    // up to 8 contacts in the classic record's layout (the shared box tests fill it).
    ContactBox dampingBox{};
    // The box placed on `e` against the shapes of the objects in the grid cells about it (BoxObjectContacts, 0x4f3950):
    // false without a mesh on `e` or a contact; the contacts in dampingBox.contacts, their count.
    bool BoxObjectContacts(const Entity& e, int32_t& count);
    struct DampingCollision {   // CSigDampingCollision: the deepest contact's
        float depth = 0;
        uint32_t material = 0;
        Vec4 otherCentre{}, point{}, velocity{};
    };
    std::function<void(Solid&)> retireCar;   // CSigRetireCar (a car on a damping object of material 200)
    std::function<void(Solid&, const DampingCollision&)> dampingCollision;
    std::function<void(float, float)> ggFeedback;   // the g-g diagram's force feedback (0x495090; RENDER_GG_DIAG)

    // The car's integration's: the stage's drive line and brake wall, the config's brake wall values, the game's parts.
    const DriveLine* driveLine = nullptr;
    BrakeWall* brakeWall = nullptr;
    float brakeWallBuffer = 0, brakeWallFactor = 0;
    void (*brakeWallQuad)(const BrakeWall::Hit& h) = nullptr;            // a pass-through quad under the car (its signal)
    std::function<void(Car&, const BrakeWall::Hit&)> brakeWallHit;       // CSigCarBrakeWallPenetration
    std::function<void(Car&, float distance)> stageTiming;               // the stage timing at the car's distance (0x4a0540)
    std::function<void(Car&)> raceState;                                 // the race state with the stand-still count (0x4e9af0)
    std::function<void(Car&)> carPosition, carState;                     // CSigCarPosition, CSigMrRCarState

    // The pass-through events dropped, the earliest time of the rest (CollisionEventsPrune, 0x4fc500; none: 2).
    float PruneEvents();
    // The collision stage (PhysicsCollisionStage, 0x526d20): the broad phase, the events flagged and pruned, the bodies
    // blended back to the earliest contact, the impulses, the rest of the step; then the awake bodies' step data reset and
    // saved.
    void CollisionStage(float dt);
    // The penetration stage (PhysicsStep's): the penetration broad phase, the contact groups anew, the events depenetrated
    // and solved.
    void PenetrationStage();
    // A step's collisions (PhysicsStep's, on the ticks it runs them): the collision stage with CHECK_COLL (dt: four steps),
    // the penetration stage with CHECK_PENETR, then the awake bodies' step data reset and saved.
    void Collide(float dt);

    // A part let go into the world (PartFree, with HingedRelease 0x51fd00 / StickRelease 0x522100): its pose and velocities
    // from its car's (`carSlot`, the part's own: emptied) into the world's - the orientation renormalised for a mounted
    // part -, out of the car's children, woken, filed, the game told. False when it is not on its car any more. (The
    // car's own releases: native/car_solids.h.)
    bool ReleasePart(Solid& part, Solid*& carSlot, bool renormalise);
    // A solid woken by the game's call (SolidWake, vslot 36: its state, re-filed, the game told).
    void WakeSolid(Solid& s)
    {
        s.Wake();
        Wake(s);
    }
    std::function<void(Solid&)> released;   // the game's part of a release (the part's id and pose signals)
    std::function<void(Solid&)> slept;      // the game's part of a sleep (its body event)
    std::function<void(Solid&)> stepped;    // the game's part of a solid's step (its CSigMrRBodyState)
    std::function<void(const TireParticles&)> tyreParticles;   // the game's tyre effects (CSigTireParticles)

    // A tick's body phases (BodyPhases, 0x525460): the awake bodies' vslot 23, then 25, then 27 and 22 each - a body
    // falling asleep there, or a car held in its forces (retired), ends that pass (the original walks on through its
    // links, into the list it was filed in: at its tail).
    void Phases(float dt);
    // One step of the physics (PhysicsStep's, 0x5273a0, without its pause and its end-of-steps call): the tick counted, the
    // awake bodies' tick start (vslot 8), the body phases, the collisions (every 4th tick when sub-stepping), the held
    // bodies' vslot 38, the time.
    void Step(float dt);
    uint32_t tick = 0;
    double time = 0;
    // A solid put to sleep (SolidSleep, vslot 37: neither awake nor held, no contact group, re-filed, the game told).
    void Sleep(Solid& s);
    // A solid held (SolidHold, vslot 86 at 0x52c030; a car's 0x4e5ee0 first sets the game's car-woken global): without a
    // parent neither awake nor held but held; no contact group; re-filed (the held list: its vslot 38 each tick), the game
    // told (its body event).
    void Hold(Solid& s);

private:
    void Wake(Solid& s);   // the solver's: re-filed (a parent's child stays), the game told
};

} // namespace phys::native
