#include "native/event.h"

#include "native/collision_mesh.h"

namespace phys::native {

CollisionEvent* EventList::Create(uint32_t kind, Body& body, const Vec4& pb, const CollisionHit& rec)
{
    m_owned.push_back(std::make_unique<CollisionEvent>());
    CollisionEvent& e = *m_owned.back();
    e.frame.parent = world;   // AliasFrameInit (0x51afa0): on the world, a level below it
    e.frame.depth = world->depth + 1;
    e.frame.owner = nullptr;
    e.hit = rec;
    e.body = &body;
    e.flags = kind;
    FrameMoveCopy(body.frame, pb, e.frame, e.bodyPoint, Move::Points);
    FrameMoveCopy(body.frame, direction, e.frame, e.bodyDirection, Move::Dirs);
    return &e;
}

CollisionEvent* EventList::CreateHit(uint32_t kind, const Vec4& point, float t, Body& body, Entity& other, const Vec4& pb, const Vec4& plane)
{
    CollisionHit hit;
    uint32_t mat = 0;
    if (CollisionMesh* const m = other.Mesh()) {   // the other's surface cells, consumed
        hit.surfaceB = m->surfaceB;
        hit.surfaceA = m->surfaceA;
        m->surfaceA = m->surfaceB = 0;
        mat = m->material;
    }
    hit.materials = {mat, mat, 0.0f};
    hit.point = point;
    hit.plane = plane;
    hit.time = t;
    hit.other = &other;
    CollisionEvent* const ev = Create(kind, body, pb, hit);
    if (const CollisionMesh* const m = body.Mesh()) ev->bodyMaterials = {m->material, m->material, 0.0f};
    return ev;
}

void EventList::Add(CollisionEvent* ev) { m_list.insert(m_list.begin(), ev); }

void EventList::Clear()
{
    m_list.clear();
    m_owned.clear();
}

} // namespace phys::native
