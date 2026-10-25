#pragma once
// Collision events, natively (M6 step 2; the classic CCollisionEvent, core/event.cpp): what a pair test found - a point
// where a body meets something (the ground, another body, a brake wall), the contact's plane and materials, when in the
// step - for the solver to resolve. Each event has an alias frame on the world (its points are the world's).
//
// The list keeps the classic order: an event added goes first (the solver walks them newest first).
#include <cstdint>
#include <memory>
#include <vector>

#include "native/entity.h"

namespace phys::native {

struct MaterialBlend {
    uint32_t material = 0, second = 0;   // the second used above weight 0.5
    float weight = 0;
};

// The contact record (the classic CollisionHit): what the pair test hands an event.
struct CollisionHit {
    Vec4 point{};                // in the world
    Vec4 plane{};                // the contact plane: its normal (towards the body) and offset (w)
    MaterialBlend materials;     // the other's
    uint32_t surfaceA = 0, surfaceB = 0;   // the other's surface cells at the hit
    float time = 0;              // when in the step (0 .. 1)
    Entity* other = nullptr;
    uint32_t unknown3c = 0;      // (the classic record's last word: stack in the original)
};

struct CollisionEvent {
    Frame frame{Frame::Kind::Alias};
    uint32_t flags = 0;                  // the kind (0 swept, 1 an edge sweep, 4 a point, 5 an edge's) and CollisionEventFlags
    CollisionHit hit;
    Body* body = nullptr;                // the body the test ran for
    uint32_t bodySurfaceB = 0, bodySurfaceA = 0;   // the body's surface cells (0: the other's are used)
    MaterialBlend bodyMaterials;
    Vec4 bodyPoint{};                    // the body's contact point, in the world
    Vec4 bodyDirection{};                // the direction given, in the world
};

class EventList {
public:
    const Frame* world = nullptr;   // the events' frames hang below it
    Vec4 direction{};               // the direction every event is given (the game's 0x8ef7e0)

    // A new event (0x4f4f10 + 0x4f50e0): of `kind` for `body`, the record `rec`; pb (in the body's frame) and the
    // direction moved into the event's frame (the world's). Not yet in the list (Add).
    CollisionEvent* Create(uint32_t kind, Body& body, const Vec4& pb, const CollisionHit& rec);
    // A new event from a hit (0x4e0d00): the record {point, plane, the other's mesh material twice, its surface bytes
    // (consumed from its mesh), t, other}, the body's mesh material as its own. Not yet in the list (Add).
    CollisionEvent* CreateHit(uint32_t kind, const Vec4& point, float t, Body& body, Entity& other, const Vec4& pb, const Vec4& plane);
    void Add(CollisionEvent* ev);   // first in the list (the list's vslot 1: ListPrepend0C)
    void Remove(size_t i) { m_list.erase(m_list.begin() + static_cast<std::ptrdiff_t>(i)); }   // out of the list (still owned)

    // Newest first.
    size_t Size() const { return m_list.size(); }
    CollisionEvent* operator[](size_t i) const { return m_list[i]; }
    void Clear();

private:
    std::vector<std::unique_ptr<CollisionEvent>> m_owned;
    std::vector<CollisionEvent*> m_list;   // [0] the newest
};

} // namespace phys::native
