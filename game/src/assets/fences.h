#pragma once
// wrally::assets::Fences - the stage's roadside fences (its .fnc: formats/fnc.h), drawn and knocked about by the car.
//
// RBR builds them itself from the .fnc's post points (StickSystem's fence handler): a post on each point, a tape (or a
// net, a wire: the group's tile texture) from each post to the next, their textures from Textures/fence.dat. They
// never push the car - the physics only has inert boxes for them (physics-rbr's FenceDummy, which makes RBR's
// spectators react) - so all of their motion is here, cosmetic, on the game's side: the car's collision hull's box
// shoves the posts and drags the tape; a post leans and springs back, or past half its height falls and stays down; a
// tape stretched by the car pulls its posts in, and stretched past half its length again it snaps, its ends hanging
// from the posts.
//
// The sizes are not RBR's (its fence renderer, 0x6058c0, is not reverse engineered): a 1.1 m post 6 cm thick, a 10 cm
// band of tape near its top, the tile texture repeated every 0.5 m.
//
// A model per group: a node per post, two per tape (its halves, the car's push between them), a mesh on each. The
// posts' nodes carry a post's lean (rows: its sides, its axis foot to top, its foot); a half's node carries a unit
// quad from one end to the other with the band's height across - an affine map, so a stretched tape stays a strip.
// Only groups that moved are sent to the renderer (its instances keep their last placement).

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.h"
#include "data/rec.h"
#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::render {
class Renderer;
}

namespace wrally::assets {

class Fences {
public:
    // The stage's fences from its .fnc, RBR's fence textures from <game>/Textures/fence.dat, on the renderer's loaded
    // scene. "" or what is wrong; a stage without fences is not wrong (Posts() 0).
    std::string Create(render::Renderer& renderer, const std::string& fnc, const std::string& game);
    // The car's box: its body hull's (the collision mesh, the model's frame) bounds.
    void SetCar(const phys::data::RecMesh& hull);
    size_t Posts() const { return m_posts.size(); }
    size_t Tapes() const { return m_tapes.size(); }

    // `seconds` of the fences' motion (0: paused) with the car at `pose` (`cog` its centre of gravity in the model's
    // frame). The groups that moved, to draw; valid until the next call.
    const std::vector<iface::InstanceUpdate>& Step(double seconds, const physics::Pose& pose, const float cog[3]);

private:
    struct Post {
        math::V3 foot;
        math::V3 lean{}, velocity{};   // its top's offset from upright (horizontal), and how it moves
        math::V3 pull{};               // the tapes' pull this step
        bool down = false;             // fallen: it stays down
    };
    struct Tape {
        uint32_t a, b;                 // its posts
        float rest;                    // its length between them
        float u0;                      // the texture's u at its start
        math::V3 bulge{}, velocity{};  // its middle's offset from the straight line, and how it moves
        bool broken = false;
    };
    struct Group {
        uint32_t posts, postCount, tapes, tapeCount;
        math::V3 lo, hi;               // the posts' box
        iface::InstanceHandle instance = 0;
        bool moving = true;            // (true: placed on the first frame)
    };

    bool Push(math::V3 point, math::V3& out) const;
    void StepGroup(Group& g, float h);
    math::V3 Top(const Post& p) const;
    math::V3 Attach(const Post& p) const;
    math::V3 Middle(const Tape& t) const;
    void Place(Group& g);

    std::vector<Post> m_posts;
    std::vector<Tape> m_tapes;
    std::vector<Group> m_groups;
    std::vector<float> m_nodes;        // 16 a node, the groups' in order
    std::vector<iface::InstanceUpdate> m_updates;
    double m_acc = 0.0;
    // The car: its box in its frame (the centre of gravity's; physics axes), its pose, its velocity (render axes).
    float m_lo[3]{}, m_hi[3]{}, m_hullLo[3]{}, m_hullHi[3]{};
    bool m_hasCar = false;
    float m_rotation[4]{0, 0, 0, 1}, m_position[3]{};
    math::V3 m_velocity{};
};

} // namespace wrally::assets
