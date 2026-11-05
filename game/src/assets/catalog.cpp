#include "assets/catalog.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "app/content.h"
#include "formats/lbs_geometry.h"
#include "formats/track_settings.h"
#include "rules/stage.h"

namespace fs = std::filesystem;

namespace wrally::assets {

namespace {

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string IniString(const std::string& file, const std::string& section, const char* key)
{
    char value[1024] = {};
    GetPrivateProfileStringA(section.c_str(), key, "", value, sizeof value, file.c_str());
    std::string v = value;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

std::vector<std::string> IniSections(const std::string& file)
{
    std::vector<char> buf(4 << 20);
    const DWORD n = GetPrivateProfileSectionNamesA(buf.data(), static_cast<DWORD>(buf.size()), file.c_str());
    std::vector<std::string> out;
    for (DWORD at = 0; at < n && buf[at];) {
        out.emplace_back(&buf[at]);
        at += static_cast<DWORD>(out.back().size()) + 1;
    }
    return out;
}

const char* TimeOfDay(char t)
{
    switch (std::toupper(static_cast<unsigned char>(t))) {
    case 'M': return "morning";
    case 'N': return "noon";
    case 'E': return "evening";
    case 'O': return "overcast";
    default: return "";
    }
}

// The weather presets an .ngg carries (its NGGW chunk's records' names), from its chunk headers alone: the file's
// header (magic, version, chunk count, reserved), then each chunk's fourcc and byte size before its payload, padded
// to 4 bytes (modules/renderer/src/format/ngg_format.h).
std::vector<std::string> NggWeathers(const fs::path& ngg)
{
    std::vector<std::string> names;
    std::ifstream f(ngg, std::ios::binary);
    uint32_t header[4] = {};
    if (!f.read(reinterpret_cast<char*>(header), sizeof header) || std::memcmp(header, "NGG0", 4)) return names;
    for (uint32_t i = 0; i < header[2]; ++i) {
        uint32_t chunk[2] = {};
        if (!f.read(reinterpret_cast<char*>(chunk), sizeof chunk)) break;
        if (std::memcmp(chunk, "NGGW", 4)) {
            f.seekg(static_cast<std::streamoff>(chunk[1] + (4 - chunk[1] % 4) % 4), std::ios::cur);
            continue;
        }
        uint32_t w[4] = {};   // version 1, count, record size, reserved
        if (!f.read(reinterpret_cast<char*>(w), sizeof w) || w[0] != 1 || w[2] < 64) break;
        std::vector<char> record(w[2]);
        for (uint32_t r = 0; r < w[1] && f.read(record.data(), w[2]); ++r) {
            record[63] = 0;   // the name: 64 bytes first
            names.emplace_back(record.data());
        }
        break;
    }
    return names;
}

} // namespace

const char* SlotPhysics(int slot)
{
    // The game's table (plugin-rbr PluginHelpers.cpp's constPhysSubDirs, from its 5th): Car00 .. Car07.
    static const char* const kSlots[8] = {"c_xsara", "h_accent", "mg_zr", "m_lancer", "p_206", "s_i2003", "t_coroll", "s_i2000"};
    return slot >= 0 && slot < 8 ? kSlots[slot] : "";
}

int RoadOfWeather(const std::string& preset)
{
    const std::string p = Lower(preset);
    if (p.find("heavyrain") != std::string::npos || p.find("_rain") != std::string::npos) return 2;
    if (p.find("lightrain") != std::string::npos) return 1;
    return 0;
}

namespace {

// The modules of `modules` that read `format` at `version`, and a variant's physics base (physics.json's physicsBase:
// "<id>@<version>", or "<id>" for its newest) as a VFS folder ("" none here).
std::vector<std::string> Drivers(const std::vector<PhysicsModule>& modules, const std::string& format, uint32_t version)
{
    std::vector<std::string> out;
    for (const PhysicsModule& m : modules)
        if (m.Reads(format, version)) out.push_back(m.id);
    return out;
}

std::string BaseFolder(const vfs::Vfs& v, const std::string& base)
{
    const size_t at = base.find('@');
    const auto b = v.Open({vfs::Kind::PhysicsBase, base.substr(0, at), "", at == std::string::npos ? "" : base.substr(at + 1)});
    return b ? vfs::ItemDir(b->Ref()) + "/" : "";
}

// A physics variant as the catalog lists it, from its physics.json (made in memory for RBR's own).
PhysicsVariant Variant(const vfs::Vfs& v, const vfs::Item& item, const std::vector<PhysicsModule>& modules)
{
    const formats::Json& j = item.Manifest();
    PhysicsVariant p;
    p.id = item.Ref().id, p.version = item.Ref().version;
    p.name = j.Str("name", p.id);
    p.folder = vfs::ItemDir(item.Ref()) + "/";
    p.format = j.Str("format");
    p.formatVersion = static_cast<uint32_t>(j.Num("formatVersion", 0));
    p.source = item.Source("common.lsp");
    if (p.source.empty()) p.source = item.Source("physics.json");
    if (const std::string base = j.Str("physicsBase"); !base.empty()) p.base = BaseFolder(v, base);
    if (!p.base.empty()) p.modules = Drivers(modules, p.format, p.formatVersion);
    return p;
}

// The car's default physics first: car.json's `physics` (a variant's id - or, as before, a physics.rbz folder's name or
// a folder under the RBR folder, made a variant "car_json"), else "rbr", else the first a module drives.
void ChooseDefault(Car& car, const CarInfo* info, const vfs::Vfs& v, const std::vector<PhysicsModule>& modules)
{
    std::string wanted = info ? info->physics : "";
    std::string from;
    const auto has = [&](const std::string& id) {
        return std::find_if(car.variants.begin(), car.variants.end(), [&](const PhysicsVariant& p) { return p.id == id; });
    };
    if (!wanted.empty() && has(wanted) == car.variants.end()) {
        PhysicsVariant p;
        p.id = "car_json", p.version = "rbr", p.name = "car.json (" + wanted + ")";
        p.folder = wanted.find_first_of("/\\") == std::string::npos ? "physics/rbr/rbr/" + wanted + "/"
                                                                    : "rbr/" + vfs::NormalPath(wanted) + "/";
        p.format = "rbr-lsp", p.formatVersion = 1, p.source = "rbr";
        p.base = BaseFolder(v, "rbr");
        if (!p.base.empty()) p.modules = Drivers(modules, p.format, p.formatVersion);
        from = p.name;
        car.variants.push_back(std::move(p));
        wanted = "car_json";
    }
    auto first = wanted.empty() ? car.variants.end() : has(wanted);
    if (first == car.variants.end()) first = has("rbr");
    if (first == car.variants.end())
        first = std::find_if(car.variants.begin(), car.variants.end(), [](const PhysicsVariant& p) { return !p.modules.empty(); });
    if (first != car.variants.end()) std::rotate(car.variants.begin(), first, first + 1);
    if (car.variants.empty()) return;
    const PhysicsVariant& d = car.variants.front();
    car.physics = d.folder;
    if (!from.empty()) car.physicsFrom = from;
    else if (d.id == "rbr" && d.source == "rbr")
        car.physicsFrom = car.slot >= 0 && !v.Find("rbr/Cars/" + car.folder + "/physics/common.lsp")
                              ? "slot " + std::to_string(car.slot) + " (" + SlotPhysics(car.slot) + ")"
                              : "its own";
    else car.physicsFrom = d.name + " " + d.version;
}

} // namespace

std::string Catalog::Scan(const app::Settings& s) { return Scan(s, app::MakeVfs(s)); }

std::string Catalog::Scan(const app::Settings& s, std::shared_ptr<const vfs::Vfs> content)
{
    stages.clear();
    cars.clear();
    protectedStages.clear();
    m_content = std::move(content);
    const vfs::Vfs& v = *m_content;
    std::error_code ec;
    // A file of an item as the disk has it ("" none, or one only the VFS holds: a physics.rbz member).
    auto disk = [](const vfs::Item& item, const std::string& rel) {
        const auto r = item.Find(rel);
        return r && !r->file.empty() ? r->file.string() : std::string();
    };

    // The weathers: RBR's TrackSettings.ini's sections, by their stage prefix.
    const auto trackSettings = v.Find("rbr/Maps/TrackSettings.ini");
    const std::vector<std::string> sections = trackSettings ? IniSections(trackSettings->file.string()) : std::vector<std::string>{};
    // A stage's: its map's sections of its time of day, and the overcast ones of it ("41M_...", "41O_M_..."; the map
    // in two digits at least, as TrackSettings.ini writes it: "04M_...", not "4M_...").
    auto stageWeathers = [&](int map, char tod) {
        const char t = static_cast<char>(std::toupper(static_cast<unsigned char>(tod)));
        const std::string mapName = formats::TrackSettingsMap(map);
        const std::string prefix = mapName + t + "_", overcast = mapName + "O_" + t + "_";
        std::vector<std::string> out;
        for (const std::string& sec : sections)
            if (!_strnicmp(sec.c_str(), prefix.c_str(), prefix.size()) || !_strnicmp(sec.c_str(), overcast.c_str(), overcast.size()))
                out.push_back(sec);
        return out;
    };
    auto title = [](const std::string& stage, const std::string& from) {
        const char* tod = TimeOfDay(stage.size() >= 2 ? stage.back() : ' ');
        return stage + (tod[0] ? std::string(" (") + tod + ")" : "") + "  -  " + from;
    };

    // The .ngg folders' (development): by lower-case stem.
    std::map<std::string, fs::path> folderNggs;
    const std::string folders = s.NggFolders();
    for (size_t at = 0; at <= folders.size();) {
        const size_t semi = std::min(folders.find(';', at), folders.size());
        const std::string dir = folders.substr(at, semi - at);
        at = semi + 1;
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && Lower(e.path().extension().string()) == ".ngg")
                folderNggs.emplace(Lower(e.path().stem().string()), e.path());
    }

    // Stages: every track's, its newest version, in the order of their maps.
    struct TrackAt {
        int map;
        std::string id;
    };
    std::vector<TrackAt> tracks;
    for (const std::string& id : v.Ids(vfs::Kind::Track)) tracks.push_back({v.MapOfTrack(id), id});
    std::sort(tracks.begin(), tracks.end(), [](const TrackAt& a, const TrackAt& b) {
        return a.map != b.map ? (a.map < 0 ? false : b.map < 0 ? true : a.map < b.map) : a.id < b.id;
    });
    for (const TrackAt& t : tracks) {
        const auto item = v.Open({vfs::Kind::Track, t.id});
        if (!item) continue;
        // its stages: its .trk files of RBR's names (track-41_M.trk)
        std::vector<std::string> variants;
        for (const std::string& f : item->List()) {
            const fs::path p(f);
            const std::string stem = p.stem().string();
            if (Lower(p.extension().string()) == ".trk" && stem.find('_') != std::string::npos && rules::StageMap(stem) >= 0)
                variants.push_back(stem);
        }
        std::sort(variants.begin(), variants.end());
        for (const std::string& variant : variants) {
            Stage st;
            st.stage = variant;
            st.map = rules::StageMap(variant);
            st.track = item->Ref().id, st.version = item->Ref().version;
            st.weathers = stageWeathers(st.map, variant.back());
            // Drawn by: the track's own .ngg, an .ngg folder's, else its .lbs imported.
            const std::string mapNgg = "track-" + std::to_string(st.map);
            fs::path ngg;
            for (const std::string& name : {variant, t.id, mapNgg})
                if (const std::string d = disk(*item, name + ".ngg"); !d.empty()) {
                    ngg = d, st.source = item->Source(name + ".ngg");
                    break;
                }
            if (ngg.empty())
                for (const std::string& name : {Lower(variant), mapNgg})
                    if (const auto it = folderNggs.find(name); it != folderNggs.end()) {
                        ngg = it->second, st.source = "ngg folder";
                        break;
                    }
            if (!ngg.empty()) {
                st.ngg = ngg.string();
                st.title = title(variant, ngg.filename().string());
                // The .ngg's own presets, when it has some: those of this stage's time of day if any are named so,
                // else all of them.
                if (const std::vector<std::string> drawn = NggWeathers(ngg); !drawn.empty()) {
                    std::vector<std::string> mine;
                    for (const std::string& d : drawn)
                        for (const std::string& w : st.weathers)
                            if (!_stricmp(d.c_str(), w.c_str())) mine.push_back(d);
                    st.weathers = mine.empty() ? drawn : mine;
                    st.weathersDrawn = true;
                }
            } else {
                // A protected (encrypted) stage is left out: it is never imported (formats::LbsEncryptedVersion);
                // only an .ngg made from it on a licensed install, or the server's, can draw it.
                const std::string lbs = disk(*item, variant + ".lbs");
                if (lbs.empty()) continue;
                if (formats::LbsEncryptedVersion(lbs)) {
                    protectedStages.push_back(variant);
                    continue;
                }
                st.ngg = lbs;
                st.source = item->Source(variant + ".lbs");
                st.title = title(variant, "imported from .lbs");
                st.weathersDrawn = st.imported = true;   // (the import writes TrackSettings.ini's sections as presets)
            }
            st.key = st.ngg + "|" + st.stage;
            stages.push_back(std::move(st));
        }
    }
    std::sort(protectedStages.begin(), protectedStages.end());

    // Cars: every car with an .sgc, its newest version.
    const auto audioIni = v.Find("rbr/audio/Cars/Cars.ini");
    for (const std::string& id : v.Ids(vfs::Kind::Car)) {
        const auto item = v.Open({vfs::Kind::Car, id});
        if (!item) continue;
        Car car;
        car.id = id, car.version = item->Ref().version;
        car.folder = v.RbrCarFolder(id).empty() ? id : v.RbrCarFolder(id);
        std::string eng;
        for (const std::string& f : item->List()) {
            const std::string ext = Lower(fs::path(f).extension().string());
            if (ext == ".sgc" && car.sgc.empty()) car.sgc = disk(*item, f), car.source = item->Source(f);
            if (ext == ".eng" && eng.empty()) eng = disk(*item, f);
        }
        if (car.sgc.empty()) continue;
        // RBR's: its Cars.ini name and slot (the manifest the VFS made of them)
        car.name = car.folder;
        if (const auto rbr = v.Open({vfs::Kind::Car, id, "", "rbr"})) {
            car.name = rbr->Manifest().Str("name", car.name);
            if (const formats::Json* r = rbr->Manifest().Get("rbr")) car.slot = static_cast<int>(r->Num("slot", -1));
        }
        // its physics: every variant, the newest version of each
        for (const std::string& pid : v.Ids(vfs::Kind::Physics, id))
            if (const auto p = v.Open({vfs::Kind::Physics, pid, id})) car.variants.push_back(Variant(v, *p, physicsModules));
        if (!eng.empty()) car.engine = eng;
        else if (car.slot >= 0 && audioIni) car.engine = IniString(audioIni->file.string(), "CARS", ("Car" + std::to_string(car.slot)).c_str());
        cars.push_back(std::move(car));
    }

    // What W-Rally knows of them: the registers over RBR's files, the server's list and the items' own manifests.
    carRegister.Build(v, cars);
    for (Car& car : cars) {
        const CarInfo* info = carRegister.Find(car.folder);
        if (!info) continue;
        car.name = info->name;
    }
    for (Car& car : cars) ChooseDefault(car, carRegister.Find(car.folder), v, physicsModules);
    trackRegister.Build(v, stages);
    for (Stage& st : stages)
        if (const TrackInfo* t = trackRegister.Find(st.map); t && !t->name.empty()) {
            const char* tod = TimeOfDay(st.stage.size() >= 2 ? st.stage.back() : ' ');
            st.title = t->name + (tod[0] ? std::string(" (") + tod + ")" : "");
        }
    std::sort(cars.begin(), cars.end(), [](const Car& a, const Car& b) { return Lower(a.name) < Lower(b.name); });
    if (stages.empty())
        return v.RbrRoot().empty() ? "no stages: no RBR folder set (Settings) and no track of W-Rally's"
                                   : "no stages in " + v.RbrRoot().string() + " or W-Rally's content";
    return "";
}

bool PhysicsModule::Reads(const std::string& format, uint32_t version) const
{
    for (const Format& f : formats)
        if (f.name == format && version >= f.minVersion && version <= f.maxVersion) return true;
    return false;
}

const PhysicsVariant* Catalog::PhysicsFor(const Car& car, const app::Settings& s) const
{
    if (const auto it = s.physics.find(car.id); it != s.physics.end())
        for (const PhysicsVariant& p : car.variants)
            if (p.id == it->second && !p.modules.empty()) return &p;
    return !car.variants.empty() && !car.variants.front().modules.empty() ? &car.variants.front() : nullptr;
}

CarFiles CarFilesOf(std::shared_ptr<const vfs::Vfs> content, const vfs::ItemRef& ref)
{
    CarFiles f;
    if (!content) return f;
    auto item = content->Open(ref);
    if (!item) return f;
    const auto shared = std::make_shared<std::pair<std::shared_ptr<const vfs::Vfs>, vfs::Item>>(std::move(content), *item);
    f.read = [shared](const std::string& rel) {
        std::vector<uint8_t> bytes;
        if (const auto r = shared->second.Find(rel)) vfs::ReadResolved(*r, bytes);
        return bytes;
    };
    f.list = [shared](const std::string& relDir) {
        std::vector<std::string> out;
        for (const std::string& n : shared->second.List(relDir))
            if (!n.empty() && n.back() != '/') out.push_back(n);
        return out;
    };
    return f;
}

bool Catalog::Drivable(const Car& car)
{
    return std::any_of(car.variants.begin(), car.variants.end(), [](const PhysicsVariant& p) { return !p.modules.empty(); });
}

std::vector<Setup> Catalog::SetupsFor(const Car& car, const PhysicsVariant* physics) const
{
    std::vector<Setup> out;
    if (!m_content) return out;
    // Its physics' setups (not the defaults, d_<surface>.lsp: those are what "default" means).
    const std::string folder = physics ? physics->folder : car.physics;
    if (!folder.empty())
        for (const std::string& n : m_content->List(folder + "setups")) {
            if (Lower(fs::path(n).extension().string()) != ".lsp" || !_strnicmp(n.c_str(), "d_", 2)) continue;
            out.push_back({fs::path(n).stem().string(), folder + "setups/" + n});
        }
    // A slot car's saved ones: rbr/SavedGames/<slot>slot<n>_<name>.lsp.
    if (car.slot >= 0)
        for (const std::string& n : m_content->List("rbr/SavedGames")) {
            const std::string prefix = std::to_string(car.slot) + "slot";
            if (Lower(fs::path(n).extension().string()) != ".lsp" || n.compare(0, prefix.size(), prefix)) continue;
            const std::string stem = fs::path(n).stem().string();
            const size_t us = stem.find('_');
            out.push_back({us == std::string::npos ? stem : stem.substr(us + 1), "rbr/SavedGames/" + n});
        }
    return out;
}

const Stage* Catalog::FindStage(const std::string& key) const
{
    for (const Stage& s : stages)
        if (s.key == key) return &s;
    return nullptr;
}

const Car* Catalog::FindCar(const std::string& folder) const
{
    for (const Car& c : cars)
        if (!_stricmp(c.folder.c_str(), folder.c_str()) || !_stricmp(c.id.c_str(), folder.c_str())) return &c;
    return nullptr;
}

} // namespace wrally::assets
