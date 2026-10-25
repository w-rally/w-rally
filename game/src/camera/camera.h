#pragma once
// The cameras (docs/design.md 6.5), as phys-drive had them: behind and above the car (chase), on its roof looking ahead
// (bonnet), above it, beside it, or - in a replay - the stage's TV cameras (camera/replay_camera.h).
//
// The chase camera trails the car on the PHYSICS' time, step by step (ChaseRig::Follow after every step), and a frame
// draws it between the last two steps like everything else - so it moves with the clock however the steps fall into
// frames. Only its DIRECTION trails the car's heading: smoothing its position instead made it trail the car itself, a
// lag of 6 / s is speed / 6 behind its target - 5 m more at 108 km/h - so the faster the car, the further back it fell.

#include <cstdint>

#include "camera/replay_camera.h"
#include "core/math.h"
#include "physics/physics.h"

namespace wrally::camera {

enum Mode { kChase = 0, kBonnet = 1, kAbove = 2, kBeside = 3, kTv = 4 };

// The chase camera at one physics step.
struct Chase {
    math::V3 eye{};      // where it is
    math::V3 behind{};   // the way from the car to it, flat and unit: what trails, smoothed step by step
    float pull = 0.0f;   // metres further back for the speed, eased
};

class ChaseRig {
public:
    // How far behind the car, metres (5.2 by default; [ / ] move it), and how much further back at speed: `pull`
    // metres per 100 km/h, at most twice that, eased over about a second - it falls back as the car accelerates and
    // closes in as it brakes.
    float distance = 5.2f;
    float pull = 1.5f;

    // Where it settles at a pose (a start: no step before).
    Chase Settle(const physics::Pose& p) const;
    // After a step of `dt` seconds to pose `p`, from where it was.
    Chase Follow(const Chase& before, const physics::Pose& p, float dt) const;
    // The eye for a pose, a direction and a pull (distance behind along `behind`, 2.2 m up).
    math::V3 Eye(const physics::Pose& p, math::V3 behind, float pullNow) const;

private:
    float PullWant(const physics::Pose& p) const;
    static math::V3 Behind(const physics::Pose& p);
};

// What a frame looks from: eye, target (physics axes) and the vertical field of view; tvCamera the TV camera shown
// (-1 none).
struct View {
    math::V3 eye{}, at{};
    float fovY = 1.0f;
    int32_t tvCamera = -1;
};

// The view of `mode` at the car's pose `p` (the frame's); `chaseEye` the chase camera's eye for this frame, `orbit` the
// beside camera's place round the car (radians from its left toward its front). A TV mode without a TV camera there
// is the chase camera.
View Look(int mode, const physics::Pose& p, math::V3 chaseEye, float orbit, ReplayCameras* tv);

} // namespace wrally::camera
