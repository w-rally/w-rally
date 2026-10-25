#pragma once
// The native physics' flag words: the values the game's own data and the physics config carry (physics.lsp's Flags and
// debug flags, the stage records', the materials', the brake walls'), and the ones the physics sets on its objects and
// events. The names are the game's where it has them (the config's keys, the original sources').
#include <cstdint>

namespace phys::native {

// An entity's kind bits (Entity::flags: a class has its own and its bases').
constexpr uint32_t kEntity = 0x1;        // CEntity
constexpr uint32_t kBody = 0x2;          // CBody
constexpr uint32_t kSolid = 0x4;         // CSolid
constexpr uint32_t kCar = 0x8;           // CCar - frozen at rest (SolidStep), its distance on the drive line kept
constexpr uint32_t kCarBody = 0x10;      // CCarBody
constexpr uint32_t kWheel = 0x20;        // CWheel
constexpr uint32_t kBrakeWall = 0x40;    // CBrakeWall (no mesh contacts: the world's visits skip it)
constexpr uint32_t kGround = 0x80;       // CGround
constexpr uint32_t kWater = 0x100;       // CCollisionMeshWater (a .col water or river quad)
constexpr uint32_t kOpenMesh = 0x200;    // CCollisionMeshOpen (the open meshes: the water's base)
constexpr uint32_t kHinged = 0x400;      // CHingedRigidBody (the doors, the bonnet, the exhaust)
constexpr uint32_t kMounted = 0x800;     // CMountedRigidBody (the bumpers, the wing)

// The physics config's Flags (physics.lsp).
constexpr uint32_t kFaceColl = 1u << 0;          // FACE_COLL
constexpr uint32_t kEdgeColl = 1u << 1;          // EDGE_COLL
constexpr uint32_t kFacePenetr = 1u << 2;        // FACE_PENETR
constexpr uint32_t kEdgePenetr = 1u << 3;        // EDGE_PENETR
constexpr uint32_t kCheckColl = 1u << 4;         // CHECK_COLL
constexpr uint32_t kCheckPenetr = 1u << 5;       // CHECK_PENETR
constexpr uint32_t kStuffRotation = 1u << 10;    // STUFF_ROTATION
constexpr uint32_t kSubStepping = 1u << 12;      // SUB_STEPPING
constexpr uint32_t kCheckWheelColl = 1u << 16;   // CHECK_WHEEL_COLL
constexpr uint32_t kForceMaterial = 1u << 18;    // FORCE_MATERIAL
constexpr uint32_t kEnableDamage = 1u << 19;     // ENABLE_DAMAGE (also the drive's gear limits and wear)

// The physics config's debug flags (values kept for display).
constexpr uint32_t kRenderSuspensionGeometry = 1u << 2;   // RENDER_SUSPENSION_GEOMETRY
constexpr uint32_t kRenderGgDiag = 1u << 4;               // RENDER_GG_DIAG
constexpr uint32_t kRenderMaterial = 1u << 5;             // RENDER_MATERIAL
constexpr uint32_t kRenderEngineTorque = 1u << 9;         // RENDER_ENGINE_TORQUE

// A collision event's flags.
constexpr uint32_t kEventBodyMoves = 0x800;           // the body has a mass
constexpr uint32_t kEventOtherMoves = 0x1000;         // the other is a solid with a mass
constexpr uint32_t kEventBodyPassThrough = 0x2000;    // the body's material is kMaterialPassThrough: no response, dropped
constexpr uint32_t kEventOtherPassThrough = 0x4000;   // the other's

// A material's flags.
constexpr uint32_t kMaterialDefined = 0x1;       // set by the material's list (else material 0)
constexpr uint32_t kMaterialPassThrough = 0x8;   // no collision response: an event with it is dropped

// A collision mesh's flags, and its record's.
constexpr uint32_t kMeshRecordFlag4 = 1;   // the record's flags bit 2
constexpr uint32_t kMeshNoEdges = 2;       // a face of material 0xff: no edge contacts
constexpr uint32_t kMeshSphere = 4;        // the shape is a sphere {centre, radius} (the record's 0x10000)
constexpr uint32_t kMeshBox = 8;           // the shape is a box {centre, half extents, axes} (0x20000)
constexpr uint32_t kRecordNoPhysics = 0x2;      // not for the physics
constexpr uint32_t kRecordFlag4 = 0x4;          // the mesh's kMeshRecordFlag4
constexpr uint32_t kRecordSphere = 0x10000;     // a shape: a sphere {centre, radius}
constexpr uint32_t kRecordBox = 0x20000;        // a box {centre, half extents, axes}

// A brake wall's entries and its hits.
constexpr uint32_t kWallSegmentMask = 0x3fff;   // the entry's first point: a quad of four from it
constexpr uint32_t kWallPassThrough = 0x4000;   // no contact: only signalled or skipped
constexpr uint32_t kWallHitFlag = 0x1;          // the quad's wall flag
constexpr uint32_t kWallHitAfterSegment = 0x2;  // a segment query hit the wall since the last point query
constexpr uint32_t kWallHitPassThrough = 0x4;   // a pass-through quad: signalled, no contact

} // namespace phys::native
