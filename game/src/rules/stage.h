#pragma once
// The stage's rules the game keeps beside the physics (docs/design.md 4.1: the stage clock is the game's, not the
// physics'): what RBR picks for a stage (its surface, the tyre type) and the stage clock. Moved here from the physics'
// sim/sim.h (plugin-rbr's phys/sim), unchanged.

#include <cstdint>
#include <string>
#include <vector>

namespace wrally::rules {

// What the game picks for a stage. The surface (the car's <surface>.lsp, its setup and options): the exe's map table
// (0x4a1330: maps 10..18 snow, 41..49, 73, 85, 86, 94 tarmac, the rest gravel), unless the map's Maps/Tracks.ini
// section says Surface= (0 tarmac, 1 gravel, 2 snow: the stage mods' key, which the plugin's hook of 0x4a1330 reads).
// `from` says which. The map: a stage name's number (track-34_E: 34), -1 for none.
int StageMap(const std::string& stage);
std::string StageSurface(const std::string& game, int map, std::string* from = nullptr);
// The tyre type (TyreTypeNN; IRBRGame::ERBRTyreTypes): tarmac dry / intermediate / wet, gravel dry / intermediate /
// wet, snow. The game's runs on tarmac, gravel and snow hold 0, 3 and 6 (the setup's tyres, the snapshots' [0x7ecd00]).
uint32_t SurfaceTyreType(const std::string& surface, int32_t wetness);
extern const char* const kTyreTypeNames[7];

// The stage time as the game keeps it for the driver (its car info, the HUD's: the countdown +0x244, the race time
// +0x140, the false start +0x248), beside the physics: it reads the car (its speed, its drive line distance), it
// never writes it. From the exe:
//  - the reset (0x5772e0): the countdown 7.0 s, the race time 0 and stopped;
//  - each frame (0x573d90): the countdown less the frame's time; it crossing 0 is GO (the race time runs from there);
//    below 6.0 the start marshal's animation (start_animation2);
//  - the countdown shown (0x578c30): 3 for (2, 3], 2 for (1, 2], 1 for (0, 1], GO for (-2, 0]; nothing after a false start;
//  - a false start (0x579590 -> 0x579430): the car above 1.0 m/s while the countdown is in (0, 4): the race time gets
//    what is left of the countdown and the penalty (0x565ea0: 10 s, the season modes their own), the countdown 0, the
//    clock running; the HUD's message for 3 s (+0x238).
// The splits and the finish are the .dls's marks (pacenotes 23 / 22, along the drive line), as the game's pacenote
// walker (0x4783b0) fires them when the car passes: a checkpoint keeps the race time as a split, shown for 5 s
// (0x572da0: +0x258 + 4 n, +0x26c = 5.0); the finish stops the race time (+0x144 = 1) and marks the stage finished
// (0x573530: +0x2c4 = 1; the plugins' HandleResults). What the game does with the car and the camera after the finish is
// not looked into: the car stays the driver's. (The physics' own clock, Regulation::CTimeKeeper / CTimeCard - core/rules.h's StageTiming - starts on the
// start mark instead; the game's standalone runs leave it out, and so does this.)
class StageClock {
public:
    enum class State { Countdown, Running, Finished };
    enum Event { kNone, kGo, kFalseStart, kSplit, kFinish };
    static constexpr float kCountdown = 7.0f;           // [0x738398]
    static constexpr float kFalseStartPenalty = 10.0f;  // [0x72b154]

    // The marks: the finish and the checkpoints' distances (a negative finish: none known, the clock never stops).
    void SetMarks(float finish, std::vector<float> checkpoints);
    void Reset(float countdown = kCountdown);
    // Time passing (the physics' step, dt s) with the car at `speed` (m/s) and `distance` along the drive line. Returns
    // what happened then (kSplit: Splits().back() is the new one).
    Event Advance(float dt, float speed, float distance);

    State GetState() const { return m_state; }
    float Countdown() const { return m_countdown; }   // > 0 before GO
    float Time() const { return m_time; }             // the stage time (the race time, the penalty in)
    bool FalseStart() const { return m_falseStart; }
    const std::vector<float>& Splits() const { return m_splits; }
    size_t SplitCount() const { return m_checkpoints.size(); }
    // What the HUD's middle shows: "3", "2", "1", "GO", "FALSE START" (3 s after it), a split (5 s after it), the finish
    // with the final time (from then on) or "".
    std::string Shown() const;
    static std::string Text(float t);   // m:ss.cc

private:
    State m_state = State::Countdown;
    float m_countdown = kCountdown, m_time = 0, m_message = 0, m_splitShown = 0;
    bool m_falseStart = false;
    float m_finish = -1;
    std::vector<float> m_checkpoints, m_splits;
};

} // namespace wrally::rules
