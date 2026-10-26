// car-parts: a part of the car drawn where the physics has it (assets::PartNode). A part torn off is the same solid
// in the world's frame (World::ReleasePart: position the car's frame moved into the world's, rotation the car's times
// its own), so at the moment it comes off its node must not move: the in-car pose and the world pose of one moment give
// the same node. And an undamaged part (at its rest pose) is placed exactly as the body.
//
// Exit code 0 when both hold.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "assets/car_render.h"
#include "core/math.h"

using namespace wrally;

namespace {

void Normalize(float q[4])
{
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i) q[i] /= l;
}

// a * b (x, y, z, w), as the physics composes rotations.
void QuatMul(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

float MaxDiff(const float* a, const float* b)
{
    float d = 0;
    for (int i = 0; i < 16; ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}

} // namespace

int main()
{
    // A car somewhere on a stage, turned and tilted; a front bumper hanging off its rest pose.
    float carRot[4] = {0.05f, -0.12f, 0.83f, 0.54f}, carPos[3] = {-1179.7f, -1402.5f, 434.5f};
    Normalize(carRot);
    physics::Part p{};
    std::snprintf(p.name, sizeof p.name, "bumper_f");
    p.restPosition[0] = 0.02f, p.restPosition[1] = -1.71f, p.restPosition[2] = -0.31f;
    p.restRotation[3] = 1;
    p.position[0] = 0.05f, p.position[1] = -1.74f, p.position[2] = -0.42f;
    float sway[4] = {0.11f, 0.02f, -0.05f, 0.99f};
    Normalize(sway);
    std::memcpy(p.rotation, sway, 16);
    p.inCar = true;

    float body[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.01f, -0.48f, 0.12f, 1};   // (the body node: less the cog)
    float car[16];
    assets::PoseMatrix(carRot, carPos, car);

    // 1: at rest, the body's node exactly.
    physics::Part rest = p;
    std::memcpy(rest.position, rest.restPosition, 12);
    std::memcpy(rest.rotation, rest.restRotation, 16);
    float node[16];
    assets::PartNode(body, rest, car, node);
    const bool restOk = !std::memcmp(node, body, sizeof node);
    std::printf("at rest: %s\n", restOk ? "the body's node exactly" : "NOT the body's node");

    // 2: the same moment in the car's frame and in the world's.
    float inCar[16], inWorld[16];
    assets::PartNode(body, p, car, inCar);
    physics::Part off = p;
    off.inCar = false;
    const math::V3 moved = math::Rotate(carRot, {p.position[0], p.position[1], p.position[2]});
    off.position[0] = carPos[0] + moved.x, off.position[1] = carPos[1] + moved.y, off.position[2] = carPos[2] + moved.z;
    QuatMul(carRot, p.rotation, off.rotation);
    assets::PartNode(body, off, car, inWorld);
    const float d = MaxDiff(inCar, inWorld);
    // (a float's rounding at 1400 m: about 1e-4 m; the rotation's entries far finer)
    const bool offOk = d < 1e-3f;
    std::printf("torn off, the same moment: the nodes differ by %g %s\n", double(d), offOk ? "(the same)" : "(DIFFERENT)");
    return restOk && offOk ? 0 : 1;
}
