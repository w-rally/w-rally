#pragma once
// The sounds a car plays, from RBR's audio folder (S1's baseline; doc/sound-engine-plan.md):
//   Cars/<engine>.eng                      the engine's loops (rpm x off / on the throttle; else ../audio.bak/Cars/<engine>.eng;
//                                          or a path to an .eng)
//   Cars/Effects/turbo, BlowOffValve, trans, gearchng / gearchn2 / gearchn3, Dogring/1..8, PopsNew/1..11, Bang/01..06
//   Surfaces/Normal|Skid/<surface>         the tyres rolling and sliding (gravel, generic|tarm for tarmac, snow)
//   Ambient/wind
// as Cars/Effects.ini names them. Cars.ini's low shelf (2.0 at 200 Hz for every car) goes on RBR's engines (not on an
// .eng given by path: our own recordings play as recorded).

#include <string>
#include <vector>

#include "audio/sample.h"

namespace audio {

struct Bank {
    std::vector<EngineLoop> engine;
    float lowShelfGain = 2.0f, lowShelfFrequency = 200.0f;
    Sample turbo, valve, transmission, wind;
    std::vector<Sample> gearChanges, dogRings, pops, bangs;
    std::vector<Sample> antiLagBangs;      // the anti-lag's exhaust bangs (LoadBangs); none: `bangs`
    std::vector<Sample> antiLagIdleBangs;  // its bangs at standstill (the bubbling idle); none: antiLagBangs
    Sample roll[3], skid[3];   // by signals::CarInfo::Surface

    // Loads what there is of the set; returns what was missing or bad, one line each ("" all there).
    std::string LoadRbr(const std::string& game, const std::string& engine);
    // Every .wav in `dir` as an anti-lag bang (`idle`: at standstill). Returns "" or what was wrong.
    std::string LoadBangs(const std::string& dir, bool idle = false);
};

} // namespace audio
