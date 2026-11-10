// ---------------------------------------------------------------------------
// The shaders compiled at build time (ngg-shaderc, src/shaderc): the table its generated ngg_shaders.cpp fills, one
// entry per line of shaders/shaders.txt, sorted by key for a binary search (shaders.cpp).
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace ngg {
namespace core {
namespace embedded {

struct Shader
{
    const char*          key;     // shaders.txt's line, one space apart: "scene.hlsl PSMain ps_6_0 -HV 2018 -DBLEND=1"
    const unsigned char* data;    // DXIL (signed) or DXBC, 16-byte aligned
    uint32_t             size;
};

extern const Shader   kShaders[];
extern const size_t   kShaderCount;
// FNV-1a 64 over every key and its bytes: the pipeline cache's key (pipeline_cache.h) - any shader changed, any added
// or removed, and the cached pipelines are thrown away.
extern const uint64_t kShaderSetHash;

} // namespace embedded
} // namespace core
} // namespace ngg
