#pragma once
// The stage's movable objects drawn where the physics has them: the .ngg keys every object built from a movable
// placement with RBR's instance id (NGGK), the same id the .trk's collision instance - the physics' solid - has. Each
// frame a solid that has moved gives its key its world-space move since the load (iface::StageObjectUpdate: the
// renderer moves every object with that key). A solid that never moved sends nothing.

#include <cstdint>
#include <vector>

#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::render {

class StageObjects {
public:
    // This frame's solids (the physics' order). The updates are valid until the next call.
    const std::vector<iface::StageObjectUpdate>& Update(const std::vector<physics::Body>& bodies);

private:
    std::vector<uint8_t> m_moving;
    std::vector<iface::StageObjectUpdate> m_updates;
};

} // namespace wrally::render
