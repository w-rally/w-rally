#include "native/car_solids.h"

#include <algorithm>
#include <cstring>

#include "num/f32.h"
#include "num/x87.h"
#include "native/collision_mesh.h"
#include "native/flags.h"
#include "native/world.h"

namespace phys::native {

using num::F32;
using num::X87;
namespace jmp = num::jmp;

namespace {
constexpr float kBig = 999999.875f;
} // namespace

// ---- the car's parts' impulses and responses (from the solver's slots)

void PartSolid::ImpulseResponse(const Frame& from, const Vec4& p, const Vec4& n, const Vec4& d, bool cross, float* self, float* other) const
{
    if (!car) {
        Solid::ImpulseResponse(from, p, n, d, cross, self, other);
        return;
    }
    const Solid* const to = parent ? const_cast<Entity*>(parent)->AsSolid() : nullptr;
    if (!to) {
        Unreached("a part's response forwarded to a parent that is not a solid");
        return;
    }
    Vec4 a, b, r;   // in the part's frame - handed to the parent's response as they are
    FrameMoveCopy(from, n, frame, a, Move::Dirs);
    FrameMoveCopy(from, d, frame, b, Move::Dirs);
    FrameMoveCopy(from, p, frame, r, Move::Points);
    to->body.ImpulseResponse(r, a, b, cross, self, other);
}

void HingedSolid::ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c)
{
    if (!car) {
        Solid::ApplyImpulse(p, J, c);
        return;
    }
    Vec4 axis;   // the hinge axis in the world
    FrameMoveCopy(car->frame, hingeAxis, WorldOf(car->frame), axis, Move::Dirs);
    const Vec4 r{(F32(p.x) - F32(pivot.x)).f(), (F32(p.y) - F32(pivot.y)).f(), (F32(p.z) - F32(pivot.z)).f(), 0.0f};   // (w: stack)
    Vec4 m;
    num::Cross3(r, J, m);
    const F32 t = (F32(axis.z) * F32(m.z) + F32(axis.y) * F32(m.y)) + F32(axis.x) * F32(m.x);   // the moment about the axis
    spin = (F32(spin) + t / F32(inertia)).f();
    F32 j = (F32(J.z) * F32(axis.z) + F32(J.y) * F32(axis.y)) + F32(axis.x) * F32(J.x);
    bool broke = false;
    if (jmp::A(j.f(), 2000.0f)) {
        j = F32(2000.0f);
        broke = true;
    } else if (jmp::A(-2000.0f, j.f())) {
        j = F32(-2000.0f);
        broke = true;
    }
    if (breakChain) return;
    Vec4 F;
    num::Scale4(axis, j.f(), F);
    const float s = jmp::A(j.f(), 0.0f) ? j.f() : (F32(0.0f) - j).f();
    Solid* const to = car;
    to->ApplyImpulse(p, F, {0, 0, c.materials, s, 0.0f, c.tangent});   // the load along the axis into the car
    if (!broke) return;
    // Overloaded: the part comes off (its damage unlocked and detached, the car lets it go into the world).
    Car* const owner = dynamic_cast<Car*>(to);
    if (!damage || !world || !owner) {
        Unreached("a hinged part torn off without its damage, its world or a car");
        return;
    }
    damage->lock = 0;
    damage->attached = 0;
    DetachHinged(*world, *owner, *this);
}

bool Car::SurfaceAt(const Vec4& pb, uint32_t& surfaceA, uint32_t& surfaceB) const
{
    const float len = num::Length3(pb);
    Vec4 p0, p1{}, point, plane;
    if (jmp::A(len, 0.0010000000474974513f)) num::Scale4(pb, (F32(10.0f) / F32(len)).f(), p0);
    else num::Vec4Zero(p0);
    p1.z = 0.3499999940395355f;
    float t;
    uint32_t mat;
    CollisionMesh* const m = Mesh();
    if (!m) {
        Unreached("CarSurfaceAt without a mesh");
        surfaceA = surfaceB = 0;
        return true;
    }
    if (m->SegmentHit(&p0.x, &p1.x, &point, &t, &plane, &mat)) {
        surfaceB = m->surfaceB, surfaceA = m->surfaceA;
        m->surfaceA = m->surfaceB = 0;
    } else {
        surfaceA = surfaceB = 0;
    }
    return true;
}

void Car::DriveLineStep(World& w)
{
    if (!w.driveLine) Unreached("a car's step without the stage's drive line");
    else driveLineDistance = w.driveLine->Project(&body.position.x, driveLineDistance).Store();
}

// ---- the car's parts let go

void DetachHinged(World& w, Car& car, HingedSolid& h)
{
    for (Solid*& slot : car.hinged)
        if (slot == &h && ReleaseHinged(w, h)) slot = nullptr;
    if (!h.hasExclusions) return;   // EntityChildRemove: the car out of the part's exclusions
    if (const auto it = std::find(h.exclusions.begin(), h.exclusions.end(), &car); it != h.exclusions.end()) h.exclusions.erase(it);
}

bool ReleaseHinged(World& w, HingedSolid& h) { return w.ReleasePart(h, h.car, false); }
bool ReleaseStick(World& w, MountedSolid& m) { return w.ReleasePart(m, m.car, true); }

void Car::ApplyImpulse(const Vec4& p, const Vec4& J, const ImpulseContact& c)
{
    Vec4 pb;
    FrameMoveCopy(WorldOf(frame), p, frame, pb, Move::Points);
    ImpulseContact own = c;   // the car's own surface there replaces the pair
    SurfaceAt(pb, own.surfaceA, own.surfaceB);
    Solid::ApplyImpulse(p, J, own);
}

// ---- the car body's frame and box

void CarBody::PointsUp(Vec4* p, uint32_t n) const
{
    for (uint32_t k = 0; k < n; ++k) {   // CarBodyToLocal4 (0x4eb820)
        p[k].x = (F32(p[k].x) - F32(centerOfGravity.x)).f();
        p[k].y = (F32(p[k].y) - F32(centerOfGravity.y)).f();
        p[k].z = (F32(p[k].z) - F32(centerOfGravity.z)).f();
    }
}

void CarBody::Notify()
{
    Body::Notify();
    Vec4 lo{kBig, kBig, kBig, 0.0f}, hi{-kBig, -kBig, -kBig, 0.0f};   // (w: the original's stack)
    stateBoundsValid = true;
    if (mesh) MeshWorldBounds(*mesh, frame, WorldOf(frame), &lo, &hi);   // BodyMeshBounds (0x4e11a0)
    stateBoundsLo = lo, stateBoundsHi = hi;
}

} // namespace phys::native
