#include "sim/native_sim.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include "native/run.h"
#include "signals/car.h"

namespace phys::sim {

void PrintFaultStack(const void* context);   // sim.cpp

namespace {

// The steps under SEH (a fault is reported, as the classic Sim's: Error(); PHYS_SIM_STACK=1 its stack to stderr).
DWORD g_fault;
void* g_faultAddress;
int Filter(EXCEPTION_POINTERS* ep)
{
    g_fault = ep->ExceptionRecord->ExceptionCode;
    g_faultAddress = ep->ExceptionRecord->ExceptionAddress;
    if (const char* s = std::getenv("PHYS_SIM_STACK"); s && *s) {
        std::fprintf(stderr, "fault %08lx at %p, data %p:\n", g_fault, ep->ExceptionRecord->ExceptionAddress,
                     ep->ExceptionRecord->NumberParameters >= 2 ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]) : nullptr);
        PrintFaultStack(ep->ContextRecord);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
bool RunStep(native::WorldLoader& L, uint32_t n)
{
    g_fault = 0;
    __try {
        L.Step(n);
    } __except (Filter(GetExceptionInformation())) {
    }
    return g_fault == 0;
}

// The cars' gear hooks (DriveHooks::gearChange, gearOut: their context the car, the engine step's) find their NativeSim here.
std::vector<std::pair<const void*, NativeSim*>> g_gearSims;

// Config::overrides (sim.cpp's CarOverride on the native car's defs): the text, `path` its name in messages. Returns ""
// or what is wrong; `log` gets each change.
std::string CarOverride(const std::string& path, const std::string& text, native::Car& car, std::string& log)
{
    std::istringstream f(text);
    struct Field {
        const char* name;
        float* (*at)(native::Car&);
    };
    static const Field kFields[] = {
        {"Engine.IdleThrottle", [](native::Car& c) { return &const_cast<native::EngineDef*>(c.engine.def)->idleThrottle; }},
    };
    std::string line;
    for (int n = 1; std::getline(f, line); ++n) {
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream in(line);
        std::string name, eq;
        float v;
        if (!(in >> name)) continue;
        if (!(in >> eq >> v) || eq != "=") return path + ":" + std::to_string(n) + ": expected `Name = number`";
        const Field* fd = nullptr;
        for (const Field& k : kFields)
            if (name == k.name) fd = &k;
        if (!fd) return path + ":" + std::to_string(n) + ": no value `" + name + "`";
        float* const at = fd->at(car);
        char t[160];
        std::snprintf(t, sizeof t, "override %s: %g -> %g\n", name.c_str(), double(*at), double(v));
        log += t;
        *at = v;
    }
    return "";
}

} // namespace

NativeSim::NativeSim() = default;

NativeSim::~NativeSim()
{
    g_gearSims.erase(std::remove_if(g_gearSims.begin(), g_gearSims.end(), [&](const auto& p) { return p.second == this; }),
                     g_gearSims.end());
}

std::string NativeSim::Open(const Config& c)
{
    m_error.clear();
    if (m_world) return m_error = "phys-sim: already open";
    if (!c.anims.empty()) return m_error = "the native physics: no animation objects (spectators) yet";
    native::WorldFiles f;
    f.rbz = c.rbz, f.trk = c.trk, f.col = c.col, f.mat = c.mat, f.fnc = c.fnc, f.lbs = c.lbs, f.sgc = c.sgc;
    f.car = c.car, f.surface = c.surface, f.tyres = c.tyres, f.setup = c.setup;
    f.tyreIndex = c.tyreIndex, f.settings = c.settings | 1, f.weather = c.weather, f.wetness = c.wetness;
    std::memcpy(f.aids, c.aids, sizeof f.aids);
    f.start = c.start, f.startHeading = c.startHeading;
    std::memcpy(f.startPosition, c.startPosition, sizeof f.startPosition);
    std::string e;
    m_world = native::LoadWorld(f, e);
    if (!m_world) return m_error = e.empty() ? "the native load failed" : e;
    native::LoadedWorld& w = *m_world;
    char line[256];
    std::snprintf(line, sizeof line, "native physics: %zu entities, %u stage meshes, %zu stage solids%s%s\n", w.loader->entities.size(),
                  w.meshes, w.stageSolids.size(), w.colError.empty() ? "" : "; no stage collision: ", w.colError.c_str());
    m_log += line;
    for (const native::LoadedWorld::StageSolid& s : w.stageSolids) {
        const native::RigidBody& rb = s.solid->body;
        Body b{};
        b.id = s.id;
        std::memcpy(b.startRotation, &rb.orientation, 16);
        std::memcpy(b.startPosition, &rb.position, 12);
        std::memcpy(b.rotation, b.startRotation, 16);
        std::memcpy(b.position, b.startPosition, 12);
        if (const native::CollisionMesh* m = s.solid->mesh) {
            std::memcpy(b.lo, &m->boundsMin, 12);
            std::memcpy(b.hi, &m->boundsMax, 12);
            b.hasMesh = m->vertexCount != 0;
        }
        m_bodies.push_back(b);
    }
    // The car's parts that can come loose: named by their def's kind (load.cpp's CarRecords: 0x25 the left door, 0x26
    // the right, 0x28 the bonnet, 0x2c the exhaust pipe hinged; 0x2a the front bumper, 0x29 the rear, 0x2b the wing
    // mounted) - the .sgc's collision mesh of each (data/sgc.h: c_e_<name>).
    {
        static const struct {
            uint32_t kind;
            bool hinged;
            const char* name;
        } kParts[] = {{0x25, true, "door_l"},   {0x26, true, "door_r"},   {0x28, true, "bonnet"}, {0x2c, true, "exhaust"},
                      {0x2a, false, "bumper_f"}, {0x29, false, "bumper_b"}, {0x2b, false, "wing"}};
        const native::Car& car = *w.loader->car;
        auto add = [&](const native::Solid* s, bool hinged) {
            if (!s) return;
            for (const auto& k : kParts) {
                const bool match = hinged ? (w.defs.hinged[k.kind] && &w.defs.hinged[k.kind]->body == s->body.def)
                                          : (w.defs.mounted[k.kind] && &w.defs.mounted[k.kind]->body == s->body.def);
                if (k.hinged != hinged || !match) continue;
                PartRef p{s, hinged, k.name, {}, {}};
                std::memcpy(p.restPosition, &s->body.position, 12);
                std::memcpy(p.restRotation, &s->body.orientation, 16);
                m_parts.push_back(p);
                return;
            }
        };
        for (const native::Solid* s : car.hinged) add(s, true);
        for (const native::MountedSolid* m : car.mounted) add(m, false);
    }
    if (!c.overrides.empty()) {
        std::string changes;
        if (std::string oe = CarOverride(c.overridesName, c.overrides, *w.loader->car, changes); !oe.empty()) {
            m_world.reset();
            return m_error = "the car's overrides: " + oe;
        }
        m_log += changes;
    }
    m_surface = c.surface;
    return "";
}

void NativeSim::ApplyWords(const uint32_t words[8])
{
    if (!m_world) return;
    native::ControlInput in;
    std::memcpy(&in.throttle, words + 0, 4);
    std::memcpy(&in.brake, words + 1, 4);
    std::memcpy(&in.handbrake, words + 2, 4);
    std::memcpy(&in.clutch, words + 3, 4);
    std::memcpy(&in.steering, words + 4, 4);
    in.flags = words[5];
    in.gear = static_cast<int32_t>(words[6]);
    in.starter = words[7];
    m_world->loader->car->Control(in);
}

void NativeSim::Apply(const Control& c)
{
    uint32_t w[8];
    std::memcpy(w, &c, 32);
    ApplyWords(w);
}

bool NativeSim::Step(uint32_t n)
{
    if (!m_world) return false;
    if (!RunStep(*m_world->loader, n)) {
        char t[96];
        std::snprintf(t, sizeof t, "the step faulted (exception %08lx at %p)", g_fault, g_faultAddress);
        m_error = t;
        m_world.reset();   // (unusable, as the classic Sim after a fault)
        return false;
    }
    if (m_bus) EmitTicks(n);
    return true;
}

void NativeSim::GearChanged(void* ctx, const native::GearChange& g)
{
    for (const auto& [car, sim] : g_gearSims) {
        if (car != ctx || !sim->m_bus) continue;
        signals::GearChange r;
        r.stamp = {sim->m_busCar, sim->Tick()};
        r.gear = static_cast<int32_t>(g.gear) - 1;
        r.level = g.level;
        r.engaged = g.engaged != 0;
        sim->m_bus->Emit(r);
    }
}

// The gear thrown out (the classic's 0x5079b0, GearChangeNotify: the gear-change record with level 0).
void NativeSim::GearOut(void* ctx)
{
    const native::Car& c = *static_cast<const native::Car*>(ctx);
    GearChanged(ctx, native::GearChange{c.drive.carId, c.drive.state->gear, 0.0f, c.drive.state->engaged});
}

void NativeSim::Connect(signals::Bus* bus, uint32_t car)
{
    m_bus = bus;
    m_busCar = car;
    if (!m_world) return;
    native::WorldLoader& L = *m_world->loader;
    native::Car& c = *L.car;
    g_gearSims.erase(std::remove_if(g_gearSims.begin(), g_gearSims.end(), [&](const auto& p) { return p.second == this; }),
                     g_gearSims.end());
    if (bus) g_gearSims.push_back({c.drive.hooks.ctx, this});
    c.drive.hooks.gearChange = bus ? &GearChanged : nullptr;
    c.drive.hooks.gearOut = bus ? &GearOut : nullptr;
    L.world.dampingCollision = nullptr;
    if (!bus) return;
    L.world.dampingCollision = [this](native::Solid&, const native::World::DampingCollision& d) {
        signals::Collision r;
        r.stamp = {m_busCar, Tick()};
        r.depth = d.depth;
        r.material = d.material;
        std::memcpy(r.point, &d.point, 12);
        std::memcpy(r.velocity, &d.velocity, 12);
        m_bus->Emit(r);
    };
    signals::CarInfo info;
    info.stamp = {car, Tick()};
    info.surface = m_surface == "gravel" ? signals::CarInfo::kGravel : m_surface == "snow" ? signals::CarInfo::kSnow : signals::CarInfo::kTarmac;
    bus->Emit(info);
    EmitEngineInfo();
}

void NativeSim::EmitEngineInfo()
{
    const native::Car& car = *m_world->loader->car;
    const native::EngineDef& D = *car.engine.def;
    signals::EngineInfo r;
    r.stamp = {m_busCar, Tick()};
    r.cylinders = std::min<uint32_t>(D.cylinderCount, 4);
    for (uint32_t i = 0; i < 4; ++i) r.phaseOffset[i] = D.cylinders[i].phaseOffset & 0xff;
    r.rpmLimit = car.controlUnit.def->rpmLimit;
    r.idleThrottle = D.idleThrottle;
    r.turbo = D.turbo != 0;
    r.dt = Dt();
    m_bus->Emit(r);
}

// The ...Tick records after a step of `ticks` (sim.cpp's EmitTicks from the native car).
void NativeSim::EmitTicks(uint32_t ticks)
{
    const native::Car& car = *m_world->loader->car;
    const signals::Stamp st{m_busCar, Tick()};
    if (m_bus->Heard<signals::EngineTick>()) {
        const native::Engine& en = car.engine;
        const native::EngineState& E = *en.state;
        signals::EngineTick r;
        r.stamp = st;
        r.ticks = ticks;
        r.crankSpeed = E.crankSpeed;
        r.crankAngle = E.crankAngle;
        r.throttle = en.throttle;
        r.easedThrottle = en.easedThrottle;
        r.torque = E.torque;
        r.power = E.power;
        for (uint32_t i = 0; i < 4; ++i) {
            r.fired |= (E.noise.fired[i] ? 1u : 0u) << i;
            r.sparks |= (E.sparks[i] ? 1u : 0u) << i;
            r.chargeMass[i] = E.cylinders[i].chargeMass;
        }
        r.misfire = E.noise.misfire;
        r.boost = E.noise.boost;
        r.turboSpeed = E.turboSpeed;
        r.wastegateOpening = E.noise.wastegateOpening;
        r.intakePressure = E.intake[2];
        r.exhaustPressure = E.exhaust[2];
        r.starter = car.drive.starterLevel;
        r.blockTemperature = E.blockTemperature;
        m_bus->Emit(r);
    }
    if (m_bus->Heard<signals::DriveTick>()) {
        const native::Drive& dr = car.drive;
        const native::DriveState& S = *dr.state;
        signals::DriveTick r;
        r.stamp = st;
        r.ticks = ticks;
        r.gear = static_cast<int32_t>(S.gear) - 1;
        r.clutch = dr.clutch;
        r.clutchLocked = S.clutchLock != 0;
        r.clutchSlip = dr.slipSpeed;
        r.clutchTorque = dr.clutchTorque;
        r.outputSpeed = dr.outputSpeed;
        r.engineTorque = dr.engineTorque;
        r.shiftBlend = S.clutchBlend;
        r.brake = dr.brake;
        r.handbrake = dr.handbrake;
        m_bus->Emit(r);
    }
    if (m_bus->Heard<signals::WheelsTick>()) {
        signals::WheelsTick r;
        r.stamp = st;
        r.ticks = ticks;
        r.radius = car.carDef->effWheelRadius;
        for (int k = 0; k < 4; ++k) {
            if (!car.wheels[k]) continue;
            const native::Wheel& w = *car.wheels[k];
            signals::WheelsTick::Wheel& o = r.wheels[k];
            o.spinRate = w.contact.spinRate;
            std::memcpy(o.contactVelocity, &w.contact.contactVelocity, 12);
            o.onGround = w.onGround != 0;
            o.travel = w.suspension.travel;
            o.travelRate = w.suspension.travelRate;
            o.surfaceA = w.contact.surfaceA;
            o.surfaceB = w.contact.surfaceB;
            o.surfaceBlend = w.contact.surfaceBlend;
            std::memcpy(o.force, &w.contact.force, 12);
            // (as sim.cpp's: the smaller difference of the roll's two signs against the spin is the slip)
            const auto &v = w.contact.contactVelocity, &roll = w.contact.rollDir, &axle = w.contact.hubAxisWorld;
            const float along = v.x * roll.x + v.y * roll.y + v.z * roll.z, across = v.x * axle.x + v.y * axle.y + v.z * axle.z;
            o.rollingSpeed = o.spinRate * r.radius;
            o.slipLongitudinal = std::min(std::fabs(along - o.rollingSpeed), std::fabs(along + o.rollingSpeed));
            o.slipLateral = std::fabs(across);
        }
        m_bus->Emit(r);
    }
    if (m_bus->Heard<signals::BodyTick>()) {
        const native::RigidBody& b = car.body;
        signals::BodyTick r;
        r.stamp = st;
        r.ticks = ticks;
        std::memcpy(r.position, &b.position, 12);
        std::memcpy(r.rotation, &b.orientation, 16);
        std::memcpy(r.velocity, &b.velocity, 12);
        std::memcpy(r.angularVelocity, &b.angularVelocity, 12);
        std::memcpy(r.feltAcceleration, &car.carState.feltAcceleration, 12);
        r.speed = car.carState.speed;
        m_bus->Emit(r);
    }
    m_bus->Emit(signals::StepEnd{st, ticks});
}

Pose NativeSim::CarPose() const
{
    Pose p{};
    if (!m_world) return p;
    const native::RigidBody& b = m_world->loader->car->body;
    std::memcpy(p.rotation, &b.orientation, 16);
    std::memcpy(p.position, &b.position, 12);
    std::memcpy(p.velocity, &b.velocity, 12);
    return p;
}

const std::vector<Body>& NativeSim::Bodies()
{
    if (m_world)
        for (size_t i = 0; i < m_bodies.size(); ++i) {
            const native::RigidBody& b = m_world->stageSolids[i].solid->body;
            std::memcpy(m_bodies[i].rotation, &b.orientation, 16);
            std::memcpy(m_bodies[i].position, &b.position, 12);
        }
    return m_bodies;
}

std::vector<Part> NativeSim::Parts() const
{
    std::vector<Part> out;
    if (!m_world) return out;
    for (const PartRef& r : m_parts) {
        Part p;
        p.name = r.name;
        const auto& part = static_cast<const native::PartSolid&>(*r.solid);
        p.inCar = part.car != nullptr;   // (a torn-off part's frame is the world's: World::ReleasePart)
        const uint32_t stick = r.hinged ? 0 : static_cast<const native::MountedSolid&>(part).Stick();
        p.state = !p.inCar ? 2 : stick == 1 ? 1 : 0;
        std::memcpy(p.position, &part.body.position, 12);
        std::memcpy(p.rotation, &part.body.orientation, 16);
        std::memcpy(p.restPosition, r.restPosition, 12);
        std::memcpy(p.restRotation, r.restRotation, 16);
        out.push_back(p);
    }
    return out;
}

Dashboard NativeSim::Dash() const
{
    Dashboard d{};
    if (!m_world) return d;
    const native::Car& car = *m_world->loader->car;
    const native::Vec4& v = car.body.velocity;
    d.speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    d.rpm = car.drive.state->engineSpeed * 9.5492966f;
    d.gear = static_cast<int>(car.drive.state->gear) - 1;
    return d;
}

void NativeSim::Wheels(Wheel out[4]) const
{
    std::memset(out, 0, 4 * sizeof(Wheel));
    if (!m_world) return;
    const native::Car& car = *m_world->loader->car;
    for (int k = 0; k < 4; ++k) {
        const native::Wheel* w = car.wheels[k];
        if (!w) continue;
        std::memcpy(out[k].position, &w->body.position, 12);
        std::memcpy(out[k].rotation, &w->body.orientation, 16);
        out[k].spin = w->suspension.spinAngle;
    }
}

std::vector<Physics::Box> NativeSim::Water() const
{
    std::vector<Box> out;
    if (!m_world) return out;
    for (const native::Water* e : m_world->loader->water) {
        Box b;
        std::memcpy(b.lo, &e->boundsMin, 12);
        std::memcpy(b.hi, &e->boundsMax, 12);
        out.push_back(b);
    }
    return out;
}

void NativeSim::CenterOfGravity(float out[3]) const
{
    std::memset(out, 0, 12);
    if (!m_world) return;
    if (const auto* body = dynamic_cast<const native::CarBody*>(m_world->loader->car->carBody)) std::memcpy(out, &body->centerOfGravity, 12);
}

void NativeSim::PoseWords(uint32_t out[13]) const
{
    std::memset(out, 0, 13 * 4);
    if (!m_world) return;
    const native::RigidBody& b = m_world->loader->car->body;
    std::memcpy(out, &b.orientation, 16);
    std::memcpy(out + 4, &b.position, 12);
    std::memcpy(out + 7, &b.angularVelocity, 12);
    std::memcpy(out + 10, &b.velocity, 12);
}

float NativeSim::Dt() const { return m_world ? m_world->defs.physics.maxTimeStep : 0.0f; }

float NativeSim::StageDistance() const
{
    return m_world ? m_world->loader->driveLine.Distance(m_world->loader->car->driveLineDistance) : 0.0f;
}

uint32_t NativeSim::Tick() const { return m_world ? m_world->loader->world.tick : 0; }

} // namespace phys::sim
