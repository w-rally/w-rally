#pragma once
// RBR stage .fnc - the roadside fences (wrally::formats, docs/design.md 6.2): the tape and net fences the game builds
// itself from a list of post points (StickSystem's CFenceHandler; its geometry is generated, nothing of it is stored),
// and RBR's Textures/fence.dat, the archive of their textures.
//
// .fnc: i32 version (2); u32 groups; per group: u32 items, i32 x2 (0, 4 in every file seen), i32 tile type, i32 post
// type, f32 box[6] (centre, half size); per item: f32 point[3] (the post's foot), f32 centre[3], f32 half size[3] (the
// box from this post to the next: the physics' fence dummy; a tiny one on a group's last post), u32 colour (a grey: the
// post's baked light). Then u32 types, u32 32, and per type a 32-byte record, its name a C string at the start
// ("Mb_fence_tile", "Mb_fencestolpe": fence.dat's textures without ".dds", in any case).
//
// fence.dat ("CHELALIC"): u32 at 12 the data's start; a directory of {u32 offset, u32 size, u32 x2, u32 name length,
// name} records (offsets from the data's start), each texture a whole .dds.
//
// Both layouts as Wallaby reads and writes them (the .fnc) and as measured on RBR's own files (fence.dat). Vectors are
// as the files have them: the renderer's axes (x, y up, z).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wrally::formats {

struct FncItem {
    float point[3];
    float centre[3], halfSize[3];
    uint32_t colour;
};

struct FncGroup {
    int32_t words[2];
    int32_t tile, post;   // indices into Fnc::types
    float box[6];
    std::vector<FncItem> items;
};

struct Fnc {
    std::vector<FncGroup> groups;
    std::vector<std::string> types;
};

// "" or what is wrong. A file of another version reads as no fences.
std::string ReadFnc(const std::vector<uint8_t>& data, Fnc& out);

// fence.dat's textures by name ("mb_fence_tile.dds"), each its .dds bytes. "" or what is wrong.
std::string ReadFenceTextures(const std::vector<uint8_t>& data, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out);

} // namespace wrally::formats
