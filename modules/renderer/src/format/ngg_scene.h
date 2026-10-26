#pragma once
// An .ngg in memory - ngg_format.h's chunks as vectors - with its writer and a strict reader. This is the format's
// library beside the renderer's own loader (core/scene.cpp), so a format change touches the reader and the writer in
// one place (docs/design.md decision 2). The writer is what WRally's legacy importer (.lbs -> .ngg) produces through;
// the reader is for tools and tests (the renderer keeps its own loader, which the round-trip test runs too).
//
// Write always writes kFormatCurrent (v3 materials). Read accepts v3 and the v2 transition, upgrading v2 materials as
// the renderer does (Material = DefaultMaterial with v2's two textures and flags).
//
// The plants (NGGG, NGGP) are carried as raw payloads: read and written as they are, not interpreted.

#include <cstddef>
#include <cstdint>
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
};

// The scene as .ngg bytes. Returns "" or what is inconsistent (an index past the vertices, an object past the
// indices or with no material, keys not one per object, a texture slot past the textures), and then writes nothing.
std::string Write(const SceneData& s, std::vector<uint8_t>& out);

// The .ngg bytes into `out`. Returns "" or why they are not an .ngg this library reads.
std::string Read(const void* bytes, size_t n, SceneData& out);

} // namespace ngg
