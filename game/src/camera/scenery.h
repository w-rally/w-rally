#pragma once
// What a camera may not go through: the stage's static collision (the .col's triangle trees, the walls and banks the
// car hits), loaded for the cameras alone - the physics has its own, on its own thread. The beside camera stood 4.5 m
// off the car whatever was there: past a retaining wall it looked through the wall's back (drawn one-sided), at the
// near side of everything lit as from inside the wall - a banner's back black with the AO of the wall 0.7 m from it.

#include <memory>
#include <string>

#include "core/math.h"

namespace wrally::camera {

class Scenery {
public:
    Scenery();
    ~Scenery();
    // The stage's .col file. Returns "" or what is wrong (then nothing blocks).
    std::string Load(const std::string& colPath);
    // How far from `from` towards `to` is clear, 0..1 (1: nothing between), physics axes.
    float Clear(math::V3 from, math::V3 to) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace wrally::camera
