#pragma once
// wrally::assets::CarRender - the car as the renderer draws it: its render model from the .sgc (the exterior, this
// surface's wheels; assets/car_model.h) made a renderer model - a node per part, placed each frame with the car and
// its wheels - or, when the model gives nothing, the body hull from its collision mesh in a red paint.
//
// Material 0 is always that paint (unused when the model is drawn), as phys-drive made it first: the renderer's
// material slots stay as they were.

#include <cstdint>
#include <string>
#include <vector>

#include "data/rec.h"
#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::render {
class Renderer;
}

namespace wrally::assets {

class CarRender {
public:
    // The model on the renderer's loaded scene. `collision`: the car's collision meshes (the hull's fallback).
    void Create(render::Renderer& renderer, const std::string& sgc, const std::string& surface,
                const std::vector<phys::data::RecMesh>& collision);
    bool Ready() const { return m_instance != 0; }

    // The frame's placement: the car at `pose`, its wheels (in its frame) at `wheels`, `cog` the body's centre of
    // gravity in the model's frame. Valid until the next call.
    const iface::InstanceUpdate& Place(const physics::Pose& pose, const physics::Wheel wheels[4], const float cog[3]);

private:
    struct Part {
        int wheel;     // -1: the car body's; else 0 lf, 1 rf, 2 lb, 3 rb (in that hub's frame)
        bool spins;    // turns with the wheel
    };
    std::vector<Part> m_parts;
    iface::InstanceHandle m_instance = 0;
    std::vector<float> m_nodes;
    iface::InstanceUpdate m_update{};
};

} // namespace wrally::assets
