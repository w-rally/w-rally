#include "native/run.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "data/lbs.h"
#include "data/lsp.h"
#include "data/sgc.h"
#include "data/trk.h"

namespace phys::native {

namespace {

// A file's bytes (none: a missing file - the .fnc is optional).
std::vector<uint8_t> ReadBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::vector<uint8_t>{};
}

} // namespace

std::string LoadToCar(const WorldFiles& f, LoadedWorld& w)
{
    DefsReport r;
    std::string e = LoadPhysicsLsp(data::ReadZipMember(f.rbz, "Physics/physics.lsp"), w.defs, r);
    for (const std::string& name : {"Physics/" + f.car + "/common.lsp", "Physics/" + f.car + "/" + f.surface + ".lsp"})
        if (e.empty()) e = LoadDefsLsp(data::ReadZipMember(f.rbz, name), w.defs, r);
    if (!e.empty()) return e;
    LoadOptions opt;
    opt.gameMode = (f.settings >> 3 & 1) != 0;
    if (f.settings & 0x10) opt.damageLevel = static_cast<int32_t>(f.settings >> 8 & 0xff);
    opt.assistanceAllowed = (f.settings >> 1 & 1) != 0, opt.assistanceFlag = (f.settings >> 2 & 1) != 0;
    w.loader = std::make_unique<WorldLoader>(w.defs, w.defs.physics.materials, opt);
    WorldLoader& L = *w.loader;
    if (e = L.maps.Load(ReadBytes(f.mat), f.weather); !e.empty()) return ".mat: " + e;
    // The game's order: the ground's record, the .col, the stage's events.
    StuffRecord ground;
    if (!L.StuffEnter(ground, e)) return "the ground: " + e;
    const uint32_t wetness = f.wetness >= 0 ? static_cast<uint32_t>(f.wetness) : f.weather / 3;
    w.colError = L.LoadCol(ReadBytes(f.col), wetness);   // (refused - not an R7CO file -: the game's world has none either)
    {
        const data::TrkFile t = data::ReadTrk(f.trk);
        w.trk = data::BuildTrkWorld(t);
        w.driveLine = t.driveLine;
    }
    w.baked.clear();
    for (const data::TrkMeshRecord& m : w.trk.bakedMeshes) w.baked.push_back(RecordOf(m));
    for (const data::TrkEvent& ev : w.trk.events) {
        if (ev.mesh) {
            const data::TrkMeshRecord& m = w.trk.meshes[ev.index];
            L.MeshEnter(RecordOf(m), m.w[0]);
            ++w.meshes;
            continue;
        }
        StuffRecord rec = RecordOf(w.trk.stuff[ev.index], w.baked, w.trk);
        w.baked0 += rec.meshRecord != nullptr, w.slot0 += rec.meshKind == 0 && rec.mesh == 0;
        w.rotated += rec.rotation.x != 0.0f || rec.rotation.y != 0.0f || rec.rotation.z != 0.0f;
        const uint32_t kindWas = rec.kind;
        Entity* const made = L.StuffEnter(rec, e);
        if (made && made->AsSolid()) w.stageSolids.push_back({made->AsSolid(), w.trk.stuff[ev.index].id});
        if (made && made->AsSolid() && w.defs.solids[kindWas] && made->AsSolid()->body.def != &w.defs.solids[kindWas]->body) ++w.renamed;
        if (!made && !w.refused++) w.firstRefused = e;
    }
    // The car: its damage handler, its meshes (the .sgc's), its records at the start.
    w.carMeshes = data::CarCollisionMeshes(data::ReadSgcCollision(f.sgc));
    if (w.carMeshes.size() != 8) return f.sgc + ": not the car's 8 collision nodes";
    DamageHandler* const dh = L.CarDamageHandler();
    for (const data::RecMesh& m : w.carMeshes) L.MeshEnter(RecordOf(m), m.w[0]);
    float start[3];
    float heading;
    if (f.start) {   // (the record's order: x, z, y)
        start[0] = f.startPosition[0], start[1] = f.startPosition[2], start[2] = f.startPosition[1];
        heading = f.startHeading;
    } else {
        const data::LbsScripts lbs = data::ReadLbsScripts(f.lbs);
        if (!lbs.hasCar) return f.lbs + ": no car info (the start)";
        std::memcpy(start, lbs.carPosition, sizeof start);
        heading = lbs.carAngles[1];
    }
    bool hingedOn[4], mountedOn[3];   // the parts the damage handler has on (attached; not off)
    for (int i = 0; i < 4; ++i) hingedOn[i] = dh->hinged[i].attached != 0;
    for (int i = 0; i < 3; ++i) mountedOn[i] = dh->mounted[i] != 2;
    for (StuffRecord& rec : CarRecords(start, heading, dh, hingedOn, mountedOn))
        if (!L.StuffEnter(rec, e)) return "a car record refused: " + e;
    return "";
}

std::string FinishLoad(const WorldFiles& f, LoadedWorld& w)
{
    WorldLoader& L = *w.loader;
    Defaults(w.setup), Defaults(w.options);
    DefsReport r;
    const std::string carDir = "Physics/" + f.car + "/";
    LoadSetup(data::ReadZipMember(f.rbz, f.setup.empty() ? carDir + "setups/d_" + f.surface + ".lsp" : f.setup), w.setup, r);
    LoadSetupOptions(data::ReadZipMember(f.rbz, carDir + "r_" + f.surface + ".lsp"), w.options, r);
    LoadSetupOptions(data::ReadZipMember(f.rbz, "Physics/" + f.tyres + "/tyres.lsp"), w.options, r);
    SetupFromOptions(w.setup, w.options);
    SetupAids(w.setup, f.aids);
    SetupTyres(w.setup, w.options, f.tyreIndex);
    if (std::string e = ApplySetup(w.setup, w.defs); !e.empty()) return "the setup: " + e;
    if (std::string e = L.FinishCar(); !e.empty()) return "the car's finish: " + e;
    if (std::string e = L.LoadDriveLine(w.driveLine); !e.empty()) return "the drive line: " + e;
    if (std::string e = L.LoadFences(ReadBytes(f.fnc)); !e.empty()) return "the fences: " + e;
    L.PrepareRun();
    return "";
}

std::unique_ptr<LoadedWorld> LoadWorld(const WorldFiles& f, std::string& error)
{
    auto w = std::make_unique<LoadedWorld>();
    try {   // (the files' readers throw on a missing or broken file)
        error = LoadToCar(f, *w);
        if (error.empty()) error = FinishLoad(f, *w);
    } catch (const std::exception& x) {
        error = x.what();
    }
    if (!error.empty()) w.reset();
    return w;
}

} // namespace phys::native
