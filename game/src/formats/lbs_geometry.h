#pragma once
// RBR stage .lbs - the render geometry (wrally::formats, docs/design.md 6.2 and decision 2): the blocks the legacy
// importer turns into an .ngg. The script objects, the car info and the drive line are phys-data's (data/lbs.h).
//
// A .lbs is a flat run of blocks, each a 16-byte header {version, type, subtype, size} and `size` payload bytes. The
// geometry's blocks:
//   (0,23) texture names   u32 n, then n x {u32 x4, cstr name}
//   (1,0)  ground          version 8 only (88/89: encrypted, below): u32 regions, f32; per region five meshes in fixed
//                          layouts (4, then 32 bytes, then 8, 14, 11, 17), u32 n render chunks of 112 bytes (sub-mesh,
//                          index range, two textures, visibility), a 24-byte box
//   (1,1)  vegetation      u32 regions; per region an i32 (0: empty), two lists of items: an attribute record, i32,
//                          a mesh, a 24-byte box
//   (1,3)  scenery         cstr group; u32 n, then n x {f32 point[3], attribute record, mesh} - in world space
//   (1,6)  movable         u32 n objects: cstr name, u8, i32 subs x {20-byte attributes, mesh}, u32 placements x
//                          {i32 instance id (the .trk solid's), f32 m[16] (row vectors, translation in row 3)}
// A mesh: u32 index count (type 3: a face count), u16 indices, for types 7 and 9 a byte (2: a second index block
// follows, u32 count and u16s), u32 vertex count, then the vertices interleaved; its type is the vertex's size in
// 4-byte words. An empty mesh (no vertices or no faces) carries no vertex data.
//
// The layouts were recovered by reverse engineering (the RBR community's, written down in Wallaby; nothing of
// Wallaby's code is here). The readers check themselves: each block must be consumed exactly, so a misread byte
// shows as a block that does not add up rather than as garbage.
//
// Positions and normals are as the file has them (the renderer's axes: x, y up, z).

#include <cstdint>
#include <string>
#include <vector>

namespace wrally::formats {

struct LbsMesh {
    uint32_t type = 0;                 // the vertex layout (its size in words)
    std::vector<uint16_t> indices;     // a triangle list (empty: no mesh)
    std::vector<float> positions;      // 3 a vertex
    std::vector<float> normals;        // 3 a vertex; empty when the layout has none
    std::vector<uint32_t> colours;     // D3DCOLOR 0xAARRGGBB (the baked light, alpha a blend weight or coverage)
    std::vector<float> uv0, uv1;       // 2 a vertex; empty when the layout has none
    size_t VertexCount() const { return positions.size() / 3; }
    bool Empty() const { return indices.empty() || positions.empty(); }
};

// A draw of a ground region's sub-mesh: an index range with two textures. RBR draws visibility 1 and 2 only.
struct LbsGroundChunk {
    int submesh = 0;                   // 0..4
    uint32_t firstIndex = 0, faceCount = 0;
    int texture1 = -1, texture2 = -1;  // into the texture names
    uint8_t visibility = 0;
};

struct LbsGroundRegion {
    LbsMesh submeshes[5];
    std::vector<LbsGroundChunk> chunks;
};

// A mesh with its texture; a movable one with its placement.
struct LbsPlaced {
    LbsMesh mesh;
    int texture = -1;
    uint32_t flags = 0;                // the attribute record's
    bool placed = false;
    float transform[16] = {};          // row vectors: p' = p * M, translation in 12..14
    int32_t instanceId = -1;           // movable: the .trk collision instance's id (the physics' solid)
};

struct LbsGeometry {
    std::vector<std::string> textureNames;
    std::vector<LbsGroundRegion> ground;
    std::vector<LbsPlaced> vegetation, scenery, movable;
    std::vector<std::string> notes;    // blocks left out and why (a ground of another version)
};

// The file's geometry blocks. Throws std::runtime_error naming the block whose layout does not add up - or saying the
// file is encrypted (LbsEncryptedVersion), which is not read at all.
LbsGeometry ReadLbsGeometry(const std::vector<uint8_t>& file);

// A PROTECTED stage: a geometry block of version 88 or 89 is encrypted (its payload is ciphertext: 8.00 bits a byte),
// and every geometry block of such a file is. Returns that version, or 0 for a plain file (also for one it cannot
// open). It reads only the block headers, so the catalog can ask it of every stage.
//
// An encrypted stage is never imported, not even its readable parts: decrypting is the licensed install's job, and an
// importer that turned a protected stage into a plain .ngg would leak it (NGG design §2.9). In the stock RBR folder: 31
// stage files, maps 10 (overcast), 95, 96, 105 and 488-499.
uint32_t LbsEncryptedVersion(const std::string& path);

} // namespace wrally::formats
