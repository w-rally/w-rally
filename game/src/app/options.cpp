#include "app/options.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rules/stage.h"

namespace wrally::app {

namespace {

using Args = std::vector<std::string>;

std::string Arg(const Args& args, const char* name, const char* def)
{
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return def;
}
bool HasFlag(const Args& args, const char* name)
{
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == name) return true;
    return false;
}
std::string ArgOrEnv(const Args& args, const char* name, const char* env)
{
    const std::string a = Arg(args, name, "");
    return !a.empty() ? a : std::string(std::getenv(env) ? std::getenv(env) : "");
}

// A path's file name without its extension.
std::string Stem(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\"), from = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    return path.substr(from, dot == std::string::npos || dot < from ? std::string::npos : dot - from);
}

// The stage an .ngg draws, from its name: track-34_E.ngg that stage; track-34.ngg the map's one stage in Maps (its
// .trk), none when it has several (`why` names them) or none.
std::string NggStage(const std::string& game, const std::string& nggPath, std::string& why)
{
    const std::string stem = Stem(nggPath);
    const int map = rules::StageMap(stem);
    if (map < 0) return why = "no track-<n> in its name", "";
    if (stem.find('_') != std::string::npos) {
        if (GetFileAttributesA((game + "/Maps/" + stem + ".trk").c_str()) != INVALID_FILE_ATTRIBUTES) return why = "its name", stem;
        return why = "no Maps/" + stem + ".trk", "";
    }
    std::vector<std::string> found;
    WIN32_FIND_DATAA fd;
    if (HANDLE h = FindFirstFileA((game + "/Maps/track-" + std::to_string(map) + "_*.trk").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do found.push_back(Stem(fd.cFileName));
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (found.size() == 1) return why = "map " + std::to_string(map) + "'s only stage in Maps", found[0];
    why = found.empty() ? "no Maps/track-" + std::to_string(map) + "_*.trk" : "map " + std::to_string(map) + " has";
    for (const std::string& f : found) why += " " + f;
    for (const std::string& f : found)   // several: the morning one (_M), as before
        if (!_stricmp(f.c_str(), ("track-" + std::to_string(map) + "_M").c_str())) return why += "; the morning one taken", f;
    return "";
}

} // namespace

std::filesystem::path ExeDir()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

std::string WRallyFile(const std::string& rel)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.erase(dir.find_last_of("\\/") + 1);
    for (const std::string& path : {dir + rel, std::string(WRALLY_SOURCE_DIR) + "/" + rel})
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    return "";
}

int Parse(int argc, char** argv, Options& o) { return Parse(Args(argv, argv + argc), o); }

int ParseSkyTime(const std::vector<std::string>& args, Options& o, const char* defaultTime)
{
    // The time and place: the sun from them (NGG design §5.4).
    const std::string t = Arg(args, "--time", defaultTime ? defaultTime : "");
    if (t.empty()) return 0;
    int h = 0, m = 0;
    if (std::sscanf(t.c_str(), "%d:%d", &h, &m) < 1 || h < 0 || h > 24 || m < 0 || m > 59)
        return std::fprintf(stderr, "--time %s: HH:MM, the stage's local time\n", t.c_str()), 2;
    o.skyTimeSet = true;
    o.skyHour = h + m / 60.0;
    if (const std::string d = Arg(args, "--date", ""); !d.empty() &&
        (std::sscanf(d.c_str(), "%d-%d-%d", &o.skyYear, &o.skyMonth, &o.skyDay) != 3 || o.skyMonth < 1 || o.skyMonth > 12 ||
         o.skyDay < 1 || o.skyDay > 31))
        return std::fprintf(stderr, "--date %s: YYYY-MM-DD\n", d.c_str()), 2;
    o.skyUtcOffset = std::atof(Arg(args, "--utc-offset", "1").c_str());
    o.skyLatitude = std::atof(Arg(args, "--latitude", "50").c_str());
    o.skyLongitude = std::atof(Arg(args, "--longitude", "14.4").c_str());
    o.skyNorth = std::atof(Arg(args, "--north", "0").c_str());
    return 0;
}

int Parse(const std::vector<std::string>& args, Options& o)
{
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: W-Rally <game dir> <stage .ngg> [--stage track-34_E] [--sgc <car .sgc>] [--car s_i2003] "
                             "[--surface gravel] [--tyre-type 3] [--tyres michelin] [--out drive.phr]\n");
        return 2;
    }
    o.game = args[1];
    o.ngg = args[2];
    const std::string& game = o.game;
    o.stage = Arg(args, "--stage", "");
    o.track = Arg(args, "--track", "");
    o.carItem = Arg(args, "--car-item", "");
    {
        std::string why;
        const std::string fromNgg = NggStage(game, o.ngg, why);
        if (o.stage.empty()) {
            if (fromNgg.empty()) return std::fprintf(stderr, "%s: --stage needed (%s)\n", o.ngg.c_str(), why.c_str()), 2;
            o.stage = fromNgg;
            std::printf("stage %s from the .ngg's name (%s)\n", o.stage.c_str(), why.c_str());
        } else if (const int a = rules::StageMap(o.stage), b = rules::StageMap(Stem(o.ngg)); a >= 0 && b >= 0 && a != b) {
            std::printf("WARNING: --stage %s is map %d, the .ngg (%s) map %d: the car runs on another stage's ground than "
                        "the one drawn\n", o.stage.c_str(), a, o.ngg.c_str(), b);
            o.stageWarning = "--stage " + o.stage + " is map " + std::to_string(a) + ", the .ngg map " + std::to_string(b) +
                             ": the car runs on ground that is not drawn";
        }
    }
    session::Config& s = o.session;
    physics::Config& c = s.physics;
    const std::string maps = game + "/Maps/" + o.stage;
    c.root = game;
    c.trk = maps + ".trk";
    c.col = maps + ".col";
    c.mat = maps + ".mat";
    c.fnc = maps + ".fnc";
    c.lbs = maps + ".lbs";
    // --sgc: under the game folder, or a full path (Quick rally's: the car's model resolved through its versions)
    if (const std::string sgc = Arg(args, "--sgc", "Cars/i20_R5/i20_R5.sgc"); std::filesystem::path(sgc).is_absolute())
        c.sgc = sgc;
    else
        c.sgc = game + "/" + sgc;
    // The car's physics: --car (a physics.rbz folder's name, a VFS folder - Quick rally's: cars/<id>/physics/<p>/<v>/ -
    // or a folder on disk), else the .sgc's folder's own (Cars/<folder>/physics/), else physics.rbz's s_i2003.
    {
        std::error_code ec;
        const std::filesystem::path own = std::filesystem::path(c.sgc).parent_path() / "physics";
        const std::string ownCar = own.generic_string() + "/";
        c.car = Arg(args, "--car", std::filesystem::exists(own / "common.lsp", ec) ? ownCar.c_str() : "s_i2003");
    }
    // --physics-base: the physics' shared files, a VFS folder (the car physics.yaml's physicsBase); RBR's by default.
    c.base = Arg(args, "--physics-base", c.base.c_str());
    // W-Rally's values on top of RBR's car: --overrides <file> ("none", or none given: RBR's car as it is).
    c.overrides = Arg(args, "--overrides", "");
    if (c.overrides == "none") c.overrides.clear();
    // --road dry | damp | wet: the surface maps' weather (the .mat's: new road), as the tyres' default follows it.
    if (const std::string road = Arg(args, "--road", ""); !road.empty()) {
        if (road == "dry") c.weather = 0;
        else if (road == "damp") c.weather = 3;
        else if (road == "wet") c.weather = 6;
        else return std::fprintf(stderr, "--road %s: dry, damp or wet\n", road.c_str()), 2;
    }
    // --damage 0..3: off, safe, reduced, realistic (the options word's damage level: bits 8..15, with its flag 0x10).
    if (const std::string d = Arg(args, "--damage", ""); !d.empty()) {
        const unsigned long level = std::strtoul(d.c_str(), nullptr, 10);
        if (level > 3) return std::fprintf(stderr, "--damage %s: 0 off, 1 safe, 2 reduced, 3 realistic\n", d.c_str()), 2;
        c.settings = (c.settings & ~0xff00u) | 0x10u | static_cast<uint32_t>(level) << 8;
    }
    // --setup <file>: a saved setup - a VFS path (rbr/SavedGames/..., the car physics' setups/...), a file on disk - or
    // a physics.rbz member (Physics/...); default the car's own for the surface.
    c.setup = Arg(args, "--setup", "");
    o.weather = Arg(args, "--weather", "");
    // --sky <clouds>,<fog>,<rain>: over the weather preset, 0..1 each (Quick rally's custom weather).
    if (const std::string sky = Arg(args, "--sky", ""); !sky.empty()) {
        float v[3];
        if (std::sscanf(sky.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3)
            return std::fprintf(stderr, "--sky %s: <clouds>,<fog>,<rain>, 0..1 each\n", sky.c_str()), 2;
        o.skySet = true, o.skyClouds = std::clamp(v[0], 0.0f, 1.0f), o.skyFog = std::clamp(v[1], 0.0f, 1.0f),
        o.skyRain = std::clamp(v[2], 0.0f, 1.0f);
    }
    if (const int code = ParseSkyTime(args, o)) return code;
    // --lamps <the ramp> --lamp-aim <spread>,<pitch>: the car's lamps (NGG design §5.5).
    o.lampLayout = Arg(args, "--lamps", "");
    if (const std::string a = Arg(args, "--lamp-aim", ""); !a.empty() &&
        std::sscanf(a.c_str(), "%f,%f", &o.lampSpread, &o.lampPitch) != 2)
        return std::fprintf(stderr, "--lamp-aim %s: <spread degrees>,<pitch degrees>\n", a.c_str()), 2;
    // The surface and the tyre type: the stage's unless given.
    {
        std::string from = "--surface";
        c.surface = Arg(args, "--surface", "");
        if (c.surface.empty()) c.surface = rules::StageSurface(game, rules::StageMap(o.stage), &from);
        if (c.surface != "tarmac" && c.surface != "gravel" && c.surface != "snow")
            return std::fprintf(stderr, "--surface %s: tarmac, gravel or snow\n", c.surface.c_str()), 2;
        const std::string type = Arg(args, "--tyre-type", "");
        c.tyreIndex = rules::SurfaceTyreType(c.surface, c.wetness >= 0 ? c.wetness : static_cast<int32_t>(c.weather / 3));
        if (!type.empty()) {
            char* end;
            c.tyreIndex = std::strtoul(type.c_str(), &end, 10);
            if (end == type.c_str() || *end)
                for (c.tyreIndex = 0; c.tyreIndex < 7 && type != rules::kTyreTypeNames[c.tyreIndex]; ++c.tyreIndex) {}
            if (c.tyreIndex >= 7) return std::fprintf(stderr, "--tyre-type %s: 0..6 or a TyreTypeNN name\n", type.c_str()), 2;
        }
        c.tyres = Arg(args, "--tyres", "michelin");
        std::printf("stage %s: surface %s (%s), tyres TyreType%02u %s%s, brand %s\n", o.stage.c_str(), c.surface.c_str(),
                    from.c_str(), c.tyreIndex, rules::kTyreTypeNames[c.tyreIndex], type.empty() ? "" : " (--tyre-type)",
                    c.tyres.c_str());
    }
    o.out = Arg(args, "--out", "drive.phr");
    // The driving aids (AutoGears, GearGuard, NeutralLock, ClutchHelp): none by default, as the game's. Without the
    // clutch help the engine stalls when a gear goes in at idle with no throttle; it restarts in neutral with the
    // throttle (StarterMotor). --aids 0,0,0,1 keeps it running.
    {
        const std::string aids = Arg(args, "--aids", "0,0,0,0");
        const char* a = aids.c_str();
        for (int k = 0; k < 4 && a; ++k) {
            char* end;
            c.aids[k] = std::strtoul(a, &end, 0);
            a = *end == ',' ? end + 1 : nullptr;
        }
    }
    s.physicsModules = ExeDir() / "modules" / "physics";
    s.physicsId = Arg(args, "--physics", "rbr");
    s.dls = maps + ".dls";
    s.out = o.out;
    s.replay = Arg(args, "--replay", "");
    s.replayAids = !Arg(args, "--aids", "").empty();
    s.quickStart = HasFlag(args, "--quick-start");
    s.entry = std::atof(Arg(args, "--entry", "2").c_str());
    s.skipTo = std::strtoul(Arg(args, "--skip-to", "0").c_str(), nullptr, 0);
    s.launchRpm = static_cast<float>(std::atof(Arg(args, "--launch-rpm", "7400").c_str()));
    s.antiLagCrack = static_cast<float>(std::atof(Arg(args, "--als-crack", "0.25").c_str()));
    // How far behind the car the chase camera sits ([ / ] move it while driving), and how much further back at speed.
    s.chaseDistance = static_cast<float>(std::atof(Arg(args, "--chase-distance", "5.2").c_str()));
    s.chaseDistance = std::max(2.0f, std::min(20.0f, s.chaseDistance));
    s.chasePull = std::max(0.0f, static_cast<float>(std::atof(Arg(args, "--chase-pull", "1.5").c_str())));

    o.rendererId = Arg(args, "--renderer", "wrenderer");
    o.sound = !HasFlag(args, "--no-sound");
    // The co-driver (sound/codriver.h): on unless --no-codriver; --codriver-lead s how far ahead it calls (the speed
    // times it), --codriver-volume its bus.
    o.codriver = !HasFlag(args, "--no-codriver");
    o.codriverReplay = HasFlag(args, "--codriver-replay");   // a replay: only with it (the calls are the live game's)
    o.codriverLead = std::clamp(static_cast<float>(std::atof(Arg(args, "--codriver-lead", "3").c_str())), 0.5f, 10.0f);
    o.codriverVolume = std::clamp(static_cast<float>(std::atof(Arg(args, "--codriver-volume", "1").c_str())), 0.0f, 1.0f);
    // The HUD's pacenotes (live only): the current call while it is spoken, the next --pacenote-next s before it.
    o.pacenoteNext = std::clamp(static_cast<float>(std::atof(Arg(args, "--pacenote-next", "8").c_str())), 0.0f, 60.0f);
    o.audioId = Arg(args, "--audio", "waudio");
    o.engine = Arg(args, "--eng", "Focus03-08");
    o.bangs = Arg(args, "--als-bangs", "");
    o.idleBangs = Arg(args, "--als-idle-bangs", "");
    o.mix = Arg(args, "--mix", "");
    if (o.mix.empty()) o.mix = WRallyFile("game/config/sound-mix.ini");
    const std::string camera = Arg(args, "--camera", "");
    o.camera = camera.empty() ? -1 : std::atoi(camera.c_str());
    o.cameraAngle = static_cast<float>(std::atof(Arg(args, "--camera-angle", "0").c_str()) * 3.14159265 / 180.0);
    o.fixedStep = std::atof(ArgOrEnv(args, "--step", "PHYS_DRIVE_STEP").c_str());
    o.pauseAt = std::strtoul(ArgOrEnv(args, "--pause-at", "PHYS_DRIVE_PAUSE_AT").c_str(), nullptr, 0);
    o.exitAt = std::strtoul(ArgOrEnv(args, "--exit-at", "PHYS_DRIVE_EXIT_AT").c_str(), nullptr, 0);
    o.hold = HasFlag(args, "--hold");
    return 0;
}

} // namespace wrally::app
