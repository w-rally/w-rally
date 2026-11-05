#include "wrally/vfs/vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "data/lsp.h"   // phys::data::ReadZip: physics.rbz, here and nowhere else

namespace fs = std::filesystem;

namespace wrally::vfs {

namespace {

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// UTF-8 to and from paths (C++20's u8string is char8_t).
std::string U8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path P(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> Split(std::string_view path)
{
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= path.size()) {
        const size_t b = std::min(path.find('/', a), path.size());
        if (b > a) out.emplace_back(path.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, size_t from)
{
    std::string out;
    for (size_t i = from; i < parts.size(); ++i) out += (out.empty() ? "" : "/") + parts[i];
    return out;
}

bool IsFile(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool IsDir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::string ReadText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// The JSON text of a string (manifests made in memory).
std::string Quoted(std::string_view s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// An .ini's sections: lowercase section -> lowercase key -> value (quotes and comments off).
std::map<std::string, std::map<std::string, std::string>> ReadIni(const fs::path& p)
{
    std::map<std::string, std::map<std::string, std::string>> out;
    std::istringstream in(ReadText(p));
    std::string line, section;
    while (std::getline(in, line)) {
        if (const size_t semi = line.find(';'); semi != std::string::npos) line.erase(semi);
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line.front() == '[') {
            section = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        out[section][Lower(key)] = value;
    }
    return out;
}

// RBR's car slots 0..7: their folders in physics.rbz (the game's table, as assets::SlotPhysics).
const char* const kSlotPhysics[8] = {"c_xsara", "h_accent", "mg_zr", "m_lancer", "p_206", "s_i2003", "t_coroll", "s_i2000"};

std::string Key(Kind kind, std::string_view car, std::string_view id)
{
    return std::to_string(static_cast<int>(kind)) + "|" + std::string(car) + "|" + std::string(id);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

std::string NormalPath(std::string_view path, bool lower)
{
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::vector<std::string> parts = Split(p);
    std::vector<std::string> kept;
    for (std::string& s : parts) {
        if (s == ".") continue;
        if (s == "..") return "";
        kept.push_back(lower ? Lower(s) : s);
    }
    return Join(kept, 0);
}

bool ValidId(std::string_view id)
{
    if (id.empty()) return false;
    bool letter = false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        letter |= !(c >= '0' && c <= '9');
    }
    return letter;   // never a bare number
}

bool ValidVersion(std::string_view v)
{
    if (v == "rbr") return true;
    if (v.empty() || v.front() == '.' || v.back() == '.') return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '.' && v[i - 1] == '.') return false;
        if (v[i] != '.' && !(v[i] >= '0' && v[i] <= '9')) return false;
    }
    return true;
}

int CompareVersions(std::string_view a, std::string_view b)
{
    if (a == b) return 0;
    if (a == "rbr") return -1;
    if (b == "rbr") return 1;
    auto parts = [](std::string_view v) {
        std::vector<unsigned long long> n;
        size_t i = 0;
        while (i <= v.size()) {
            const size_t j = std::min(v.find('.', i), v.size());
            n.push_back(std::strtoull(std::string(v.substr(i, j - i)).c_str(), nullptr, 10));
            i = j + 1;
        }
        return n;
    };
    const std::vector<unsigned long long> p = parts(a), q = parts(b);
    for (size_t i = 0; i < std::max(p.size(), q.size()); ++i) {
        const unsigned long long u = i < p.size() ? p[i] : 0, w = i < q.size() ? q[i] : 0;
        if (u != w) return u < w ? -1 : 1;
    }
    return 0;   // 1.0 and 1.0.0: the same
}

std::string ItemDir(const ItemRef& r)
{
    switch (r.kind) {
    case Kind::Track: return "tracks/" + r.id + "/" + r.version;
    case Kind::Car: return "cars/" + r.id + "/" + r.version;
    case Kind::Physics: return "cars/" + r.car + "/physics/" + r.id + "/" + r.version;
    case Kind::PhysicsBase: return "physics/" + r.id + "/" + r.version;
    }
    return "";
}

const char* ManifestName(Kind kind)
{
    switch (kind) {
    case Kind::Track: return "track.yaml";
    case Kind::Car: return "car.yaml";
    default: return "physics.yaml";
    }
}

const std::vector<std::string>& ManifestNames(Kind kind)
{
    static const std::vector<std::string> track = {"track.yaml", "track.yml", "track.json"};
    static const std::vector<std::string> car = {"car.yaml", "car.yml", "car.json"};
    static const std::vector<std::string> physics = {"physics.yaml", "physics.yml", "physics.json"};
    switch (kind) {
    case Kind::Track: return track;
    case Kind::Car: return car;
    default: return physics;
    }
}

bool IsManifestName(Kind kind, std::string_view name)
{
    const std::string lower = Lower(name);
    for (const std::string& n : ManifestNames(kind))
        if (lower == n) return true;
    return false;
}

std::string Slug(std::string_view name)
{
    std::string s = Lower(name);
    for (char& c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// One version's own files.

struct Item::Version {
    enum Type { Folder, Maps, Rbz } type = Folder;
    std::string version;
    std::string base;            // the manifest's base, "" none
    fs::path dir;                // Folder: the version's folder; Maps: the folder with the map's files
    std::string prefix;          // Maps: "track-41"; Rbz: the members' prefix, "physics/s_i2003/" (lowercase)
    std::string source;          // the layer's name, "rbr"
    std::string manifestText;    // made in memory ("" the folder's own file)
    std::string manifestFile;    // the folder's own file's name ("car.yaml", "car.json"), "" none
    formats::Json manifest;
};

struct Vfs::Index {
    std::map<std::string, std::vector<std::unique_ptr<Item::Version>>> items;   // Key() -> versions, newest first
    std::map<std::string, std::string> rbz;                                     // physics.rbz: lowercase name -> bytes
    std::map<int, std::string> mapTrack;
    std::map<std::string, int> trackMap;
    std::map<std::string, std::string> carFolder;
};

namespace {

using Version = Item::Version;

// A version's own file `rel` (normalized; `lower` its lowercase).
std::optional<Resolved> OwnFile(const Version& v, const std::string& rel, const std::string& lower,
                                const std::map<std::string, std::string>& rbz)
{
    switch (v.type) {
    case Version::Folder:
        if (IsFile(v.dir / P(rel))) return Resolved{v.dir / P(rel), nullptr, v.source};
        break;
    case Version::Maps:
        if (lower.find('/') == std::string::npos &&
            (StartsWith(lower, v.prefix + "_") || StartsWith(lower, v.prefix + ".")) && IsFile(v.dir / P(rel)))
            return Resolved{v.dir / P(rel), nullptr, v.source};
        break;
    case Version::Rbz:
        if (const auto it = rbz.find(v.prefix + lower); it != rbz.end()) return Resolved{{}, &it->second, v.source};
        break;
    }
    return std::nullopt;
}

// A version's own entries in `relDir` ("" its root), folders ending in '/'.
void OwnList(const Version& v, const std::string& relDir, const std::map<std::string, std::string>& rbz,
             std::set<std::string>& out)
{
    std::error_code ec;
    switch (v.type) {
    case Version::Folder:
        for (const auto& e : fs::directory_iterator(v.dir / P(relDir), ec))
            out.insert(U8(e.path().filename()) + (e.is_directory() ? "/" : ""));
        break;
    case Version::Maps:
        if (!relDir.empty()) break;
        for (const auto& e : fs::directory_iterator(v.dir, ec)) {
            const std::string n = Lower(U8(e.path().filename()));
            if (e.is_regular_file() && (StartsWith(n, v.prefix + "_") || StartsWith(n, v.prefix + ".")))
                out.insert(U8(e.path().filename()));
        }
        break;
    case Version::Rbz: {
        const std::string p = v.prefix + (relDir.empty() ? "" : Lower(relDir) + "/");
        for (const auto& [name, bytes] : rbz) {
            if (!StartsWith(name, p) || name.size() == p.size()) continue;
            const std::string rest = name.substr(p.size());
            const size_t slash = rest.find('/');
            out.insert(slash == std::string::npos ? rest : rest.substr(0, slash + 1));
        }
        break;
    }
    }
    if (relDir.empty() && !v.manifestText.empty()) out.insert("");   // (the caller names the manifest)
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

const formats::Json& Item::Manifest() const { return m_chain.front()->manifest; }

std::string Item::ManifestFile() const
{
    const Version& v = *m_chain.front();
    return !v.manifestText.empty() ? ManifestName(m_ref.kind) : v.manifestFile;
}

std::vector<std::string> Item::Chain() const
{
    std::vector<std::string> out;
    for (const Version* v : m_chain) out.push_back(v->version);
    return out;
}

std::optional<Resolved> Item::Find(std::string_view relIn) const
{
    const std::string rel = NormalPath(relIn), lower = Lower(rel);
    if (rel.empty()) return std::nullopt;
    if (IsManifestName(m_ref.kind, lower)) {   // the version's own, never its base's, whichever name it has
        const Version& v = *m_chain.front();
        if (!v.manifestText.empty()) return Resolved{{}, &v.manifestText, v.source};
        if (v.manifestFile.empty()) return std::nullopt;
        return OwnFile(v, v.manifestFile, Lower(v.manifestFile), m_vfs->m_index->rbz);
    }
    for (const Version* v : m_chain)
        if (auto r = OwnFile(*v, rel, lower, m_vfs->m_index->rbz)) return r;
    return std::nullopt;
}

std::string Item::Source(std::string_view rel) const
{
    const auto r = Find(rel);
    return r ? r->source : "";
}

std::vector<std::string> Item::List(std::string_view relDirIn) const
{
    const std::string relDir = NormalPath(relDirIn);
    std::set<std::string> names;
    for (const Version* v : m_chain) OwnList(*v, relDir, m_vfs->m_index->rbz, names);
    names.erase("");
    // the manifest: this version's, if it has one
    for (auto it = names.begin(); it != names.end();)
        it = IsManifestName(m_ref.kind, *it) ? names.erase(it) : std::next(it);
    if (const std::string manifest = ManifestFile(); relDir.empty() && !manifest.empty()) names.insert(manifest);
    // one name per entry whatever its case in each version
    std::map<std::string, std::string> byLower;
    for (const std::string& n : names) byLower.emplace(Lower(n), n);
    std::vector<std::string> out;
    for (const auto& [l, n] : byLower) out.push_back(n);
    return out;
}

std::vector<std::string> Item::Files() const
{
    std::vector<std::string> out;
    std::vector<std::string> dirs = {""};
    while (!dirs.empty()) {
        const std::string d = dirs.back();
        dirs.pop_back();
        for (const std::string& n : List(d)) {
            const std::string p = d.empty() ? n : d + "/" + n;
            if (!n.empty() && n.back() == '/') dirs.push_back(p.substr(0, p.size() - 1));
            else out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------

Vfs::Vfs(std::vector<Layer> layers, fs::path rbr) : m_layers(std::move(layers)), m_rbr(std::move(rbr)), m_index(std::make_unique<Index>())
{
    ReadLayers();
    if (!m_rbr.empty()) ReadRbr();
    for (auto& [key, versions] : m_index->items)
        std::stable_sort(versions.begin(), versions.end(),
                         [](const auto& a, const auto& b) { return CompareVersions(a->version, b->version) > 0; });
}

Vfs::~Vfs() = default;

void Vfs::ReadLayers()
{
    std::error_code ec;
    auto add = [&](Kind kind, const std::string& car, const std::string& id, const fs::path& idDir, const std::string& layer) {
        for (const auto& e : fs::directory_iterator(idDir, ec)) {
            if (!e.is_directory()) continue;
            const std::string version = Lower(U8(e.path().filename()));
            if (version == "physics" && kind == Kind::Car) continue;
            if (!ValidVersion(version) || version == "rbr") {
                m_log += layer + ": " + NormalPath(U8(e.path())) + ": not a version (1, 1.2, 1.2.3), left out\n";
                continue;
            }
            auto& versions = m_index->items[Key(kind, car, id)];
            const bool known = std::any_of(versions.begin(), versions.end(),
                                           [&](const auto& v) { return CompareVersions(v->version, version) == 0; });
            if (known) continue;   // a higher layer has this version: it is that layer's, whole
            auto v = std::make_unique<Version>();
            v->type = Version::Folder;
            v->version = version;
            v->dir = e.path();
            v->source = layer;
            for (const std::string& name : ManifestNames(kind)) {
                if (!IsFile(e.path() / name)) continue;
                if (v->manifestFile.empty()) v->manifestFile = name;
                else m_log += layer + ": " + NormalPath(U8(e.path() / name)) + ": left out, " + v->manifestFile + " is the manifest\n";
            }
            if (!v->manifestFile.empty()) {
                const fs::path manifest = e.path() / v->manifestFile;
                std::string error;
                v->manifest = formats::ReadDataFile(manifest.string(), error);
                if (!error.empty()) m_log += layer + ": " + NormalPath(U8(manifest)) + ": " + error + "\n";
                v->base = Lower(v->manifest.Str("base"));
                if (!v->base.empty() && !ValidVersion(v->base))
                    m_log += layer + ": " + U8(manifest) + ": base \"" + v->base + "\" is not a version\n", v->base.clear();
            }
            versions.push_back(std::move(v));
        }
    };
    auto ids = [&](const fs::path& dir, const std::string& layer, auto&& each) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_directory()) continue;
            const std::string id = Lower(U8(e.path().filename()));
            if (!ValidId(id)) {
                m_log += layer + ": " + NormalPath(U8(e.path())) + ": not an id (a-z, 0-9, _), left out\n";
                continue;
            }
            each(id, e.path());
        }
    };
    for (const Layer& l : m_layers) {
        ids(l.root / "tracks", l.name, [&](const std::string& id, const fs::path& p) { add(Kind::Track, "", id, p, l.name); });
        ids(l.root / "physics", l.name, [&](const std::string& id, const fs::path& p) { add(Kind::PhysicsBase, "", id, p, l.name); });
        ids(l.root / "cars", l.name, [&](const std::string& car, const fs::path& p) {
            add(Kind::Car, "", car, p, l.name);
            ids(p / "physics", l.name, [&](const std::string& id, const fs::path& pp) { add(Kind::Physics, car, id, pp, l.name); });
        });
    }
}

void Vfs::ReadRbr()
{
    Index& ix = *m_index;
    auto addRbr = [&](Kind kind, const std::string& car, const std::string& id, std::unique_ptr<Version> v) {
        v->version = "rbr";
        v->source = "rbr";
        try {
            v->manifest = formats::ParseJson(v->manifestText);
        } catch (const std::exception& e) {
            m_log += std::string("rbr: a manifest made for ") + id + ": " + e.what() + "\n";
        }
        ix.items[Key(kind, car, id)].push_back(std::move(v));
    };

    // physics.rbz, whole: it is small (RBR's is 240 KB), and nothing else opens it.
    const fs::path rbzPath = m_rbr / "physics.rbz";
    if (IsFile(rbzPath)) {
        try {
            for (auto& [name, bytes] : phys::data::ReadZip(rbzPath.string())) ix.rbz[NormalPath(name, true)] = std::move(bytes);
        } catch (const std::exception& e) {
            m_log += std::string("rbr: physics.rbz: ") + e.what() + "\n";
        }
    }
    if (!ix.rbz.empty()) {
        auto v = std::make_unique<Version>();
        v->type = Version::Rbz;
        v->prefix = "physics/";
        v->manifestText = R"({"id": "rbr", "version": "rbr", "name": "RBR", "format": "rbr-lsp", "formatVersion": 1})";
        addRbr(Kind::PhysicsBase, "", "rbr", std::move(v));
    }

    // Tracks: Tracks.ini's maps, their ids from rbr-tracks.yaml ("41: cz_ploskovice"), else the older rbr-tracks.json.
    std::map<int, std::string> table;
    std::string tableName = "rbr-tracks.yaml";
    auto tableFile = FindPlain(tableName);
    if (!tableFile) tableFile = FindPlain(tableName = "rbr-tracks.json");
    if (tableFile) {
        std::vector<uint8_t> bytes;
        ReadResolved(*tableFile, bytes);
        try {
            const formats::Json j = formats::ParseData(std::string(bytes.begin(), bytes.end()), tableName);
            for (const auto& [map, id] : j.members) {
                const std::string slug = id.text;
                if (id.type != formats::Json::String || !ValidId(slug))
                    m_log += tableName + ": map " + map + ": \"" + slug + "\" is not an id\n";
                else table[std::atoi(map.c_str())] = slug;
            }
        } catch (const std::exception& e) {
            m_log += tableName + ": " + e.what() + "\n";
        }
    }
    // Tracks.ini's maps (where they are: TrackName), then those with files in Maps that Tracks.ini does not list
    // (track-<n>_<M|E|N|O>.trk: RBR does not offer them, W-Rally does).
    std::vector<std::string> bases;
    for (const auto& [section, keys] : ReadIni(m_rbr / "Maps" / "Tracks.ini"))
        if (const auto name = keys.find("trackname"); StartsWith(section, "map") && name != keys.end())
            bases.push_back(NormalPath(name->second));                  // Maps/track-41
    {
        std::set<int> listed;
        for (const std::string& b : bases)
            if (const size_t at = Lower(b).rfind("track-"); at != std::string::npos) listed.insert(std::atoi(b.c_str() + at + 6));
        std::set<int> found;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(m_rbr / "Maps", ec)) {
            const std::string n = Lower(U8(e.path().filename()));
            if (!e.is_regular_file() || !StartsWith(n, "track-") || n.size() < 5 || n.substr(n.size() - 4) != ".trk") continue;
            const size_t us = n.find('_');
            if (us == std::string::npos) continue;
            const int map = std::atoi(n.c_str() + 6);
            if (!listed.count(map) && found.insert(map).second) bases.push_back("Maps/track-" + std::to_string(map));
        }
    }
    for (const std::string& base : bases) {
        const size_t slash = base.find_last_of('/');
        const std::string file = Lower(slash == std::string::npos ? base : base.substr(slash + 1));
        if (!StartsWith(file, "track-")) continue;
        const int map = std::atoi(file.c_str() + 6);
        if (ix.mapTrack.count(map)) continue;   // (listed twice)
        auto v = std::make_unique<Version>();
        v->type = Version::Maps;
        v->dir = m_rbr / P(slash == std::string::npos ? "" : base.substr(0, slash));
        v->prefix = file;
        std::set<std::string> files;
        OwnList(*v, "", ix.rbz, files);
        if (files.empty()) continue;   // listed, but no file of it here
        std::string id = table.count(map) ? table[map] : "rbr_" + std::to_string(map);
        if (ix.trackMap.count(id)) {
            m_log += "rbr: maps " + std::to_string(ix.trackMap[id]) + " and " + std::to_string(map) + " both " + id +
                     " in " + tableName + "; map " + std::to_string(map) + " is rbr_" + std::to_string(map) + "\n";
            id = "rbr_" + std::to_string(map);
        }
        if (!table.count(map)) m_log += "rbr: map " + std::to_string(map) + " is in no " + tableName + ": " + id + "\n";
        ix.mapTrack[map] = id;
        ix.trackMap[id] = map;
        v->manifestText = "{\"id\": " + Quoted(id) + ", \"version\": \"rbr\", \"rbr\": " + std::to_string(map) + "}";
        addRbr(Kind::Track, "", id, std::move(v));
    }

    // Cars: every folder in Cars with an .sgc; Cars.ini's slots name theirs and give them physics.rbz's.
    std::map<std::string, std::pair<int, std::string>> slotOf;   // lowercase folder -> slot, CarName
    for (const auto& [section, keys] : ReadIni(m_rbr / "Cars" / "Cars.ini")) {
        if (section.size() != 5 || !StartsWith(section, "car0") || section[4] < '0' || section[4] > '7') continue;
        const auto f = keys.find("filename");
        if (f == keys.end()) continue;
        const std::vector<std::string> parts = Split(NormalPath(f->second, true));
        if (parts.size() >= 2 && parts[0] == "cars") {
            const auto n = keys.find("carname");
            slotOf[parts[1]] = {section[4] - '0', n == keys.end() ? "" : n->second};
        }
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(m_rbr / "Cars", ec)) {
        if (!e.is_directory()) continue;
        bool sgc = false;
        for (const auto& f : fs::directory_iterator(e.path(), ec)) sgc |= Lower(U8(f.path().extension())) == ".sgc";
        if (!sgc) continue;
        const std::string folder = U8(e.path().filename()), id = Slug(folder);
        if (!ValidId(id)) {
            m_log += "rbr: Cars/" + folder + ": no id can be made of its name, left out\n";
            continue;
        }
        if (ix.carFolder.count(id)) {
            m_log += "rbr: Cars/" + folder + " and Cars/" + ix.carFolder[id] + " are both " + id + "; the first kept\n";
            continue;
        }
        ix.carFolder[id] = folder;
        const auto slot = slotOf.find(Lower(folder));
        const std::string name = slot != slotOf.end() && !slot->second.second.empty() ? slot->second.second : folder;
        auto v = std::make_unique<Version>();
        v->type = Version::Folder;
        v->dir = e.path();
        v->manifestText = "{\"id\": " + Quoted(id) + ", \"version\": \"rbr\", \"name\": " + Quoted(name) +
                          ", \"physics\": \"rbr\", \"rbr\": {\"folder\": " + Quoted(folder) +
                          (slot != slotOf.end() ? ", \"slot\": " + std::to_string(slot->second.first) : std::string()) + "}}";
        addRbr(Kind::Car, "", id, std::move(v));

        // its physics: its own loose folder, else its slot's in physics.rbz
        auto p = std::make_unique<Version>();
        if (IsFile(e.path() / "physics" / "common.lsp")) {
            p->type = Version::Folder;
            p->dir = e.path() / "physics";
        } else if (slot != slotOf.end() && !ix.rbz.empty()) {
            p->type = Version::Rbz;
            p->prefix = std::string("physics/") + kSlotPhysics[slot->second.first] + "/";
        } else {
            continue;   // none: unless a download brings some, the car cannot be driven
        }
        p->manifestText = R"({"id": "rbr", "version": "rbr", "name": "RBR original", "format": "rbr-lsp", )"
                          R"("formatVersion": 1, "physicsBase": "rbr@rbr"})";
        addRbr(Kind::Physics, id, "rbr", std::move(p));
    }
}

// ---------------------------------------------------------------------------------------------------------------------

std::vector<std::string> Vfs::Ids(Kind kind, std::string_view car) const
{
    const std::string prefix = Key(kind, Lower(car), "");
    std::vector<std::string> out;
    for (const auto& [key, versions] : m_index->items)
        if (StartsWith(key, prefix) && !versions.empty()) out.push_back(key.substr(prefix.size()));
    return out;
}

std::vector<std::string> Vfs::Versions(Kind kind, std::string_view id, std::string_view car) const
{
    std::vector<std::string> out;
    const auto it = m_index->items.find(Key(kind, Lower(car), Lower(id)));
    if (it != m_index->items.end())
        for (const auto& v : it->second) out.push_back(v->version);
    return out;
}

std::optional<Item> Vfs::Open(const ItemRef& refIn, std::string* error) const
{
    auto fail = [&](const std::string& why) -> std::optional<Item> {
        if (error) *error = why;
        return std::nullopt;
    };
    ItemRef ref = refIn;
    ref.id = Lower(ref.id), ref.car = Lower(ref.car), ref.version = Lower(ref.version);
    const auto it = m_index->items.find(Key(ref.kind, ref.car, ref.id));
    if (it == m_index->items.end() || it->second.empty()) return fail(ItemDir({ref.kind, ref.id, ref.car, "*"}) + ": none here");
    const auto& versions = it->second;
    auto find = [&](const std::string& v) -> const Version* {
        for (const auto& x : versions)
            if (CompareVersions(x->version, v) == 0) return x.get();
        return nullptr;
    };
    Item item;
    item.m_vfs = this;
    const Version* v = ref.version.empty() ? versions.front().get() : find(ref.version);
    if (!v) return fail(ItemDir(ref) + ": not here (here: " + [&] {
        std::string s;
        for (const auto& x : versions) s += (s.empty() ? "" : ", ") + x->version;
        return s;
    }() + ")");
    ref.version = v->version;
    item.m_ref = ref;
    while (v) {
        item.m_chain.push_back(v);
        if (v->base.empty()) break;
        const Version* base = find(v->base);
        if (!base) return fail(ItemDir({ref.kind, ref.id, ref.car, v->version}) + ": its base " + v->base + " is not here");
        if (std::find(item.m_chain.begin(), item.m_chain.end(), base) != item.m_chain.end())
            return fail(ItemDir(ref) + ": its bases go round (" + v->version + " -> " + v->base + ")");
        v = base;
    }
    return item;
}

std::optional<Resolved> Vfs::FindPlain(std::string_view path) const
{
    for (const Layer& l : m_layers)
        if (IsFile(l.root / P(path))) return Resolved{l.root / P(path), nullptr, l.name};
    return std::nullopt;
}

std::optional<Resolved> Vfs::Find(std::string_view pathIn) const
{
    const std::string path = NormalPath(pathIn);
    if (path.empty()) return std::nullopt;
    const std::vector<std::string> p = Split(path);
    std::vector<std::string> l;
    for (const std::string& s : p) l.push_back(Lower(s));
    if (l[0] == "rbr" && p.size() > 1) {   // raw
        if (m_rbr.empty()) return std::nullopt;
        const fs::path f = m_rbr / P(Join(p, 1));
        if (IsFile(f)) return Resolved{f, nullptr, "rbr"};
        return std::nullopt;
    }
    // an item's file: the kind's folders, an id, a version and at least one more part
    auto item = [&](Kind kind, const std::string& car, size_t idAt) -> std::optional<Resolved> {
        if (p.size() <= idAt + 2 || !ValidVersion(l[idAt + 1])) return std::nullopt;
        const auto it = Open({kind, l[idAt], car, l[idAt + 1]});
        return it ? it->Find(Join(p, idAt + 2)) : std::nullopt;
    };
    if (l[0] == "tracks" && p.size() > 3) return item(Kind::Track, "", 1);
    if (l[0] == "physics" && p.size() > 3) return item(Kind::PhysicsBase, "", 1);
    if (l[0] == "cars" && p.size() > 3) {
        if (l[2] == "physics") return p.size() > 5 ? item(Kind::Physics, l[1], 3) : std::nullopt;
        return item(Kind::Car, "", 1);
    }
    return FindPlain(path);
}

bool ReadResolved(const Resolved& r, std::vector<uint8_t>& out)
{
    if (r.bytes) {
        out.assign(r.bytes->begin(), r.bytes->end());
        return true;
    }
    std::ifstream f(r.file, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) || out.empty();
}

bool Vfs::Read(std::string_view path, std::vector<uint8_t>& out) const
{
    const auto r = Find(path);
    return r && ReadResolved(*r, out);
}

std::vector<std::string> Vfs::List(std::string_view dirIn) const
{
    const std::string dir = NormalPath(dirIn);
    const std::vector<std::string> p = Split(dir);
    std::vector<std::string> l;
    for (const std::string& s : p) l.push_back(Lower(s));
    std::set<std::string> out;
    auto withSlash = [&](const std::vector<std::string>& names) {
        for (const std::string& n : names) out.insert(n + "/");
    };
    std::error_code ec;
    if (!l.empty() && l[0] == "rbr") {
        if (!m_rbr.empty())
            for (const auto& e : fs::directory_iterator(m_rbr / P(Join(p, 1)), ec))
                out.insert(U8(e.path().filename()) + (e.is_directory() ? "/" : ""));
        return {out.begin(), out.end()};
    }
    // the items' own levels: ids, versions, then inside a version
    auto inside = [&](Kind kind, const std::string& car, size_t idAt) {
        if (p.size() == idAt + 1) {
            withSlash(Versions(kind, l[idAt], car));
            if (kind == Kind::Car && !Ids(Kind::Physics, l[idAt]).empty()) out.insert("physics/");
        } else if (const auto it = Open({kind, l[idAt], car, l[idAt + 1]})) {
            for (const std::string& n : it->List(Join(p, idAt + 2))) out.insert(n);
        }
    };
    if (!l.empty() && l[0] == "tracks" && p.size() >= 2) inside(Kind::Track, "", 1);
    else if (!l.empty() && l[0] == "physics" && p.size() >= 2) inside(Kind::PhysicsBase, "", 1);
    else if (!l.empty() && l[0] == "cars" && p.size() >= 3 && l[2] == "physics") {
        if (p.size() == 3) withSlash(Ids(Kind::Physics, l[1]));
        else inside(Kind::Physics, l[1], 3);
    } else if (!l.empty() && l[0] == "cars" && p.size() >= 2) inside(Kind::Car, "", 1);
    else {
        if (p.size() == 1 && l[0] == "tracks") withSlash(Ids(Kind::Track));
        if (p.size() == 1 && l[0] == "cars") withSlash(Ids(Kind::Car));
        if (p.size() == 1 && l[0] == "physics") withSlash(Ids(Kind::PhysicsBase));
        if (p.empty() && !m_rbr.empty()) out.insert("rbr/");
        // plain files and folders, over the layers (the item folders listed above, by id)
        if (p.empty() || (l[0] != "tracks" && l[0] != "cars" && l[0] != "physics"))
            for (const Layer& layer : m_layers)
                for (const auto& e : fs::directory_iterator(layer.root / P(dir), ec))
                    out.insert(U8(e.path().filename()) + (e.is_directory() ? "/" : ""));
    }
    return {out.begin(), out.end()};
}

fs::path Vfs::WritePath(std::string_view pathIn) const
{
    const std::string path = NormalPath(pathIn);
    if (path.empty() || m_layers.empty()) return {};
    if (Lower(Split(path)[0]) == "rbr") return {};   // the RBR folder is never written
    return m_layers.front().root / P(path);
}

std::string Vfs::VfsPath(const fs::path& file) const
{
    if (file.empty()) return "";
    std::error_code ec;
    const fs::path abs = fs::absolute(file, ec).lexically_normal();
    auto under = [&](const fs::path& root) -> std::optional<std::string> {
        if (root.empty()) return std::nullopt;
        const fs::path rel = abs.lexically_relative(fs::absolute(root, ec).lexically_normal());
        if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) return std::nullopt;
        return NormalPath(U8(rel));
    };
    if (const auto r = under(m_rbr)) return "rbr/" + *r;
    for (const Layer& l : m_layers)
        if (const auto r = under(l.root)) return *r;
    return file.is_absolute() ? U8(file) : NormalPath(U8(file));
}

std::string Vfs::TrackOfMap(int map) const
{
    const auto it = m_index->mapTrack.find(map);
    return it == m_index->mapTrack.end() ? "" : it->second;
}

int Vfs::MapOfTrack(std::string_view id) const
{
    const auto it = m_index->trackMap.find(Lower(id));
    return it == m_index->trackMap.end() ? -1 : it->second;
}

std::string Vfs::RbrCarFolder(std::string_view id) const
{
    const auto it = m_index->carFolder.find(Lower(id));
    return it == m_index->carFolder.end() ? "" : it->second;
}

} // namespace wrally::vfs
