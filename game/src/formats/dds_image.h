#pragma once
// A .dds's top level to RGBA8 on the CPU (wrally::formats) - for the UI's pictures (a stage's splash), which go to the
// renderer as UI textures (createTextureRgba) and so outlive the scenes. DXT1 (its punch-through alpha), DXT3, DXT5 and
// uncompressed 32-bit (A8R8G8B8 / X8R8G8B8); anything else is not read.

#include <cstdint>
#include <string>
#include <vector>

namespace wrally::formats {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4, rows top down
};

// "" or what is wrong.
std::string DecodeDds(const std::vector<uint8_t>& dds, Image& out);

} // namespace wrally::formats
