#pragma once
// The stage's static collision, natively (M6 step 2): the .col file's triangle trees - the root tree and its branches,
// grafted where the root leaves a placeholder - with their vertex banks, and the stage's surface maps (the .mat's), which
// name the material at a point of a face. Queries: a segment against the whole tree (the wheels' ground rays), a box for
// the leaves near a body, and a segment or a point against those leaves.
//
// The same arithmetic as the classic code (core/tree.cpp: CCollisionSystem and CMaterialMaps), the same walk order and the
// same quirks, so a query gives the classic one's bits (tools/native_check.cpp checks it). Only the storage is native:
// owned buffers, 64-bit pointers, nodes in an array.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "num/collide.h"
#include "data/col.h"

namespace phys::native {

using num::AABBox;
using data::ColFaceBr;
using num::Vec3;

// The stage's surface maps (CMaterialMaps): per map index a byte map of material ids over a face's uv square.
class MaterialMaps {
public:
    // The .mat file (per weather: a name, a count, per map {int32 width, int32 height, the bytes}); `weather` picks one.
    // Returns "" or what is wrong.
    std::string Load(std::vector<uint8_t> file, uint32_t weather);
    // The material at (u, v) of map `index` (MapSetSample): 0 without maps or past them.
    uint8_t Sample(uint8_t index, float u, float v) const;

private:
    struct Map {
        uint8_t width = 0, height = 0, flags = 0;
        const uint8_t* data = nullptr;
    };
    std::vector<uint8_t> m_file;
    std::array<Map, 256> m_maps{};
    uint32_t m_count = 0;
    bool m_loaded = false;
};

// A hit on the stage's ground (the classic GroundContact's values).
struct GroundHit {
    Vec3 point;
    Vec3 normal;
    float plane;             // -dot(a face vertex, normal)
    float light, blend;      // the face's values at the hit, 0..1
    uint32_t material[2];    // the two materials (the face's maps sampled at the hit)
    float t;                 // along the segment (0 for a point query)
    uint32_t blendFlag, lightFlag;
};

class StageCollision {
public:
    struct Node {
        AABBox box;
        uint32_t numFaces = 0;             // the file's word: the face count (0: inner), the node flags, a leaf's bank
        int32_t children = -1;             // an inner node's first child (the second follows it); -1 none
        const ColFaceBr* faces = nullptr;  // a leaf's
    };

    // The .col file (R7CO): the root tree, then every branch. Returns "" or what is wrong.
    std::string Load(std::vector<uint8_t> file);

    // The segment p0 .. p1 against the whole tree (ColSystemSegmentQuery): the nearest hit with t <= 1.
    bool SegmentQuery(const MaterialMaps& maps, const Vec3& p0, const Vec3& p1, GroundHit& out) const;
    // The leaves whose boxes overlap the box pos +- ext (ColSystemBoxQuery), in the walk's order.
    void BoxQuery(const Vec3& pos, const Vec3& ext, std::vector<const Node*>& leaves) const;
    // The same for the box mn .. mx (ColSystemMinMaxQuery: as centre (mx + mn) / 2 and half extents (mx - mn) / 2).
    void MinMaxQuery(const Vec3& mn, const Vec3& mx, std::vector<const Node*>& leaves) const;
    // The segment against the given leaves (TreeLeavesRaycast) and a point just below one of their faces
    // (TreeLeavesPointQuery).
    bool LeavesRaycast(const MaterialMaps& maps, const Vec3& p0, const Vec3& p1, const std::vector<const Node*>& leaves,
                       GroundHit& out) const;
    bool LeavesPointQuery(const MaterialMaps& maps, const Vec3& p, const std::vector<const Node*>& leaves, GroundHit& out) const;

    const Node* Root() const { return m_root < 0 ? nullptr : &m_nodes[m_root]; }
    // The root's brake wall block (TreeLoad's: version 1 right after the tree header, 2 .. 4 at the root header's offset;
    // null: none, or no segments - BrakeWallLoad's test), in the owned file.
    const uint8_t* BrakeWallBlock() const;
    size_t NodeCount() const { return m_nodes.size(); }
    // The root's water (TreeLoad, versions 3 and 4): the grid made for it (the world's) and its quads - four vec4 corners
    // each - in the order the world loads them: version 3 its water, version 4 its water only when wet (wetness 2), then
    // its rivers.
    bool MakesGrid() const { return m_rootVersion == 3 || m_rootVersion == 4; }
    struct Quads {
        const num::Vec4* corners;
        uint32_t count;
    };
    std::vector<Quads> WaterQuads(uint32_t wetness) const;

private:
    uint32_t AllocBank(const Vec3* vertices);
    void LoadTree(size_t header);
    void FillNode(int32_t slot, size_t at, size_t base, uint32_t endMark, uint32_t bank);
    void Graft(uint32_t traversal, int32_t branch);
    const Vec3* Bank(const Node& n) const { return m_banks[n.numFaces >> data::kNodeBankShift]; }
    template <class Test> void PushChildren(const Node& n, const Node** stack, int& top, Test test) const;

    std::vector<uint8_t> m_file;
    size_t m_brakeWall = 0;   // the brake wall block's offset (0: none)
    uint32_t m_rootVersion = 0;
    std::pair<size_t, uint32_t> m_water{}, m_river{};   // the quads' offsets and counts
    std::vector<Node> m_nodes;
    int32_t m_root = -1;
    std::array<const Vec3*, 1024> m_banks{};
    uint32_t m_bankCursor = 0;
};

} // namespace phys::native
