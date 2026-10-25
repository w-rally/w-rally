#pragma once
// S4's engine (doc/sound-engine-plan.md): the engine's loops cut into one grain per firing, and a grain played at
// every firing the physics reports. A loop slowed down to lower revs stretches every combustion pulse (the dull, "slow
// tape" low end of RBR's engines); a real engine's pulses stay about the same and only come further apart - which is
// what playing the grains, unpitched, at the firings' spacing does. A misfire or the limiter's cut is a missing grain.
//
// Cutting (CutGrains): a loop at rpm r fires every rate * 30 / r samples (four cylinders: two per turn); that period
// refined by the envelope's autocorrelation (+-7 %), the pulses' place in it from the envelope folded over the period
// (its peak) - the first mark - then each next mark where a period of the signal is most like the last mark's (the
// normalized cross-correlation, within a tenth of a period of a period on): consecutive grains line up in shape.
// A grain far quieter than the loop's usual one (under half its median: a silent lead-in, a dropout) or far brighter
// (high frequencies over 2.5 x the median share: a click, a stone) is dropped; the
// others get a gain that evens out the recording's slow level (its rms over +-0.25 s against the loop's median) - a fade
// or a car coming closer would otherwise swell and drop each time the grains wrap round - keeping the fast,
// firing-to-firing differences.
// A grain: kAttack periods before its mark to kTail after, and a crossfade of 0.2 periods at each end (sine-squared:
// grains a period apart sum to one through it). Firings further apart than that (revs below the lowest loop's) would
// leave silence between the pulses: the part after the mark is then read slower (down to kMinStretch of its speed),
// so the pulse's decay reaches the next one.

#include <array>
#include <cstdint>
#include <vector>

#include "audio/sample.h"

namespace audio {

struct GrainLoop {
    const Sample* sample = nullptr;
    float rpm = 0, load = 0;
    float period = 0;               // samples between firings (the sample's rate), refined
    float periodicity = 0;          // the envelope's autocorrelation at it (1 regular .. 0 none)
    std::vector<uint32_t> marks;    // each firing's peak
    std::vector<float> gains;       // each grain's: the recording's slow level (a fade, a car coming closer) evened out
};

// The grains of an engine's loops (four cylinders recorded).
std::vector<GrainLoop> CutGrains(const std::vector<EngineLoop>& loops, uint32_t cylinders = 4);

class GrainEngine {
public:
    static constexpr float kAttack = 0.3f, kTail = 0.7f;   // a grain's reach before and after its mark, in periods
    static constexpr float kMinStretch = 0.5f;

    void Set(const std::vector<GrainLoop>* loops);
    // A firing now at `rpm`: a grain from each of the loops around the revs (crossfaded by the revs' place between
    // their rpm) and off / on the throttle (`load` 0..1), times onGain / offGain. `firingSpacing`: the samples (48 kHz)
    // between firings at these revs - closer than the loop's own, the grain's span shrinks with them. Half the firings
    // jump to a random grain (8 or more away), so the recording's sequence never loops round audibly.
    void Fire(float rpm, float load, float onGain, float offGain, float firingSpacing, float rpmBlend = 1, float delay = 0);
    // rpmBlend: the share of the way between two loops' rpm over which both give grains (the mix's engine.rpmBlend).
    // delay: samples (48 kHz) before the grains start.
    float Next();   // one output sample (48 kHz)

    float jumps = 0.5f;   // the share of firings whose grain jumps to a random place in its loop

private:
    struct Voice {
        const GrainLoop* loop = nullptr;
        double position = 0, end = 0;   // in the sample
        double start = 0, mark = 0;
        float gain = 0, attack = 0, fade = 0;
        float stretch = 1;                // the read speed after the mark
        uint32_t wait = 0;                // samples before it starts
    };
    const std::vector<GrainLoop>* m_loops = nullptr;
    std::vector<float> m_rpm[2];       // the loops' rpm points off [0] and on [1] the throttle, sorted, unique
    std::vector<uint32_t> m_cursor;    // each loop's next grain
    uint32_t m_random = 0x2545f491;
    std::array<Voice, 48> m_voices{};
};

} // namespace audio
