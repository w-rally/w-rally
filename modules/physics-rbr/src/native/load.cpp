#include "native/load.h"

#include <cmath>
#include <cstring>

#include "num/f32.h"
#include "num/x87.h"
#include "num/math.h"
#include "native/flags.h"
#include "native/records.h"

namespace phys::native {

using num::F32;
using num::X87;

namespace {

// 0x52e4f0: a gas's amount from its pressure, temperature and volume (R = 287, air's).
void GasDensity(float* g)
{
    g[0] = g[1] == 0.0f ? 0.0f : ((F32(g[3]) * F32(g[2])) / (F32(287.0f) * F32(g[1]))).f();
}

float Bits(uint32_t w)
{
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// 0x531500: the tyre's state (240 kPa, 333.15 K, its volume; the elements at 65 C).
void TyreStateInit(TyreState& s)
{
    s = {};
    s.pressureBracket = s.treadTemperatureBracket = s.carcassTemperatureBracket = s.loadBracket = s.workBracket = {0, 1, 0.0f};
    s.pressure = 240000.0f, s.gasTemperature = 333.15f, s.gasVolume = Bits(0x3acde32e);
    GasDensity(&s.gasMass);
    for (float& t : s.elementTemperatures) t = 338.15f;
    s.carcassTemperature = s.innerTemperature = 338.15f;
}

// 0x505480: the drive's state (the engine at 400 rad/s outside the game mode, neutral, the brakes at 293.15 K).
void DriveStateInit(DriveState& s, bool gameMode)
{
    s = {};
    s.random = 0x130807, s.rebuildPending = 1, s.lockState = 2;
    static const float kHandbrake[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // the rear wheels'
    for (uint32_t k = 0; k < 4; ++k) {
        DriveSlot& e = s.slots[k];
        e.temperature = e.temperature2 = 0x11791d29;
        e.weight = e.inertia = 1.0f;
        e.handbrakeRatio = kHandbrake[k];
    }
    s.engineSpeed = gameMode ? 0.0f : 400.0f;
    s.gear = s.requestedGear = 1;   // neutral
    s.engineTorqueAverage.count = 2;
    s.inertia = 1.0f;
}

// 0x50bed0: the engine's state (the intake at 101.3 kPa and 293.14 K, the plenums' gas, the coolant, the block at 423 K).
void EngineStateInit(EngineState& s)
{
    s = {};
    for (Cylinder& c : s.cylinders) {
        c.chargeMass = Bits(0x3c23d70a);   // 0.01
        c.blend[0] = 1.0f;
        c.stiffness = 1.0f;
    }
    for (uint32_t& k : s.sparks) k = 1;
    s.intakePressure = 101300.0f;
    s.intakeTemperature = s.spentChargeTemperature = s.chargeDensity = Bits(0x439291ec);   // 293.14
    for (float* g : {s.intake, s.exhaust}) {   // amount (the gas law), temperature, pressure, volume
        g[3] = Bits(0x3cf5c28f), g[2] = 110300.0f, g[1] = Bits(0x43929333);
        GasDensity(g);
    }
    s.intercoolerStiffness = 1.0f;
    s.radiatorCoolant[1] = s.engineCoolant[1] = 373.0f;
    s.engineCoolant[0] = 1.5f;
    s.radiatorCoolant[0] = Bits(0x40a66666);   // 5.2
    s.blockTemperature = 423.0f;
}

float CarForwardSpeed(void* ctx) { return static_cast<const Car*>(ctx)->carState.forwardSpeed; }

// 0x4ea9d0's parts: the drive, the engine, the lights and the control unit linked to the def's and the scratch's, the
// lights' groups (the rear and the low beam on), the aids until the setup's (the automatic gearbox), the assistance.
void CarSystemsInit(Car& c, CarRootDef& def, CarSystems& S, bool gameMode)
{
    DriveStateInit(S.drive, gameMode);
    EngineStateInit(S.engine);
    S.controlUnit = {};
    S.lights = {};
    S.lights.on[2] = S.lights.on[0] = 1;
    c.drive.def = &def.drive, c.drive.state = &S.drive, c.drive.carId = c.id;
    c.engine.def = &def.engine, c.engine.state = &S.engine;
    Vcu& v = c.controlUnit;
    v.def = &def.controlUnit, v.state = &S.controlUnit, v.drive = &c.drive, v.engine = &c.engine, v.lights = &S.lights;
    v.hooks = {&CarForwardSpeed, &c};
    v.autoGears = 1;
    c.carState.assistanceTime = 4.0f;
}

} // namespace

MeshRecord RecordOf(const data::TrkMeshRecord& m)
{
    static const uint8_t kNoFaces[8] = {};
    MeshRecord rec;
    rec.flags = m.w[1];
    rec.name = m.name.c_str();
    rec.vertexCount = static_cast<uint16_t>(m.w[3] & 0xffff), rec.faceCount = static_cast<uint16_t>(m.w[3] >> 16);
    rec.vertices = reinterpret_cast<const Vec4*>(m.vertices.data());
    rec.faces = m.faces.empty() ? kNoFaces : m.faces.data();
    rec.shape = m.shape.empty() ? nullptr : m.shape.data();
    rec.material = static_cast<uint8_t>(m.w[12] & 0xff);
    return rec;
}

MeshRecord RecordOf(const data::RecMesh& m)
{
    MeshRecord rec;
    rec.flags = m.w[1];
    rec.name = m.name.c_str();
    rec.vertexCount = static_cast<uint16_t>(m.w[3] & 0xffff), rec.faceCount = static_cast<uint16_t>(m.w[3] >> 16);
    rec.vertices = reinterpret_cast<const Vec4*>(m.verts.data());
    rec.faces = m.faces.data();
    rec.uvCount = m.w[6];
    rec.uvs = m.tables[0].empty() ? nullptr : m.tables[0].data();
    rec.gridWidthA = static_cast<uint16_t>(m.w[8] & 0xffff), rec.gridWidthB = static_cast<uint16_t>(m.w[8] >> 16);
    rec.gridA = m.tables[1].empty() ? nullptr : m.tables[1].data();
    rec.gridB = m.tables[2].empty() ? nullptr : m.tables[2].data();
    rec.material = static_cast<uint8_t>(m.w[12] & 0xff);
    return rec;
}

std::vector<StuffRecord> CarRecords(const float start[3], float heading, DamageListener* damage, const bool hingedOn[4], const bool mountedOn[3])
{
    auto rec = [&](uint32_t slot, uint32_t kind, uint32_t flags) {
        StuffRecord r;
        r.mesh = slot, r.kind = kind, r.flags = flags, r.damage = damage;
        return r;
    };
    std::vector<StuffRecord> out;
    StuffRecord car = rec(1, 0x31, 3);
    const float a = (3.14159274f - heading) * 0.5f;
    car.position = {start[0], start[2], start[1] + 0.5f, 0.0f};
    car.rotation = {0.0f, 0.0f, static_cast<float>(std::sin(static_cast<double>(a))), static_cast<float>(std::cos(static_cast<double>(a)))};
    out.push_back(car);
    for (uint32_t k = 0; k < 4; ++k) out.push_back(rec(0, 0x21 + k, 1));
    static constexpr uint32_t kHingedParts[4][2] = {{0x25, 2}, {0x26, 3}, {0x28, 4}, {0x2c, 5}}, kMountedParts[3][2] = {{0x2a, 6}, {0x29, 7}, {0x2b, 8}};
    for (uint32_t i = 0; i < 4; ++i)
        if (hingedOn[i]) out.push_back(rec(kHingedParts[i][1], kHingedParts[i][0], 0));
    for (uint32_t i = 0; i < 3; ++i)
        if (mountedOn[i]) out.push_back(rec(kMountedParts[i][1], kMountedParts[i][0], 0));
    out.push_back(rec(0, 0x27, 5));
    return out;
}

StuffRecord RecordOf(const data::TrkStuffRecord& s, std::vector<MeshRecord>& baked, const data::TrkWorld& trk)
{
    StuffRecord rec;
    std::memcpy(&rec.position, &s.w[0], 16), std::memcpy(&rec.rotation, &s.w[4], 16), std::memcpy(rec.scale, &s.w[8], 12);
    std::memcpy(&rec.driveLineDistance, &s.w[12], 4);
    rec.flags = s.w[17], rec.meshKind = s.w[17] & 0x30, rec.kind = s.w[14], rec.id = s.w[16];
    if (s.meshRecord >= 0) {
        rec.meshRecord = &baked[static_cast<size_t>(s.meshRecord)];
        rec.meshRecordSlot = trk.bakedMeshes[static_cast<size_t>(s.meshRecord)].w[0];
    } else {
        rec.mesh = s.w[13];
    }
    return rec;
}

WorldLoader::WorldLoader(Defs& defs, const MaterialTable& materials, const LoadOptions& options) : m_defs(defs), m_options(options)
{
    root.frame.owner = root.olderFrame.owner = &root;
    world.world = &root.frame;
    world.materials = &materials;
    const PhysicsConfig& P = defs.physics;
    world.physicsFlags = P.flags, world.debugFlags = P.debugFlags, world.gravity = P.gravity;
    world.frictionScale = P.maxTimeStep;   // (ContactSum reads the step's slot)
    world.forceMaterial = (P.flags & kForceMaterial) != 0, world.forceMaterialId = P.forceMaterial;
    world.brakeWallBuffer = P.brakeWallBuffer, world.brakeWallFactor = P.brakeWallFactor;
    world.events.world = world.world;
    for (int k = 0; k < 3; ++k) (&world.dampingBox.localAxes[k].x)[k] = 1.0f;   // (the game's box object, *0x7c7040, as the exe
                                                                                 // starts it: a point, its axes the identity)
    world.time = 43185.0;   // (the physics state's constructor, 0x5270c0: +0x20 - noon, in seconds)
}

template <class T> T* WorldLoader::Make()
{
    entities.push_back(std::make_unique<T>());
    return static_cast<T*>(entities.back().get());
}

void WorldLoader::Init(Entity& e, uint32_t id, Entity* parent, uint32_t flags)
{
    // EntityInit (0x51a640): both frames under the parent's (or the world's, depth 1).
    e.frame.owner = e.olderFrame.owner = &e;
    e.frame.parent = parent ? &parent->frame : &root.frame;
    e.olderFrame.parent = parent ? &parent->olderFrame : &root.frame;
    e.frame.depth = e.olderFrame.depth = parent ? parent->frame.depth + 1 : 1;
    e.id = id;
    e.flags = flags | kEntity;
    e.parent = parent;
}

void WorldLoader::EnsureGrid()
{
    // GridEnsure (0x523d60): the first time, over the brake wall's root (its centre +- half size), else the default area.
    if (m_grid) return;
    m_grid = true;
    float x0 = -5200.0f, y0 = -7200.0f, w = 10000.0f, h = 15000.0f;
    if (brakeWall && !brakeWall->nodes.empty()) {
        const BrakeWall::Node& r = brakeWall->nodes[0];
        const float mnx = (F32(r.x) - r.hx).f(), mny = (F32(r.y) - r.hy).f();
        const float mxx = (F32(r.x) + r.hx).f(), mxy = (F32(r.hy) + r.y).f();
        x0 = mnx, y0 = mny;
        w = (F32(mxx) - mnx).f(), h = (F32(mxy) - mny).f();
    }
    world.grid.Create(x0, y0, w, h);
}

std::string WorldLoader::LoadCol(std::vector<uint8_t> file, uint32_t wetness)
{
    const std::string e = collision.Load(std::move(file));
    world.collision = &collision;   // (refused - not an R7CO file -: the empty tree, as the classic's, its queries finding nothing)
    if (ground) ground->collision = &collision;
    if (!e.empty()) return e;
    if (const uint8_t* block = collision.BrakeWallBlock()) {   // BrakeWallLoad: an entity filed in the world
        BrakeWall* const w = Make<BrakeWall>();
        Init(*w, 0, nullptr, kBrakeWall);
        w->Load(block);
        brakeWall = w;
        world.brakeWall = w;
        world.File(*w);
    }
    if (!collision.MakesGrid()) return "";
    EnsureGrid();
    for (const StageCollision::Quads& q : collision.WaterQuads(wetness))   // WaterLoad: at most 512
        for (uint32_t i = 0; i < q.count && water.size() < 0x200; ++i) {
            Water* const w = Make<Water>();   // WaterInit (0x4fb110)
            Init(*w, 0, nullptr, kWater | kOpenMesh);
            const Vec4* c = q.corners + 4 * i;
            std::memcpy(w->quad, c, sizeof w->quad);
            w->boundsMin = w->boundsMax = c[0];
            for (int k = 1; k < 4; ++k) {
                num::Min4(w->boundsMin, c[k]);
                num::Max4(w->boundsMax, c[k]);
            }
            w->plane = {0.0f, 0.0f, 1.0f, (((F32(c[0].y) + c[0].x) * F32(0.0f)) - c[0].z).f()};
            // WaterEnter (0x4faf00): first in the grid cell under its bounds' centre, then the physics' add and refile.
            const float x = ((X87(w->boundsMax.x) + X87(w->boundsMin.x)) * X87(0.5f)).Store();
            const float y = ((X87(w->boundsMax.y) + X87(w->boundsMin.y)) * X87(0.5f)).Store();
            w->cell = world.grid.CellAt(x, y);
            w->cell->insert(w->cell->begin(), w);
            world.File(*w);
            world.Refile(*w);
            water.push_back(w);
        }
    return "";
}

CollisionMesh* WorldLoader::MeshAt(uint32_t slot)
{
    std::unique_ptr<CollisionMesh>& m = m_meshes[slot];
    if (!m) m = std::make_unique<CollisionMesh>();
    return m.get();
}

uint32_t WorldLoader::FreeSlot() const
{
    // ColMeshFreeSlot (0x4f9a90): the first slot from 10 with neither vertices nor a shape.
    for (uint32_t slot = 10; 0x6b0 * slot < 0xd60000; ++slot) {
        const auto it = m_meshes.find(slot);
        if (it == m_meshes.end() || (!it->second->vertexCount && !it->second->shape)) return slot;
    }
    return 0xffffffffu;
}

void WorldLoader::MeshEnter(const MeshRecord& r, uint32_t slot)
{
    // ColMeshEnter (0x4f9e50): a car's slot 1 without a name is "Looks like a car"; slot 0 and the soft water are skipped.
    if (r.flags & kRecordNoPhysics) return;
    static const char kCarName[] = "Looks like a car";
    MeshRecord rec = r;
    if (slot == 1 && !rec.name) rec.name = kCarName;
    else if (slot == 0 || (rec.name && std::strstr(static_cast<const char*>(rec.name), "WATER_SOFT"))) return;
    CollisionMesh* const m = MeshAt(slot);
    if (m->vertexCount || m->shape) m->uvs = nullptr, m->gridA = m->gridB = nullptr, m->faceCount = m->vertexCount = 0, m->shape = nullptr;
    if (!rec.vertexCount && !rec.shape) return;
    m->Build(rec);
}

void WorldLoader::SolidFromRecord(Solid& s, const StuffRecord& r)
{
    // 0x52c700: the rotation (the record's with STUFF_ROTATION), the scale, the damage handler, the position, first in the
    // grid cell under it; then the body's (the mesh) and vslot 16.
    RigidBody& b = s.body;
    b.orientation = (m_defs.physics.flags & kStuffRotation) ? r.rotation : Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    b.scale[2] = r.scale[2], b.scale[0] = r.scale[0], b.scale[1] = r.scale[1];
    if (!(b.scale[0] == 1.0f) || !(b.scale[1] == 1.0f) || !(b.scale[2] == 1.0f)) b.scaleOn = 1.0f;
    if ((s.flags & kCar) && !r.damage) {
        error_ = "a car's own damage handler (a record without one)";
        return;
    }
    s.damage = r.damage;
    std::memcpy(&b.position, &r.position, 12);
    s.cell = world.grid.CellAt(b.position.x, b.position.y);
    s.cell->insert(s.cell->begin(), &s);
}

Entity* WorldLoader::StuffEnter(StuffRecord& r, std::string& error)
{
    EnsureGrid();
    if (r.meshKind == 0x10 || r.meshKind == 0x30) {   // StuffMeshPrepare: its own mesh record (a new one into a free slot)
        if (r.meshKind == 0x30) {
            r.meshRecordSlot = FreeSlot();
            r.meshRecord->flags |= kRecordFlag4;
        }
        MeshEnter(*r.meshRecord, r.meshRecordSlot);
    }
    CollisionMesh* mesh = nullptr;   // StuffMesh
    if (r.meshKind == 0) mesh = MeshAt(r.mesh);
    else if (r.meshKind == 0x10 || r.meshKind == 0x30) mesh = MeshAt(r.meshRecordSlot);
    uint32_t kind = r.kind;
    if (mesh && mesh->name) {
        const char* name = static_cast<const char*>(mesh->name);
        if (std::strstr(name, "CONE")) kind = 0x19;
        else if (std::strstr(name, "BANNER_STAND")) kind = 0x15;
    }
    Entity* const parent = m_parents.empty() ? nullptr : m_parents.back();
    Entity* e = nullptr;
    if (kind == 0) {   // the ground (CGround::CDef vslot 20: the one ground)
        Ground* const g = Make<Ground>();
        Init(*g, r.id, parent, kGround);
        g->collision = world.collision ? &collision : nullptr;
        g->maps = &maps;
        ground = g;
        world.maps = &maps;
        e = g;
        e->Notify();   // EntityFromRecord: vslot 16
    } else if (kind < kKinds && m_defs.solids[kind] && (m_defs.classOf[kind].empty() || m_defs.classOf[kind] == "Solid")) {
        Solid* const s = Make<Solid>();   // SolidCreate (0x52d640)
        Init(*s, r.id, parent, kBody | kSolid);
        s->body.def = &m_defs.solids[kind]->body;
        e = s;
    } else if (kind < kKinds && m_defs.classOf[kind] == "Car" && m_defs.cars[kind]) {   // CarCreate (0x4eb3e0)
        Car* const c = Make<Car>();
        Init(*c, r.id, parent, kBody | kSolid | kCar);
        c->body.def = &m_defs.cars[kind]->body;
        c->carDef = &m_defs.cars[kind]->car;
        c->hasExclusions = true;   // EntityChildArray(0x20)
        car = c;
        CarSystemsInit(*c, *m_defs.cars[kind], CarSystemsOf(), m_options.gameMode);
        e = c;
    } else if (kind < kKinds && m_defs.classOf[kind] == "Wheel" && m_defs.wheels[kind]) {   // WheelCreate (0x53d1c0)
        Wheel* const w = Make<Wheel>();
        Init(*w, r.id, parent, kBody | kSolid | kWheel);
        w->body.def = &m_defs.wheels[kind]->body;
        w->wheelDef = &m_defs.wheels[kind]->wheel;
        w->suspension.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        w->suspension.travel = 0.5f;
        w->slot = 0xffffffffu;
        w->car = parent && (parent->flags & kCar) ? static_cast<Car*>(parent) : nullptr;
        CarSystems& S = CarSystemsOf();   // its tyre and spring-damper (WheelStateInit's blocks, the links to the def's)
        const uint32_t i = S.wheels++ & 3;
        TyreStateInit(S.tyres[i]);
        S.springDampers[i] = {};
        w->tyre.def = &m_defs.wheels[kind]->tyre, w->tyre.state = &S.tyres[i];
        w->springDamper.def = &m_defs.wheels[kind]->springDamper, w->springDamper.state = &S.springDampers[i];
        e = w;
    } else if (kind < kKinds && m_defs.classOf[kind] == "CarBody" && m_defs.carBodies[kind]) {   // CarBodyCreate (0x4ebb00)
        CarBody* const b = Make<CarBody>();
        Init(*b, r.id, parent, kBody | kCarBody);
        b->centerOfGravity = m_defs.carBodies[kind]->centerOfGravity;
        e = b;
    } else if (kind < kKinds && m_defs.classOf[kind] == "HingedBody" && m_defs.hinged[kind]) {   // HingedCreate (0x51f8e0)
        HingedSolid* const h = Make<HingedSolid>();
        Init(*h, r.id, parent, kBody | kSolid | kHinged);
        h->body.def = &m_defs.hinged[kind]->body;
        h->hingeDef = &m_defs.hinged[kind]->hinge;
        h->angle = -0.75f;
        h->car = parent && (parent->flags & kCar) ? parent->AsSolid() : nullptr;
        h->hasExclusions = true;   // EntityChildArray(2)
        h->hingeAxis = h->hingeDef->hingeAxisRF, h->inertia = h->hingeDef->inertia, h->breakChain = m_defs.hinged[kind]->breakChain != 0;
        e = h;
    } else if (kind < kKinds && m_defs.classOf[kind] == "MountedBody" && m_defs.mounted[kind]) {   // MountedCreate (0x521d20)
        MountedSolid* const m = Make<MountedSolid>();
        Init(*m, r.id, parent, kBody | kSolid | kMounted);
        m->body.def = &m_defs.mounted[kind]->body;
        m->car = parent && (parent->flags & kCar) ? parent->AsSolid() : nullptr;
        m->hasExclusions = true;
        e = m;
    } else {
        error = "kind " + std::to_string(kind) + " (" + (kind < kKinds ? kKindNames[kind] : "?") + "): its class not built";
        return nullptr;
    }
    if (Solid* const s = e->AsSolid()) {   // SolidInit's state, then the record (vslot 17: SolidFromRecord)
        s->body.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        s->body.blend = 1.0f, s->body.accumulateScale = 1.0f;
        s->world = &world;
        if (auto* w = dynamic_cast<Wheel*>(s); w && parent) world.WakeSolid(*w);   // (WheelCreate: with a parent, vslot 36)
        error_.clear();
        SolidFromRecord(*s, r);
        if (!error_.empty()) {
            error = error_;
            return nullptr;
        }
        s->mesh = mesh;
        s->Notify();   // vslot 16
    } else if (auto* b = dynamic_cast<CarBody*>(e)) {   // BodyFromRecord (0x4e1140): the mesh, vslot 16
        b->mesh = mesh;
        b->Notify();
    }
    if (parent) {
        if (auto* c = dynamic_cast<Car*>(parent)) CarAttach(*c, *e);
        e->parent = parent;
        parent->children.push_back(e);   // EntityAttach (vslot 84)
    } else {
        world.File(*e);
    }
    if (Solid* const s = e->AsSolid()) {
        if (r.flags & 1) world.WakeSolid(*s);
        else world.Sleep(*s);
    }
    if (r.flags & 2) m_parents.push_back(e);
    else if ((r.flags & 4) && !m_parents.empty()) m_parents.pop_back();
    return e;
}


CarSystems& WorldLoader::CarSystemsOf()
{
    if (!systems) systems = std::make_unique<CarSystems>();
    return *systems;
}

DamageHandler* WorldLoader::CarDamageHandler()
{
    CarSystems& S = CarSystemsOf();
    S.damage.Create();
    if (m_options.damageLevel >= 0) S.damage.SetLevel(static_cast<uint32_t>(m_options.damageLevel));
    return &S.damage;
}

Vec4 WorldLoader::CarFromBody(const Car& c, const Vec4& p, Move m) const
{
    Vec4 out;
    FrameMoveCopy(c.carBody->frame, p, c.frame, out, m);
    return out;
}

WheelPartDef& WorldLoader::PartDefOf(const Wheel& w)
{
    for (uint32_t k = 0; k < kKinds; ++k)
        if (m_defs.wheels[k] && &m_defs.wheels[k]->wheel == w.wheelDef) return *m_defs.wheels[k];
    return *m_defs.wheels[KindOf("CAR_WHEEL_LF")];
}

void WorldLoader::WheelFinish(Wheel& w)
{
    // WheelDamageApply (0x53d030): the geometry into the car's frame (WheelGeometryToCar), the top mount and the rows, the
    // travel between the rubbers and the suspension there, the damper's damping from its wear.
    WheelPartDef& P = PartDefOf(w);
    WheelDef& D = P.wheel;
    const Car& c = *w.car;
    D.topMountDeformUpAxis = CarFromBody(c, P.topMountDeformUpAxisRF, Move::Dirs);
    D.topMountDeformSideAxis = CarFromBody(c, P.topMountDeformSideAxisRF, Move::Dirs);
    D.jointMount = CarFromBody(c, P.jointMountVF, Move::Points);
    D.jointAxis = CarFromBody(c, P.jointAxisVF, Move::Dirs);
    D.topMount = CarFromBody(c, P.topMountVF, Move::Points);
    D.steeringMount = CarFromBody(c, P.steeringMountVF, Move::Points);
    D.steeringAxis = CarFromBody(c, P.steeringAxisVF, Move::Dirs);
    D.driveAxleJointPos = CarFromBody(c, P.driveAxleJointPosVF, Move::Points);
    w.TopMountDeform();
    w.SteerRows();
    const SpringDamperDef& sd = *w.springDamper.def;   // between the bump rubbers (0.67 one ulp low, 0.33)
    w.suspension.travel = (F32(sd.outerBumpRubberPos) * F32(Bits(0x3f2b851e)) + F32(sd.innerBumpRubberPos) * F32(Bits(0x3ea8f5c3))).f();
    w.SuspensionUpdate(w.suspension.travel, c.carState.steeringAngle, (world.debugFlags & kRenderSuspensionGeometry) != 0);
    w.springDamper.Wear();
    w.springDamper.peakRate = w.springDamper.bumpRubberRate = w.springDamper.droopRate = 0.0f;
}

void WorldLoader::HingeFinish(HingedSolid& h, HingedPartDef& P)
{
    // HingeDamageApply (0x51efe0).
    HingeDef& D = P.hinge;
    if (P.unlocked) {
        h.damage->lock = 0;
        world.WakeSolid(h);
    } else {
        h.awake = false;   // (the Tweak set, not re-filed)
    }
    h.angle = D.defaultAngle;
    if (h.damage->lock == 0) {   // towards the nearer limit (sic: the blend gives the max either way)
        const F32 q = (F32(D.defaultAngle) - F32(D.minAngle)) / (F32(D.maxAngle) - F32(D.minAngle));
        const F32 k = num::jmp::A(q.f(), 0.5f) ? F32(Bits(0x3f666666)) : F32(Bits(0x3dcccccd));   // 0.9 / 0.1
        h.angle = (((F32(1.0f) - k) + k) * F32(D.maxAngle)).f();
    }
    // HingeBasis (0x51e070): rows x = d1 normalised, z = d1 x d2 normalised, y = z x x; the transpose; the anchor (the rim's
    // is not kept).
    auto basis = [](const Vec4& d1, const Vec4& d2, float* m, float* t) {
        Vec4 x, z, y;
        num::Normalize3Approx4To(d1, x);
        num::Cross3(d1, d2, z);
        num::Normalize3Approx4(z);
        num::Cross3(z, x, y);
        std::memcpy(m, &x, 12), std::memcpy(m + 3, &y, 12), std::memcpy(m + 6, &z, 12);
        static constexpr int kT[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
        for (int i = 0; i < 9; ++i) t[i] = m[kT[i]];
    };
    float rft[9], bf[9];
    basis(D.hingeAxisRF, P.zeroAxisRF, D.basisRF, rft);
    basis(D.hingeAxisBF, P.zeroAxisBF, bf, D.basisBFT);
    // The anchor -(t^T . the hinge point), four lanes: each row loaded 16 bytes from the transpose, the last one's 4th lane
    // the word after it (the classic's basisBF[0]).
    float t[10];
    std::memcpy(t, D.basisBFT, sizeof D.basisBFT);
    t[9] = bf[0];
    const Vec4& a = D.hingePointBF;
    const num::F32x4 o = (num::F32x4::Load(t) * num::F32x4::Splat(F32(a.x)) + num::F32x4::Load(t + 3) * num::F32x4::Splat(F32(a.y))) +
                             num::F32x4::Load(t + 6) * num::F32x4::Splat(F32(a.z));
    (num::F32x4::Splat(F32(-1.0f)) * o).Store(&D.anchorBF.x);
    D.hingePoint = CarFromBody(*static_cast<const Car*>(h.car), D.hingePointRF, Move::Points);
    h.FrameSetup(world);
    h.body.savedPosition = h.body.position, h.body.savedOrientation = h.body.orientation;
    if (h.mesh)   // MeshTranslate (0x4f65b0): the mesh onto the hinge point
        for (uint32_t i = 0; i < h.mesh->vertexCount; ++i) {
            Vec4& v = h.mesh->vertices[i];
            v.x = (F32(v.x) + F32(D.hingePointBF.x)).f(), v.y = (F32(v.y) + F32(D.hingePointBF.y)).f(), v.z = (F32(v.z) + F32(D.hingePointBF.z)).f();
        }
    h.springPos[0] = D.springs[0].pos, h.springPos[1] = D.springs[1].pos;
}

void WorldLoader::MountFinish(MountedSolid& m, const MountedPartDef& P)
{
    // MountDamageApply (0x521980): awake while loose or off, the base point into the car's frame, the pose there.
    m.awake = *m.stick != 0;
    m.basePoint = CarFromBody(*static_cast<const Car*>(m.car), P.basePoint, Move::Points);
    m.body.position = m.basePoint;
    m.body.savedPosition = m.body.position, m.body.savedOrientation = m.body.orientation;
}

std::string WorldLoader::FinishCar()
{
    if (!car || !systems) return "no car";
    Car& c = *car;
    DamageHandler& d = systems->damage;
    {   // (the setup's driving aids into the control unit: VcuSetup)
        const CarRootDef& def = *m_defs.cars[KindOf("CAR_ROOT")];
        Vcu& v = c.controlUnit;
        v.autoGears = def.autoGears, v.gearGuard = def.gearGuard, v.neutralLock = def.neutralLock;
        v.twoPedals = def.twoPedals, v.clutchHelp = def.clutchHelp;
    }
    for (Wheel* w : c.wheels)   // (the setup's tyre type and pressure into the tyres' state: TyreSetup, then the gas)
        if (w) {
            TyreState& st = *w->tyre.state;
            const TyreState& from = PartDefOf(*w).tyreStart;
            st.treadElasticity = from.treadElasticity, st.carcassStiffnessVertical = from.carcassStiffnessVertical;
            st.pressure = from.pressure, st.rollingRadius = from.rollingRadius, st.workBlend = from.workBlend;
            GasDensity(&st.gasMass);
        }
    d.Reset();
    c.drive.damage = &d.drive;
    d.drive.gearActuator = 1;
    c.engine.damage = &d.engine;
    systems->lights.changed = d.lights.changed;
    for (uint32_t& f : d.lights.changed) f = 1;
    for (uint32_t k = 0; k < 4; ++k) {
        Wheel* const w = c.wheels[k];
        if (!w) return "a wheel missing";
        w->damage = &d.wheels[k];
        w->springDamper.damage = &d.dampers[k];
        w->tyre.wear = &d.tyres[8 * k];
        WheelFinish(*w);
    }
    for (uint32_t k = 0; k < 4; ++k)
        if (auto* h = dynamic_cast<HingedSolid*>(c.hinged[k])) {
            h->damage = &d.hinged[k];
            HingeFinish(*h, *m_defs.hinged[KindOfDef(h->body.def)]);
        }
    for (uint32_t k = 0; k < 4; ++k)
        if (MountedSolid* const m = c.mounted[k]) {
            m->stick = &d.mounted[k];
            MountFinish(*m, *m_defs.mounted[KindOfDef(m->body.def)]);
        }
    c.Control(ControlInput{});   // a zero record
    return "";
}

void WorldLoader::CarAttach(Car& c, Entity& part)
{
    // CarAttach (0x4e5f20): a wheel into the first free slot (its index), the body, a hinged / mounted part into its def's
    // BodyId slot when free and into the car's and the part's exclusions.
    if (auto* w = dynamic_cast<Wheel*>(&part)) {
        for (uint32_t i = 0; i < 4; ++i)
            if (!c.wheels[i]) {
                c.wheels[i] = w;
                w->slot = i;
                break;
            }
    }
    if (auto* b = dynamic_cast<CarBody*>(&part)) c.carBody = b;
    auto joint = [&](Solid& p, uint32_t i, auto& table, auto* typed) {
        if (i < 4 && !table[i]) table[i] = typed;
        c.exclusions.push_back(&p);
        p.exclusions.push_back(&c);
    };
    if (auto* h = dynamic_cast<HingedSolid*>(&part)) joint(*h, m_defs.hinged[KindOfDef(h->body.def)]->bodyId, c.hinged, static_cast<Solid*>(h));
    if (auto* m = dynamic_cast<MountedSolid*>(&part)) joint(*m, m_defs.mounted[KindOfDef(m->body.def)]->bodyId, c.mounted, m);
}

uint32_t WorldLoader::KindOfDef(const RigidBodyDef* def) const
{
    for (uint32_t k = 0; k < kKinds; ++k)
        if ((m_defs.hinged[k] && &m_defs.hinged[k]->body == def) || (m_defs.mounted[k] && &m_defs.mounted[k]->body == def)) return k;
    return 0;
}

std::string WorldLoader::LoadDriveLine(const std::vector<uint8_t>& block)
{
    // DriveLineLoad (0x4ba9b0): its count, then per sample position, direction, distance and one more float.
    if (block.size() < 4) return "";
    uint32_t n;
    std::memcpy(&n, block.data(), 4);
    if (n > (block.size() - 4) / 32 || n > 10000) return "";
    std::vector<DriveLineSample> samples(n);
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t* const f = block.data() + 4 + 32 * k;
        DriveLineSample& s = samples[k];
        std::memcpy(&s.position, f, 12);
        s.position.w = 0.0f;
        std::memcpy(&s.direction, f + 12, 12);
        s.direction.w = 0.0f;
        std::memcpy(&s.distance, f + 24, 8);
    }
    driveLine.Load(samples.data(), n);
    world.driveLine = &driveLine;
    return "";
}

std::string WorldLoader::LoadFences(const std::vector<uint8_t>& data)
{
    // FenceLoad's physics part (0x6038c0): version 2; per group its item count, four words, its box, then per item its
    // point, centre, half size and colour; then every dummy into the grid, in order.
    size_t at = 0;
    auto take = [&](void* dst, size_t n) {
        if (at + n > data.size()) return false;
        std::memcpy(dst, data.data() + at, n);
        at += n;
        return true;
    };
    uint32_t version = 0, groups = 0;
    if (!take(&version, 4) || version != 2 || !take(&groups, 4) || !groups) return "";
    std::vector<std::array<float, 3>> points;
    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t items = 0, words[4];
        float box[6];
        if (!take(&items, 4) || !take(words, 16) || !take(box, 24)) return ".fnc: cut short";
        for (uint32_t i = 0; i < items; ++i) {
            FenceDummy* const d = Make<FenceDummy>();   // CDummyObject (0x600c30): an entity of no def, flags 0
            Init(*d, 0, nullptr, 0);
            std::array<float, 3> p;
            uint32_t colour;
            if (!take(p.data(), 12) || !take(d->centre, 12) || !take(d->halfSize, 12) || !take(&colour, 4)) return ".fnc: cut short";
            points.push_back(p);
            fences.push_back(d);
        }
    }
    EnsureGrid();
    for (size_t i = 0; i < fences.size(); ++i) {   // FenceDummyEnterGrid (0x600bb0): the cell at its point's x and y
        FenceDummy* const d = fences[i];
        d->cell = world.grid.CellAt(points[i][0], points[i][2]);
        d->cell->insert(d->cell->begin(), d);
    }
    return "";
}

namespace {
num::X87 RunEngineStep(void* ctx, float dt, float w) { return static_cast<Car*>(ctx)->engine.Step(dt, w); }
void RunBodyVelocity(void* ctx, Vec4& out)
{
    const Car& c = *static_cast<const Car*>(ctx);
    FrameMoveCopy(*c.world->world, c.body.velocity, c.frame, out, Move::Dirs);
}
} // namespace

void WorldLoader::PrepareRun()
{
    if (!car || !systems) return;
    Car& c = *car;
    c.drive.hooks.engineStep = &RunEngineStep, c.drive.hooks.ctx = &c;
    c.drive.hooks.gearLimits = (m_defs.physics.flags & kEnableDamage) != 0;   // (the gears' torque limits and wear)
    c.engine.hooks.bodyVelocity = &RunBodyVelocity, c.engine.hooks.ctx = &c, c.engine.hooks.gasketFactors = systems->gasketFactors;
    c.engine.hooks.torqueTelemetry = (m_defs.physics.debugFlags & kRenderEngineTorque) != 0;
    systems->damage.hooks.enabled = (m_defs.physics.flags & kEnableDamage) != 0;
    c.assistanceAllowed = m_options.assistanceAllowed, c.assistanceFlag = m_options.assistanceFlag;
    world.retireCar = [this](Solid& s) {   // (CarRetire: only the car has the material-200 test's car flag)
        if (Car* const car = dynamic_cast<Car*>(&s)) car->Retire(world);
    };
}

void WorldLoader::Step(uint32_t n)
{
    for (uint32_t k = 0; k < n; ++k) world.Step(m_defs.physics.maxTimeStep);
    if (car) car->drive.throttle = car->drive.clutch = 0.0f;   // (DriveScratchReset, 0x504510)
}

} // namespace phys::native
