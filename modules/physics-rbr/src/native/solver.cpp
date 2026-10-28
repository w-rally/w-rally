#include "native/solver.h"

#include <cstring>

#include "num/f32.h"
#include "num/x87.h"
#include "native/flags.h"
#include "native/collision_mesh.h"
#include "native/world.h"

namespace phys::native {

using num::F32;
using num::F32x4;
using num::X87;
namespace jmp = num::jmp;

const SurfaceMaterial& MaterialTable::operator[](uint32_t id) const
{
    return id < 0x100 && (id == 0 || (entries[id].flags & kMaterialDefined)) ? entries[id] : entries[0];
}

// ---- the bodies' solver slots

void Solid::VelocityAt(const Vec4& p, Vec4& out) const
{
    const F32 wx(body.angularVelocity.x), wy(body.angularVelocity.y), wz(body.angularVelocity.z);
    const Vec4& v = body.velocity;
    if (parent) {   // the parent's velocity there, plus this one's in the parent's frame
        const Frame& root = WorldOf(frame);
        parent->VelocityAt(p, out);
        Vec4 r;
        FrameMoveCopy(root, p, parent->frame, r, Move::Points);
        const F32 rx = r.x, ry = r.y, rz = r.z;
        const Vec4 l = {((wy * rz + F32(v.x)) - ry * wz).f(), ((rx * wz + F32(v.y)) - wx * rz).f(), ((wx * ry + F32(v.z)) - wy * rx).f(), 0.0f};
        Vec4 o;
        FrameMoveCopy(parent->frame, l, root, o, Move::Dirs);
        out.x = (F32(out.x) + F32(o.x)).f();
        out.y = (F32(o.y) + F32(out.y)).f();
        out.z = (F32(o.z) + F32(out.z)).f();
        return;
    }
    const F32 rx = F32(p.x) - F32(body.position.x), ry = F32(p.y) - F32(body.position.y), rz = F32(p.z) - F32(body.position.z);
    out.x = ((wy * rz + F32(v.x)) - wz * ry).f();
    out.y = ((wz * rx + F32(v.y)) - wx * rz).f();
    out.z = ((wx * ry + F32(v.z)) - wy * rx).f();
}

void Solid::ImpulseResponse(const Frame& from, const Vec4& p, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const
{
    Vec4 a, b, r;   // the directions and the point in the body's frame
    FrameMoveCopy(from, n, frame, a, Move::Dirs);
    FrameMoveCopy(from, d, frame, b, Move::Dirs);
    FrameMoveCopy(from, p, frame, r, Move::Points);
    body.ImpulseResponse(r, a, b, cross, self, other);
}

void Solid::ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c)
{
    const Frame& root = WorldOf(frame);
    const Vec4& pos = body.position;
    const Vec4 r{(F32(p.x) - F32(pos.x)).f(), (F32(p.y) - F32(pos.y)).f(), (F32(p.z) - F32(pos.z)).f(), 0.0f};
    Vec4 t{}, tb, wb, w;
    num::Cross3(r, J, t);
    FrameMoveCopy(root, t, frame, tb, Move::Dirs);   // the torque into the body, times the inverse inertia, back out
    num::Mul4(body.def->localInverseInertia, tb, wb);
    FrameMoveCopy(frame, wb, root, w, Move::Dirs);
    float* const av = &body.angularVelocity.x;
    (F32x4::Load(&w.x) * F32x4::Splat(F32(0.6f)) + F32x4::Load(av)).Store(av);
    float* const lv = &body.velocity.x;
    (F32x4::Load(&J.x) * F32x4::Splat(F32(body.def->inverseMass)) + F32x4::Load(lv)).Store(lv);
    if (damage) {
        ImpulseHit im;
        im.surfaceA = c.surfaceA, im.surfaceB = c.surfaceB;
        im.material = jmp::A(0.5f, c.materials->weight) ? c.materials->material : c.materials->second;
        im.normal = c.normal, im.slide = c.slide;
        FrameMoveCopy(root, p, frame, im.pointInBody, Move::Points);
        im.point = p;
        im.tangent = c.tangent;
        damage->Hit(im);
    }
    // (The impact recorder - a debug ring drawn with RenderDamageInfo - is not kept.)
}

// ---- the events

namespace {
// CollisionEventMaterial (0x4f4b60): the other side's when the body moves, else the body's (the second id above weight 0.5).
const SurfaceMaterial& EventMaterial(const CollisionEvent& ev, const MaterialTable& materials, uint32_t& id)
{
    const MaterialBlend& blend = (ev.flags & kEventBodyMoves) ? ev.hit.materials : ev.bodyMaterials;
    id = jmp::A(blend.weight, 0.5f) ? blend.second : blend.material;
    return materials[id];
}
} // namespace

void EventMaterials(CollisionEvent& ev)
{
    struct Side {
        MaterialBlend& materials;
        Entity* object;
    };
    for (const Side& side : {Side{ev.hit.materials, ev.hit.other}, Side{ev.bodyMaterials, ev.body}}) {   // no material: its mesh's
        MaterialBlend& m = side.materials;
        if (jmp::BE(0.5f, m.weight) ? m.second : m.material) continue;
        if (!side.object || !(side.object->flags & kBody)) continue;
        const CollisionMesh* const mesh = side.object->Mesh();
        if (!mesh) {
            Unreached("an event's body without a mesh");
            continue;
        }
        m = {mesh->material, 0, 0.0f};
    }
}

void EventMaterialFlags(CollisionEvent& ev, const MaterialTable& materials)
{
    if (materials[ev.bodyMaterials.material].flags & kMaterialPassThrough) ev.flags |= kEventBodyPassThrough;
    if (materials[ev.hit.materials.material].flags & kMaterialPassThrough) ev.flags |= kEventOtherPassThrough;
}

void EventsFlag(EventList& list, const MaterialTable& materials)
{
    for (size_t i = 0; i < list.Size(); ++i) {
        CollisionEvent& ev = *list[i];
        EventMaterials(ev);
        EventMaterialFlags(ev, materials);
        if (jmp::A(static_cast<double>(ev.body->InverseMass()), 0.0)) ev.flags |= kEventBodyMoves;
        const Entity* const other = ev.hit.other;
        if ((other->flags & kSolid) && jmp::A(static_cast<double>(other->InverseMass()), 0.0)) ev.flags |= kEventOtherMoves;
    }
}

// ---- the solve

void ImpulseSolver::RelativeVelocity(const CollisionEvent& ev, Vec4& out) const
{
    Vec4 a{}, b{};
    if (ev.body) ev.body->VelocityAt(ev.hit.point, a);
    if (ev.hit.other) ev.hit.other->VelocityAt(ev.hit.point, b);
    out.x = (F32(a.x) - F32(b.x)).f();
    out.y = (F32(a.y) - F32(b.y)).f();
    out.z = (F32(a.z) - F32(b.z)).f();
}

void ImpulseSolver::Response(const CollisionEvent& ev, const Vec4& v0, const Vec4& v1, bool cross, float* self, float* other) const
{
    *self = 0.0f;
    *other = 0.0f;
    for (const Entity* e : {static_cast<const Entity*>(ev.body), static_cast<const Entity*>(ev.hit.other)})
        e->ImpulseResponse(ev.frame, ev.hit.point, v0, v1, cross, self, other);
}

void ImpulseSolver::Apply(const CollisionEvent& ev, float j, float L, const Vec4& J, const Vec4& K) const
{
    if (ev.flags & (kEventBodyPassThrough | kEventOtherPassThrough)) return;
    const Vec4& p = ev.hit.point;
    if (ev.body) {
        const bool own = ev.bodySurfaceB != 0;   // the body's own surface cells, else the other's
        ev.body->ApplyImpulse(p, J, {own ? ev.bodySurfaceA : ev.hit.surfaceA, own ? ev.bodySurfaceB : ev.hit.surfaceB, &ev.hit.materials, j, L, &K});
    }
    if (ev.hit.other) {
        Vec4 nk, nj;
        num::Scale4To(K, nk, -1.0f);
        num::Scale4To(J, nj, -1.0f);
        ev.hit.other->ApplyImpulse(p, nj, {ev.hit.surfaceA, ev.hit.surfaceB, &ev.bodyMaterials, j, L, &nk});
    }
}

float ImpulseSolver::Solve(CollisionEvent& ev) const
{
    Vec4 rv{};   // (w: the original's stack - it reaches the slide's w and so the velocities' w lanes; 0 here)
    RelativeVelocity(ev, rv);
    const Vec4 n = ev.hit.plane;
    const F32 vn = (F32(rv.z) * F32(n.z) + F32(rv.y) * F32(n.y)) + F32(rv.x) * F32(n.x);   // the approach speed
    bool solve = (ev.flags & kEventBodyMoves) && (ev.flags & kEventOtherMoves);
    if (jmp::A(-1.0f, vn.f())) solve = true;
    if ((ev.flags & (kEventBodyPassThrough | kEventOtherPassThrough)) || !solve || !jmp::A(0.0f, vn.f())) return 0.0f;
    // The slide rv - n vn, its direction when above 0.001.
    Vec4 t;
    (F32x4::Load(&n.x) * F32x4::Splat(F32(0.0f) - vn) + F32x4::Load(&rv.x)).Store(&t.x);
    const float L = num::Length3(t);
    Vec4 dir{};
    const bool hasT = jmp::A(L, 0.001f);
    if (hasT) num::Scale4(t, (F32(1.0f) / F32(L)).f(), dir);
    uint32_t id;
    const SurfaceMaterial& M = EventMaterial(ev, *materials, id);
    F32 k = vn * F32(-0.05f);   // the impact's weight between the material's two values, clamped to [0, 1]
    if (jmp::A(0.0f, k.f())) k = F32(0.0f);
    else if (jmp::A(k.f(), 1.0f)) k = F32(1.0f);
    const float mu = ((F32(M.frictionHigh) - F32(M.frictionLow)) * k + F32(M.frictionLow)).f();
    float self, other;
    Response(ev, n, dir, hasT, &self, &other);
    // The normal impulse j = -(restitution + 1) vn / response (x87).
    const X87 bounce = ((X87(M.restitutionHigh) - X87(M.restitutionLow)) * X87(k.f()) + X87(M.restitutionLow)) + X87(1.0f);
    const float j = ((bounce * (X87(-1.0f) / X87(self))) * X87(vn.f())).Store();
    Vec4 J;
    num::Scale4(n, j, J);
    if (hasT) {   // friction: the impulse stopping the slide, at most |j| mu
        float s2, o2;
        Response(ev, dir, n, true, &s2, &o2);
        const F32 num = F32(o2) * F32(j) + F32(L);
        const F32 f = num / F32(s2);
        auto abs = [](F32 x) { return jmp::A(x.f(), 0.0f) ? x : F32(0.0f) - x; };
        F32 ft = abs(F32(j)) * F32(mu);
        if (jmp::A(ft.f(), abs(f).f())) ft = abs(f);
        auto sign = [](float x) { return jmp::A(x, 0.0f) ? 1 : (jmp::A(0.0f, x) ? -1 : 0); };
        if (sign(num.f()) != sign(L)) ft = F32(0.0f) - ft;
        (F32x4::Load(&dir.x) * F32x4::Splat(F32(0.0f) - ft) + F32x4::Load(&J.x)).Store(&J.x);
    }
    Apply(ev, j, L, J, t);
    return j;
}

void ImpulseSolver::SolveAll(EventList& list) const
{
    for (size_t i = 0; i < list.Size(); ++i) {
        CollisionEvent& ev = *list[i];
        Solid* const A = ev.body->AsSolid();
        if (!A) {
            Unreached("a solved event's body that is not a solid");
            continue;
        }
        Solid* const B = (ev.hit.other->flags & kSolid) ? ev.hit.other->AsSolid() : nullptr;   // an other that steps
        const float tf = (X87(ev.hit.time) - X87(0.1f)).Store();
        float lim = A->body.blend;   // how far the bodies have stepped
        if (B && jmp::A(lim, B->body.blend)) lim = B->body.blend;
        if (jmp::A(tf, lim)) continue;   // the event comes after the bodies' step
        for (Solid* s : {A, B}) {   // an asleep body with a mass is woken
            if (!s) continue;
            if (jmp::A(s->InverseMass(), 0.0f) && !s->Awake() && !s->held) {
                s->Wake();
                if (woken) woken(*s);
            }
        }
        Solve(ev);
    }
}

// ---- the penetration pass

bool ContactGroups::AddEvent(Solid& body, const CollisionEvent& ev, bool side, const MaterialTable& materials)
{
    if (!body.contactGroup) {   // a new group from the pool
        if (groupCount >= groups.size()) return false;
        ContactGroup& g = groups[groupCount++];
        g.body = &body;
        g.count = 0;
        body.contactGroup = &g;
        body.body.inContactGroup = true;
    }
    ContactGroup& group = *body.contactGroup;
    ContactRecord rec;
    const SurfaceMaterial& M = EventMaterial(ev, materials, rec.material);
    rec.frictionHigh = M.frictionHigh;
    rec.frictionLow = M.frictionLow;
    const Vec4* src;
    uint32_t flag;
    if (side) {   // the body's side
        rec.surfaceA = ev.bodySurfaceA, rec.surfaceB = ev.bodySurfaceB;
        rec.point = ev.hit.point;
        rec.normal = ev.hit.plane;
        rec.worldPoint = ev.bodyPoint;
        src = &ev.bodyPoint;
        flag = kEventOtherPassThrough;
    } else {      // the other's: the plane turned round
        rec.surfaceA = ev.hit.surfaceA, rec.surfaceB = ev.hit.surfaceB;
        rec.point = ev.bodyPoint;
        rec.normal = ev.hit.plane;
        (F32x4::Splat(F32(-1.0f)) * F32x4::Load(&rec.normal.x)).Store(&rec.normal.x);
        rec.worldPoint = ev.hit.point;
        src = &ev.hit.point;
        flag = kEventBodyPassThrough;
    }
    if (ev.flags & flag) {   // a pass-through material: a soft contact, its restitution pair the spring
        rec.softStiffness = M.restitutionLow;
        rec.softLimit = M.restitutionHigh;
        rec.soft = true;
    }
    FrameMoveCopy(WorldOf(body.frame), *src, body.frame, rec.bodyPoint, Move::Points);
    if (group.count >= group.records.size()) return false;
    group.records[group.count++] = rec;
    return true;
}

bool ContactGroups::AddPoint(Solid& body, const Vec4& point)
{
    if (PushGroup* const own = body.pushGroup) {
        if (own->count >= own->points.size()) return false;
        own->points[own->count++] = point;
        return true;
    }
    if (pushCount >= pushes.size()) return false;   // the pool is full
    PushGroup& group = pushes[pushCount++];
    group.points[0] = point;
    group.body = &body;
    group.count = 1;
    body.pushGroup = &group;
    return true;
}

void ResolvePushGroup(PushGroup& G)
{
    RigidBody& body = G.body->body;
    const uint32_t n = G.count;
    F32 sx(0.0f), sy(0.0f), sz(0.0f);
    for (uint32_t i = 0; i < n; ++i) {   // point first in each sum
        sx = F32(G.points[i].x) + sx;
        sy = F32(G.points[i].y) + sy;
        sz = F32(G.points[i].z) + sz;
    }
    const X87 l2 = (X87(sz.f()) * X87(sz.f()) + X87(sy.f()) * X87(sy.f())) + X87(sx.f()) * X87(sx.f());
    if (!jmp::A(l2.d, 1e-5)) {
        G.body->pushGroup = nullptr;
        return;
    }
    const F32 inv = F32((X87(1.0f) / Sqrt(l2)).Store());
    Vec4 dir{sx.f(), sy.f(), sz.f(), 0.0f};
    (F32x4::Splat(inv) * F32x4::Load(&dir.x)).Store(&dir.x);
    const F32 nx(dir.x), ny(dir.y), nz(dir.z);
    // The deepest point along the direction; the original's unrolled blocks of four sum the first point of a block
    // (z + x) + y and the other three (z + y) + x, the tail (z + x) + y.
    F32 m(0.0f);
    const uint32_t blocks = n >= 4 ? n & ~3u : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = &G.points[i].x;
        F32 d;
        if (i < blocks && (i & 3)) d = (F32(p[2]) * nz + F32(p[1]) * ny) + F32(p[0]) * nx;
        else if (i < blocks) d = (F32(p[2]) * nz + F32(p[0]) * nx) + ny * F32(p[1]);
        else d = (F32(p[2]) * nz + F32(p[0]) * nx) + F32(p[1]) * ny;
        if (jmp::A(d.f(), m.f())) m = d;
    }
    if (jmp::A(m.f(), 0.03f)) m = F32(0.03f);
    float* const pos = &body.position.x;
    (F32x4::Load(&dir.x) * F32x4::Splat(m) + F32x4::Load(pos)).Store(pos);
    G.body->pushGroup = nullptr;
}

void ContactGroups::Depenetrate(EventList& list, const MaterialTable& materials)
{
    float margin = 0.02f;
    for (size_t i = 0; i < list.Size(); ++i) {
        CollisionEvent& ev = *list[i];
        if (!(ev.body->flags & kSolid)) continue;   // the body does not step
        Solid* const A = ev.body->AsSolid();
        if (!A) {
            Unreached("a depenetrated event's body that is not a solid");
            continue;
        }
        const bool a = (ev.flags & kEventBodyMoves) != 0, b = (ev.flags & kEventOtherMoves) != 0;   // the sides taking part
        Solid* B = nullptr;
        bool aPath = a;
        if (ev.hit.other->flags & kSolid) {
            B = ev.hit.other->AsSolid();
            if (b) {
                if (!a) aPath = false;
                else margin = -0.001f;   // (kept for the following events too)
            }
        }
        // The penetration depth: the body's point . the normal + the normal's offset + the margin.
        auto depth = [&]() {
            const Vec4& p = ev.bodyPoint;
            const Vec4& n = ev.hit.plane;
            return (((F32(p.z) * F32(n.z) + F32(p.y) * F32(n.y)) + F32(p.x) * F32(n.x)) + F32(n.w)) + F32(margin);
        };
        auto push = [&](F32 d) {
            Vec4 P = ev.hit.plane;
            (F32x4::Splat(d) * F32x4::Load(&P.x)).Store(&P.x);
            return P;
        };
        const bool through = (ev.flags & (kEventBodyPassThrough | kEventOtherPassThrough)) != 0;
        if (aPath) {
            const F32 d = depth();
            if (jmp::A(0.0f, d.f()) && !through) {
                const Vec4 P = push(d);
                if (b) {   // both bodies: shared by their inverse masses
                    if (!B) {
                        Unreached("a depenetrated event's other that is not a solid");
                        continue;
                    }
                    const float ra = A->InverseMass();
                    const float w = (X87(1.0f) / (X87(ra) / X87(B->InverseMass()) + X87(1.0f))).Store();
                    Vec4 pa, pb;
                    num::Scale4(P, -((X87(1.0f) - X87(w)).Store()), pa);
                    AddPoint(*A, pa);
                    num::Scale4(P, w, pb);
                    AddPoint(*B, pb);
                    continue;
                }
                Vec4 pa;
                num::Scale4(P, -1.0f, pa);
                AddPoint(*A, pa);
                AddEvent(*A, ev, true, materials);
                continue;
            }
            if (!b) AddEvent(*A, ev, true, materials);
            continue;
        }
        if (!b) continue;
        if (!B) {
            Unreached("a depenetrated event's other that is not a solid");
            continue;
        }
        const F32 d = depth();
        if (jmp::A(0.0f, d.f()) && !through) {
            Vec4 pb;
            num::Scale4(push(d), 1.0f, pb);
            AddPoint(*B, pb);
        }
        AddEvent(*B, ev, false, materials);
    }
    for (uint32_t i = 0; i < pushCount; ++i) ResolvePushGroup(pushes[i]);
    pushCount = 0;
}

void ContactGroups::Clear()
{
    for (uint32_t i = 0; i < groupCount; ++i)   // each group's body forgets it
        if (Solid* const body = groups[i].body) body->contactGroup = nullptr, body->body.inContactGroup = false;
    groupCount = 0;
}

// ---- the resting contacts

namespace {
// (n.y a.y + n.z a.z) + n.x a.x: the original's order for dots with a contact's normal.
F32 DotN(const Vec4& n, const Vec4& a) { return (F32(n.y) * F32(a.y) + F32(n.z) * F32(a.z)) + F32(n.x) * F32(a.x); }
} // namespace

bool ContactResponse(Solid& body, const ContactRecord& R, float weight, float frictionScale, Vec4& force, Vec4& torque, float& impulse,
                     float& depth)
{
    const Frame& world = WorldOf(body.frame);
    const Vec4& n = R.normal;
    Vec4 p;   // the body's point now, in the world
    FrameMoveCopy(body.frame, R.bodyPoint, world, p, Move::Points);
    const F32 dz = F32(p.z) - F32(R.point.z), dy = F32(p.y) - F32(R.point.y), dx = F32(p.x) - F32(R.point.x);
    depth = ((F32(n.z) * dz + F32(n.y) * dy) + dx * F32(n.x)).f();
    if (jmp::BE(0.0f, depth)) return false;   // not below the other side's point
    const F32 k = F32(R.softStiffness);
    if (k.f() == 0.0f && jmp::A(-0.02f, depth)) depth = -0.02f;   // a hard contact's depth at most 2 cm
    Vec4 v;
    body.VelocityAt(p, v);
    const F32 vn = DotN(n, v);
    F32 slip = vn;
    const F32 one = 1.0f, zero = 0.0f;
    F32 e = one;   // the response's weight: 1 approaching, falling to 0 separating at 0.4 m/s
    if (!jmp::BE(vn.f(), 0.001f)) {
        F32 s = vn;
        const float speed = num::Length3(v);
        if (jmp::A(speed, 1.0f)) {
            Vec4 u;
            num::Scale4(v, (one / F32(speed)).f(), u);
            s = DotN(n, u) * vn;
        }
        e = !jmp::BE(s.f(), 0.4f) ? zero : (F32(0.4f) - s) * F32(2.5f);
    }
    const F32 inverseMass = F32(body.body.def->inverseMass);
    F32 K;
    if (!jmp::BE(R.softLimit, 0.0f)) {   // a soft contact: its spring, capped
        K = k;
        const F32 J = zero - (F32(depth) * k) * e;
        impulse = J.f();
        const F32 cap = F32(R.softLimit) * e;
        if (!jmp::BE(J.f(), cap.f())) impulse = cap.f();
        uint32_t bits = 0x42443334;   // 49.05 (one ulp above 49.05f)
        float c;
        std::memcpy(&c, &bits, 4);
        const F32 lim = (e / inverseMass) * F32(c);
        if (!jmp::BE(impulse, lim.f())) impulse = lim.f();
    } else {   // a hard one: a spring holding the body's weight at 3 cm
        K = (F32(weight) * F32(9.8100004f)) / (inverseMass * F32(0.03f));
        const F32 J = zero - (F32(depth) * K) * e;
        impulse = J.f();
        const F32 lim = (e / inverseMass) * F32(14.715f);
        if (!jmp::BE(J.f(), lim.f())) impulse = lim.f();
    }
    if (impulse == 0.0f) return false;
    Vec4 F, w;
    num::Scale4(n, impulse, F);
    num::Scale4(n, vn.f(), w);
    Vec4 t = {(F32(v.x) - F32(w.x)).f(), (F32(v.y) - F32(w.y)).f(), (F32(v.z) - F32(w.z)).f(), 0.0f};   // (w: the original's stack)
    const float len = num::Length3(t);
    slip = len;
    if (jmp::A(len, 1e-4f)) {   // friction against the slide
        (F32x4::Splat(one / F32(len)) * F32x4::Load(&t.x)).Store(&t.x);
        const F32 mu = (F32(R.frictionLow) + F32(R.frictionHigh)) * F32(0.5f);
        F32 f = ((F32(frictionScale) * F32(len)) * K) * F32(3.0f);
        const F32 g = F32(impulse) * mu;
        if (!jmp::BE(f.f(), g.f())) f = g;
        if (!jmp::BE(k.f(), 0.0f) && !jmp::BE(impulse, f.f())) {
            F32 s = F32(len) * F32(0.2f);
            if (!jmp::BE(0.0f, s.f())) s = zero;
            else if (!jmp::BE(s.f(), 1.0f)) s = one;
            const F32 m = F32(impulse) * s;
            if (!jmp::BE(m.f(), f.f())) f = m;
        }
        (F32x4::Load(&t.x) * F32x4::Splat(zero - f) + F32x4::Load(&F.x)).Store(&F.x);
    }
    FrameMoveCopy(world, F, body.frame, force, Move::Dirs);   // into the body's axes, and its torque about the body's origin
    num::Cross3(R.bodyPoint, force, torque);
    if (body.damage && (body.flags & kCar)) {   // the car's damage handler feels the push
        ContactImpact im;
        if (!body.SurfaceAt(R.bodyPoint, im.surfaceA, im.surfaceB)) {   // (the car's slot: CarSurfaceAt)
            Unreached("a contact impact on a car that is not a Car");
            return true;
        }
        im.material = R.material;
        im.force = num::Length3(F);
        im.slide = slip.f();
        im.bodyPoint = R.bodyPoint;
        im.point = p;
        im.direction = &t;
        body.damage->Impact(im);
    }
    return true;
}

bool ContactSum(Solid& body, float weight, float frictionScale, Vec4& force, Vec4& torque)
{
    const ContactGroup* const group = body.contactGroup;
    if (!group) return false;
    num::Vec4Zero(force);
    num::Vec4Zero(torque);
    bool any = false;
    for (uint32_t i = 0; i < group->count; ++i) {
        Vec4 f, t;
        float impulse, depth;
        if (!ContactResponse(body, group->records[i], weight, frictionScale, f, t, impulse, depth)) continue;
        force.x = (F32(force.x) + F32(f.x)).f();
        force.y = (F32(force.y) + F32(f.y)).f();
        force.z = (F32(force.z) + F32(f.z)).f();
        torque.x = (F32(torque.x) + F32(t.x)).f();
        torque.y = (F32(torque.y) + F32(t.y)).f();
        torque.z = (F32(torque.z) + F32(t.z)).f();
        any = true;
    }
    return any;
}

} // namespace phys::native
