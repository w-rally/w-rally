#pragma once
// A placeholder tree model generated in code, placed in place of every "tree-card" plant (placeholder_tree.cpp).

#include <cstdint>
#include <string>
#include <vector>

#include "ngg_scene.h"

struct PlaceholderReport {
    uint32_t placements = 0, skipped = 0;
    std::vector<uint32_t> trianglesPerLod;
    float minHeight = 0.0f, maxHeight = 0.0f;
};

// rtLod: 0 or 1 (a drawn LOD ray traced), or kPlaceholderRtShell - a ray-tracing-only LOD (ngg_format.h
// kModelLodRayOnly) after the billboard: a closed low-poly hull round the needle tiers with alpha holes, and the trunk.
constexpr uint32_t kPlaceholderRtShell = 0xFFFFFFFFu;

// Adds the model (its textures after the file's), and a placement replacing each plant tagged tree-card. "" or why
// not (no plant table, no tree-card tag, a library already there).
// wind: the model sways (ngg_format.h kModelWind, NGGD version 3): its vertices' uv1 carry the weights baked as Wallaby's
// generator bakes them (tools/vegetation README, "Wind"), windBend 0.3 m (its spruce preset). Without it uv1 is uv0, as
// before (a model's uv1 is never read without the flag).
std::string AddPlaceholderTrees(ngg::SceneData& s, PlaceholderReport& report, uint32_t rtLod = 1, bool wind = false);
