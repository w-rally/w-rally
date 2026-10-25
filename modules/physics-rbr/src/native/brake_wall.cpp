#include "native/brake_wall.h"

#include <cassert>
#include <cstring>

#include "num/f32.h"
#include "num/x87.h"
#include "native/flags.h"

namespace phys::native {

using num::F32;
using num::X87;
namespace jmp = num::jmp;

namespace {
float FromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
template <class T> T Read(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
constexpr int kStack = 256;   // the original's walk stack (no bound checked there)
} // namespace

void BrakeWall::Fill(int32_t slot, const uint8_t* at, const uint8_t* base)
{
    // BrakeWallRelocate (0x4e3810): the offsets from the tree's root; a leaf's entries, an inner node's two children.
    const data::ColNode2D n = Read<data::ColNode2D>(at);
    Node& d = nodes[slot];
    d.x = n.x, d.y = n.y, d.hx = n.hx, d.hy = n.hy;
    d.count = n.obj;
    if (n.obj) {
        d.entries = reinterpret_cast<const uint16_t*>(base + n.children);
        return;
    }
    if (!n.children) return;
    const int32_t c = static_cast<int32_t>(nodes.size());
    nodes.resize(nodes.size() + 2);
    nodes[slot].children = c;
    Fill(c, base + n.children, base);
    Fill(c + 1, base + n.children + sizeof(data::ColNode2D), base);
}

void BrakeWall::Load(const uint8_t* block)
{
    nodes.clear();
    found.clear();
    hit = false;
    segmentCount = Read<uint32_t>(block + 4);
    points = reinterpret_cast<const WallPoint*>(block + Read<uint32_t>(block + 8));
    const uint8_t* const root = block + Read<uint32_t>(block + 0xc);
    nodes.resize(1);
    Fill(0, root, root);
}

template <class Test> void BrakeWall::Walk(std::vector<const Node*>& leaves, Test enter) const
{
    // Tree2DQuery / Tree2DPointQuery (0x4e3f10 / 0x4e3dc0): from the root, a leaf collected, the children (first, then
    // second) entered when the test holds.
    leaves.clear();
    const Node* stack[kStack];
    int top = 0;
    stack[top++] = &nodes[0];
    do {
        const Node* n = stack[--top];
        if (n->count) {
            leaves.push_back(n);
            continue;
        }
        if (n->children < 0) continue;
        for (int k = 0; k < 2; ++k) {
            const Node& c = nodes[n->children + k];
            if (!enter(c)) continue;
            assert(top < kStack);
            stack[top++] = &c;
        }
    } while (top);
}

bool BrakeWall::BoxQuery(const FramePoint& fa, const FramePoint& fb)
{
    Vec4 a, b;
    FramePointMove(fa, a, frame);   // both corners in the wall's own frame
    FramePointMove(fb, b, frame);
    const F32 half = F32(FromBits(0x3f000000));
    const float q[4] = {((F32(b.x) + F32(a.x)) * half).f(), ((F32(a.y) + F32(b.y)) * half).f(), ((F32(b.x) - F32(a.x)) * half).f(),
                        ((F32(b.y) - F32(a.y)) * half).f()};
    if (jmp::A(q[2], 10.0f) || jmp::A(q[3], 10.0f)) return false;   // wider than 20 in x or y
    Walk(found, [&](const Node& c) {
        // fld q.x; fsub c.x; fabs; fld c.hx; fadd q.hx; fcomip; jbe - then y with an SSE difference
        if (jmp::BE((X87(c.hx) + X87(q[2])).d, Abs(X87(q[0]) - X87(c.x)).d)) return false;
        const float dy = (F32(q[1]) - F32(c.y)).f();
        return !jmp::BE((X87(c.hy) + X87(q[3])).d, Abs(X87(dy)).d);
    });
    return !found.empty();
}

bool BrakeWall::SegmentContact(const FramePoint& fa, const FramePoint& fb, CollisionHit& out, bool enabled)
{
    if (!enabled) return false;
    const Frame& world = WorldOf(frame);
    Vec4 a, b;
    FramePointMove(fa, a, world);
    FramePointMove(fb, b, world);
    const float dx = (F32(b.x) - F32(a.x)).f(), dy = (F32(b.y) - F32(a.y)).f();
    if (jmp::A(FromBits(0x3dcccccd), (F32(dy) * F32(dy) + F32(dx) * F32(dx)).f())) return false;   // shorter than sqrt(0.1)
    bool hitNow = false;
    for (size_t i = 0; i < found.size() && !hitNow; ++i) {   // the last box query's leaves
        const Node& leaf = *found[i];
        for (uint32_t j = 0; j < leaf.count && !hitNow; ++j) {
            const uint16_t entry = leaf.entries[j];
            const WallPoint* const Q = points + (entry & kWallSegmentMask);
            const WallPoint& p0 = Q[1], &p1 = Q[3];   // the quad's segment
            const float ex = (F32(p1.x) - F32(p0.x)).f(), ey = (F32(p1.y) - F32(p0.y)).f();
            float nx = ey, ny = Neg(X87(ex)).Store();
            const float ey2 = (X87(ey) * X87(ey)).Store();
            const float len = Sqrt(X87(ny) * X87(ny) + X87(ey2)).Store();
            if (jmp::A(len, FromBits(0x3727c5ac))) {   // 1e-5
                const F32 inv = F32(1.0f) / F32(len);
                ny = (inv * F32(ny)).f();
                nx = (inv * F32(ey)).f();
            }
            const X87 dnx = X87(dy) * X87(ny) + X87(dx) * X87(nx);
            const float dn = dnx.Store();
            if (!jmp::A(Abs(dnx).d, X87(FromBits(0x358637bd)).d)) continue;   // parallel (1e-6)
            const F32 d = F32(0.0f) - (F32(p0.x) * F32(nx) + F32(p0.y) * F32(ny));
            const F32 da = (F32(a.x) * F32(nx) + F32(a.y) * F32(ny)) + d;
            const F32 db = (F32(b.x) * F32(nx) + F32(b.y) * F32(ny)) + d;
            const bool cross = (jmp::A(da.f(), 0.0f) && jmp::A(0.0f, db.f())) || (jmp::A(0.0f, da.f()) && jmp::A(db.f(), 0.0f));
            if (!cross) continue;
            const F32 s = F32(0.0f) - da / F32(dn);
            const float px = (F32(a.x) + F32(dx) * s).f(), py = (F32(dy) * s + F32(a.y)).f();
            const F32 proj = (F32(py) - F32(p0.y)) * F32(ey) + (F32(px) - F32(p0.x)) * F32(ex);
            if (jmp::B(proj.f(), 0.0f)) continue;   // before p0
            if (jmp::B((F32(ex) * F32(ex) + F32(ey2)).f(), proj.f())) continue;   // past p1
            hit = true;
            if (entry & kWallPassThrough) continue;
            out.point.x = px;   // (w as the record had it)
            out.point.y = py;
            out.point.z = ((F32(b.z) - F32(a.z)) * s + F32(a.z)).f();
            out.plane = {nx, ny, 0.0f, d.f()};
            out.time = s.f();
            out.other = this;
            hitNow = true;
        }
    }
    return hitNow;
}

bool BrakeWall::SegmentPoint(const FramePoint& a, const FramePoint& b, Vec4* point, Vec4* plane, uint32_t* mat)
{
    CollisionHit rec;   // (the original zeroes it)
    if (!SegmentContact(a, b, rec, true)) return false;
    *point = rec.point;
    *plane = rec.plane;
    *mat = 0;   // (the time and the two other outputs: the caller's unused words)
    return true;
}

bool BrakeWall::PointHit(const float* p, Hit& H, void (*signal)(const Hit& h))
{
    std::vector<const Node*> leaves;
    Walk(leaves, [&](const Node& c) {
        // fld p.x; fsub c.x; fabs; fld c.hx; fcomip; jbe - then y with an SSE difference
        if (jmp::BE(X87(c.hx).d, Abs(X87(p[0]) - X87(c.x)).d)) return false;
        const float dy = (F32(p[1]) - F32(c.y)).f();
        return !jmp::BE(X87(c.hy).d, Abs(X87(dy)).d);
    });
    if (leaves.empty()) {
        hit = false;
        return false;
    }
    const F32 zero = 0.0f, one = 1.0f;
    for (const Node* leaf : leaves) {
        for (uint32_t j = 0; j < leaf->count; ++j) {
            const uint16_t w = leaf->entries[j];
            const WallPoint* const q = points + (w & kWallSegmentMask);   // a quad: v0 v1 v2 v3, edges v0 v1 v3 v2
            const F32 e0x = F32(q[1].x) - F32(q[0].x), e0y = F32(q[1].y) - F32(q[0].y);
            const F32 e1x = F32(q[3].x) - F32(q[1].x), e1y = F32(q[3].y) - F32(q[1].y);
            const F32 e2x = F32(q[2].x) - F32(q[3].x), e2y = F32(q[2].y) - F32(q[3].y);
            const F32 e3x = F32(q[0].x) - F32(q[2].x), e3y = F32(q[0].y) - F32(q[2].y);
            const F32 d1x = F32(p[0]) - F32(q[1].x), d1y = F32(p[1]) - F32(q[1].y);
            const F32 d2x = F32(p[0]) - F32(q[2].x), d2y = F32(p[1]) - F32(q[2].y);
            if (jmp::BE((d1y * (zero - e0x) + d1x * e0y).f(), 0.0f)) continue;   // comiss c, 0; jbe
            if (jmp::BE((d1y * (zero - e1x) + d1x * e1y).f(), 0.0f)) continue;
            if (jmp::BE((d2y * (zero - e2x) + d2x * e2y).f(), 0.0f)) continue;
            if (jmp::BE((d2y * (zero - e3x) + d2x * e3y).f(), 0.0f)) continue;
            // Inside: the normal of edge v1 -> v3, the distance to it and the depth fraction.
            const F32 ne = zero - e1x;
            const float len = Sqrt(X87(ne.f()) * X87(ne.f()) + X87(e1y.f()) * X87(e1y.f())).Store();
            F32 nx = e1y, ny = ne;
            if (jmp::A(len, FromBits(0x3727c5ac))) {   // 1e-5
                const F32 inv = one / F32(len);
                nx = inv * e1y;
                ny = inv * ne;
            }
            const F32 dist = ny * d1y + nx * d1x;
            const F32 h = zero - (ny * e0y + nx * e0x);
            H.distance = dist.f();
            H.onWall = jmp::A(h.f(), 0.0f) ? (one - dist / h).f() : 0.0f;
            H.normal.x = nx.f();
            H.normal.y = ny.f();
            H.normal.z = 0.0f;
            uint32_t hitFlags = static_cast<uint32_t>(w) >> 15;   // kWallFlag -> kWallHitFlag
            if (hit) hitFlags |= kWallHitAfterSegment;
            H.flags = hitFlags;
            hit = false;
            if (w & kWallPassThrough) {
                H.flags = hitFlags | kWallHitPassThrough;
                if (signal) signal(H);   // (the game's; none: no one hears it)
                return false;
            }
            return true;
        }
    }
    hit = false;
    return false;
}

} // namespace phys::native
