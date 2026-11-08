#include "assets/track_register.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "assets/catalog.h"
#include "wrally/formats/json.h"
#include "rules/stage.h"

namespace wrally::assets {

namespace {

// The server's track list, until there is a server (its API): by the map's number. Empty for now - what is not sure is
// not written (a name, a country, a length and the splash the server will give; RBR's files and track.yaml
// meanwhile). The original stages' splashes come this way: RBR picks them (Textures\splash.dat's load_NN.dds) by a
// table in its code, so a list says which is which - `splash` a path in the RBR folder
// ("Textures\\Splash\\load_23.dds") or a URL the server serves it at.
struct ServerTrack {
    int map;
    const char* name;
    const char* country;
    const char* surface;
    float lengthKm;
    const char* splash;
};
const ServerTrack kServerTracks[] = {{-1, "", "", "", 0, ""}};

std::string IniString(const std::string& file, const std::string& section, const char* key)
{
    char value[1024] = {};
    GetPrivateProfileStringA(section.c_str(), key, "", value, sizeof value, file.c_str());
    std::string v = value;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

// The country RBR's particle set names (Tracks.ini's Particles = "Maps\ps_<set>"), where it names one.
std::string CountryOfParticles(std::string particles)
{
    std::transform(particles.begin(), particles.end(), particles.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::pair<const char*, const char*> kSets[] = {
        {"ps_british", "United Kingdom"}, {"ps_usa", "USA"}, {"ps_japan", "Japan"}, {"ps_australia", "Australia"}, {"ps_montblanc", "France"}};
    for (const auto& [set, country] : kSets)
        if (particles.find(set) != std::string::npos) return country;
    return "";
}

} // namespace

void TrackRegister::Build(const vfs::Vfs& content, const std::vector<Stage>& stages)
{
    m_tracks.clear();
    const std::string rbrPath = content.RbrRoot().string();
    const std::string tracksIni = rbrPath + "\\Maps\\Tracks.ini";
    for (const Stage& st : stages) {
        if (st.map < 0 || Find(st.map)) continue;
        // 1. RBR's files
        TrackInfo t;
        t.map = st.map;
        const std::string sec = "Map" + std::to_string(st.map);
        t.name = IniString(tracksIni, sec, "StageName");
        t.splash = IniString(tracksIni, sec, "SplashScreen");
        t.country = CountryOfParticles(IniString(tracksIni, sec, "Particles"));
        t.surface = rules::StageSurface(rbrPath, st.map);
        t.from = "RBR";
        // 2. the server
        for (const ServerTrack& s : kServerTracks)
            if (s.map == st.map) {
                if (*s.name) t.name = s.name;
                if (*s.country) t.country = s.country;
                if (*s.surface) t.surface = s.surface;
                if (s.lengthKm > 0) t.lengthKm = s.lengthKm;
                if (*s.splash) t.splash = s.splash;
                t.from += " + server";
            }
        // 2b. the track table (rbr-tracks.yaml, from the onlineracing.cz tracks table): the stage's name, length and
        // surface, its track's name and country - in the track.yaml the VFS made of RBR's files, its "stages"
        bool tableStage = false;   // the table named this stage (its length and surface are its own, not the track's)
        if (const auto rbr = content.Open({vfs::Kind::Track, st.track, "", "rbr"})) {
            const formats::Json& j = rbr->Manifest();
            bool& said = tableStage;
            if (const formats::Json* listed = j.Get("stages"); listed && listed->type == formats::Json::Array)
                for (const formats::Json& s : listed->items)
                    if (static_cast<int>(s.Num("rbr", -1)) == st.map) {
                        t.name = s.Str("name", t.name);
                        t.surface = s.Str("surface", t.surface);
                        t.lengthKm = static_cast<float>(s.Num("length_km", t.lengthKm));
                        said = s.Get("name") != nullptr;
                    }
            t.trackName = j.Str("name", t.trackName);
            t.country = j.Str("country", t.country);
            if (said || j.Get("name")) t.from += " + rbr-tracks.yaml";
        }
        // 3. track.yaml: the track's newest version's (one the VFS made of RBR's files is no manifest of its own)
        if (const auto item = content.Open({vfs::Kind::Track, st.track, "", st.version}))
            if (const std::string source = item->Source(vfs::ManifestName(vfs::Kind::Track)); !source.empty() && source != "rbr") {
                const std::string file = item->ManifestFile();
                const formats::Json& j = item->Manifest();
                if (j.type == formats::Json::Object) {
                    // A track.yaml speaks for the TRACK: its name is the place's (and the stage's, when nothing named
                    // the stage); its surface and length are a stage's only where the table gave the stage none.
                    t.trackName = j.Str("name", t.trackName);
                    if (t.name.empty()) t.name = j.Str("name", t.name);
                    t.country = j.Str("country", t.country);
                    if (!tableStage) {
                        t.surface = j.Str("surface", t.surface);
                        t.lengthKm = static_cast<float>(j.Num("length_km", t.lengthKm));
                    }
                    t.splash = j.Str("splash", t.splash);
                    t.from += " + " + file + " (" + source + " " + item->Ref().version + ")";
                } else {
                    t.error = file + " (" + source + " " + item->Ref().version + "): not an object";
                }
            }
        if (t.trackName.empty()) t.trackName = t.name;
        m_tracks.push_back(std::move(t));
    }
}

const TrackInfo* TrackRegister::Find(int map) const
{
    for (const TrackInfo& t : m_tracks)
        if (t.map == map) return &t;
    return nullptr;
}

std::vector<std::string> TrackRegister::Countries() const
{
    std::vector<std::string> out;
    for (const TrackInfo& t : m_tracks)
        if (!t.country.empty() && std::find(out.begin(), out.end(), t.country) == out.end()) out.push_back(t.country);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace wrally::assets
