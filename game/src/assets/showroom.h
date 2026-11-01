#pragma once
// wrally::assets::BuildShowroom - the garage's showroom (Quick rally -> Garage): a synthetic scene, built in memory as an
// .ngg as the lamp lab's is (assets/lamp_lab.h), for one car on a turntable - a dark floor all round and the turntable
// at the origin, kShowroomDeck high, a little lighter. The car stands on it facing the render frame's -z.

#include <cstdint>
#include <vector>

namespace wrally::assets {

constexpr float kShowroomDeck = 0.04f;     // the turntable's top over the floor, metres
constexpr float kShowroomRadius = 3.2f;    // its radius

std::vector<uint8_t> BuildShowroom();

}  // namespace wrally::assets
