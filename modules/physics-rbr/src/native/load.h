#pragma once
// The world built from the game's files, natively (M6 step 2, the load; the classic load, oracle/loadreplay.h's RunOurs
// over core/stuff.cpp, core/tree.cpp, core/colmesh.cpp): what the physics is sent at a stage's load, in the game's
// order - the ground's record, the stage's .col (its collision, the brake wall, the object grid, the water), the
// collision meshes and the CSigStuff records of the stage's objects (each its kind's def, a mesh from the table or its
// own record, a pose; filed in the world and the grid, woken or put to sleep), then the car's.
//
// The loader owns what it builds: the entities, the mesh table, the stage collision and the records' data.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "data/rec.h"
#include "data/trkworld.h"
#include "native/collision_mesh.h"
#include "native/contacts.h"
#include "native/damage.h"
#include "native/driveline.h"
#include "native/defs.h"
#include "native/stage_collision.h"
#include "native/world.h"

namespace phys::native {

// A CSigStuff record (model/stuff.h).
struct StuffRecord {
    Vec4 position{};                   // (w 0)
    Vec4 rotation{0, 0, 0, 1};         // a quaternion (used with the config's STUFF_ROTATION)
    float scale[3] = {1, 1, 1};
    float driveLineDistance = 0;       // +0x30
    uint32_t meshKind = 0;             // flags & 0x30: 0 the table's `mesh`, 0x10 / 0x30 the record's own (`meshRecord`), 0x20 none here
    uint32_t mesh = 0;                 // the table slot
    MeshRecord* meshRecord = nullptr;  // its own (its slot `meshRecordSlot`: given a free one for 0x30)
    uint32_t meshRecordSlot = 0;
    uint32_t kind = 0;                 // the def kind
    uint32_t id = 0;                   // +0x40
    uint32_t flags = 0;                // StuffFlags: bit 0 awake, 1 / 2 push / pop the parent
    DamageListener* damage = nullptr;  // +0x48
};

// The .trk's records as the load sends them (data/trkworld.h; the record's own data stays the TrkWorld's): a mesh record
// (its faces pointer set even without faces, as the sent record's), a stuff record (a baked mesh's own record from
// `baked`, the views of TrkWorld::bakedMeshes).
MeshRecord RecordOf(const data::TrkMeshRecord& m);
StuffRecord RecordOf(const data::TrkStuffRecord& s, std::vector<MeshRecord>& baked, const data::TrkWorld& trk);

// One of the car's collision meshes (the .sgc's, data/rec.h's RecMesh): its record, with its surface tables (the uvs and
// the two grids).
MeshRecord RecordOf(const data::RecMesh& m);

// The car's CSigStuff records as the game sends them (0x46b160, after its meshes): the car (mesh slot 1, kind CAR_ROOT,
// awake and the parent) at the start - the .lbs's position (x, z, y: the renderer's order) half a metre up, the yaw
// quaternion (0, 0, sin a, cos a) with a = (pi - heading) / 2 -, the four wheels (slot 0, awake), the doors, bonnet and
// exhaust (slots 2 .. 5) while the damage handler has them on, the bumpers and the wing (slots 6 .. 8) while not off, and
// the body (slot 0, awake, the parent popped); each with scale 1 and the damage handler.
std::vector<StuffRecord> CarRecords(const float start[3], float heading, DamageListener* damage, const bool hingedOn[4], const bool mountedOn[3]);

// The game's options the load reads.
struct LoadOptions {
    bool gameMode = false;     // the game's mode test (0x566690): the drive starts with the engine at 0 (else 400 rad/s)
    int32_t damageLevel = -1;  // the game option's damage level (-1: none set)
    bool assistanceAllowed = false;   // the options' bit 1: outside assistance allowed (a retired car put back)
    bool assistanceFlag = false;      // the options' bit 2: the game's assistance flag (help without the ground's test)
};

// What the car's systems keep (the classic car's own blocks: its scratch's states, the wheels' tyre and spring-damper
// states) and its damage handler.
struct CarSystems {
    DriveState drive;
    EngineState engine;
    VcuState controlUnit;
    LightGroups lights;
    DamageHandler damage;
    TyreState tyres[4];
    SpringDamperState springDampers[4];
    uint32_t wheels = 0;       // made so far
    // The game's per-cylinder array the head gaskets' wear scales (its global 0x7ca668): zero at the start, so it stays.
    float gasketFactors[4] = {};
};

class WorldLoader {
public:
    WorldLoader(Defs& defs, const MaterialTable& materials, const LoadOptions& options = {});

    World world;
    Entity root;                       // the world's frame (depth 0)
    std::vector<std::unique_ptr<Entity>> entities;   // in the order made
    StageCollision collision;
    MaterialMaps maps;
    Ground* ground = nullptr;
    BrakeWall* brakeWall = nullptr;
    std::vector<Water*> water;
    Car* car = nullptr;                // the car (the game's global car)
    std::unique_ptr<CarSystems> systems;

    // The car's damage handler (DamageHandlerCreate, then the option's level): before the car's records, which name it.
    DamageHandler* CarDamageHandler();
    // The setup's end (CarSetupFinish, 0x4e9c70), once its values are in the defs (ApplySetup): the damage handler reset,
    // its records handed to the systems and parts - each wheel's geometry into the car's frame, its top mount and rows,
    // its travel between the rubbers and its suspension, its damper; each hinge's bases, frame and mesh; each mount's base
    // point -, then a zero control record. "" or what is missing.
    std::string FinishCar();
    // The load's end, before tick 0: the .trk's drive line block (3,20; DriveLineLoad) and the .fnc's fence items
    // (FenceLoad: one dummy a item, first in the grid cell under its point). "" or what is wrong.
    std::string LoadDriveLine(const std::vector<uint8_t>& block);
    std::string LoadFences(const std::vector<uint8_t>& fnc);
    DriveLine driveLine;
    std::vector<FenceDummy*> fences;
    // The world ready to step: the car's systems' hooks (the engine's step for the drive, the car's velocity and the
    // gasket factors for the engine, the forward speed for the control unit), the damage handler's (on with the
    // config's ENABLE_DAMAGE).
    void PrepareRun();
    // The game's StepForward(n): n ticks (World::Step), then the end-of-steps call (the drive's throttle and clutch 0).
    void Step(uint32_t n = 1);

    // The .col (ColLoadFile: the tree; its root's brake wall, the grid, the water by the wetness). "" or what is wrong.
    std::string LoadCol(std::vector<uint8_t> file, uint32_t wetness);
    // A CSigCollisionMesh (ColMeshEnter): the mesh at its slot built from the record (the record's data stays the caller's).
    void MeshEnter(const MeshRecord& r, uint32_t slot);
    // A CSigStuff (StuffEnter): its entity (null: none made) - "" or what is not ported in `error`.
    Entity* StuffEnter(StuffRecord& r, std::string& error);

    CollisionMesh* MeshAt(uint32_t slot);   // the table's entry (made empty when first asked for)

private:
    Defs& m_defs;
    LoadOptions m_options;
    std::map<uint32_t, std::unique_ptr<CollisionMesh>> m_meshes;   // the static mesh table (0x8f7030) by slot
    std::vector<Entity*> m_parents;                                 // the parent stack (StuffEnter's)
    bool m_grid = false;
    std::string error_;   // (a record the native load does not take yet)

    void EnsureGrid();
    uint32_t FreeSlot() const;
    template <class T> T* Make();
    void Init(Entity& e, uint32_t id, Entity* parent, uint32_t flags);   // EntityInit's frames and links
    void SolidFromRecord(Solid& s, const StuffRecord& r);
    void CarAttach(Car& c, Entity& part);
    uint32_t KindOfDef(const RigidBodyDef* def) const;   // a part's kind by its def
    void WheelFinish(Wheel& w);
    WheelPartDef& PartDefOf(const Wheel& w);   // a wheel's kind def
    CarSystems& CarSystemsOf();
    void HingeFinish(HingedSolid& h, HingedPartDef& def);
    void MountFinish(MountedSolid& m, const MountedPartDef& def);
    Vec4 CarFromBody(const Car& c, const Vec4& p, Move m) const;   // a point / direction from the car body's frame into the car's
};

} // namespace phys::native
