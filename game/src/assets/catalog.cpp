#include "assets/catalog.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "data/lsp.h"
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

std::string Catalog::Scan(const app::Settings& s)
{
    stages.clear();
    cars.clear();
    m_root = s.rbrPath;
    std::error_code ec;
    if (s.rbrPath.empty()) return "no RBR folder set (Settings)";
    if (!fs::exists(fs::path(s.rbrPath) / "Maps" / "Tracks.ini", ec) || !fs::exists(fs::path(s.rbrPath) / "physics.rbz", ec))
        return s.rbrPath + " is not an RBR folder (no Maps/Tracks.ini or physics.rbz)";
    const std::string maps = s.rbrPath + "\\Maps\\";

    // The weathers: TrackSettings.ini's sections, by their stage prefix.
    const std::vector<std::string> sections = IniSections(maps + "TrackSettings.ini");
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

    // Stages: every .ngg in the folders, each for the stages of its map it draws.
    std::string folders = s.NggFolders();
    for (size_t at = 0; at <= folders.size();) {
        const size_t semi = std::min(folders.find(';', at), folders.size());
        const std::string dir = folders.substr(at, semi - at);
        at = semi + 1;
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> nggs;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && Lower(e.path().extension().string()) == ".ngg") nggs.push_back(e.path());
        std::sort(nggs.begin(), nggs.end());
        for (const fs::path& ngg : nggs) {
            const std::string stem = ngg.stem().string();
            const int map = rules::StageMap(stem);
            if (map < 0) continue;
            const std::vector<std::string> drawn = NggWeathers(ngg);
            std::vector<std::string> variants;
            if (stem.find('_') != std::string::npos) {
                if (fs::exists(maps + stem + ".trk", ec)) variants.push_back(stem);
            } else {
                for (const auto& e : fs::directory_iterator(maps, ec)) {
                    const std::string n = e.path().filename().string();
                    if (Lower(e.path().extension().string()) == ".trk" && rules::StageMap(e.path().stem().string()) == map &&
                        e.path().stem().string().find('_') != std::string::npos)
                        variants.push_back(e.path().stem().string());
                }
                std::sort(variants.begin(), variants.end());
            }
            for (const std::string& v : variants) {
                Stage st;
                st.key = ngg.string() + "|" + v;
                st.ngg = ngg.string();
                st.stage = v;
                st.map = map;
                st.title = title(v, ngg.filename().string());
                st.weathers = stageWeathers(map, v.size() >= 2 ? v.back() : ' ');
                // The .ngg's own presets, when it has some: those of this stage's time of day if any are named so,
                // else all of them.
                if (!drawn.empty()) {
                    std::vector<std::string> mine;
                    for (const std::string& d : drawn)
                        for (const std::string& w : st.weathers)
                            if (!_stricmp(d.c_str(), w.c_str())) mine.push_back(d);
                    st.weathers = mine.empty() ? drawn : mine;
                    st.weathersDrawn = true;
                }
                stages.push_back(std::move(st));
            }
        }
    }

    // Every other RBR stage, imported when it is driven (assets/legacy_stage.h): an .lbs with its .trk in Maps that no
    // .ngg above draws. Its weathers are drawn, since the import writes TrackSettings.ini's sections as presets.
    // Where a stage comes from, best first: an .ngg in the folders (a Wallaby conversion), then this import. A
    // hand-finished .ngg from the stage server goes before both when there is one (not yet).
    // A protected (encrypted) stage is left out: it is never imported (formats::LbsEncryptedVersion); only an .ngg
    // made from it on a licensed install, or the server's, can draw it.
    std::vector<std::string> drawn;   // lower-case stage names an .ngg covers
    for (const Stage& st : stages) drawn.push_back(Lower(st.stage));
    std::vector<fs::path> lbss;
    protectedStages.clear();
    for (const auto& e : fs::directory_iterator(maps, ec)) {
        const std::string stem = e.path().stem().string();
        if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".lbs" || rules::StageMap(stem) < 0 ||
            stem.find('_') == std::string::npos || !fs::exists(maps + stem + ".trk", ec) ||
            std::find(drawn.begin(), drawn.end(), Lower(stem)) != drawn.end())
            continue;
        if (formats::LbsEncryptedVersion(e.path().string())) protectedStages.push_back(stem);
        else lbss.push_back(e.path());
    }
    std::sort(protectedStages.begin(), protectedStages.end());
    std::sort(lbss.begin(), lbss.end(), [](const fs::path& a, const fs::path& b) {
        const int ma = rules::StageMap(a.stem().string()), mb = rules::StageMap(b.stem().string());
        return ma != mb ? ma < mb : a.stem().string() < b.stem().string();
    });
    for (const fs::path& lbs : lbss) {
        Stage st;
        st.ngg = lbs.string();
        st.stage = lbs.stem().string();
        st.key = st.ngg + "|" + st.stage;
        st.map = rules::StageMap(st.stage);
        st.title = title(st.stage, "imported from .lbs");
        st.weathers = stageWeathers(st.map, st.stage.back());
        st.weathersDrawn = true;
        st.imported = true;
        stages.push_back(std::move(st));
    }

    // Cars: every folder in Cars with an .sgc; Cars.ini names the slot cars.
    const std::string carsIni = s.rbrPath + "\\Cars\\Cars.ini";
    std::map<std::string, std::pair<int, std::string>> slotOf;   // lower folder -> slot, name
    for (int slot = 0; slot < 8; ++slot) {
        const std::string sec = "Car0" + std::to_string(slot);
        std::string file = IniString(carsIni, sec, "FileName");
        std::replace(file.begin(), file.end(), '/', '\\');
        const size_t a = Lower(file).find("cars\\");
        if (a == std::string::npos) continue;
        const size_t b = file.find('\\', a + 5);
        slotOf[Lower(file.substr(a + 5, b == std::string::npos ? std::string::npos : b - a - 5))] = {slot, IniString(carsIni, sec, "CarName")};
    }
    const std::string audioIni = s.rbrPath + "\\audio\\Cars\\Cars.ini";
    for (const auto& e : fs::directory_iterator(s.rbrPath + "\\Cars", ec)) {
        if (!e.is_directory()) continue;
        Car car;
        car.folder = e.path().filename().string();
        std::string eng;
        for (const auto& f : fs::directory_iterator(e.path(), ec)) {
            const std::string ext = Lower(f.path().extension().string());
            if (ext == ".sgc" && car.sgc.empty()) car.sgc = f.path().string();
            if (ext == ".eng" && eng.empty()) eng = f.path().string();
        }
        if (car.sgc.empty()) continue;
        car.name = car.folder;
        if (const auto it = slotOf.find(Lower(car.folder)); it != slotOf.end()) {
            car.slot = it->second.first;
            if (!it->second.second.empty()) car.name = it->second.second;
        }
        if (fs::exists(e.path() / "physics" / "common.lsp", ec)) {
            car.physics = "Cars/" + car.folder + "/physics/";
            car.physicsFrom = "its own";
        } else if (car.slot >= 0) {
            car.physics = SlotPhysics(car.slot);
            car.physicsFrom = "slot " + std::to_string(car.slot) + " (" + car.physics + ")";
        }   // (else none: unless its car.json names some, it cannot be driven)
        if (!eng.empty()) car.engine = eng;
        else if (car.slot >= 0) car.engine = IniString(audioIni, "CARS", ("Car" + std::to_string(car.slot)).c_str());
        cars.push_back(std::move(car));
    }

    // What W-Rally knows of them: the registers over RBR's files, the server's list and the cars' and tracks' own
    // manifests.
    carRegister.Build(s.rbrPath, cars);
    for (Car& car : cars) {
        const CarInfo* info = carRegister.Find(car.folder);
        if (!info) continue;
        car.name = info->name;
        if (!info->physics.empty() && info->physics != car.physics) car.physics = info->physics, car.physicsFrom = "car.json (" + info->physics + ")";
    }
    trackRegister.Build(s.rbrPath, stages);
    for (Stage& st : stages)
        if (const TrackInfo* t = trackRegister.Find(st.map); t && !t->name.empty()) {
            const char* tod = TimeOfDay(st.stage.size() >= 2 ? st.stage.back() : ' ');
            st.title = t->name + (tod[0] ? std::string(" (") + tod + ")" : "");
        }
    std::sort(cars.begin(), cars.end(), [](const Car& a, const Car& b) { return Lower(a.name) < Lower(b.name); });
    if (stages.empty()) return "no stages: no .ngg with a matching Maps/track-<n>_*.trk in " + folders;
    return "";
}

std::vector<Setup> Catalog::SetupsFor(const Car& car) const
{
    std::vector<Setup> out;
    std::error_code ec;
    // Its own physics' setups (not the defaults, d_<surface>.lsp: those are what "default" means).
    if (car.physics.find('/') != std::string::npos)
        for (const auto& e : fs::directory_iterator(m_root + "\\" + car.physics + "setups", ec)) {
            const std::string n = e.path().filename().string();
            if (Lower(e.path().extension().string()) != ".lsp" || !_strnicmp(n.c_str(), "d_", 2)) continue;
            out.push_back({e.path().stem().string(), car.physics + "setups/" + n});
        }
    // A slot car's saved ones: SavedGames/<slot>slot<n>_<name>.lsp.
    if (car.slot >= 0)
        for (const auto& e : fs::directory_iterator(m_root + "\\SavedGames", ec)) {
            const std::string n = e.path().filename().string();
            const std::string prefix = std::to_string(car.slot) + "slot";
            if (Lower(e.path().extension().string()) != ".lsp" || n.compare(0, prefix.size(), prefix)) continue;
            const size_t us = n.find('_');
            out.push_back({us == std::string::npos ? e.path().stem().string() : e.path().stem().string().substr(us + 1), "SavedGames/" + n});
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
        if (!_stricmp(c.folder.c_str(), folder.c_str())) return &c;
    return nullptr;
}

std::string ExtractSlotPhysics(const std::string& rbrPath, const Catalog& catalog)
{
    std::string log;
    const std::string rbz = rbrPath + "\\physics.rbz";
    std::vector<std::string> members;
    try {
        members = phys::data::ListZip(rbz);
    } catch (const std::exception& e) {
        return e.what();
    }
    for (const Car& car : catalog.cars) {
        if (car.slot < 0) continue;
        const fs::path dir = fs::path(rbrPath) / "Cars" / car.folder / "physics";
        std::error_code ec;
        if (fs::exists(dir / "common.lsp", ec)) {
            log += car.name + ": has its own physics already\n";
            continue;
        }
        const std::string prefix = std::string("physics/") + SlotPhysics(car.slot) + "/";
        size_t files = 0;
        for (const std::string& m : members) {
            if (Lower(m).compare(0, prefix.size(), prefix) || m.back() == '/') continue;
            const fs::path to = dir / fs::path(m.substr(prefix.size()));
            fs::create_directories(to.parent_path(), ec);
            try {
                const std::string bytes = phys::data::ReadZipMember(rbz, m);
                std::ofstream(to, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                ++files;
            } catch (const std::exception& e) {
                log += car.name + ": " + e.what() + "\n";
            }
        }
        log += car.name + ": " + std::to_string(files) + " files of " + SlotPhysics(car.slot) + " to " + dir.string() + "\n";
    }
    return log.empty() ? "no slot cars\n" : log;
}

} // namespace wrally::assets
