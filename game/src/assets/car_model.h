#pragma once
// The car's render model (M5) from its .sgc - the renderer's scene file (HedgeHog3D), read as HedgeHog3D.dll's loader
// reads it (0x1001f2a0: records {u32 0xc, u32 kind, u32 version, u32 1, u32 size}; kind 0 a mesh - versions 1 and 3,
// the deformable parts carrying damage data after it -, kind 5 a material) - and its textures, through the car's
// <car>_shaders.ini; a mesh group's material is <car>.ini's [<mesh>] FaceBlock<g>_Material where it names one.
//
// A mesh (0x1000acf0): cstr name, u32 id, u32 parent, f32[14] the local transform, f32[12] the world transform (rows
// {r0 r1 r2 t}), f32[6] the bounds, u32 nv, u32 flags; with flags bit 0 nv x {position, normal} (f32[6]), bit 2 nv x uv
// (f32[2]), bit 4 nv x u32 (skipped by the game); u32 nt, nt x u16[3]; u32 ng, ng x {cstr material, u32 triangles,
// u32 first index, u32 vertices, u32 min index}.
//
// Nodes (meshes, helpers kind 3, cameras kind 2, transform nodes kind 19) form a tree by their parent ids; a node's
// 12-float matrix is relative to its parent. The root ("parent") is left out: the frame inside it is the renderer's
// car frame (y up, forward -z) - the physics' body frame with y and z swapped, as the collision meshes are.
//
// The wheels: the game places them itself, where the physics has them; a wheel's mesh is one for all four (its own
// matrix a leftover), its rim towards +x - outward on the left, turned half about y on the right. A wheel, and a part
// under a hub's helper (Point_e_wheel_<lf|rf|lb|rb>: brake disc, caliper, ...), is kept in its hub's frame (CarPart::
// wheel: the helper's place at the origin, the car's axes), for a renderer to put on the physics' wheel.

#include <cstdint>
#include <string>
#include <vector>

namespace wrally::assets {

struct CarVertex {
    float pos[3], normal[3], uv[2];
};

struct CarPart {
    std::string name;
    int wheel = -1;        // 0 lf, 1 rf, 2 lb, 3 rb: in that hub's frame; -1 in the car's
    bool spins = false;    // turns with the wheel (the tyre, the brake disc)
    std::string material;          // the group's material name (the .sgc's)
    std::vector<CarVertex> vertices;
    std::vector<uint32_t> indices;  // into vertices
};

struct CarTexture {
    std::string material;
    std::vector<uint8_t> dds;   // empty: none
    float diffuse[3] = {0.5f, 0.5f, 0.5f};
    bool transparent = false;
};

struct CarModel {
    std::vector<CarPart> parts;         // one per mesh and material group, in the car's frame
    std::vector<CarTexture> materials;  // per material name used
};

// The exterior as seen from outside: the "e_" meshes, the wheels and spare wheel of `surface` only (tarmac, gravel,
// snow), without the lamps' lit variants. Throws std::runtime_error on a bad file.
CarModel ReadCarModel(const std::string& sgcPath, const std::string& surface);

} // namespace wrally::assets
