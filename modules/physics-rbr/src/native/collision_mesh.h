#pragma once
// A collision mesh, natively (M6 step 2; the classic CCollisionMesh, core/colmesh.cpp): the convex mesh of a stage object,
// the car body or a part - up to 30 vertices, a BSP of up to 56 faces, up to 84 edges (the sharp ones kept), the bounds,
// and the surface grids that give a hit's surface types from its texture coordinates. Built from a mesh record (the
// CSigCollisionMesh one: its vertices, faces, texture coordinates, grids and an optional cheap shape), moved between
// frames with the bodies.
//
// The classic build's behaviour is kept bit for bit, quirks included: a face inserted into the BSP adds the normal of the
// face at the BSP's index (not its own) to its vertices' sums; an edge's other face is left as the mesh had it when no
// face lies across (a new mesh: 0); the bounds' w lanes are the vertices'.
#include <cstdint>

#include "num/f32.h"
#include "num/math.h"
#include "num/x87.h"
#include "native/entity.h"

namespace phys::native {

// A mesh record's data (CSigCollisionMesh).
struct MeshRecord {
    uint32_t flags = 0;                 // CollisionMeshRecordFlags (model/colmesh.h)
    const void* name = nullptr;
    uint16_t vertexCount = 0, faceCount = 0;
    const Vec4* vertices = nullptr;
    const uint8_t* faces = nullptr;     // 7 bytes a face: three vertices, a material (0xff off), three attributes
    uint32_t uvCount = 0;
    const uint8_t* uvs = nullptr;       // two bytes an attribute
    uint16_t gridWidthA = 0, gridWidthB = 0;
    const uint8_t* gridA = nullptr;
    const uint8_t* gridB = nullptr;
    const void* shape = nullptr;        // a sphere or a box (the flags)
    uint8_t material = 0;
};

struct MeshEdge {
    uint8_t a = 0, b = 0;                     // its vertices
    uint8_t face = 0, otherFace = 0;          // the face it was added from, the face across
    uint8_t attributeA = 0, attributeB = 0;   // the vertices' attribute indices
};

struct CollisionMesh {
    Vec4 boundsMin{}, boundsMax{}, centre{}, halfExtent{};
    Vec4 vertices[30] = {};             // xyz from the record (w and the rest as the mesh had them: a new one 0)
    Vec4 normalSums[30] = {};              // each vertex's faces' normals summed
    const uint8_t* uvs = nullptr;
    uint16_t gridWidthA = 0, gridWidthB = 0;
    const uint8_t* gridA = nullptr;
    const uint8_t* gridB = nullptr;
    const uint8_t* faces = nullptr;     // the record's
    uint8_t bspChildren[112] = {};      // two a face: [0] behind, [1] in front (0 none)
    MeshEdge edges[84];
    uint32_t problems = 0;              // the build's (1 not closed, 2 the adjacency, 4 a face split; -1 no vertices)
    const void* shape = nullptr;
    const void* name = nullptr;
    uint8_t surfaceA = 0, surfaceB = 0; // the last hit's cells in the two grids
    uint8_t flags = 0;                  // CollisionMeshFlags (model/colmesh.h)
    uint8_t faceCount = 0, edgeCount = 0, vertexCount = 0, uvCount = 0, material = 0;
    uint8_t attributes[30] = {};        // each vertex's attribute index (from its faces)

    void Build(const MeshRecord& r);                              // ColMeshBuild (0x4f9cd0)
    int32_t AddEdge(uint32_t face, uint32_t a, uint32_t b, uint8_t x, uint8_t y);   // 0x4f7140: 1 added, -1 the reverse found, 0 a repeat or full
    void FacePlane(uint32_t face, float out[4]) const;            // 0x4f7320
    uint32_t BspInsert(const uint8_t* v, uint32_t face);          // 0x4f9780
    bool BspInside(const float* p, uint32_t face) const;          // 0x4f8a20

    // The queries a contact asks of a mesh (its vertices in the frame it was built into).
    // The surface bytes from the two grids at vertex idx's texture coordinates / on edge k at t (0x4e0950 / 0x4e0840).
    void SurfaceAtUv(uint32_t idx, uint32_t* b, uint32_t* a) const;
    void SurfaceOnEdge(uint32_t k, float t, uint32_t* b, uint32_t* a) const;
    // The plane through edge k along b - a (0x4f74a0; always returns 1).
    float EdgePlane(uint32_t k, const Vec4& a, const Vec4& b, Vec4& out) const;
    // The segment moving from (A, B) to (C, D) against edge k (0x4f5ff0): its time s, or 2 for a miss.
    num::X87 EdgeSweep(uint32_t k, const Vec4& A, const Vec4& B, const Vec4& C, const Vec4& D, Vec4* P, Vec4* Q, Vec4* R) const;
    // ... against all the edges (0x4f8560): the earliest (below 1.01), its plane and its surface bytes.
    bool EdgesSweep(const Vec4& A, const Vec4& B, const Vec4& C, const Vec4& D, Vec4* P, float* best, Vec4* plane);
    // The segment's midpoint box test against the mesh's box (0x4f7640).
    bool BoxSegmentOverlap(const float* p, const float* h) const;
    // The segment p0 .. p1 against the mesh's enabled faces (0x4f8280): the first hit with 0 < t < 1.01, its plane and
    // material, the surface bytes at it.
    bool SegmentHit(const float* p0, const float* p1, Vec4* point, float* t, Vec4* plane, uint32_t* material);

private:
    void SurfaceCells(num::F32 s, num::F32 t, uint32_t* a, uint32_t* b) const;
};

// src copied into mesh and moved from frame `from` into frame `to` (MeshBuild, 0x4f6610): its vertices and normal sums,
// then the bounds anew. No src: the empty mesh.
CollisionMesh* MeshBuild(CollisionMesh* mesh, const CollisionMesh* src, const Frame& from, const Frame& to);
// The mesh's box in the frame `world`, its vertices moved there from `entity` (0x4f7090). False without vertices.
bool MeshWorldBounds(const CollisionMesh& mesh, const Frame& entity, const Frame& world, Vec4* lo, Vec4* hi);

// n meshes in a row moved from frame t into frame a (MeshFrameMove, 0x51b410): their vertices as points, their normal
// sums as directions - the count always the first mesh's (the original's, kept).
CollisionMesh* MeshMove(const Frame& t, const Frame& a, CollisionMesh* m, uint32_t n);

} // namespace phys::native
