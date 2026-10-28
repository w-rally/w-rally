#include "native/entity.h"

#include <cstring>

#include "num/f32.h"
#include "native/collision_mesh.h"
#include "native/event.h"

namespace phys::native {

using num::F32;

void Frame::Up(Vec4* p, uint32_t n, Move m) const
{
    switch (kind) {
    case Kind::Entity:   // CEntityFrame vslots 1 / 3 / 5 (0x519530 ...): the entity's own moves (none at the world)
        if (!depth) return;
        if (m == Move::Points) owner->PointsUp(p, n);
        else if (m == Move::Dirs) owner->DirsUp(p, n);
        else owner->PlanesUp(p, n);
        return;
    case Kind::Older:   // CEntityOldFrame vslots 1 / 3 (0x5196d0 ...): through the saved pose while awake
        if (!depth) return;
        if (m == Move::Points) owner->Awake() ? owner->SavedPointsUp(p, n) : owner->PointsUp(p, n);
        else if (m == Move::Dirs) owner->Awake() ? owner->SavedDirsUp(p, n) : owner->DirsUp(p, n);
        else Unreached("CEntityOldFrame vslot 5");
        return;
    case Kind::Alias:   // CAliasFrame vslots 1 / 3 / 5: nothing
        return;
    }
}

void Frame::Down(Vec4* p, uint32_t n, Move m) const
{
    switch (kind) {
    case Kind::Entity:
        if (!depth) return;
        if (m == Move::Points) owner->PointsDown(p, n);
        else if (m == Move::Dirs) owner->DirsDown(p, n);
        else owner->PlanesDown(p, n);
        return;
    case Kind::Older:
        if (!depth) return;
        if (m == Move::Points) owner->Awake() ? owner->SavedPointsDown(p, n) : owner->PointsDown(p, n);
        else if (m == Move::Dirs) owner->Awake() ? owner->SavedDirsDown(p, n) : owner->DirsDown(p, n);
        else Unreached("CEntityOldFrame vslot 6");
        return;
    case Kind::Alias:
        if (m == Move::Planes) Unreached("CAliasFrame vslot 6");
        return;
    }
}

bool Body::Collidable() const { return mesh && mesh->faceCount; }

bool Entity::SegmentContact(const FramePoint&, const FramePoint&, CollisionHit&, bool)
{
    Unreached("CEntity vslot 58");
    return false;
}

bool Body::SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool enabled)
{
    if (!enabled) return false;
    Vec4 pa, pb;   // BodySegmentQuery (vslot 61)
    FramePointMove(a, pa, olderFrame);
    FramePointMove(b, pb, frame);
    uint32_t material = 0;
    if (!mesh || !mesh->SegmentHit(&pa.x, &pb.x, &out.point, &out.time, &out.plane, &material)) return false;
    out.materials.material = material;   // (+0x20 and +0x24: the material twice; the weight +0x28 = 0)
    out.materials.second = material;
    out.materials.weight = 0.0f;
    const Frame& world = WorldOf(frame);
    FrameMoveCopy(frame, out.point, world, out.point, Move::Points);
    FrameMoveCopy(frame, out.plane, world, out.plane, Move::Planes);
    out.surfaceB = mesh->surfaceB;   // the mesh's surface cells, consumed
    out.surfaceA = mesh->surfaceA;
    mesh->surfaceA = mesh->surfaceB = 0;
    out.other = this;
    return true;
}

const Frame& WorldOf(const Frame& f)
{
    const Frame* g = &f;
    while (g->parent) g = g->parent;
    return *g;
}

Vec4* FramePointMove(const FramePoint& fp, Vec4& out, const Frame& to)
{
    std::memcpy(&out, fp.p, 16);
    return FrameMove(*fp.frame, to, &out, 1, Move::Points);
}

void CarBody::PointsUp(Vec4* p, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {   // CarBodyToLocal4 (0x4eb820)
        p[k].x = (F32(p[k].x) - F32(centerOfGravity.x)).f();
        p[k].y = (F32(p[k].y) - F32(centerOfGravity.y)).f();
        p[k].z = (F32(p[k].z) - F32(centerOfGravity.z)).f();
    }
}

Vec4* FrameMove(const Frame& t, const Frame& a, Vec4* p, uint32_t n, Move m)
{
    struct Ops {
        Vec4* p;
        Move m;
        void Up(const Frame& f, uint32_t k) const { f.Up(p, k, m); }
        void Down(const Frame& f, uint32_t k) const { f.Down(p, k, m); }
    };
    walk::Move(t, a, n, Ops{p, m});
    return p;
}

Vec4* FrameMoveCopy(const Frame& t, const Vec4& src, const Frame& a, Vec4& out, Move m)
{
    std::memcpy(&out, &src, 16);
    return FrameMove(t, a, &out, 1, m);
}

Vec4* FrameMoveCopyN(const Frame& t, const Vec4* src, const Frame& a, Vec4* dst, uint32_t n, Move m)
{
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&dst[i], &src[i], 16);
    return FrameMove(t, a, dst, n, m);
}

} // namespace phys::native
