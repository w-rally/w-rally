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

// The stage a time of day's file (track-42_O) is one of.
const wrally::assets::Stage* Find(const wrally::assets::Catalog& c, const char* stage)
{
    for (const wrally::assets::Stage& s : c.stages)
        for (const wrally::assets::StageTime& t : s.times)
            if (!_stricmp(t.stage.c_str(), stage)) return &s;
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
    // A stage is a ROUTE: one a map of a track, its times of day inside it; every weather on one of its times, the
    // one its name says (42O_M_...: the _O file), a kept choice of any time finding the stage.
    size_t several = 0;
    for (size_t i = 0; i < c.stages.size(); ++i) {
        const wrally::assets::Stage& st = c.stages[i];
        for (size_t j = i + 1; j < c.stages.size(); ++j)
            if (c.stages[j].track == st.track && c.stages[j].map == st.map) return Fail(st.stage + ": its map offered twice");
        if (st.weathers.size() != st.weatherTime.size() || st.weathers.size() != st.weatherTitles.size() ||
            st.weathers.size() != st.weatherDays.size() || st.weathers.size() != st.weatherKinds.size())
            return Fail(st.stage + ": weathers, their times and titles disagree");
        for (size_t w = 0; w < st.weathers.size(); ++w) {
            const std::string& name = st.weathers[w];
            const std::string& file = st.times[st.weatherTime[w]].stage;
            const std::string map = std::to_string(st.map), padded = st.map < 10 ? "0" + map : map;
            if (_strnicmp(name.c_str(), padded.c_str(), padded.size()) || st.TimeOf(name).stage != file) continue;
            const char want = static_cast<char>(std::toupper(static_cast<unsigned char>(name[padded.size()])));
            const char got = static_cast<char>(std::toupper(static_cast<unsigned char>(file.back())));
            bool fileThere = false;
            for (const wrally::assets::StageTime& t : st.times) fileThere |= std::toupper(static_cast<unsigned char>(t.stage.back())) == want;
            if (fileThere && want != got) return Fail(name + ": driven on " + file);
        }
        for (const wrally::assets::StageTime& t : st.times)
            if (c.FindStage(t.ngg + "|" + t.stage) != &st) return Fail(t.stage + ": a kept choice does not find its stage");
        if (st.times.size() < 2) continue;
        ++several;
        if (several <= 4) {
            std::string times;
            for (const wrally::assets::StageTime& t : st.times) times += (times.empty() ? "" : ", ") + t.stage;
            std::printf("%s (track %s): times %s, %zu weathers, e.g. \"%s\"\n", st.title.c_str(), st.track.c_str(), times.c_str(),
                        st.weathers.size(), st.weatherTitles.empty() ? "" : st.weatherTitles.back().c_str());
        }
    }
    std::printf("%zu stages hold several times of day\n", several);
    // Weather names for people
    for (const auto& [in, out] : {std::pair<const char*, const char*>{"42M_crisp_clear", "Morning crisp, clear"},
                                  {"42O_M_heavyrain_heavycloud", "Morning heavy rain, heavy clouds"},
                                  {"04O_E_lightfog_partcloud", "Evening light fog, partly cloudy"},
                                  {"69S_N_heavysnow_ligthcloud", "Noon heavy snow, light clouds"}, {"Evening_Clear", "Evening Clear"}})
        if (wrally::assets::WeatherTitle(in) != out) return Fail(std::string(in) + " reads \"" + wrally::assets::WeatherTitle(in) + "\"");
    if (wrally::assets::WeatherDay("42O_M_heavyrain_heavycloud") != 'M' || wrally::assets::WeatherKind("42O_M_heavyrain_heavycloud") != "Heavy rain, heavy clouds" ||
        wrally::assets::WeatherDay("Evening_Clear") != 0)
        return Fail("the time of day and the weather are not told apart");

    // The names the track table gives (rbr-tracks.yaml, from the onlineracing.cz table): a stage's own, its track's (the
    // place), its country, length and surface - and the menus' title is the stage's name.
    size_t named = 0;
    for (const wrally::assets::Stage& st : c.stages)
        if (const wrally::assets::TrackInfo* t = c.trackRegister.Find(st.map); t && t->from.find("rbr-tracks.yaml") != std::string::npos) {
            ++named;
            if (t->name.empty() || t->trackName.empty() || st.title.compare(0, t->name.size(), t->name))
                return Fail(st.stage + ": named by the table, but its title is \"" + st.title + "\"");
        }
    std::printf("%zu stages named by rbr-tracks.yaml\n", named);
    if (const wrally::assets::Stage* jv = Find(c, "track-45_N")) {
        const wrally::assets::TrackInfo* t = c.trackRegister.Find(45);
        if (!t || t->name != "Joux Verte II" || t->trackName != "Joux Verte" || t->country != "France" || t->surface != "tarmac" ||
            t->lengthKm < 7.7f || t->lengthKm > 7.9f || jv->track != "fr_joux_verte")
            return Fail("track-45_N is not Joux Verte II of the track Joux Verte (France, tarmac, 7.8 km): " +
                        (t ? t->name + " / " + t->trackName + " / " + t->country + " / " + t->surface : std::string("not registered")));
        std::printf("track-45_N: %s, a stage of %s (%s, %s, %.1f km)\n", t->name.c_str(), t->trackName.c_str(), t->country.c_str(),
                    t->surface.c_str(), t->lengthKm);
    }

    // No encrypted stage offered for import (it would fail when picked, and must not be imported anyway).
    for (const wrally::assets::Stage& st : c.stages)
        if (st.imported && wrally::formats::LbsEncryptedVersion(st.ngg)) return Fail(st.stage + ": encrypted, but offered");

    // Map 4 is RBR's main menu backdrop, no stage (rbr-tracks.yaml's not_stages): never offered.
    if (Find(c, "track-4_E") || Find(c, "track-4_M")) return Fail("map 4, the menu's backdrop, is offered as a stage");
    // An overcast weather of a stage with an _O file is driven on it; its morning, noon and evening ones on theirs.
    if (const wrally::assets::Stage* jv = Find(c, "track-42_M")) {
        size_t onO = 0;
        for (size_t w = 0; w < jv->weathers.size(); ++w) {
            const std::string& name = jv->weathers[w];
            const std::string& file = jv->TimeOf(name).stage;
            if (!_strnicmp(name.c_str(), "42O_", 4) && (++onO, file != "track-42_O")) return Fail(name + ": driven on " + file);
            if (!_strnicmp(name.c_str(), "42M_", 4) && file != "track-42_M") return Fail(name + ": driven on " + file);
        }
        std::printf("%s: %zu weathers, %zu on track-42_O (\"%s\")\n", jv->title.c_str(), jv->weathers.size(), onO,
                    jv->weatherTitles.empty() ? "" : jv->weatherTitles.back().c_str());
    }
    std::printf("catalog-check: all good\n");
    return 0;
}
