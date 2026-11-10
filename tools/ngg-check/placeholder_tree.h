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

// Adds the model (its textures after the file's), and a placement replacing each plant tagged tree-card. "" or why
// not (no plant table, no tree-card tag, a library already there).
std::string AddPlaceholderTrees(ngg::SceneData& s, PlaceholderReport& report, uint32_t rtLod = 1);
