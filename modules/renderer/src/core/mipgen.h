// ---------------------------------------------------------------------------
// Mip-chain generation for block-compressed DDS textures that shipped without
// one (design §2.7).
//
// Community content routinely ships single-level textures: track-108 has 76 of
// 88, including its 2048 atlas and both 3200x800 backdrops. A single-level
// texture cannot be minified, so it aliases at distance, anisotropic filtering
// has nothing to filter, and TAA spends its budget fighting shimmer the asset
// created. The cure belongs in the converter; this is the RENDERER FALLBACK in
// the same pattern as single-stage detection and normal welding: it measures
// first (a file with mips is left alone) and stands down once the converter
// does the job.
//
// What it does, and why each part:
//   - MIP 0 IS NEVER TOUCHED: its original blocks are copied verbatim, so the
//     texture at full resolution is byte-identical to what shipped. Only the
//     levels that did not exist are created.
//   - Each level is built from the previous one IN LINEAR LIGHT: a colour
//     texture's sRGB values are decoded, averaged and re-encoded. Averaging the
//     encoded values darkens every minified edge.
//   - ALPHA-TESTED textures keep their coverage: a box filter averages alpha
//     towards the middle, so at the alpha-test cutoff a tree loses leaves at
//     every level until it vanishes at distance - a worse artefact than the
//     shimmer this replaces. Each level's alpha is rescaled so the fraction of
//     texels passing the cutoff matches mip 0 (Castano 2010). Colour is
//     alpha-weighted, so the transparent texels' colour (often black) does not
//     bleed into the leaves.
//   - Levels 1+ are re-encoded in the SOURCE format (DXT1/3/5), so the chain
//     costs a third more than the texture rather than 4-8x as uncompressed.
//
// Scope is what RBR ships: DXT1, DXT3, DXT5. Anything else is left alone.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace ngg {
namespace core {

struct MipGenOptions
{
    bool  srgb = true;              // colour data: average in linear light
    bool  alphaTested = false;      // preserve alpha-test coverage per level
    float alphaCutoff = 170.0f / 255.0f;
    bool  preserveCoverage = true;  // false = plain box filter (the A/B reference)
};

struct MipGenResult
{
    uint32_t levels = 0;            // in the output, including mip 0
    float    meanAbsError = 0.0f;   // re-encoding error over levels 1+, 0..255 per channel
    // Alpha-tested only: the fraction of texels passing the cutoff at mip 0,
    // and at level 4 (or the last) with a plain box filter vs as written.
    float    coverage0 = -1.0f;
    float    coverageBox = -1.0f;
    float    coverageKept = -1.0f;
    uint32_t coverageLevel = 0;
};

// Returns false (and leaves `out` alone) for anything it does not handle: a DDS
// that already has mips, a format outside DXT1/3/5, a DX10 header, a malformed
// file. The caller then uses the original bytes, exactly as before.
bool GenerateDdsMips(const uint8_t* dds, size_t bytes, const MipGenOptions& options,
                     std::vector<uint8_t>& out, MipGenResult* result = nullptr);

// The top level's ALPHA, one byte a texel, rows top down - for measuring what an
// alpha test would cut (Scene::MeasureSolidCards). DXT1 (its punch-through),
// DXT3, DXT5 and uncompressed 32-bit with an alpha mask; 32-bit without one is
// opaque (255). Returns false for anything else or a malformed file.
bool DecodeDdsAlpha(const uint8_t* dds, size_t bytes, uint32_t& width, uint32_t& height,
                    std::vector<uint8_t>& alpha);

} // namespace core
} // namespace ngg
