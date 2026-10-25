// WPhysicsRBR.dll - the native RBR physics (sim/native_sim.h) behind wrally/iface/physics.h (docs/design.md 4.2).
//
// A thin adapter: WorldDesc becomes NativeSim's Config, the control records and steps pass straight through, CarState
// is read from NativeSim's queries (the wheels' contact data from the step's WheelsTick), and the records NativeSim
// emits on its bus go to the game's RecordSink as wrally/iface/records.h's (signals/convert.h).
//
// Files: every read goes through the game's HostServices::files (data/files.h's reader), so the paths in WorldDesc are
// the game's VFS paths.
//
// One World at a time: NativeSim keeps its gear hooks and the file reader process-wide.

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "data/files.h"
#include "signals/bus.h"
#include "signals/convert.h"
#include "sim/native_sim.h"
#include "wrally/iface/physics.h"

using namespace wrally::iface;

// The interface's opaque handles, defined.
namespace wrally::iface {

struct PhysicsModule {
    HostServices host;
    std::string error;
    bool worldOpen = false;
};

struct World {
    PhysicsModule* module = nullptr;
    phys::sim::NativeSim sim;
    signals::Bus bus;
    RecordSink sink{};
    records::WheelsTick wheels{};   // the last step's, for CarState
    std::string error;
};

} // namespace wrally::iface

namespace {

constexpr uint32_t kModuleVersion = MakeVersion(0, 1, 0);

void Log(PhysicsModule& m, LogLevel level, const std::string& text)
{
    if (m.host.log) m.host.log(m.host.ctx, level, text.c_str());
}

// The game's VFS as the physics' file reader.
void InstallReader(PhysicsModule& m)
{
    const HostServices host = m.host;
    phys::data::SetFileReader([host](const std::string& path, std::vector<uint8_t>& out) {
        Blob b{};
        if (host.files.read(host.ctx, path.c_str(), &b) != Status::Ok) return false;
        const auto* p = static_cast<const uint8_t*>(b.data);
        out.assign(p, p + b.size);
        host.files.release(host.ctx, &b);
        return true;
    });
}

std::string Str(const char* s) { return s ? s : ""; }

// The bus's records to the game's sink, converted.
template <class T> void Forward(World& w)
{
    w.bus.Connect<T>([&w](const T& r) {
        const auto o = signals::ToIface(r);
        w.sink.onRecord(w.sink.ctx, T::kKind, &o, static_cast<uint32_t>(sizeof o));
    });
}

const char* WRALLY_CALL LastError(PhysicsModule* m) { return m ? m->error.c_str() : "no module"; }

void WRALLY_CALL Destroy(PhysicsModule* m)
{
    if (m) phys::data::SetFileReader(nullptr);
    delete m;
}

Status WRALLY_CALL CreateWorld(PhysicsModule* m, const WorldDesc* d, World** out)
{
    if (out) *out = nullptr;
    if (!m) return Status::ErrInvalidArg;
    m->error.clear();
    if (!d || !out) return m->error = "createWorld: null argument", Status::ErrInvalidArg;
    if (m->worldOpen) return m->error = "one world at a time", Status::ErrNotSupported;
    if (d->carCount != 1 || !d->cars) return m->error = "exactly one car (no kPhysicsCapMultiCar)", Status::ErrNotSupported;
    if (d->stage.flags & kStageSpectators) return m->error = "no animation objects (spectators) yet", Status::ErrNotSupported;
    const CarDesc& car = d->cars[0];
    if (Str(car.physicsCar).empty()) return m->error = "an RBR car only (CarDesc::physicsCar)", Status::ErrNotSupported;

    phys::sim::Config c;
    c.rbz = Str(d->stage.rbz);
    c.trk = Str(d->stage.trk), c.col = Str(d->stage.col), c.mat = Str(d->stage.mat);
    c.fnc = Str(d->stage.fnc), c.lbs = Str(d->stage.lbs);
    c.weather = d->stage.weather, c.wetness = d->stage.wetness, c.settings = d->stage.settings;
    c.car = Str(car.physicsCar);
    c.sgc = Str(car.model);
    c.setup = Str(car.setup);
    if (!Str(car.tyres).empty()) c.tyres = car.tyres;
    c.overrides = Str(car.overrides);
    c.overridesName = "the car's overrides";
    c.surface = car.surface == Surface::Gravel ? "gravel" : car.surface == Surface::Snow ? "snow" : "tarmac";
    c.tyreIndex = car.tyreType;
    std::memcpy(c.aids, car.aids, sizeof c.aids);
    c.start = car.useStart != 0;
    std::memcpy(c.startPosition, car.startPosition, sizeof c.startPosition);
    c.startHeading = car.startHeading;

    auto w = std::make_unique<World>();
    w->module = m;
    w->sink = d->sink;
    try {
        if (const std::string e = w->sim.Open(c); !e.empty()) return m->error = e, Status::ErrFormat;
    } catch (const std::exception& e) {
        return m->error = e.what(), Status::ErrIo;
    }
    if (const std::string log = w->sim.Log(); !log.empty()) Log(*m, LogLevel::Info, log);

    World& ref = *w;
    ref.bus.Connect<signals::WheelsTick>([&ref](const signals::WheelsTick& r) { ref.wheels = signals::ToIface(r); });
    if (ref.sink.onRecord) {
        Forward<signals::CarInfo>(ref);
        Forward<signals::EngineInfo>(ref);
        Forward<signals::EngineTick>(ref);
        Forward<signals::DriveTick>(ref);
        Forward<signals::WheelsTick>(ref);
        Forward<signals::BodyTick>(ref);
        Forward<signals::StepEnd>(ref);
        Forward<signals::GearChange>(ref);
        Forward<signals::Collision>(ref);
    }
    ref.sim.Connect(&ref.bus, 0);   // CarInfo and EngineInfo now
    m->worldOpen = true;
    *out = w.release();
    return Status::Ok;
}

void WRALLY_CALL DestroyWorld(World* w)
{
    if (!w) return;
    w->module->worldOpen = false;
    delete w;
}

const char* WRALLY_CALL WorldError(World* w) { return w ? w->error.c_str() : "no world"; }
float WRALLY_CALL Dt(World* w) { return w ? w->sim.Dt() : 0.0f; }
uint32_t WRALLY_CALL Tick(World* w) { return w ? w->sim.Tick() : 0; }

Status WRALLY_CALL SetControls(World* w, uint32_t car, const Controls* c)
{
    if (!w || !c || car != 0) return Status::ErrInvalidArg;
    uint32_t words[8];
    std::memcpy(words, c, sizeof words);
    w->sim.ApplyWords(words);
    return Status::Ok;
}

Status WRALLY_CALL Step(World* w, uint32_t ticks)
{
    if (!w) return Status::ErrInvalidArg;
    if (!w->sim.Step(ticks)) return w->error = w->sim.Error(), Status::ErrFault;
    return Status::Ok;
}

Status WRALLY_CALL GetCarState(World* w, uint32_t car, CarState* out)
{
    if (!w || !out || car != 0) return Status::ErrInvalidArg;
    *out = {};
    const phys::sim::Pose p = w->sim.CarPose();
    std::memcpy(out->position, p.position, 12);
    std::memcpy(out->rotation, p.rotation, 16);
    std::memcpy(out->velocity, p.velocity, 12);
    uint32_t pose[13];
    w->sim.PoseWords(pose);
    std::memcpy(out->angularVelocity, pose + 7, 12);
    phys::sim::Wheel wheels[4];
    w->sim.Wheels(wheels);
    for (int k = 0; k < 4; ++k) {
        WheelState& o = out->wheels[k];
        std::memcpy(o.position, wheels[k].position, 12);
        std::memcpy(o.rotation, wheels[k].rotation, 16);
        o.spin = wheels[k].spin;
        const auto& t = w->wheels.wheels[k];
        o.spinRate = t.spinRate;
        o.travel = t.travel;
        o.onGround = t.onGround;
        o.surface = t.surfaceA;
        o.slipLongitudinal = t.slipLongitudinal;
        o.slipLateral = t.slipLateral;
    }
    const phys::sim::Dashboard dash = w->sim.Dash();
    out->speed = dash.speed;
    out->rpm = dash.rpm;
    out->gear = dash.gear;
    out->steeringTorque = 0;   // not read out of the native car yet
    out->stageDistance = w->sim.StageDistance();
    out->damaged = 0;          // likewise
    return Status::Ok;
}

Status WRALLY_CALL CenterOfGravity(World* w, uint32_t car, float* out)
{
    if (!w || !out || car != 0) return Status::ErrInvalidArg;
    w->sim.CenterOfGravity(out);
    return Status::Ok;
}

uint32_t WRALLY_CALL Bodies(World* w, BodyState* out, uint32_t capacity)
{
    if (!w) return 0;
    const std::vector<phys::sim::Body>& bodies = w->sim.Bodies();
    for (uint32_t i = 0; out && i < capacity && i < bodies.size(); ++i) {
        const phys::sim::Body& b = bodies[i];
        BodyState& o = out[i];
        o.id = b.id;
        std::memcpy(o.startPosition, b.startPosition, 12);
        std::memcpy(o.startRotation, b.startRotation, 16);
        std::memcpy(o.position, b.position, 12);
        std::memcpy(o.rotation, b.rotation, 16);
    }
    return static_cast<uint32_t>(bodies.size());
}

uint32_t WRALLY_CALL Water(World* w, Box* out, uint32_t capacity)
{
    if (!w) return 0;
    const std::vector<phys::sim::Physics::Box> water = w->sim.Water();
    for (uint32_t i = 0; out && i < capacity && i < water.size(); ++i) {
        std::memcpy(out[i].lo, water[i].lo, 12);
        std::memcpy(out[i].hi, water[i].hi, 12);
    }
    return static_cast<uint32_t>(water.size());
}

// Putting the car back on the road is done by the game reopening the world at a start (phys-drive's R), for now.
Status WRALLY_CALL ResetCar(World* w, uint32_t, const ResetDesc*)
{
    if (w) w->error = "resetCar: not in the native physics yet";
    return Status::ErrNotSupported;
}
Status WRALLY_CALL SaveState(World*, Blob* out)
{
    if (out) *out = {};
    return Status::ErrNotSupported;
}
void WRALLY_CALL ReleaseState(World*, Blob*) {}
Status WRALLY_CALL LoadState(World*, const void*, uint64_t) { return Status::ErrNotSupported; }

const PhysicsApi kApi = {&Destroy, &LastError, &CreateWorld, &DestroyWorld, &WorldError, &Dt, &Tick,
                         &SetControls, &Step, &GetCarState, &CenterOfGravity, &Bodies, &Water, &ResetCar,
                         &SaveState, &ReleaseState, &LoadState};

} // namespace

WRALLY_EXPORT uint32_t WRALLY_CALL GetWInterfaceVer(ModuleKind kind)
{
    return kind == ModuleKind::Physics ? kPhysicsInterfaceVer : 0;
}

WRALLY_EXPORT Status WRALLY_CALL InitWInterface(uint32_t gameVersion, const StructHeader* in, StructHeader* out)
{
    if (!in || !out) return Status::ErrInvalidArg;
    if (in->kind != ModuleKind::Physics || out->kind != ModuleKind::Physics) return Status::ErrKind;
    if (in->interfaceVer != kPhysicsInterfaceVer || out->interfaceVer != kPhysicsInterfaceVer) return Status::ErrVersion;
    if (in->size != sizeof(PhysicsInput) || out->size != sizeof(PhysicsOutput)) return Status::ErrStructSize;
    if (gameVersion < MakeVersion(0, 1, 0)) return Status::ErrGameVersion;
    try {
        const auto& input = *reinterpret_cast<const PhysicsInput*>(in);
        auto& output = *reinterpret_cast<PhysicsOutput*>(out);
        auto m = std::make_unique<PhysicsModule>();
        m->host = input.host;
        if (!m->host.files.read || !m->host.files.release) return Status::ErrInvalidArg;
        InstallReader(*m);

        output.info = {};
        std::snprintf(output.info.id, sizeof output.info.id, "rbr");
        std::snprintf(output.info.name, sizeof output.info.name, "RBR original");
        output.info.moduleVersion = kModuleVersion;
        output.info.minGameVersion = MakeVersion(0, 1, 0);
        output.info.buildHash = WPHYSICS_RBR_BUILD_HASH;
        output.info.caps = kPhysicsCapRbrCars | kPhysicsCapRbrStages | kPhysicsCapCarOverrides;
        output.api = &kApi;
        output.instance = m.release();
        return Status::Ok;
    } catch (...) {
        return Status::ErrInternal;
    }
}
