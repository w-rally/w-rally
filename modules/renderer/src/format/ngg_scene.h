#pragma once
// An .ngg in memory - ngg_format.h's chunks as vectors - with its writer and a strict reader. This is the format's
// library beside the renderer's own loader (core/scene.cpp), so a format change touches the reader and the writer in
// one place (docs/design.md decision 2). The writer is what W-Rally's legacy importer (.lbs -> .ngg) produces through;
// the reader is for tools and tests (the renderer keeps its own loader, which the round-trip test runs too).
//
// Write always writes kFormatCurrent (v3 materials), and Read accepts only kFormatCurrent, as the renderer does.
//
// The plants (NGGG, NGGP) are carried as raw payloads: read and written as they are, not interpreted.

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "ngg_format.h"

namespace ngg {

struct Texture {
    std::string name;
    std::vector<uint8_t> dds;
};

struct SceneData {
    uint32_t version = kFormatCurrent;   // as read (Write writes kFormatCurrent whatever this says)
    uint32_t flags = 0;                  // SceneHeader.flags
    float bakedScale = 1.0f;
    float bboxMin[3] = {}, bboxMax[3] = {};   // Write computes them from the vertices
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Material> materials;
    std::vector<Object> objects;
    std::vector<Texture> textures;
    bool hasSpawn = false;
    SpawnPoint spawn{};
    std::vector<int32_t> objectKeys;     // parallel to objects; empty: no NGGK
    std::vector<WeatherRecord> weather;  // the first the default; empty: no NGGW
    std::vector<uint8_t> tagNames, plants;   // NGGG, NGGP payloads as they are (empty: none)
    std::vector<std::string> variants;       // NGGR: the stage variants, the main one first; empty: none
    std::vector<uint8_t> variantHasSpawn;    // parallel to variants: 1 when the variant has its own car start
    std::vector<SpawnPoint> variantSpawns;   // parallel to variants (zero where it has none)
    std::vector<std::array<float, 3>> variantOffsets;   // parallel to variants: its world relative to the file's
    std::vector<uint32_t> variantMasks;      // NGGQ, parallel to objects; empty: every object in every variant
    std::vector<std::pair<uint32_t, std::string>> variantAliases;   // NGGA: (variant index, a name it answers to)
};

// The scene as .ngg bytes. Returns "" or what is inconsistent (an index past the vertices, an object past the
// indices or with no material, keys not one per object, a texture slot past the textures), and then writes nothing.
std::string Write(const SceneData& s, std::vector<uint8_t>& out);

// The .ngg bytes into `out`. Returns "" or why they are not an .ngg this library reads.
std::string Read(const void* bytes, size_t n, SceneData& out);

} // namespace ngg
