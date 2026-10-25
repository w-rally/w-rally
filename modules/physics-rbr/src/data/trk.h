#pragma once
// RBR stage .trk reader: the stage's collision objects - block (3,22), the movable / breakable "stuff" the game hands to
// the physics one record per placed instance (CSigStuff -> CPhysics 0x5267d0; doc/physics-standalone-plan.md, S1) - and
// the drive line block (3,20), kept raw. Layout as Wallaby reads it (MapObjects/CollisionMesh.cs, MeshInstance.cs),
// checked against the probe's load log on stage 41: every instance's record carries the file's floats bit for bit.
//
// A file is a sequence of blocks {int32 8, int32 type, int32 subtype, int32 size; size bytes}.

#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

struct TrkVec3 {
    float x, y, z;
};

struct TrkFace {
    uint8_t flags;
    uint32_t v[3];
};

// One placement of a collision mesh. The record the game sends holds the translation (+0), the rotation (+0x10, the
// file's order) and the scale (+0x20) as stored here - except for a mesh with `baked` set, whose rotation is already in
// its vertices: its record has the identity rotation.
struct TrkInstance {
    int32_t id;
    TrkVec3 translation, scale;
    float rotation[4];   // a quaternion x, y, z, w
};

struct TrkCollisionMesh {
    std::string name;    // the shape ("MAPS/.../X_COL.KSH"), or "u: <id>" for a unique, pre-rotated one
    int32_t type;        // the physics def: 0 GENERIC, 1 / 2 / 3 / 6 / 7 / 9 / 10 the named Solid defs (kind 25 / 24 /
                         // 13 / 21 / 28 / 31 / 32 in the record; 0 -> 1)
    uint8_t surface;     // the material (200: the spectator areas' damping material)
    uint8_t baked;       // 1: the rotation is in the vertices ("u:" meshes)
    int32_t shape;       // 0 a mesh; 1 a box (box[6]: centre, half size); 2 four values (shape4)
    float box[6];
    float shape4[3];
    uint32_t shape4w;
    std::vector<TrkVec3> vertices;
    std::vector<TrkFace> faces;
    std::vector<TrkInstance> instances;
};

struct TrkFile {
    std::vector<TrkCollisionMesh> meshes;
    std::vector<uint8_t> driveLine;   // block (3,20), raw
};

// Throws std::runtime_error on a malformed file.
TrkFile ReadTrk(const std::string& path);

// The def kind the record carries for a mesh type (0x38 in the CSigStuff record), as the game's .trk loader (0x46e300)
// sets it: 1 -> 25, 2 -> 24, 3 -> 13, 6 -> 21, 7 -> 28, 8 -> 30, 9 -> 31, 10 -> 32, any other -> 1 (GENERIC).
int32_t TrkDefKind(int32_t type);

// Whether the loader looks the instance up as an "interactive object" (the renderer's handle, 40000 on, into record
// +0x40; by the instance id): every type but 0 and 5. A failed lookup leaves 0.
bool TrkHasObjectHandle(int32_t type);

} // namespace phys::data
