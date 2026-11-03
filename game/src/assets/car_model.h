#pragma once
// The car's render model (M5) from its .sgc - the renderer's scene file (HedgeHog3D), read as HedgeHog3D.dll's loader
// reads it (0x1001f2a0: records {u32 0xc, u32 kind, u32 version, u32 1, u32 size}; kind 0 a mesh - versions 1 and 3,
// the deformable parts carrying damage data after it -, kind 5 a material) - and its textures, through the car's
// <car>_shaders.ini; a mesh group's material is <car>.ini's [<mesh>] FaceBlock<g>_Material where it names one, its
// finish that section's FaceBlock<g>_Shader (CarPaint, CarPlastic, CarTire, CarRim, ...: the game's shader classes,
// <car>_shader_settings_*.ini). The textures carry no gloss or reflection mask (their alpha is opaque), so the class
// is what tells paint from rubber and plastic.
//
// A mesh (0x1000acf0): cstr name, u32 id, u32 parent, f32[14] the local transform, f32[12] the world transform (rows
// {r0 r1 r2 t}), f32[6] the bounds, u32 nv, u32 flags; with flags bit 0 nv x {position, normal} (f32[6]), bit 2 nv x uv
// (f32[2]), bit 4 nv x u32 (skipped by the game); u32 nt, nt x u16[3]; u32 ng, ng x {cstr material, u32 triangles,
// u32 min index, u32 vertices, u32 first index}.
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

// The lamps' meshes (NGG design 5.4 N3b), by what switches them: RBR's lamp meshes come in pairs - e_light_lf and its
// lit twin e_light_lf_lit, one shown at a time (the .ini's SwitchBox = light) -, the ramp's housing (e_bonnet_ramp,
// e_ramp, their glass) shows with the car's own ramp.
enum LampMeshGroup : int { kLampHead, kLampTail, kLampBrake, kLampReverse, kLampRamp, kLampRampHousing, kLampGroups };

struct CarPart {
    std::string name;
    int lampGroup = -1;    // a LampMeshGroup, -1 not a lamp's
    bool lit = false;      // a lamp's lit twin (<name>_lit)
    int wheel = -1;        // 0 lf, 1 rf, 2 lb, 3 rb: in that hub's frame; -1 in the car's
    bool spins = false;    // turns with the wheel (the tyre, the brake disc)
    std::string material;          // the group's material name (the .sgc's)
    std::string shader;            // the group's shader class (<car>.ini FaceBlock<g>_Shader), "" none given
    std::vector<std::string> parents;   // the node tree above its mesh, nearest first (the root left out)
    std::vector<CarVertex> vertices;
    std::vector<uint32_t> indices;  // into vertices
};

// W-Rally's PBR maps for a car texture (none of them RBR's): files beside it in the car's textures folder, named after
// it - i20_R5_external.dds has i20_R5_external_basecolor.dds, _normal.dds, _orm.dds -, each optional, each found by its
// name alone. glTF's meaning: the base colour (sRGB) in place of the texture; the normal map tangent-space, two
// channels enough (BC5: z is rebuilt); ORM linear, r occlusion, g roughness, b metallic - and a the clear coat (0 matte,
// 1 lacquer: BC3 or BC7 to have one). Where a texture has an ORM map, it says the finish; without one the shader class
// does (assets/car_render.cpp). DDS: BC1-3, or BC4, BC5, BC7 behind a DX10 header (DirectXTex's texconv), with mips.
enum CarMapKind : int { kMapBaseColour, kMapNormal, kMapOrm, kCarMaps };
struct CarMap {
    std::vector<uint8_t> dds;   // empty: none
    uint32_t uploaded = 0;      // as CarTexture::uploaded
};

struct CarTexture {
    std::string material;
    std::string file;           // its texture's file name in the car's textures folder ("" none)
    std::vector<uint8_t> dds;   // empty: none
    uint32_t uploaded = 0;      // the renderer's texture made of it already (a frame at a time: the showroom's); 0 not yet
    float diffuse[3] = {0.5f, 0.5f, 0.5f};
    bool transparent = false;
    CarMap maps[kCarMaps];      // its PBR maps (CarMapKind)
};

struct CarModel {
    std::vector<CarPart> parts;         // one per mesh and material group, in the car's frame
    std::vector<CarTexture> materials;  // per material name used
    // Where the wheels stand without the physics (a car in a menu): each Point_e_wheel_<lf|rf|lb|rb>'s place in the car's
    // frame (hubFound: the model has it), and the tyres' radius (the spinning wheel meshes' reach below their hub; 0
    // none).
    bool hubFound[4] = {};
    float hubs[4][3] = {};
    float wheelRadius = 0;
    // Each wheel mesh's own node's place (e_wheel_<surface>_<w>): where its maker drew the wheel - the physics' place for
    // it, where the hub helper is off it (Hyundai i20 R5's: 5 to 15 cm). wheelFound: the model has that wheel.
    bool wheelFound[4] = {};
    float wheelAt[4][3] = {};
};

// The exterior as seen from outside: the "e_" meshes, the wheels and spare wheel of `surface` only (tarmac, gravel,
// snow), the lamps' lit twins too (CarPart::lit; other _lit meshes - hot brake discs - left out). Throws
// std::runtime_error on a bad file.
CarModel ReadCarModel(const std::string& sgcPath, const std::string& surface);

} // namespace wrally::assets
