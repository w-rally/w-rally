#pragma once
// wrally::assets::CarRender - the car as the renderer draws it: its render model from the .sgc (the exterior, this
// surface's wheels; assets/car_model.h) made a renderer model on a node tree, or - when the model gives nothing - the
// body hull from its collision mesh in a red paint.
//
// The nodes (each placed relative to the car's frame, CarPose's, but a spin node to its hub):
//   0          the body: the model's frame to the car's (less the centre of gravity)
//   1 + k      wheel k's hub (k = 0 lf, 1 rf, 2 lb, 3 rb): the physics' knuckle - steering, camber, toe - at the hub,
//              for the parts that do not turn with the wheel (calipers)
//   5 + k      its spin, under the hub: the parts that turn with the wheel (the tyre, the brake disc)
//   9 + i      the car's part i that can come loose (physics::Part: a door, the bonnet, a bumper), for the meshes of
//              the same name (e_<name>, e_int_<name>): where the physics has it - swinging on its hinge, hanging, or off
//              the car in the world
// A part where it sits at the load is placed exactly as the body, so an undamaged car draws as a rigid one.
//
// Material 0 is always that paint (unused when the model is drawn), as phys-drive made it first: the renderer's
// material slots stay as they were.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/car_lights.h"
#include "assets/car_model.h"
#include "data/rec.h"
#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::render {
class Renderer;
}

namespace wrally::assets {

// A physics pose (quaternion, position: physics axes) as the renderer's row-major matrix.
void PoseMatrix(const float rotation[4], const float position[3], float out[16]);
// A part's node, relative to the car's frame: the body node `body` (the model's frame to the car's), out of the part's
// frame at the load and into its frame now - in the car's frame (p.inCar), or the world's turned back into the car's
// by `car` (the car's pose matrix). Where it sits at the load, the body's exactly.
void PartNode(const float body[16], const physics::Part& p, const float car[16], float out[16]);

class CarRender {
public:
    // The model on the renderer's loaded scene. `collision`: the car's collision meshes (the hull's fallback); `parts`
    // the physics' parts that can come loose (their names place the meshes of the same names on their nodes).
    void Create(render::Renderer& renderer, const std::string& sgc, const std::string& surface,
                const std::vector<phys::data::RecMesh>& collision, const std::vector<physics::Part>& parts);
    // The same from a model read already (ReadCarModel: any thread - the reading is the slow part; null: it failed).
    void Create(render::Renderer& renderer, const CarModel* model, const std::vector<phys::data::RecMesh>& collision,
                const std::vector<physics::Part>& parts);
    bool Ready() const { return m_instance != 0; }
    // Its instances and models gone from the renderer's scene (its textures stay with the scene: a scene's go with it).
    // For a car taken off while the scene stays (the showroom's).
    void Release(render::Renderer& renderer);

    // The frame's placement: the car at `pose`, its wheels (in its frame) at `wheels`, `cog` the body's centre of
    // gravity in the model's frame, `parts` as Create had them (in that order). Valid until the next call.
    const iface::InstanceUpdate& Place(const physics::Pose& pose, const physics::Wheel wheels[4], const float cog[3],
                                       const std::vector<physics::Part>& parts);

    // THE LAMPS' MESHES (NGG design 5.4 N3b), placed as the car's parts are at the last Place - a lamp on a part that
    // came loose goes with it: per LampMeshGroup and side the unlit meshes or their lit twins (glowing in the lamp's
    // colour), as `lit` says - dark where the lamp's part is off the car (its cable torn); the ramp's housing when
    // `housing` (the car's own ramp chosen). Appended to `out`.
    void PlaceLamps(const bool lit[kLampGroups], bool housing, const std::vector<physics::Part>& parts,
                    std::vector<iface::InstanceUpdate>& out) const;
    // Where the lamps sit, for their lights (assets::CarLights): their parts gone or moved, by the last Place.
    void Mounts(const std::vector<physics::Part>& parts, LampMounts& out) const;

    // The car standing without the physics (a menu's): its wheels where the model's hubs are (physics axes, in the
    // model's frame - the body's origin - as Place takes them), and how far the origin stands over the ground (the hubs'
    // height plus the tyres' radius). False when the model has no hubs.
    bool Stance(physics::Wheel wheels[4], float& height) const;

private:
    bool m_hubFound[4] = {};
    float m_hubs[4][3] = {};     // the model's (render axes)
    float m_wheelRadius = 0;
    static constexpr uint32_t kBody = 0, kHub = 1, kSpin = 5, kParts = 9;
    iface::InstanceHandle m_instance = 0;
    std::vector<iface::ModelHandle> m_models;   // the car's and its lamps' (Release)
    uint32_t m_partNodes = 0;    // the parts' nodes (9 ..)
    std::vector<float> m_nodes;  // 16 a node
    iface::InstanceUpdate m_update{};
    // The lamps' instances, [group][side: 0 left, 1 right, 2 the middle][0 unlit, 1 lit] (0: the car has none), each a
    // model on the car's node tree; and the part each sits on ([group][side], -1 the body, -2 no mesh).
    iface::InstanceHandle m_lamps[kLampGroups][3][2] = {};
    int m_lampPart[kLampGroups][3];
};

} // namespace wrally::assets
