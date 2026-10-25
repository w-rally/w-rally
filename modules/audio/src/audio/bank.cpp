#include "audio/bank.h"

#include <cstdio>
#include <filesystem>

namespace audio {

std::string Bank::LoadBangs(const std::string& dir, bool idle)
{
    std::vector<Sample>& set = idle ? antiLagIdleBangs : antiLagBangs;
    set.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".wav") continue;
        Sample s;
        if (std::string err = ReadWav(e.path().string(), s); !err.empty()) return err;
        set.push_back(std::move(s));
    }
    if (ec) return "cannot list " + dir;
    return set.empty() ? dir + ": no .wav" : "";
}

std::string Bank::LoadRbr(const std::string& game, const std::string& engineName)
{
    const std::string a = game + "/audio/";
    std::string missing;
    auto note = [&](const std::string& e) {
        if (!e.empty()) missing += e + "\n";
    };
    auto one = [&](const std::string& path, Sample& s) { note(ReadWav(a + path + ".wav", s)); };
    auto set = [&](const std::string& path, int from, int to, int digits, std::vector<Sample>& v) {
        v.clear();
        for (int i = from; i <= to; ++i) {
            char n[8];
            std::snprintf(n, sizeof n, "%0*d", digits, i);
            Sample s;
            if (std::string e = ReadWav(a + path + n + ".wav", s); e.empty()) v.push_back(std::move(s));
            else note(e);
        }
    };

    std::string warning;
    // A name is RBR's (Cars/<name>.eng, else the same in audio.bak - the older sets kept there); a path (a slash or
    // .eng in it) is taken as it is.
    const bool path = engineName.find_first_of("/\\") != std::string::npos || engineName.find(".eng") != std::string::npos;
    std::string eng = path ? engineName : a + "Cars/" + engineName + ".eng";
    if (!path)
        if (std::FILE* f = std::fopen(eng.c_str(), "rb")) std::fclose(f);
        else eng = game + "/audio.bak/Cars/" + engineName + ".eng";
    note(ReadEng(eng, engine, &warning));
    // Cars.ini's low shelf is RBR's mix for its own loops; a recording of our own (a path) plays as recorded - the
    // shelf on a real engine buried its mids (2.5 - 5 dB under the recording's, a Rally1 preview).
    if (path) lowShelfGain = 1;
    note(warning);
    one("Cars/Effects/turbo", turbo);
    one("Cars/Effects/BlowOffValve", valve);
    one("Cars/Effects/trans", transmission);
    one("Ambient/wind", wind);
    gearChanges.assign(3, Sample{});
    one("Cars/Effects/gearchng", gearChanges[0]);
    one("Cars/Effects/gearchn2", gearChanges[1]);
    one("Cars/Effects/gearchn3", gearChanges[2]);
    set("Cars/Effects/Dogring/", 1, 8, 1, dogRings);
    set("Cars/Effects/PopsNew/", 1, 11, 1, pops);
    set("Cars/Effects/Bang/", 1, 6, 2, bangs);
    one("Surfaces/Normal/generic", roll[0]);
    one("Surfaces/Skid/tarm", skid[0]);
    one("Surfaces/Normal/gravel", roll[1]);
    one("Surfaces/Skid/gravel", skid[1]);
    one("Surfaces/Normal/snow", roll[2]);
    one("Surfaces/Skid/snow", skid[2]);
    return missing;
}

} // namespace audio
