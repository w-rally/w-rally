#pragma once
// W-Rally's own legacy stage importer (wrally::assets, docs/design.md 6.3 and decision 2): an RBR stage's .lbs and its
// textures (Maps/<stage>_textures.rbz beside it) into an .ngg in memory, for the renderer to load - so an RBR stage
// draws with no converted .ngg. Simple and automatic, needing nobody: Wallaby stays the tool for a hand-finished .ngg,
// which wins whenever there is one.
//
// What it makes (step 1 of phase 8):
//   - layers as the .ngg has them: 0 the ground, 1 vegetation, 2 scenery, 3 movable objects (each placement of a
//     movable object its own geometry, keyed in NGGK by its instance id - the physics' solid, so it follows it)
//   - the ground's two-texture blend (detail by uv1, weighted by vertex alpha); RBR's hidden ground chunks left out
//   - vegetation alpha-tested and two-sided, each connected piece of its meshes a plant in the plant table (NGGP, no
//     tags: topology only), which the renderer gives volume normals; scenery and movable objects alpha-tested only
//     where their texture has alpha (an opaque texture's vertex alpha is not coverage), two-sided always
//   - default PBR materials (DefaultMaterial): no PBR data in an RBR stage
//   - RBR's baked lighting NOT used for light: it stays in the vertex colour, as the renderer's A/B reference (its
//     light mode 0), and the default dynamic mode lights the stage from the sun and the sky alone; normals where the
//     file has none are the faces' (area-weighted)
//   - the weather presets (NGGW): every TrackSettings.ini section of the stage's map, translated, the stage's own time
//     of day the default (assets/legacy_weather.h)
// Not yet: welded normals, the start point (step 2). No cache: an import takes ~0.5 s, and the stage server's
// hand-finished .ngg files are to replace most imports (docs/design.md phase 8 step 3).

#include <cstdint>
#include <string>
#include <vector>

namespace wrally::assets {

struct LegacyImport {
    std::vector<uint8_t> ngg;
    std::string log;                    // what was made, a line each (counts, textures missing, blocks left out)
};

// `lbsPath`: Maps/<stage>.lbs. `surface`: which of the texture archive's surface folders first (0 dry, 1 damp, 2 wet).
// Returns "" or why there is no stage to draw (the file unreadable, a block that does not add up, no geometry).
std::string ImportLegacyStage(const std::string& lbsPath, int surface, LegacyImport& out);

} // namespace wrally::assets
