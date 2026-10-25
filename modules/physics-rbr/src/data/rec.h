#pragma once
// Our own physics recording (M4, `.phr`): what drives the physics after the stage's load, in step with it - every
// StepForward call, every car control record (CSigCarControl, what CarControlInput reads) - with the car's pose after
// each step as the check. Written by the probe from the game; phys-run replays it (PHYS_RUN_REC) bit-exact.
//
// Little-endian. Header: "PHR1", u32 version (1), u32 stage, u32 tyres[4] (the tyre types the setup's sender stores),
// u32 settings (bit 0 given, 1 outside assistance allowed, 2 the assistance flag, 3 the game-mode test: the game's
// options as the physics asks them; bit 4 the damage level given, bits 8..15 the level - the car's damage handler's
// +0x3df0, as the option sets it, 0x4ebfb0), u32 reserved. Then records, each a type byte and a fixed payload:
//   'S'  u32 tick, u32 steps        StepForward(steps) at world tick `tick`
//   'C'  u32 control[8]             a control record (+0 .. +0x1c: steering, throttle, brake, handbrake, clutch, flags,
//                                   gear, +0x1c) to CarControlInput on the car
//   'P'  u32 pose[13]               the car after the step before: quaternion +0x100, position +0x140, +0x190, velocity
//                                   +0x1c0 (xyz each)
//   'G'  u8 n, char name[n], u32 words[16]   another physics signal after the load (kept, not replayed)
//   'U'  u32 setup, u32 tyre, u8 setupBytes[0x1700], u8 tyreBytes[0x6ac]   the car's setup as the game sent it: its
//                                   CCompleteCarSetup ([0x7ecd00], the player's choice through the setup screen, the
//                                   driving aids in) and the tyre type the setup's tyres name (a CTyre::CTyreTypes of
//                                   the setup options), with their game addresses (the setup record points into them)
//   'A'  u32 n, u32 position[n][3]  the stage's animation objects (spectators, marshals, crews: the .lbs script objects
//                                   the game picked - one variant of each random group, data/lbs.h) as it sent them, in
//                                   its order: each one's position (x, y, z); the record is built from the .lbs entry
//                                   there
//   'K'  u32 w[13], u32 nv, u32 verts[nv][4], u32 nf, u8 faces[nf][7], u8 n, char name[n], u32 size[3], the tables
//                                   one of the car's collision meshes (CSigCollisionMesh, slots 1..8: MrCarCollision and
//                                   the c_e_* parts) as the game sent it - the renderer (HedgeHog3D) makes them from the
//                                   car's model: the record's first 13 words, the vertices (vec4), the faces, the name
//                                   and the surface tables (the body's: uvs +0x1c, the grids +0x24 / +0x28; size 0 none)
//   'E'                             the end (missing when the game ended first: the file's end is the end)

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

constexpr uint32_t kRecVersion = 1;

struct RecEvent {
    char type;            // 'S', 'C', 'P' or 'G'
    uint32_t tick = 0, steps = 0;
    uint32_t words[16] = {};   // 'C': 8, 'P': 13, 'G': 16
    std::string name;          // 'G'
};

struct RecMesh {
    uint32_t w[13] = {};
    std::vector<uint32_t> verts;   // 4 words a vertex
    std::vector<uint8_t> faces;    // 7 bytes a face
    std::string name;
    std::vector<uint8_t> tables[3];
};

constexpr uint32_t kRecSetupSize = 0x1700, kRecTyreTypeSize = 0x6ac;

struct Recording {
    uint32_t stage = 0;
    uint32_t tyres[4] = {};
    uint32_t settings = 0;
    std::vector<RecEvent> events;
    // 'U' (empty: none)
    uint32_t setupGame = 0, tyreGame = 0;
    std::vector<uint8_t> setup, tyreType;
    // 'A' (empty: none)
    std::vector<std::array<uint32_t, 3>> anims;
    // 'K', in order
    std::vector<RecMesh> carMeshes;
};

// A COLMESH line of the probe's load log ("COLMESH <rec> <13 words> | v n ... | f n <hex> | "name" [| tables ...]")
// into a mesh. Returns false if it is not one.
bool ParseColMeshLine(const std::string& line, RecMesh& m);

// Throws std::runtime_error on a bad file.
Recording ReadRecording(const std::string& path);
void WriteRecording(const std::string& path, const Recording& r);

} // namespace phys::data
