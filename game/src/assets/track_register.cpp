#include "assets/track_register.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "assets/catalog.h"
#include "formats/json.h"
#include "rules/stage.h"

namespace wrally::assets {

namespace {

// The server's track list, until there is a server (its API): by the map's number. Empty for now - what is not sure is
// not written (a name, a country, a length and the splash the server will give; RBR's files and track-<n>.json
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

void TrackRegister::Build(const std::string& rbrPath, const std::vector<Stage>& stages)
{
    m_tracks.clear();
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
        // 3. track-<n>.json: beside the stage's .ngg, else in Maps
        const std::string file = "track-" + std::to_string(st.map) + ".json";
        const std::string beside = (std::filesystem::path(st.ngg).parent_path() / file).string();
        std::error_code ec;
        const std::string path = std::filesystem::exists(beside, ec) ? beside : rbrPath + "\\Maps\\" + file;
        std::string error;
        const formats::Json j = formats::ReadJsonFile(path, error);
        if (error.empty() && j.type == formats::Json::Object) {
            t.name = j.Str("name", t.name);
            t.country = j.Str("country", t.country);
            t.surface = j.Str("surface", t.surface);
            t.lengthKm = static_cast<float>(j.Num("length_km", t.lengthKm));
            t.splash = j.Str("splash", t.splash);
            t.from += " + " + file;
        } else if (error != "no file") {
            t.error = file + ": " + error;
        }
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
