#pragma once
// An .ngg in memory - ngg_format.h's chunks as vectors - with its writer and a strict reader. This is the format's
// library beside the renderer's own loader (core/scene.cpp), so a format change touches the reader and the writer in
// one place (docs/design.md decision 2). The writer is what W-Rally's legacy importer (.lbs -> .ngg) produces through;
// the reader is for tools and tests (the renderer keeps its own loader, which the round-trip test runs too).
//
// Write always writes kFormatCurrent (v3 materials), and Read accepts only kFormatCurrent, as the renderer does.
//
// The plants (NGGG, NGGP) are carried as raw payloads: read and written as they are, not interpreted.
// The instanced models (NGGD) and their placements (NGGE) are structured, and checked whole on both sides.

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

// NGGD in memory: the arrays as the chunk lays them out (ngg_format.h).
struct ModelLibrary {
    std::vector<ModelRecord> models;
    std::vector<ModelLod> lods;
    std::vector<ModelSubmesh> submeshes;
    std::vector<Material> materials;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    bool empty() const { return models.empty(); }
};

// "" or the first thing in the library that does not hold together: a model's LODs or a LOD's submeshes past their
// arrays, a submesh past the vertices or indices, an index past its submesh's vertices, a material slot past
// `textureCount`, LOD distances not increasing, a billboard as the ray tracer's LOD, a name not terminated; and a
// ray-only LOD (kModelLodRayOnly) that is not the model's last, a second one, one rtLod does not name, one that is
// also a billboard or has a distance, one with no drawn LOD before it. Read takes NGGD versions 1 and 2 (a version-1
// chunk with a ray-only LOD is refused); Write writes version 2.
std::string CheckModels(const ModelLibrary& m, size_t textureCount);
// "" or the first placement naming a model past the library or a plant past `plantCount` (pass kNoPlant to skip
// that check: no plant table to check against).
std::string CheckPlacements(const std::vector<Placement>& p, size_t modelCount, uint32_t plantCount);
// The NGGP payload's plant count (its header), 0 for none.
uint32_t PlantCount(const std::vector<uint8_t>& plants);

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
    ModelLibrary models;                     // NGGD; empty: none
    std::vector<Placement> placements;       // NGGE; empty: none
};

// The scene as .ngg bytes. Returns "" or what is inconsistent (an index past the vertices, an object past the
// indices or with no material, keys not one per object, a texture slot past the textures, a model library or a
// placement that does not hold together - CheckModels, CheckPlacements), and then writes nothing.
std::string Write(const SceneData& s, std::vector<uint8_t>& out);

// The .ngg bytes into `out`. Returns "" or why they are not an .ngg this library reads.
std::string Read(const void* bytes, size_t n, SceneData& out);

} // namespace ngg
