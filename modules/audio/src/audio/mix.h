#pragma once
// The mix by hand, until there is a GUI: a text file of `name = volume [solo]` lines (phys/drive/sound-mix.ini), read
// again whenever it changes. A volume multiplies the layer's level (1 as audio.cpp sets it); a layer marked solo plays
// with the other solo ones only. `engine` sets both of the engine's layers, `master` scales everything.

#include <string>

namespace audio {

struct Mix {
    enum Layer {
        kEngineOn,       // the engine's on-throttle loops (or S0's pulses)
        kEngineOff,      // its off-throttle loops
        kTurbo, kValve, kPops, kBangs, kGear, kDogRing, kTransmission, kRoll, kSkid, kWind,
        kLayers
    };
    static const char* const kNames[kLayers];

    float volume[kLayers];
    bool solo[kLayers] = {};
    float master = 1;
    // The engine's crossfades, as a share of the way between two recorded revs (and between off and on the throttle)
    // over which the two blend, in its middle: 1 all the way, 0 a cut.
    float rpmBlend = 1, loadBlend = 1;
    // The engine played as grains, one per firing (1: S4) or as loops pitched to the revs (0: RBR's way).
    float grains = 1;
    // The idle's wander (a real idle hunts: its firings drift early and late and swell a few times a second): x this.
    float idleWander = 1;
    // The grains' random jumps per pass through a loop (on average; else the next in the recording's order).
    float grainJumps = 2;
    // The anti-lag: the share of firings that set off a bang in the exhaust - 0.4, by ear against a Rally1 recording (over
    // 0.15 and 0.25; an onset count saturates at ~11-13 a second, overlapping bangs uncounted).
    float antiLagBangs = 0.4f;
    // At standstill (the bubbling idle, building up over its first seconds): 0.6, by ear against a Rally1 recording (over
    // 0.4, 0.8 and 1.0).
    float antiLagIdleBangs = 0.6f;

    Mix();
    // The layer's volume with the solos applied.
    float Gain(Layer l) const;
    // Reads `path` over the defaults. Returns "" or what was wrong (the lines it could read still apply).
    std::string Read(const std::string& path);
    // The same from the file's text; `path` names it in the messages.
    std::string Parse(const std::string& text, const std::string& path);
    std::string Describe() const;   // one line: what is not at its default
};

} // namespace audio
