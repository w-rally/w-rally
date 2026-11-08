// catalog-check: what the Quick rally menu offers (assets::Catalog) from an RBR folder, checked against what it must
// hold - every RBR stage, the ones no .ngg draws imported from their .lbs with their weathers drawn, but no protected
// (encrypted) one, and the TrackSettings.ini weathers of a map under 10 found under their two-digit names ("04E_...");
// every slot car with RBR's physics (its variant "rbr") a module reading rbr-lsp 1 drives, as the default. A car with
// more physics than RBR's (a download's) is listed with them. A map's .ngg holding its stages (NGGR/NGGA): each of its
// stages offered once, standing for the times of day the file made one - none of those offered again - with all their
// weathers, and in the track of the map's place (rbr-tracks.yaml: a reversed stage is a stage of the same track).
//
//   catalog-check <RBR folder> [<.ngg folders>]
//
// Prints the counts and "catalog-check: all good", or what is wrong and exits 1.

#include <cstdio>
#include <cstring>
#include <string>

#include "app/settings.h"
#include "assets/catalog.h"
#include "formats/lbs_geometry.h"

namespace {

const wrally::assets::Stage* Find(const wrally::assets::Catalog& c, const char* stage)
{
    for (const wrally::assets::Stage& s : c.stages)
        if (!_stricmp(s.stage.c_str(), stage)) return &s;
    return nullptr;
}

int Fail(const std::string& why)
{
    std::printf("catalog-check: %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return std::printf("usage: catalog-check <RBR folder> [<.ngg folders>]\n"), 2;
    wrally::app::Settings s;
    s.rbrPath = argv[1];
    if (argc > 2) s.nggFolders = argv[2];
    wrally::assets::Catalog c;
    c.physicsModules = {{"rbr", "RBR original", 1, {{"rbr-lsp", 1, 1}}}};   // (WPhysicsRBR's answer, without loading it)
    if (const std::string e = c.Scan(s); !e.empty()) return Fail(e);
    for (const wrally::assets::Car& car : c.cars) {
        if (car.slot >= 0) {
            const wrally::assets::PhysicsVariant* p = c.PhysicsFor(car, s);
            if (!p || p->id != "rbr" || p->modules.empty() || p->base != "physics/rbr/rbr/")
                return Fail(car.name + ": a slot car without RBR's physics as its default");
        }
        // a newer version than RBR's (a download's patch: car-pbr's maps): the model read through the item
        if (car.version != "rbr" && !car.sgc.empty()) {
            const wrally::assets::CarFiles files = wrally::assets::CarFilesOf(c.Content(), {wrally::vfs::Kind::Car, car.id, "", car.version});
            size_t textured = 0, maps = 0;
            for (const wrally::assets::CarTexture& t : wrally::assets::ReadCarModel(car.sgc, "gravel", &files).materials) {
                textured += !t.dds.empty();
                for (const auto& m : t.maps) maps += !m.dds.empty();
            }
            std::printf("%s %s (%s over RBR's): %zu textured materials, %zu PBR maps\n", car.name.c_str(), car.version.c_str(),
                        car.source.c_str(), textured, maps);
        }
        if (car.variants.size() > 1) {
            std::printf("%s's physics:", car.name.c_str());
            for (const wrally::assets::PhysicsVariant& p : car.variants)
                std::printf(" %s %s (%s, %s%s)", p.id.c_str(), p.version.c_str(), p.source.c_str(), p.folder.c_str(),
                            p.modules.empty() ? ", no module" : "");
            std::printf("\n");
        }
    }

    size_t imported = 0, drawn = 0;
    for (const wrally::assets::Stage& st : c.stages) {
        imported += st.imported;
        drawn += st.weathersDrawn;
        if (st.imported && !st.weathersDrawn) return Fail(st.stage + ": imported, but its weathers are not drawn");
    }
    std::printf("%zu stages: %zu from .ngg files, %zu imported from .lbs; %zu with drawn weathers; %zu protected (encrypted, "
                "not offered); %zu cars\n", c.stages.size(), c.stages.size() - imported, imported, drawn,
                c.protectedStages.size(), c.cars.size());
    // Stages a map's .ngg made of several times of day: offered once, the others not at all.
    size_t collapsed = 0;
    for (const wrally::assets::Stage& st : c.stages) {
        if (st.times.size() < 2) continue;
        ++collapsed;
        for (size_t i = 1; i < st.times.size(); ++i)
            for (const wrally::assets::Stage& other : c.stages)
                if (other.ngg == st.ngg && !_stricmp(other.stage.c_str(), st.times[i].c_str()))
                    return Fail(st.times[i] + ": offered on its own, and as a time of day of " + st.stage);
        std::string times;
        for (const std::string& t : st.times) times += (times.empty() ? "" : ", ") + t;
        std::printf("%s (track %s, stage %s): one stage for %s, %zu weathers%s\n", st.stage.c_str(), st.track.c_str(),
                    st.trackStage.c_str(), times.c_str(), st.weathers.size(), st.weathersDrawn ? " drawn" : "");
        if (c.FindStage(st.ngg + "|" + st.times[1]) != &st) return Fail(st.times[1] + ": a kept choice does not find " + st.stage);
    }
    if (collapsed) std::printf("%zu stages stand for several times of day\n", collapsed);

    // No encrypted stage offered for import (it would fail when picked, and must not be imported anyway).
    for (const wrally::assets::Stage& st : c.stages)
        if (st.imported && wrally::formats::LbsEncryptedVersion(st.ngg)) return Fail(st.stage + ": encrypted, but offered");

    // A map under 10, imported (no .ngg draws it in a stock RBR folder): its evening weathers, two-digit names.
    const wrally::assets::Stage* four = Find(c, "track-4_E");
    if (!four) return Fail("track-4_E is not offered");
    if (four->weathers.empty()) return Fail("track-4_E has no weathers (a \"4E_\" prefix finds none)");
    for (const std::string& w : four->weathers)
        if (_strnicmp(w.c_str(), "04E_", 4) && _strnicmp(w.c_str(), "04O_E_", 6)) return Fail("track-4_E offers " + w);
    std::printf("track-4_E: %s, %zu weathers (%s ...)\n", four->imported ? "imported" : "from an .ngg", four->weathers.size(),
                four->weathers[0].c_str());
    std::printf("catalog-check: all good\n");
    return 0;
}
