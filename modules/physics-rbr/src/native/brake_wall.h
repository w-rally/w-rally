#pragma once
// The brake wall, natively (M6 step 2; the classic CBrakeWall, core/tree.cpp): the stage's invisible walls that keep the
// car on the road - quads in the ground plane (four corners each, from the .col's brake wall block) filed in a 2D tree.
// The car's integration asks which quad lies under a point (PointHit: the wall's normal, how far onto it); the pair
// tests sweep segments against the quads' inner edges (BoxQuery, then SegmentContact / SegmentPoint).
//
// The classic behaviour is kept: the segment query walks only the leaves the last box query found; a segment hit marks
// the wall (even on a pass-through entry), which the next point query reports and clears.
#include <cstdint>
#include <vector>

#include "data/col.h"
#include "native/entity.h"
#include "native/event.h"

namespace phys::native {

// A point of the wall's quads: the ground plane's x, y.
struct WallPoint {
    float x, y;
};

class BrakeWall : public Entity {
public:
    bool Static() const override { return true; }
    struct Node {
        float x = 0, y = 0, hx = 0, hy = 0;    // centre, half extents
        uint32_t count = 0;                    // a leaf's entries (0: an inner node)
        int32_t children = -1;                 // an inner node's first child (the second follows); -1 none
        const uint16_t* entries = nullptr;     // a leaf's (BrakeWallEntry: the quad's first point, flags)
    };
    // A point on the wall (the classic BrakeWallHit record).
    struct Hit {
        Vec4 normal{};          // the wall's, in the plane (z 0; w not written)
        float onWall = 0;       // how far onto the wall's band: 1 - distance / its width (0 a zero width)
        float distance = 0;     // from the wall's inner edge along the normal
        uint32_t flags = 0;     // BrakeWallHitFlags (model/colsystem.h)
    };

    // The .col's brake wall block (its offsets from the block: +4 the segment count, +8 the points, +0xc the 2D tree);
    // the block stays the owner's (the stage collision's file). BrakeWallInit (0x4e4220).
    void Load(const uint8_t* block);
    bool Loaded() const { return !nodes.empty(); }

    bool Collidable() const override { return true; }   // vslot 68 (0x4e3860)
    bool BoxTest(const FramePoint& a, const FramePoint& b) override { return BoxQuery(a, b); }   // vslot 51
    // BrakeWallSegmentQuery (vslot 58, 0x4e3a10): the 2D segment between two frame points (in the world) against the
    // last box query's segments.
    bool SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool enabled) override;
    // BrakeWallContact (vslot 60, 0x4e3970): SegmentContact as a point, plane and time; the other outputs 0.
    bool SegmentPoint(const FramePoint& a, const FramePoint& b, Vec4* point, Vec4* plane, uint32_t* mat) override;
    // BrakeWallBoxQuery (vslot 51, 0x4e42f0): the box between two frame points (in the wall's frame) - the leaves the
    // segment query then walks. False for a box over 20 wide.
    bool BoxQuery(const FramePoint& a, const FramePoint& b);
    // BrakeWallHit (0x4e4570): the quad under p (x, y). A pass-through quad calls `signal` and returns false.
    bool PointHit(const float* p, Hit& out, void (*signal)(const Hit& h));

    std::vector<Node> nodes;               // [0] the root
    const WallPoint* points = nullptr;
    uint32_t segmentCount = 0;
    std::vector<const Node*> found;        // the last box query's leaves
    bool hit = false;                      // a segment hit since the last point query

private:
    void Fill(int32_t slot, const uint8_t* at, const uint8_t* base);
    template <class Test> void Walk(std::vector<const Node*>& leaves, Test enter) const;
};

} // namespace phys::native
