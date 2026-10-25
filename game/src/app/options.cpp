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

std::string Arg(int argc, char** argv, const char* name, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
bool HasFlag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], name)) return true;
    return false;
}
std::string ArgOrEnv(int argc, char** argv, const char* name, const char* env)
{
    const std::string a = Arg(argc, argv, name, "");
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

int Parse(int argc, char** argv, Options& o)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: WRally <game dir> <stage .ngg> [--stage track-34_E] [--sgc <car .sgc>] [--car s_i2003] "
                             "[--surface gravel] [--tyre-type 3] [--tyres michelin] [--out drive.phr]\n");
        return 2;
    }
    o.game = argv[1];
    o.ngg = argv[2];
    const std::string& game = o.game;
    o.stage = Arg(argc, argv, "--stage", "");
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
    c.rbz = game + "/physics.rbz";
    c.trk = maps + ".trk";
    c.col = maps + ".col";
    c.mat = maps + ".mat";
    c.fnc = maps + ".fnc";
    c.lbs = maps + ".lbs";
    c.sgc = game + "/" + Arg(argc, argv, "--sgc", "Cars/i20_R5/i20_R5.sgc");
    c.car = Arg(argc, argv, "--car", "s_i2003");
    // WRally's values on top of RBR's car: --overrides, else content/cars/<car>.ini (WRallyFile) if there is one
    // ("none": RBR's car as it is).
    c.overrides = Arg(argc, argv, "--overrides", "");
    if (c.overrides.empty()) c.overrides = WRallyFile("content/cars/" + c.car + ".ini");
    else if (c.overrides == "none") c.overrides.clear();
    // The surface and the tyre type: the stage's unless given.
    {
        std::string from = "--surface";
        c.surface = Arg(argc, argv, "--surface", "");
        if (c.surface.empty()) c.surface = rules::StageSurface(game, rules::StageMap(o.stage), &from);
        if (c.surface != "tarmac" && c.surface != "gravel" && c.surface != "snow")
            return std::fprintf(stderr, "--surface %s: tarmac, gravel or snow\n", c.surface.c_str()), 2;
        const std::string type = Arg(argc, argv, "--tyre-type", "");
        c.tyreIndex = rules::SurfaceTyreType(c.surface, c.wetness >= 0 ? c.wetness : static_cast<int32_t>(c.weather / 3));
        if (!type.empty()) {
            char* end;
            c.tyreIndex = std::strtoul(type.c_str(), &end, 10);
            if (end == type.c_str() || *end)
                for (c.tyreIndex = 0; c.tyreIndex < 7 && type != rules::kTyreTypeNames[c.tyreIndex]; ++c.tyreIndex) {}
            if (c.tyreIndex >= 7) return std::fprintf(stderr, "--tyre-type %s: 0..6 or a TyreTypeNN name\n", type.c_str()), 2;
        }
        c.tyres = Arg(argc, argv, "--tyres", "michelin");
        std::printf("stage %s: surface %s (%s), tyres TyreType%02u %s%s, brand %s\n", o.stage.c_str(), c.surface.c_str(),
                    from.c_str(), c.tyreIndex, rules::kTyreTypeNames[c.tyreIndex], type.empty() ? "" : " (--tyre-type)",
                    c.tyres.c_str());
    }
    o.out = Arg(argc, argv, "--out", "drive.phr");
    // The driving aids (AutoGears, GearGuard, NeutralLock, ClutchHelp): none by default, as the game's. Without the
    // clutch help the engine stalls when a gear goes in at idle with no throttle; it restarts in neutral with the
    // throttle (StarterMotor). --aids 0,0,0,1 keeps it running.
    {
        const std::string aids = Arg(argc, argv, "--aids", "0,0,0,0");
        const char* a = aids.c_str();
        for (int k = 0; k < 4 && a; ++k) {
            char* end;
            c.aids[k] = std::strtoul(a, &end, 0);
            a = *end == ',' ? end + 1 : nullptr;
        }
    }
    s.physicsModules = ExeDir() / "modules" / "physics";
    s.physicsId = Arg(argc, argv, "--physics", "rbr");
    s.dls = maps + ".dls";
    s.out = o.out;
    s.replay = Arg(argc, argv, "--replay", "");
    s.replayAids = !Arg(argc, argv, "--aids", "").empty();
    s.quickStart = HasFlag(argc, argv, "--quick-start");
    s.entry = std::atof(Arg(argc, argv, "--entry", "2").c_str());
    s.skipTo = std::strtoul(Arg(argc, argv, "--skip-to", "0").c_str(), nullptr, 0);
    s.launchRpm = static_cast<float>(std::atof(Arg(argc, argv, "--launch-rpm", "7400").c_str()));
    s.antiLagCrack = static_cast<float>(std::atof(Arg(argc, argv, "--als-crack", "0.25").c_str()));
    // How far behind the car the chase camera sits ([ / ] move it while driving), and how much further back at speed.
    s.chaseDistance = static_cast<float>(std::atof(Arg(argc, argv, "--chase-distance", "5.2").c_str()));
    s.chaseDistance = std::max(2.0f, std::min(20.0f, s.chaseDistance));
    s.chasePull = std::max(0.0f, static_cast<float>(std::atof(Arg(argc, argv, "--chase-pull", "1.5").c_str())));

    o.rendererId = Arg(argc, argv, "--renderer", "wrenderer");
    o.sound = !HasFlag(argc, argv, "--no-sound");
    o.audioId = Arg(argc, argv, "--audio", "waudio");
    o.engine = Arg(argc, argv, "--eng", "Focus03-08");
    o.bangs = Arg(argc, argv, "--als-bangs", "");
    o.idleBangs = Arg(argc, argv, "--als-idle-bangs", "");
    o.mix = Arg(argc, argv, "--mix", "");
    if (o.mix.empty()) o.mix = WRallyFile("game/config/sound-mix.ini");
    const std::string camera = Arg(argc, argv, "--camera", "");
    o.camera = camera.empty() ? -1 : std::atoi(camera.c_str());
    o.cameraAngle = static_cast<float>(std::atof(Arg(argc, argv, "--camera-angle", "0").c_str()) * 3.14159265 / 180.0);
    o.fixedStep = std::atof(ArgOrEnv(argc, argv, "--step", "PHYS_DRIVE_STEP").c_str());
    o.pauseAt = std::strtoul(ArgOrEnv(argc, argv, "--pause-at", "PHYS_DRIVE_PAUSE_AT").c_str(), nullptr, 0);
    o.exitAt = std::strtoul(ArgOrEnv(argc, argv, "--exit-at", "PHYS_DRIVE_EXIT_AT").c_str(), nullptr, 0);
    o.hold = HasFlag(argc, argv, "--hold");
    return 0;
}

} // namespace wrally::app
