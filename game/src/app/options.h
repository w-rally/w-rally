#pragma once
// WRally's command line (docs/design.md 6.5, wrally::app) - phase 1's phys-drive's, read into one Options.
//
//   WRally <game dir> <stage .ngg or .lbs> [--stage <the .ngg's>] [--sgc Cars/i20_R5/i20_R5.sgc] [--car s_i2003]
//          [--surface <the stage's>] [--tyre-type <its>] [--tyres michelin] [--aids 0,0,0,0] [--camera 0] [--out drive.phr]
//          [--replay run.phr [--hold]] [--physics rbr] [--audio waudio] [--renderer wrenderer]
//          [--setup <file>] [--damage 0..3] [--weather <preset>] [--road dry|damp|wet]
//          [--time HH:MM [--date YYYY-MM-DD] [--utc-offset h] [--latitude deg] [--longitude deg] [--north deg]]
//          [--skip-to <tick>] [--pause-at <tick>] [--exit-at <tick>] [--step <seconds>] [--camera-angle <degrees>]
//          [--chase-distance <metres>] [--chase-pull <metres per 100 km/h>] [--entry <seconds>] [--quick-start]
//          [--no-sound] [--no-codriver] [--codriver-lead 3] [--codriver-volume 1] [--eng Focus03-08]
//          [--mix sound-mix.ini] [--overrides cars/s_i2003.ini | none]
//          [--launch-rpm 7400] [--als-crack 0.25] [--als-bangs <folder of .wav>] [--als-idle-bangs <folder>]
//
// The stage (Maps/<stage>.*: the physics' ground) defaults to the .ngg's: track-34_E.ngg that one, track-34.ngg the
// map's one stage in Maps (several: the morning one, _M, or --stage needed); a --stage of another map than the .ngg's
// is warned about - on the screen too: the car would run on ground that is not drawn.
//
// The surface (the car's <surface>.lsp, setup and options) and the tyre type (TyreType00..06: tarmac dry /
// intermediate / wet, gravel dry / intermediate / wet, snow; a number or its name) default to the stage's, as the game
// picks them (rules::StageSurface); --surface and --tyre-type override them. --tyres is the tyre brand.
//
// --skip-to runs the physics to that tick at full speed before the first frame (nothing drawn), --pause-at pauses on
// reaching a tick, --exit-at quits there (as Esc would: the recording is written), --step advances every frame by a
// fixed time - the same frames at the same ticks run to run, the simulation in lockstep with the frames
// (session/sim_thread.h). --pause-at, --exit-at and --step also read PHYS_DRIVE_PAUSE_AT / _EXIT_AT / _STEP. A replay
// quits at its end (its check reported, as at Esc); --hold keeps its last frame up instead.
//
// The sound: RBR's (<game dir>/audio), the engine's loops from Cars/<--eng>.eng ("none": the pulse engine); --mix's
// file (default game/config/sound-mix.ini) read again whenever it is saved. Launch control (--launch-rpm, 0 off) and
// the anti-lag (--als-crack, 0 off; N arms it at standstill) sit between the pedal and the engine.
//
// The environment: PHYS_DRIVE_READOUT the readout on from the start, PHYS_DRIVE_OFF=taa,ao,shadows,rt those off from the
// start, PHYS_DRIVE_SHOTS=<n> a capture every n ticks, PHYS_DRIVE_JITTER=<tick> how steadily the car sits in the
// picture up to that tick (then quit), PHYS_DRIVE_NGGLOG the renderer's log on stdout, PHYS_DRIVE_MATERIALS the car's
// materials listed, PHYS_DRIVE_LOOK_UP=<degrees> every view tilted up by that much (the sky), PHYS_DRIVE_TEST_LIGHTS
// test lamps on the car (two headlight spots, a lamp behind it).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "session/session.h"

namespace wrally::app {

struct Options {
    std::string game, ngg, stage;
    std::string stageWarning;            // shown on the screen ("" none)
    session::Config session;
    std::string out = "drive.phr";
    std::string rendererId = "wrenderer";
    bool sound = true;
    bool codriver = true;                // the pacenotes read out (--no-codriver: not)
    float codriverLead = 3.0f;           // --codriver-lead: seconds ahead at the speed
    float codriverVolume = 1.0f;         // --codriver-volume: 0..1
    std::string audioId = "waudio", engine = "Focus03-08", bangs, idleBangs, mix;
    int camera = -1;                     // -1: the replay's TV cameras in a replay, else the chase camera
    float cameraAngle = 0.0f;            // the beside camera's place round the car, radians
    double fixedStep = 0.0;              // --step: every frame this long (0: the frame's real time)
    uint32_t pauseAt = 0, exitAt = 0;
    bool hold = false;                   // --hold: a replay's end keeps its last frame up (else the game quits)
    std::string weather;                 // --weather: the .ngg's weather preset by name ("" its default)
    // --time HH:MM [--date YYYY-MM-DD] [--utc-offset h] [--latitude deg] [--longitude deg] [--north deg]: the time and
    // place, the sun from them over the weather's (NGG design 5.4). Without --time, the weather's own sun.
    bool skyTimeSet = false;
    int skyYear = 2026, skyMonth = 6, skyDay = 21;
    double skyHour = 12.0, skyUtcOffset = 1.0, skyLatitude = 50.0, skyLongitude = 14.4, skyNorth = 0.0;
};

// The command line into `o`. Returns 0, or the exit code with what was wrong on stderr.
int Parse(int argc, char** argv, Options& o);
// The same from a list (args[0] the program) - what the menus build: a run from the menu is a command line too.
int Parse(const std::vector<std::string>& args, Options& o);

// The folder the exe is in.
std::filesystem::path ExeDir();

// One of WRally's own files (`rel` from the repository root: content/..., game/config/...): beside the exe as the game
// ships, else in the source tree the build came from (WRALLY_SOURCE_DIR) - where editing it shows at once, as the
// watched sound mix wants. "" if neither has it.
std::string WRallyFile(const std::string& rel);

} // namespace wrally::app
