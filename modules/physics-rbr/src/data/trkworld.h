#pragma once
// A stage's physics world from its .trk (S4 / M2, doc/physics-standalone-plan.md): the records the game sends the physics
// at load - one CSigCollisionMesh per non-baked collision mesh, then one CSigStuff per placed instance (a baked "u:"
// mesh's own mesh record travels with its instance). Pointers inside the records are left 0: whoever places the
// records in memory fills them (TrkMeshRecord::Ptr*).

#include <cstdint>
#include <string>
#include <vector>

#include "trk.h"

namespace phys::data {

// CSigCollisionMesh (0x34 bytes; core/colmesh.h): +0 slot (10 + the ordinal among the non-baked meshes; a baked mesh's
// is assigned at load by ColMeshFreeSlot), +4 flags (0x10000 a sphere, 0x20000 a box; meshPrepare adds 4 to a baked one's), +8 the name, +0xc the
// vertex count (byte), +0xe the face count, +0x10 the vertices (vec4, w 0), +0x14 the faces (7 bytes: the three
// indices, the face's material (the file's face flags), three attributes 0), +0x2c the shape (sphere: centre vec4 and
// radius; box: centre vec4, half size vec4; w lanes 0), +0x30 the mesh's surface.
struct TrkMeshRecord {
    uint32_t w[13];
    std::string name;
    std::vector<float> vertices;   // 4 per vertex
    std::vector<uint8_t> faces;    // 7 per face
    std::vector<float> shape;      // the +0x2c block (5 or 8 floats), empty without one
    size_t mesh;                   // the .trk mesh
};

// CSigStuff (0x4c bytes used; core/stuff.h): +0 translation (w 0), +0x10 rotation (identity for a baked mesh), +0x20
// scale (w 0), +0x34 the mesh slot (or, flags 0x30, the mesh record), +0x38 the def kind, +0x40 the object id, +0x44
// flags.
struct TrkStuffRecord {
    uint32_t w[19];
    size_t mesh;        // the .trk mesh
    size_t instance;    // its instance
    int32_t id;         // the instance's id (RBR's: the drawn movable object of it has the same, .ngg NGGK)
    int32_t meshRecord; // a baked mesh's own record (index into TrkWorld::bakedMeshes), else -1
};

// One record sent, in the game's order (0x46e300): per non-baked mesh its CSigCollisionMesh, then its instances'
// CSigStuffs; the baked meshes' instances (each with its own mesh record) at the end.
struct TrkEvent {
    bool mesh;        // a mesh record (meshes[index]), else a stuff record (stuff[index])
    uint32_t index;
};

struct TrkWorld {
    std::vector<TrkMeshRecord> meshes;        // the non-baked meshes, in slot order
    std::vector<TrkMeshRecord> bakedMeshes;   // one per baked instance
    std::vector<TrkStuffRecord> stuff;        // the non-baked meshes' instances in file order, then the baked ones
    std::vector<TrkEvent> events;             // the order the game sends them in
};

TrkWorld BuildTrkWorld(const TrkFile& trk);

} // namespace phys::data
