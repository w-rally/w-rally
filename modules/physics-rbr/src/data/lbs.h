#pragma once
// RBR stage .lbs reader (M4): the script objects - block (2,14), the stage's Lua-scripted animation objects (spectators,
// marshals, the start and finish crews, birds), and block (2,15), the random groups some of them belong to. Wallaby
// names the blocks (LbsFile.DescribeBlock); the layout is from the file, checked on stage 41: every animation object
// the game sends the physics (a CSigStuff of kind 9, the animation object's mesh slot) is at one entry's position.
//
// (2,14): u32 n, then n x {cstr name, u32 id, u32 variant, u32 group, cstr loadScript, cstr runScript, f32 position[3]
// (x, z, y: the renderer's axes), f32 rotation[3], f32 scale[3], f32 boxCentre[3], f32 boxHalfSize[3], u32 colour}.
// (2,15): u32 n, then n x {cstr name, u32 id, u32 a, u32 b} (RandomGroupNN: a 1, b the variant count).
// (2,11), the car info: f32 m[12] (a 3x4 rotation, rows), f32 position[3] (x, z, y), f32 1, f32 position[3] again,
// f32 angles[3] (the second the heading: what the renderer gives the car's start, CPhysics' car record 0x46b160).
// (2,13), the drive line: u32 n, n x {f32 position[3] (x, y, z: the physics' axes), f32 v[3], f32 distance (from the
// start, along the line)}.
// A group's entries carry its id and their variant; the game picks one variant per group at the load (at random: a run
// input, recorded in the .phr's 'A').

#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

struct LbsScriptObject {
    std::string name, loadScript, runScript;
    uint32_t id = 0, variant = 0, group = 0;
    float position[3] = {};   // the file's order: x, z, y
    float rotation[3] = {}, scale[3] = {}, boxCentre[3] = {}, boxHalfSize[3] = {};
    uint32_t colour = 0;
};

struct LbsScriptGroup {
    std::string name;
    uint32_t id = 0, a = 0, b = 0;
};

struct LbsScripts {
    std::vector<LbsScriptObject> objects;
    std::vector<LbsScriptGroup> groups;
    // (2,11)
    bool hasCar = false;
    float carMatrix[12] = {}, carPosition[3] = {}, carAngles[3] = {};   // carPosition: x, z, y
    // (2,13)
    struct DrivePoint {
        float position[3], distance;
    };
    std::vector<DrivePoint> driveLine;
};

// Throws std::runtime_error on a bad file. A stage with no script blocks gives empty lists.
LbsScripts ReadLbsScripts(const std::string& path);

} // namespace phys::data
