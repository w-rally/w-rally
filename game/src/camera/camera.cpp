#include "camera/camera.h"

#include <algorithm>
#include <cmath>

namespace wrally::camera {

using math::V3;

float ChaseRig::PullWant(const physics::Pose& p) const
{
    const float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
    return std::min(2.0f * pull, pull * speed * 3.6f / 100.0f);
}

V3 ChaseRig::Behind(const physics::Pose& p)
{
    const V3 fwd = math::Rotate(p.rotation, {0, -1, 0});
    return math::Norm({-fwd.x, -fwd.y, 0});
}

V3 ChaseRig::Eye(const physics::Pose& p, V3 behind, float pullNow) const
{
    return V3{p.position[0], p.position[1], p.position[2]} + behind * (distance + pullNow) + V3{0, 0, 2.2f};
}

Chase ChaseRig::Settle(const physics::Pose& p) const
{
    Chase c;
    c.behind = Behind(p);
    c.pull = PullWant(p);
    c.eye = Eye(p, c.behind, c.pull);
    return c;
}

Chase ChaseRig::Follow(const Chase& before, const physics::Pose& p, float dt) const
{
    Chase c;
    const V3 want = Behind(p);
    const V3 mixed = before.behind + (want - before.behind) * std::min(1.0f, dt * 6.0f);
    // Opposite directions mix to nothing (a spin): take the new one then.
    c.behind = (mixed.x * mixed.x + mixed.y * mixed.y > 1e-6f) ? math::Norm(mixed) : want;
    c.pull = before.pull + (PullWant(p) - before.pull) * std::min(1.0f, dt * 1.2f);
    c.eye = Eye(p, c.behind, c.pull);
    return c;
}

View Look(int mode, const physics::Pose& p, V3 chaseEye, float orbit, ReplayCameras* tv)
{
    const V3 pos{p.position[0], p.position[1], p.position[2]};
    const V3 fwd = math::Rotate(p.rotation, {0, -1, 0}), upv = math::Rotate(p.rotation, {0, 0, 1});
    View v;
    ReplayView shot;
    if (mode == kTv && tv && tv->View(p.position, &shot)) {
        v.eye = {shot.eye[0], shot.eye[1], shot.eye[2]};
        v.at = {shot.at[0], shot.at[1], shot.at[2]};
        v.fovY = shot.fovY;
        v.tvCamera = shot.camera;
    } else if (mode == kChase || mode == kTv) {
        v.eye = chaseEye;
        v.at = pos + V3{0, 0, 0.8f};
    } else if (mode == kBeside) {   // at its left front wheel (, / . turn it round): a check of the wheels
        const V3 left = math::Rotate(p.rotation, {1, 0, 0});
        const float ca = std::cos(orbit), sa = std::sin(orbit);
        const V3 side = left * ca + fwd * sa, ahead = fwd * ca - left * sa;   // left and fwd turned by the orbit
        v.at = pos + fwd * 0.4f;
        v.eye = v.at + side * 4.5f + ahead * 0.2f + upv * 0.6f;
    } else if (mode == kAbove) {   // north up: a check of the car's placement
        v.eye = pos + V3{0, -0.01f, 25.0f};
        v.at = pos;
    } else {   // the bonnet
        v.eye = pos + upv * 1.25f - fwd * 0.3f;
        v.at = v.eye + fwd * 10.0f;
    }
    return v;
}

} // namespace wrally::camera
