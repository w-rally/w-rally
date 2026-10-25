#include "audio/mix.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace audio {

const char* const Mix::kNames[kLayers] = {"engine.on", "engine.off", "turbo", "valve", "pops", "bangs", "gear", "dogring",
                                          "transmission", "roll", "skid", "wind"};

Mix::Mix()
{
    for (float& v : volume) v = 1;
}

float Mix::Gain(Layer l) const
{
    bool anySolo = false;
    for (bool s : solo) anySolo |= s;
    return anySolo && !solo[l] ? 0.0f : volume[l] * master;
}

std::string Mix::Read(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "cannot open " + path;
    *this = Mix{};
    std::string line, errors;
    for (int n = 1; std::getline(f, line); ++n) {
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream in(line);
        std::string name, eq, flag;
        float v;
        if (!(in >> name)) continue;
        if (!(in >> eq >> v) || eq != "=") {
            errors += path + ":" + std::to_string(n) + ": expected `name = number [solo]`\n";
            continue;
        }
        const bool soloed = (in >> flag) && flag == "solo";
        if (name == "master") master = v;
        else if (name == "engine.rpmBlend") rpmBlend = v;
        else if (name == "engine.loadBlend") loadBlend = v;
        else if (name == "engine.grains") grains = v;
        else if (name == "engine.idleWander") idleWander = v;
        else if (name == "engine.grainJumps") grainJumps = v;
        else if (name == "antiLag.bangs") antiLagBangs = v;
        else if (name == "antiLag.idleBangs") antiLagIdleBangs = v;
        else if (name == "engine") volume[kEngineOn] = volume[kEngineOff] = v, solo[kEngineOn] = solo[kEngineOff] = soloed;
        else {
            int k = 0;
            while (k < kLayers && name != kNames[k]) ++k;
            if (k == kLayers) {
                errors += path + ":" + std::to_string(n) + ": no layer `" + name + "`\n";
                continue;
            }
            volume[k] = v;
            solo[k] = soloed;
        }
    }
    return errors;
}

std::string Mix::Describe() const
{
    std::string s;
    char t[64];
    for (int k = 0; k < kLayers; ++k)
        if (volume[k] != 1 || solo[k]) {
            std::snprintf(t, sizeof t, " %s %.2f%s", kNames[k], volume[k], solo[k] ? " solo" : "");
            s += t;
        }
    char u[96];
    std::snprintf(u, sizeof u, " master %.2f, blends rpm %.2f load %.2f, %s, idle wander %.2f", master, rpmBlend, loadBlend,
                  grains >= 0.5f ? "grains" : "loops", idleWander);
    return s + u;
    return s + t;
}

} // namespace audio
