// The .col file's records (R7CO: the stage's static collision), as the file holds them. Names follow the original
// sources (phys/re/original: CollisionFormat.h for SColNode / SColFaceBr / SAABBox, CollisionLoadPseudo.cpp for
// CCollisionSystem::TreeLoad). The file holds a main AABB tree and branches; at load each branch gets its own vertex
// bank (its index written into the leaves' numFaces flags) and is grafted into the main tree in place of its
// branch-start node (flag cleared), so at run time one walk covers everything; a branch start that was never grafted
// keeps NODE_FLAG_BRANCHSTART and is not entered. The root's header also points at the brake wall, the water and the
// river blocks. The offsets in the records are the file's (a 32-bit game holding the file in memory makes them its
// addresses).
#pragma once
#include <cstdint>

#include "num/collide.h"

namespace phys::data {

using num::AABBox;

// SColNode, 0x20 bytes.
struct ColNode {
    AABBox bbox;           // +0x00
    uint32_t numFaces;     // +0x18  bits 0-20 face count (0: inner node), 21-31 node flags
    union {                // +0x1c
        uint32_t children; // inner node: two consecutive child nodes (0: nothing below)
        uint32_t faces;    // leaf: numFaces ColFaceBr records
    };
};
static_assert(sizeof(ColNode) == 0x20, "file layout");

constexpr uint32_t kNumFacesMask = 0x001fffff;
constexpr uint32_t kNodeFlagBranchStart = 0x00200000;   // NODE_FLAG_BRANCHSTART
constexpr uint32_t kNodeFlagBankMask = 0xffc00000;      // NODE_FLAG_BANK_MASK: vertex bank of a leaf
constexpr uint32_t kNodeBankShift = 22;
constexpr uint32_t kMaxColCandidates = 511;             // MAX_COL_CANDIDATES

// SColTreeHeader (the .col's tree and branch headers), 0x14 bytes; the offsets from the header.
struct ColTreeHeader {
    enum : uint8_t { kTypeDefault = 0, kTypeBranch = 1, kTypeBranchProcessed = 2, kTypeDefaultBrakeWall = 3 };
    uint8_t version;
    uint8_t type;
    uint16_t bankIdx;        // the vertex bank, set by the load
    uint32_t traversalId;    // a branch's: bits 0-23 the path (0 left, 1 right), 24-31 its length
    uint32_t numVertices;
    uint32_t vertexOffset;
    uint32_t treeOffset;
};
static_assert(sizeof(ColTreeHeader) == 0x14, "file layout");

// SColRootHeader: after the root's tree header (type 3), from version 2 on.
struct ColRootHeader {
    uint32_t brakeWallOffset;
    uint32_t numWaterSurfaces;
    uint32_t waterOffset;
    uint32_t numRiverSurfaces;
    uint32_t riverOffset;
};
static_assert(sizeof(ColRootHeader) == 0x14, "file layout");

// SColFaceBr, 18 bytes. blend and light: 5 bits per vertex (vertex 0 in bits 0-4) and a flag in bit 15; uv[m][vertex]:
// u in bits 0-3, v in bits 4-7, for material mat[m].
struct ColFaceBr {
    uint16_t idxVertex[3];
    uint16_t blend;
    uint16_t light;
    uint8_t mat[2];
    uint8_t uv[2][3];
};
static_assert(sizeof(ColFaceBr) == 18, "file layout");

// The brake wall's 2D tree node, 0x18 bytes: centre x, y, half extents hx, hy; obj != 0 marks a leaf; children: two
// consecutive nodes (0: none).
struct ColNode2D {
    float x, y, hx, hy;
    uint32_t obj;
    uint32_t children;
};
static_assert(sizeof(ColNode2D) == 0x18, "file layout");

} // namespace phys::data
