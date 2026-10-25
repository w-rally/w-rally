#pragma once
// The native physics' records whose layout someone else shares: the drive line's samples (the .trk's block 3,20, as the
// load lays them out), the tyre's records for the game's effects (WheelRecord, CSigTireParticles), the suspension's
// working frame, the damping box and its contacts, and the collision meshes' cheap shapes (the stage records' box and
// sphere blocks). They keep the game's layouts (the offsets in the comments; the words not understood `unknownXX`), so
// the checks can compare them with the classic physics' word by word.
#include <cstdint>

#include "num/math.h"

namespace phys::native {

using num::Vec4;

// A drive line sample, 0x30 bytes.
struct DriveLineSample {
    Vec4 position;                           // +0x0 (w 0)
    Vec4 direction;                          // +0x10 the tangent there (w 0; DrivelineDirection lerps two, HermiteKey4 the curve's)
    float distance;                          // +0x20 along the line from its start (DriveLineDistance)
    float unknown24;                         // +0x24 the block's eighth float
    uint32_t unknown028[2];
};

// The tyre's record for the game (WheelRecord; the physics does not read it), 0x60 bytes.
struct TyreRecord {
    uint32_t debugId;                        // +0x0 the def's DebugId (the tyre's number)
    float compression;                       // +0x4 the deepest the tread was pressed in (a running maximum, per the radius)
    float work;                              // +0x8 the slip, brake and drive work's sum, * 1000
    float slide;                             // +0xc |velocityForward - slipVelocity|: the tread sliding
    float squeal;                            // +0x10 the larger slip curve * 0.75 + 0.5
    float load;                              // +0x14
    float unknown018;                        // +0x18 320
    float spinRate;                          // +0x1c
    float slipRatio;                         // +0x20 |slipRatio|
    float slipSpeed;                         // +0x24
    uint32_t unknown028[2];
    Vec4 contactPoint;                       // +0x30
    Vec4 contactVelocity;                    // +0x40
    uint32_t surface[2];                     // +0x50 the wheel's two surface entries
    float surfaceBlend;                      // +0x58
    uint32_t inContact;                      // +0x5c 1 in contact
};

// The CSigTireParticles record (the wheel's step sends it: dust, gravel, smoke), 0xb0 bytes.
struct TireParticles {
    uint32_t slot;                           // +0x0 the wheel's
    uint32_t unknown004[3];
    Vec4 patchEnds[2];                       // +0x10 the patch's two ends (along its slide)
    Vec4 hub;                                // +0x30 in the car's frame
    Vec4 groundNormal;                       // +0x40
    Vec4 force;                              // +0x50 the tread's push on the ground (what is thrown)
    uint32_t unknown060[8];
    float wear[2];                           // +0x80 the wear rate, clamped to 0..1, when the ground doesn't yield (else 0)
    float work;                              // +0x88 the drive work * 1000
    uint32_t unknown08c;
    float groundDepth;                       // +0x90 the contact solver's ground depth
    float slipCurve;                         // +0x94
    float quadVolume;                        // +0x98 (contactLength |vLat| + |vFwd| treadWidth) quadDepth: 0 off a quad
    uint32_t unknown09c;
    uint32_t surface[2];                     // +0xa0 the wheel's two surface entries
    float surfaceBlend;                      // +0xa8
    uint32_t valid;                          // +0xac 1
};

// WheelSuspensionUpdate's frame, 0x300 bytes.
struct SuspensionFrame {
    uint32_t unknown000[5];
    float jointT;                            // +0x14 the top mount's projection on the joint axis (from the joint's mount)
    float wishboneSign;                      // +0x18 the def's
    uint32_t unknown01c[4];
    float wishboneLength;                    // +0x2c
    Vec4 knuckleY;                           // +0x30 the knuckle's rows (N): normalised strutAxis x knuckleX
    Vec4 wishboneEnd;                        // +0x40 the wishbone's outer joint: the joint's mount + wishbone
    Vec4 knuckleX;                           // +0x50
    Vec4 strutAxis;                          // +0x60 from the wishbone end to the top mount, unit
    uint32_t unknown070[4];
    Vec4 strutDim;                           // +0x80 (strutDimX, 0, strutDimY + a1) / strutLength
    Vec4 rodOffset;                          // +0x90 the def's strut rod mount less its part along strutDim
    Vec4 up;                                 // +0xa0 (0, 0, 1), then made square to the axle
    Vec4 steeringPoint;                      // +0xb0 the def's steering mount + its axis * the steering angle
    Vec4 rodMount;                           // +0xc0 the def's strut rod mount through the knuckle's rows
    Vec4 topMount;                           // +0xd0 the suspension's top mount
    Vec4 wishbone;                           // +0xe0 its direction, then * its length
    Vec4 rodFoot;                            // +0xf0 wishboneEnd + strutAxis * the rod mount's height
    Vec4 knuckleZ;                           // +0x100
    Vec4 rodDir;                             // +0x110 rodOffset, unit
    Vec4 side;                               // +0x120 up x the axle
    Vec4 steeringRod;                        // +0x130 steeringPoint - wishboneEnd - rodMount
    Vec4 jointToFoot;                        // +0x140 the joint's mount to the top mount's foot on its axis
    Vec4 rodSpan;                            // +0x150 rodReach - rodFoot
    Vec4 steeringRodDir;                     // +0x160 steeringRod / the rod's length
    Vec4 cross;                              // +0x170 (1 - strutDim.x^2, 0, -strutDim.x strutDim.z)
    Vec4 jointFoot;                          // +0x180 the top mount's foot on the joint axis
    Vec4 driveAxle;                          // +0x190 the drive axle's mount - its joint
    Vec4 driveAxleMount;                     // +0x1a0 the hub + its axis * the def's drive axle mount offset
    Vec4 crossDir;                           // +0x1b0 cross, unit
    Vec4 dimCrossRod;                        // +0x1c0 strutDim x rodDir
    Vec4 strutVector;                        // +0x1d0 topMount - wishboneEnd
    Vec4 rodReach;                           // +0x1e0 steeringPoint projected on the strut
    Vec4 strutTop;                           // +0x1f0 rodMount + wishboneEnd
    Vec4 armCross;                           // +0x200 rodMount x steeringRodDir
    Vec4 dimScaled;                          // +0x210 strutDim * strutDim.x
    Vec4 rodSpanDir;                         // +0x220 rodSpan, unit
    Vec4 jointDir;                           // +0x230 jointToFoot, unit
    Vec4 jointNormal;                        // +0x240 the joint axis x jointDir
    Vec4 strutCrossSpan;                     // +0x250 strutAxis x rodSpanDir
    Vec4 wishboneNormal;                     // +0x260 the joint axis x the wishbone (not read)
    uint32_t unknown270[5];
    float knuckleRows[9];                    // +0x284 N: knuckleX, knuckleY, knuckleZ (3 floats each)
    float axleRows[9];                       // +0x2a8 M: the axle, side, up
    float hubMatrix[9];                      // +0x2cc M N: the hub's rows (row 0 the hub axis)
    uint32_t unknown2f0[4];
};

// A damping contact (BoxBoxContact / BoxSphereContact), 0x40 bytes.
struct BoxContact {
    Vec4 point;                              // +0x0 in the world
    Vec4 normal;                             // +0x10 from the other shape's centre
    Vec4 otherCentre;                        // +0x20
    float depth;                             // +0x30 how deep (0..1; SolidDampingContacts' push and damping)
    uint32_t material;                       // +0x34 the other mesh's
    float volume;                            // +0x38 the overlap's (box against box: ex ez ey 4 depth; a sphere: 0)
    uint32_t unknown03c;
};

// The damping box (the classic *0x7c7040): an oriented box in a solid's frame and the world, its contacts.
struct ContactBox {
    Vec4 localCentre;                        // +0x0 in the solid's frame
    Vec4 centre;                             // +0x10 in the world
    Vec4 halfExtent;                         // +0x20
    Vec4 axes[3];                            // +0x30 in the world (unit rows)
    Vec4 localAxes[3];                       // +0x60
    BoxContact contacts[8];                  // +0x90
};

// A collision mesh's box shape (CollisionMesh::shape without kMeshSphere).
struct ShapeBox {
    Vec4 centre;                             // +0x0
    Vec4 halfExtent;                         // +0x10 (a mesh's shape: its scales of the axes; the tests' copy: the lengths)
    Vec4 axes[3];                            // +0x20
};

// A collision mesh's sphere shape (kMeshSphere).
struct ShapeSphere {
    Vec4 centre;                             // +0x0
    float radius;                            // +0x10
    uint32_t unknown014[3];
};

} // namespace phys::native
