#pragma once
// The car model's collision nodes (M4): `Cars/<car>/<car>.sgc`, the renderer's (HedgeHog3D) scene file, holds a
// collision node for the car's body (c_e_body) and each detachable part (c_e_door_l .. c_e_wing); HedgeHog3D sends
// them to the physics as the car's collision meshes (CSigCollisionMesh, slots 1..8: 0x1001ec90 in HedgeHog3D.dll).
//
// A node is a record {u32 0xc, u32 0x10 (a collision node), u32 0, u32 1, u32 size; size bytes}: cstr name, u32 id,
// u32 parent, f32 [14] (the node's local transform, unused here), f32 m[12] (its world transform, 3 rows {r0 r1 r2 t}),
// u32 nv, f32 v[nv][3] (the file's x, y up, z), u32 nuv, f32 uv[nuv][2], u32 nf, the faces (u32 [nf][6]: the vertices
// and the corners' uvs - with nuv 0 only the vertices, u32 [nf][3]), u8 hasGrids, then (with it) two grids {cstr name,
// u32 n, u8, n * n bytes}: the body's surface tables (its uvs and grids name the surface at a point of the body).

#include <cstdint>
#include <string>
#include <vector>

#include "rec.h"

namespace phys::data {

struct SgcGrid {
    std::string name;
    uint32_t n = 0;
    std::vector<uint8_t> cells;   // n * n
};

struct SgcCollisionNode {
    std::string name;
    uint32_t id = 0, parent = 0;
    float local[14] = {};
    float world[12] = {};
    std::vector<float> verts;      // 3 a vertex
    std::vector<float> uvs;        // 2 a uv
    std::vector<uint32_t> faces;   // 6 a face (the uv corners 0 when the node has no uvs)
    std::vector<SgcGrid> grids;    // 0 or 2
};

// Throws std::runtime_error on a bad file.
std::vector<SgcCollisionNode> ReadSgcCollision(const std::string& path);

// The car's collision meshes as HedgeHog3D sends them (data/rec.h's RecMesh, as the probe records them): the body
// (c_e_body, slot 1, named "MrCarCollision", with its uvs as bytes (uv * 255, truncated) and its grids), then the parts
// in HedgeHog3D's order (0x101d7eb0: c_e_door_l 2, c_e_door_r 3, c_e_bonnet 4, c_e_exhaust 5, c_e_bumper_f 6,
// c_e_bumper_b 7, c_e_wing 8). A vertex is the node's world transform applied as D3DX does on an SSE2 CPU
// (D3DXMatrixMultiply 0x101552fa with the identity, D3DXVec3TransformCoord 0x10156035: the rows summed
// ((r0 x + r1 y) + (r2 z + r3)) and divided by w through rcpps and one Newton step - the CPU's reciprocal estimate, so
// the result is the CPU's), stored (x, z, y, 0). A face is {v0, v1, v2, 0, uv0, uv1, uv2}. Missing nodes are skipped.
std::vector<RecMesh> CarCollisionMeshes(const std::vector<SgcCollisionNode>& nodes);

} // namespace phys::data
