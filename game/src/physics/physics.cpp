#include "physics/physics.h"

#include "app/paths.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "wrally/modules/file_host.h"
#include "signals/bus.h"
#include "signals/convert.h"
#include "wrally/iface/physics.h"

namespace wrally::physics {

using iface::Status;

struct Physics::Impl {
    modules::ModuleHost host;
    std::unique_ptr<modules::FileHost> files;
    iface::PhysicsOutput out{};
    iface::World* world = nullptr;
    signals::Bus* bus = nullptr;
    // The load's once-only records, replayed on Connect.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> infos;

    ~Impl()
    {
        if (world) out.api->destroyWorld(world);
        if (out.instance) out.api->destroy(out.instance);
    }

    iface::CarState State() const
    {
        iface::CarState s{};
        if (world) out.api->carState(world, 0, &s);
        return s;
    }

    static void WRALLY_CALL OnRecord(void* ctx, uint32_t kind, const void* data, uint32_t bytes)
    {
        Impl& m = *static_cast<Impl*>(ctx);
        if (kind == iface::records::CarInfo::kKind || kind == iface::records::EngineInfo::kKind) {
            const auto* p = static_cast<const uint8_t*>(data);
            m.infos.push_back({kind, std::vector<uint8_t>(p, p + bytes)});
        }
        if (m.bus) signals::EmitFromIface(*m.bus, kind, data, bytes);
    }
};

namespace {

// physics.rbz's Physics/, as the VFS maps it (docs/content-layers.md): the RBR physics base.
constexpr const char* kRbrBase = "physics/rbr/rbr/";

// A physics.rbz member path (Physics/...) under the RBR base, else "".
std::string RbzMember(const std::string& path)
{
    if (path.size() < 8 || (_strnicmp(path.c_str(), "Physics/", 8) != 0 && _strnicmp(path.c_str(), "Physics\\", 8) != 0))
        return "";
    std::string rest = path.substr(8);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    return kRbrBase + rest;
}

} // namespace

const std::vector<ModuleInfo>& ListModules(const std::filesystem::path& modulesDir)
{
    static std::map<std::filesystem::path, std::vector<ModuleInfo>> known;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    if (const auto it = known.find(modulesDir); it != known.end()) return it->second;
    std::vector<ModuleInfo>& out = known[modulesDir];
    modules::ModuleHost host;
    host.Scan(modulesDir);
    modules::FileHost files(nullptr, app::UserDir() / "cache", [](iface::LogLevel, const char*) {});
    for (const modules::ModuleEntry* e : host.Available(iface::ModuleKind::Physics)) {
        iface::PhysicsInput in{};
        iface::PhysicsOutput o{};
        iface::InitHeader(in, iface::ModuleKind::Physics, iface::kPhysicsInterfaceVer);
        iface::InitHeader(o, iface::ModuleKind::Physics, iface::kPhysicsInterfaceVer);
        in.host = files.Services(e->path.stem().string());
        in.config = "";
        if (host.Init(*e, &in.header, &o.header) != Status::Ok || !o.instance) continue;
        ModuleInfo m;
        m.id = o.info.id, m.name = o.info.name, m.simVersion = o.simVersion;
        for (uint32_t i = 0; i < o.formatCount && o.formats; ++i)
            m.formats.push_back({o.formats[i].name, o.formats[i].minVersion, o.formats[i].maxVersion});
        o.api->destroy(o.instance);
        if (std::none_of(out.begin(), out.end(), [&](const ModuleInfo& x) { return x.id == m.id; })) out.push_back(std::move(m));
    }
    return out;
}

Physics::Physics() = default;
Physics::~Physics() = default;

std::string Physics::Open(const std::filesystem::path& modulesDir, const std::string& moduleId, const Config& c)
{
    m = std::make_unique<Impl>();
    m->host.Scan(modulesDir);
    const std::shared_ptr<const vfs::Vfs> content =
        c.content ? c.content : std::make_shared<vfs::Vfs>(std::vector<vfs::Layer>{}, std::filesystem::path(c.root));
    m->files = std::make_unique<modules::FileHost>(content, app::UserDir() / "cache",
                                                   [this](iface::LogLevel level, const char* text) {
                                                       if (level >= iface::LogLevel::Info) m_log += text;
                                                   });

    // The first compatible physics module whose id is `moduleId`.
    std::string tried;
    for (const modules::ModuleEntry* e : m->host.Available(iface::ModuleKind::Physics)) {
        iface::PhysicsInput in{};
        iface::PhysicsOutput out{};
        iface::InitHeader(in, iface::ModuleKind::Physics, iface::kPhysicsInterfaceVer);
        iface::InitHeader(out, iface::ModuleKind::Physics, iface::kPhysicsInterfaceVer);
        in.host = m->files->Services(moduleId);
        in.config = "";
        std::string error;
        if (m->host.Init(*e, &in.header, &out.header, &error) != Status::Ok) {
            tried += "\n  " + e->path.filename().string() + ": " + error;
            continue;
        }
        if (moduleId == out.info.id) {
            m->out = out;
            break;
        }
        tried += "\n  " + e->path.filename().string() + ": id " + out.info.id;
        out.api->destroy(out.instance);
    }
    if (!m->out.instance) {
        for (const modules::ModuleEntry& e : m->host.Entries())
            if (!e.compatible && e.kind == iface::ModuleKind::Physics)
                tried += "\n  " + e.path.filename().string() + ": interface " + std::to_string(e.interfaceVer) +
                         ", the game needs " + std::to_string(iface::kPhysicsInterfaceVer);
        return m_error = "no physics module \"" + moduleId + "\" in " + modulesDir.string() + (tried.empty() ? "" : ":") + tried;
    }

    std::string overrides;
    if (!c.overrides.empty()) {
        std::ifstream f(c.overrides, std::ios::binary);
        if (!f) return m_error = "cannot open " + c.overrides;
        overrides.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    // A file or folder on disk as its VFS path; anything else is one already (Quick rally's "cars/.../physics/...").
    auto vfsPath = [&](const std::string& p) {
        std::error_code ec;
        const std::filesystem::path f(p);
        if (p.empty() || f.is_absolute() || std::filesystem::exists(f, ec)) return content->VfsPath(f);
        return vfs::NormalPath(p);
    };
    const std::string trk = vfsPath(c.trk), col = vfsPath(c.col), mat = vfsPath(c.mat), fnc = vfsPath(c.fnc),
                      lbs = vfsPath(c.lbs), sgc = vfsPath(c.sgc);
    const std::string setup = c.setup.empty() ? "" : !RbzMember(c.setup).empty() ? RbzMember(c.setup) : vfsPath(c.setup);
    // The car's physics, a folder of loose files: a physics.rbz folder (a name) under the RBR base, or a folder of its
    // own (a path).
    std::string carPhysics = c.car;
    if (carPhysics.find_first_of("/\\") == std::string::npos) {
        carPhysics = kRbrBase + carPhysics + "/";
    } else {
        carPhysics = vfsPath(carPhysics);
        if (carPhysics.back() != '/') carPhysics += '/';
    }
    std::string base = vfsPath(c.base);
    if (!base.empty() && base.back() != '/') base += '/';
    iface::CarDesc car{};
    car.physicsCar = carPhysics.c_str();
    car.carData = "";
    car.model = sgc.c_str();
    car.setup = setup.c_str();
    car.tyres = c.tyres.c_str();
    car.overrides = overrides.c_str();
    car.surface = c.surface == "gravel" ? iface::Surface::Gravel : c.surface == "snow" ? iface::Surface::Snow : iface::Surface::Tarmac;
    car.tyreType = c.tyreIndex;
    std::memcpy(car.aids, c.aids, sizeof car.aids);
    car.useStart = c.start ? 1u : 0u;
    std::memcpy(car.startPosition, c.startPosition, sizeof car.startPosition);
    car.startHeading = c.startHeading;

    iface::WorldDesc d{};
    d.stage.trk = trk.c_str(), d.stage.col = col.c_str(), d.stage.mat = mat.c_str();
    d.stage.fnc = fnc.c_str(), d.stage.lbs = lbs.c_str(), d.stage.dls = "", d.stage.physicsBase = base.c_str();
    d.stage.weather = c.weather, d.stage.wetness = c.wetness, d.stage.settings = c.settings, d.stage.flags = 0;
    d.cars = &car;
    d.carCount = 1;
    d.sink = {m.get(), &Impl::OnRecord};
    if (m->out.api->createWorld(m->out.instance, &d, &m->world) != Status::Ok)
        return m_error = m->out.api->lastError(m->out.instance);
    return "";
}

std::string Physics::ModuleName() const
{
    if (!m || !m->out.instance) return "none";
    return std::string(m->out.info.name) + " (" + m->out.info.id + ")";
}

void Physics::ApplyWords(const uint32_t words[8])
{
    if (!m || !m->world) return;
    iface::Controls c;
    std::memcpy(&c, words, sizeof c);
    m->out.api->setControls(m->world, 0, &c);
}

void Physics::Apply(const Control& c)
{
    uint32_t w[8];
    std::memcpy(w, &c, sizeof w);
    ApplyWords(w);
}

bool Physics::Step(uint32_t n)
{
    if (!m || !m->world) return false;
    if (m->out.api->step(m->world, n) != Status::Ok) {
        m_error = m->out.api->worldError(m->world);
        m->out.api->destroyWorld(m->world);   // (unusable after a fault)
        m->world = nullptr;
        return false;
    }
    return true;
}

Pose Physics::CarPose() const
{
    const iface::CarState s = m ? m->State() : iface::CarState{};
    Pose p;
    std::memcpy(p.position, s.position, 12);
    std::memcpy(p.rotation, s.rotation, 16);
    std::memcpy(p.velocity, s.velocity, 12);
    return p;
}

Dashboard Physics::Dash() const
{
    const iface::CarState s = m ? m->State() : iface::CarState{};
    return {s.speed, s.rpm, s.gear};
}

void Physics::Wheels(Wheel out[4]) const
{
    const iface::CarState s = m ? m->State() : iface::CarState{};
    for (int k = 0; k < 4; ++k) {
        std::memcpy(out[k].position, s.wheels[k].position, 12);
        std::memcpy(out[k].rotation, s.wheels[k].rotation, 16);
        out[k].spin = s.wheels[k].spin;
    }
}

void Physics::Connect(signals::Bus* bus)
{
    if (!m) return;
    m->bus = bus;
    if (bus)
        for (const auto& [kind, bytes] : m->infos)
            signals::EmitFromIface(*bus, kind, bytes.data(), static_cast<uint32_t>(bytes.size()));
}

void Physics::CenterOfGravity(float out[3]) const
{
    std::memset(out, 0, 12);
    if (m && m->world) m->out.api->centerOfGravity(m->world, 0, out);
}

std::vector<Box> Physics::Water() const
{
    std::vector<Box> out;
    if (!m || !m->world) return out;
    std::vector<iface::Box> w(m->out.api->water(m->world, nullptr, 0));
    m->out.api->water(m->world, w.data(), static_cast<uint32_t>(w.size()));
    for (const iface::Box& b : w) {
        Box o;
        std::memcpy(o.lo, b.lo, 12);
        std::memcpy(o.hi, b.hi, 12);
        out.push_back(o);
    }
    return out;
}

const std::vector<Body>& Physics::Bodies()
{
    if (!m || !m->world) return m_bodies;
    std::vector<iface::BodyState> b(m->out.api->bodies(m->world, nullptr, 0));
    m->out.api->bodies(m->world, b.data(), static_cast<uint32_t>(b.size()));
    m_bodies.resize(b.size());
    for (size_t i = 0; i < b.size(); ++i) {
        Body& o = m_bodies[i];
        o.id = b[i].id;
        std::memcpy(o.startPosition, b[i].startPosition, 12);
        std::memcpy(o.startRotation, b[i].startRotation, 16);
        std::memcpy(o.position, b[i].position, 12);
        std::memcpy(o.rotation, b[i].rotation, 16);
    }
    return m_bodies;
}

void Physics::Parts(std::vector<Part>& out) const
{
    out.clear();
    if (!m || !m->world) return;
    iface::PartState buf[16];
    const uint32_t n = std::min<uint32_t>(m->out.api->carParts(m->world, 0, buf, 16), 16);
    for (uint32_t i = 0; i < n; ++i) {
        const iface::PartState& s = buf[i];
        Part p{};
        std::memcpy(p.name, s.name, sizeof p.name);
        p.name[sizeof p.name - 1] = 0;
        p.state = s.state;
        p.inCar = s.inCar != 0;
        std::memcpy(p.position, s.position, 12);
        std::memcpy(p.rotation, s.rotation, 16);
        std::memcpy(p.restPosition, s.restPosition, 12);
        std::memcpy(p.restRotation, s.restRotation, 16);
        out.push_back(p);
    }
}

void Physics::PoseWords(uint32_t out[13]) const
{
    const iface::CarState s = m ? m->State() : iface::CarState{};
    std::memcpy(out, s.rotation, 16);
    std::memcpy(out + 4, s.position, 12);
    std::memcpy(out + 7, s.angularVelocity, 12);
    std::memcpy(out + 10, s.velocity, 12);
}

float Physics::StageDistance() const { return m ? m->State().stageDistance : 0.0f; }
float Physics::Dt() const { return m && m->world ? m->out.api->dt(m->world) : 0.0f; }
uint32_t Physics::Tick() const { return m && m->world ? m->out.api->tick(m->world) : 0; }

} // namespace wrally::physics
