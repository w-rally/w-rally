#pragma once
// wrally::assets::BuildLampLab - the lamp lab (NGG design §5.5): a synthetic scene in which to look at a car's lamps,
// built in memory as an .ngg (the old lab's way, ngg/host/testscene). The car stands at the origin, facing the render
// frame's -z (the physics frame's -y), its centre of gravity kLabCogHeight over the floor; in front of it a flat floor,
// a wall 10 m ahead of its ramp (an aiming screen), and thin white poles and a person-sized block a few metres ahead,
// so each lamp's shadow falls apart from the others'. Untextured, exact linear albedos (floor 0.18, wall 0.5, poles 0.9).

#include <cstdint>
#include <vector>

namespace wrally::assets {

constexpr float kLabCogHeight = 0.45f;   // the car's centre of gravity over the floor, metres
constexpr float kLabWallAhead = 11.9f;   // the wall's face, metres ahead of the centre of gravity (10 m from the ramp)

std::vector<uint8_t> BuildLampLab();

}  // namespace wrally::assets
