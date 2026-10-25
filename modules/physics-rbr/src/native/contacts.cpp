#include "native/contacts.h"

#include <cstring>

#include "num/f32.h"
#include "num/x87.h"
#include "native/flags.h"

namespace phys::native {

using num::F32;
using num::Vec3;
using num::X87;
namespace jmp = num::jmp;

namespace {
Vec3 Xyz(const Vec4& v) { return {v.x, v.y, v.z}; }
Vec4 Point(const GroundHit& h) { return {h.point.x, h.point.y, h.point.z, 0.0f}; }   // (+0xc: never written, 0)
Vec4 PlaneOf(const GroundHit& h) { return {h.normal.x, h.normal.y, h.normal.z, h.plane}; }
} // namespace

bool GroundBodyContacts(Ground& g, Body& body, EventList& list)
{
    // The contact record handed to the events: the ground's materials, no surface cells, the depth as its time.
    CollisionHit rec;
    auto setSurface = [&](const GroundHit& h) { rec.materials = {h.material[0], h.material[1], h.blend}; };
    rec.other = &g;
    const Vec4 lo = body.boundsMin, hi = body.boundsMax;   // vslot 72
    std::vector<const StageCollision::Node*> leaves;
    g.collision->MinMaxQuery(Xyz(lo), Xyz(hi), leaves);
    if (leaves.empty()) return false;
    CollisionMesh mesh;
    const Frame& world = *list.world;
    MeshBuild(&mesh, body.mesh, body.frame, world);
    const Vec4* const verts = mesh.vertices;
    Vec4 centre = mesh.centre;
    GroundHit h{}, h2{};
    bool found = false;
    const uint32_t nv = mesh.vertexCount;
    for (uint32_t i = 0; i < nv; ++i) {   // the segments centre -> vertex
        if (!g.collision->LeavesRaycast(*g.maps, Xyz(centre), Xyz(verts[i]), leaves, h)) continue;
        setSurface(h);
        rec.plane = PlaneOf(h);
        rec.point = Point(h);
        Vec4 pb;
        FrameMoveCopy(world, verts[i], body.frame, pb, Move::Points);
        CollisionEvent* const ev = list.Create(4, body, pb, rec);
        mesh.SurfaceAtUv(i, &ev->bodySurfaceB, &ev->bodySurfaceA);
        list.Add(ev);
        found = true;
    }
    const bool edges = body.flags & kCar;   // a car's edges too
    if (edges) {
        const uint32_t ne = mesh.edgeCount;
        for (uint32_t k = 0; k < ne; ++k) {   // each edge both ways
            const Vec4& va = verts[mesh.edges[k].a];
            const Vec4& vb = verts[mesh.edges[k].b];
            if (!g.collision->LeavesRaycast(*g.maps, Xyz(va), Xyz(vb), leaves, h) || !g.collision->LeavesRaycast(*g.maps, Xyz(vb), Xyz(va), leaves, h2))
                continue;
            Vec4 n, mid;
            n.x = (X87(h2.normal.x) + X87(h.normal.x)).Store();
            n.y = (X87(h2.normal.y) + X87(h.normal.y)).Store();
            n.z = (X87(h2.normal.z) + X87(h.normal.z)).Store();
            n.w = 0.0f;   // (stack in the original)
            num::Lerp4(Point(h), Point(h2), 0.5f, mid);
            const X87 sum = Abs(X87(n.y)) + (Abs(X87(n.x)) + Abs(X87(n.z)));
            const bool opposite = jmp::A(1.0, sum.d);   // the normals cancel: from the middle towards the centre
            if (opposite) {
                n.x = (F32(centre.x) - F32(mid.x)).f();
                n.y = (F32(centre.y) - F32(mid.y)).f();
                n.z = (F32(centre.z) - F32(mid.z)).f();
            }
            num::Normalize3Approx4(n);
            rec.point.x = (F32(n.x) + F32(mid.x)).f();
            rec.point.y = (F32(mid.y) + F32(n.y)).f();
            rec.point.z = (F32(mid.z) + F32(n.z)).f();
            if (!g.collision->LeavesRaycast(*g.maps, Xyz(rec.point), Xyz(mid), leaves, h)) continue;
            rec.point = Point(h);
            setSurface(h);
            if (opposite) {
                rec.plane = PlaneOf(h);
            } else {
                const F32 d = (F32(rec.point.x) * F32(n.x) + F32(rec.point.z) * F32(n.z)) + F32(rec.point.y) * F32(n.y);
                rec.plane = {n.x, n.y, n.z, (F32(0.0f) - d).f()};
            }
            Vec4 midBody;
            FrameMoveCopy(world, mid, body.frame, midBody, Move::Points);
            CollisionEvent* const ev = list.Create(5, body, midBody, rec);
            const Vec4 bh{(F32(vb.x) - F32(rec.point.x)).f(), (F32(vb.y) - F32(rec.point.y)).f(), (F32(vb.z) - F32(rec.point.z)).f(), 0.0f};
            const Vec4 ba{(F32(vb.x) - F32(va.x)).f(), (F32(vb.y) - F32(va.y)).f(), (F32(vb.z) - F32(va.z)).f(), 0.0f};
            const float t = (X87(num::Length3(bh)) / X87(num::Length3(ba))).Store();
            mesh.SurfaceOnEdge(k, t, &ev->bodySurfaceB, &ev->bodySurfaceA);
            list.Add(ev);
            found = true;
        }
    }
    if (found || !g.collision->LeavesPointQuery(*g.maps, Xyz(centre), leaves, h)) return found;
    // Nothing crossed: the centre itself in the ground.
    rec.time = h.t;
    rec.plane = PlaneOf(h);
    rec.point = Point(h);
    setSurface(h);
    if (!edges) {   // pushed down by half the body's height along the normal
        const F32 height = F32(hi.z) - F32(lo.z);
        centre.x = (F32(centre.x) - height * F32(h.normal.x) * F32(0.5f)).f();
        centre.y = (F32(centre.y) - height * F32(h.normal.y) * F32(0.5f)).f();
        centre.z = (F32(centre.z) - height * F32(h.normal.z) * F32(0.5f)).f();
    }
    Vec4 centreBody;
    FrameMoveCopy(world, centre, body.frame, centreBody, Move::Points);
    list.Add(list.Create(4, body, centreBody, rec));
    return true;
}

bool Body::BoxTest(const FramePoint& lo, const FramePoint& hi)
{
    // BodyBoxOverlap (0x4e1230): both frame points into the world, strictly inside the world box's reach per axis.
    const Frame& world = WorldOf(frame);
    Vec4 a, b;
    FramePointMove(lo, a, world);
    FramePointMove(hi, b, world);
    for (uint32_t k = 0; k < 3; ++k)   // 0 > lo - body max (comiss 0, d; jbe: fails)
        if (!jmp::A(0.0f, (F32((&a.x)[k]) - F32((&boundsMax.x)[k])).f())) return false;
    for (uint32_t k = 0; k < 3; ++k)   // 0 > body min - hi
        if (!jmp::A(0.0f, (F32((&boundsMin.x)[k]) - F32((&b.x)[k])).f())) return false;
    return true;
}

bool Body::Penetrate(const MeshInFrame& rec, Entity& other, EventList& list, uint32_t physicsFlags) { return CarPenetrate(*this, rec, other, list, physicsFlags); }
bool Body::Sweep(const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list) { return BodySweepContacts(*this, a, b, other, list); }
bool Body::EdgeSweep(const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list) { return BodyEdgeSweep(*this, a, b, other, list); }

bool Ground::SegmentContact(const FramePoint& a, const FramePoint& b, CollisionHit& out, bool)
{
    const Frame& world = WorldOf(frame);
    Vec4 pa, pb;
    FramePointMove(a, pa, world);
    FramePointMove(b, pb, world);
    GroundHit h{};
    if (!collision->SegmentQuery(*maps, Xyz(pa), Xyz(pb), h)) return false;
    out.point = Point(h);   // (and the hit's unwritten word: 0)
    out.plane = PlaneOf(h);
    out.materials = {h.material[0], h.material[1], h.blend};
    out.surfaceA = out.surfaceB = 0;
    out.time = h.t;
    out.other = this;
    return true;
}

bool BodySweepContacts(Body& body, const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list)
{
    // The mesh where it was, in the other's older frame, and where it is, in its current one (FramePairMove, 0x51b780).
    CollisionMesh meshA = *a.mesh, meshB = *b.mesh;
    MeshMove(*a.frame, other.olderFrame, &meshA, 1);
    MeshMove(*b.frame, other.frame, &meshB, 1);
    CollisionHit out;   // the query's contact, kept across the vertices (the original clears only its materials)
    const uint32_t n = body.mesh->vertexCount;
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        const FramePoint fb{&meshB.vertices[i], &other.frame}, fa{&meshA.vertices[i], &other.olderFrame};
        if (!other.SegmentContact(fa, fb, out, true)) continue;
        Vec4 pb;
        FrameMoveCopy(*list.world, out.point, body.frame, pb, Move::Points);
        CollisionEvent* const ev = list.Create(0, body, pb, out);
        body.mesh->SurfaceAtUv(i, &ev->bodySurfaceB, &ev->bodySurfaceA);
        list.Add(ev);
        found = true;
    }
    return found;
}

bool BodyEdgeSweep(Body& body, const MeshInFrame& a, const MeshInFrame& b, Entity& other, EventList& list)
{
    CollisionMesh* const otherMesh = other.Mesh();
    CollisionMesh meshA = *a.mesh, meshB = *b.mesh;   // where the mesh was, in the other's older frame, and where it is
    MeshMove(*a.frame, other.olderFrame, &meshA, 1);
    MeshMove(*b.frame, other.frame, &meshB, 1);
    CollisionMesh* const own = body.mesh;
    if (!own || !otherMesh || !otherMesh->vertexCount || !meshA.vertexCount) return false;
    CollisionMesh meshO;
    MeshBuild(&meshO, otherMesh, other.frame, other.frame);
    Vec4 sweptLo, sweptHi, lo, hi;   // the overlap of the swept box and the other's
    num::Min4To(meshA.boundsMin, meshB.boundsMin, sweptLo);
    num::Max4To(meshA.boundsMax, meshB.boundsMax, sweptHi);
    num::Max4To(meshO.boundsMin, sweptLo, lo);
    num::Min4To(meshO.boundsMax, sweptHi, hi);
    const F32 half(0.5f);
    const Vec4 centre{((F32(hi.x) + F32(lo.x)) * half).f(), ((F32(hi.y) + F32(lo.y)) * half).f(), ((F32(hi.z) + F32(lo.z)) * half).f(), 0.0f};
    const Vec4 ext{((F32(hi.x) - F32(lo.x)) * half).f(), ((F32(hi.y) - F32(lo.y)) * half).f(), ((F32(hi.z) - F32(lo.z)) * half).f(), 0.0f};
    Vec4 P{}, plane{}, pb;
    float best = 0.0f;
    bool found = false;
    const Frame& world = *list.world;
    const uint32_t ne = own->edgeCount;
    for (uint32_t k = 0; k < ne; ++k) {
        const Vec4& aB = meshB.vertices[meshB.edges[k].a];
        const Vec4& bB = meshB.vertices[meshB.edges[k].b];
        if (!num::SegmentBoxTest(&aB.x, &bB.x, &centre.x, &ext.x)) continue;
        const Vec4& aA = meshA.vertices[meshA.edges[k].a];
        const Vec4& bA = meshA.vertices[meshA.edges[k].b];
        if (!meshO.EdgesSweep(aA, bA, aB, bB, &P, &best, &plane)) continue;   // (its surface bytes stay in the copy)
        found = true;
        FrameMoveCopy(other.frame, P, world, P, Move::Points);
        FrameMoveCopy(other.frame, plane, world, plane, Move::Planes);
        FrameMoveCopy(world, P, body.frame, pb, Move::Points);
        CollisionEvent* const ev = list.CreateHit(1, P, best, body, other, pb, plane);
        ev->bodySurfaceB = own->surfaceB;   // the body mesh's surface bytes, consumed
        ev->bodySurfaceA = own->surfaceA;
        own->surfaceA = own->surfaceB = 0;
        list.Add(ev);
    }
    return found;
}

bool CarPenetrate(Body& car, const MeshInFrame& rec, Entity& other, EventList& list, uint32_t physicsFlags)
{
    if (other.flags & kGround) return GroundBodyContacts(dynamic_cast<Ground&>(other), car, list);   // the track
    if (!other.Collidable()) return false;
    CollisionMesh mesh0 = *rec.mesh;   // the record's mesh in the other's frame
    MeshMove(*rec.frame, other.frame, &mesh0, 1);
    const Vec4* const verts0 = mesh0.vertices;
    const Vec4 centre0 = mesh0.centre;
    const Frame& world = *list.world;
    Vec4 pt{}, pt2{}, plane{}, plane2{}, pb{};
    float t1 = 0.0f, t2 = 0.0f;
    uint32_t mat = 0;
    bool found = false;
    const uint32_t nv0 = mesh0.vertexCount;
    if ((physicsFlags & kFacePenetr) && ((other.flags & kBrakeWall) || !other.Mesh())) {
        // No mesh to test against: the other's own segment point, centre -> vertex.
        const FramePoint fpCentre{&centre0, &other.frame};
        for (uint32_t i = 0; i < nv0; ++i) {
            const FramePoint fpV{&verts0[i], &other.frame};
            if (!other.SegmentPoint(fpCentre, fpV, &pt, &plane, &mat)) continue;
            FramePointMove(fpV, pb, car.frame);
            CollisionEvent* const ev = list.CreateHit(4, pt, 0.0f, car, other, pb, plane);
            mesh0.SurfaceAtUv(i, &ev->bodySurfaceB, &ev->bodySurfaceA);
            list.Add(ev);
            found = true;
        }
        return found;
    }
    CollisionMesh mesh1;
    MeshBuild(&mesh1, other.Mesh(), other.frame, other.frame);
    const Vec4 centre1 = mesh1.centre;
    const bool fromOther = other.Awake() || !car.Awake();   // which centre the sideways direction comes from
    auto midpoint = [&](Vec4& mid) {
        const F32 h(0.5f);
        mid.x = ((F32(pt2.x) + F32(pt.x)) * h).f();
        mid.y = ((F32(pt2.y) + F32(pt.y)) * h).f();
        mid.z = ((F32(pt2.z) + F32(pt.z)) * h).f();
    };
    auto planeAlong = [&](const Vec4& dir) {   // the plane through the hit with normal dir
        const F32 d = (F32(dir.x) * F32(pt.x) + F32(dir.z) * F32(pt.z)) + F32(dir.y) * F32(pt.y);
        plane = {dir.x, dir.y, dir.z, (F32(0.0f) - d).f()};
    };
    auto emit = [&](uint32_t kind, Vec4& bodyPoint) {   // the hit into the world, the event
        FrameMoveCopy(other.frame, pt, world, pt, Move::Points);
        if (kind == 5) FrameMoveCopy(other.frame, bodyPoint, car.frame, bodyPoint, Move::Points);
        FrameMoveCopy(other.frame, plane, world, plane, Move::Planes);
        return list.CreateHit(kind, pt, 0.0f, car, other, bodyPoint, plane);
    };
    if (physicsFlags & kFacePenetr) {   // the vertices into the other
        for (uint32_t i = 0; i < nv0; ++i) {   // centre -> vertex through the other's mesh
            const Vec4& v = verts0[i];
            if (!mesh1.SegmentHit(&centre0.x, &v.x, &pt, &t1, &plane, &mat)) continue;
            if (!mesh1.SegmentHit(&v.x, &centre0.x, &pt2, &t1, &plane2, &mat)) {
                FrameMoveCopy(other.frame, v, car.frame, pb, Move::Points);
            } else {   // wholly inside: sideways from the middle
                Vec4 mid{}, dir{}, q;
                midpoint(mid);
                if (fromOther) {
                    dir.x = (F32(mid.x) - F32(centre1.x)).f();
                    dir.y = (F32(mid.y) - F32(centre1.y)).f();
                } else {
                    dir.x = (F32(centre0.x) - F32(mid.x)).f();
                    dir.y = (F32(centre0.y) - F32(mid.y)).f();
                }
                dir.z = 0.0f;
                num::Normalize3Approx4(dir);
                num::AddXyz4(mid, q, dir);
                if (!mesh1.SegmentHit(&q.x, &mid.x, &pt, &t1, &plane, &mat)) continue;
                planeAlong(dir);
                FrameMoveCopy(other.frame, mid, car.frame, pb, Move::Points);
            }
            CollisionEvent* const ev = emit(4, pb);
            mesh0.SurfaceAtUv(i, &ev->bodySurfaceB, &ev->bodySurfaceA);
            list.Add(ev);
            found = true;
        }
        if (found) return found;
    }
    if (!(physicsFlags & kEdgePenetr)) return false;   // the edges
    if ((mesh1.flags & kMeshNoEdges) || !mesh1.vertexCount || !nv0) return false;
    const uint32_t ne = mesh0.edgeCount;
    for (uint32_t k = 0; k < ne; ++k) {   // the edges, both ways through the other's mesh
        const Vec4& va = verts0[mesh0.edges[k].a];
        const Vec4& vb = verts0[mesh0.edges[k].b];
        if (!mesh1.SegmentHit(&va.x, &vb.x, &pt, &t2, &plane, &mat)) continue;
        if (!mesh1.SegmentHit(&vb.x, &va.x, &pt2, &t2, &plane2, &mat)) continue;
        Vec4 mid{}, dir{}, q;
        midpoint(mid);
        if (fromOther) {
            dir.x = (F32(mid.x) - F32(centre1.x)).f();
            dir.y = (F32(mid.y) - F32(centre1.y)).f();
            dir.z = (F32(mid.z) - F32(centre1.z)).f();
        } else {
            dir.x = (F32(centre0.x) - F32(mid.x)).f();
            dir.y = (F32(centre0.y) - F32(mid.y)).f();
            dir.z = (F32(centre0.z) - F32(mid.z)).f();
        }
        if (jmp::A(dir.x, dir.z) || jmp::A(dir.y, dir.z)) dir.z = 0.0f;
        num::Normalize3Approx4(dir);
        num::AddXyz4(mid, q, dir);
        if (!mesh1.SegmentHit(&q.x, &mid.x, &pt, &t2, &plane, &mat)) continue;
        Vec4 ba, ha;
        const float lab = num::Length3(*num::Sub3W(vb, va, ba));
        const float lah = num::Length3(*num::Sub3W(pt, va, ha));
        float t = 0.0f;   // where along the edge
        if (jmp::A(lab, 0.0)) {
            t = (F32(lah) / F32(lab)).f();
            if (jmp::A(t, 1.0)) t = 1.0f;
        }
        planeAlong(dir);
        CollisionEvent* const ev = emit(5, mid);
        mesh0.SurfaceOnEdge(k, t, &ev->bodySurfaceB, &ev->bodySurfaceA);
        list.Add(ev);
        found = true;
    }
    return found;
}

} // namespace phys::native
