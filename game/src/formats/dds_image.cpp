#include "formats/dds_image.h"

#include <cstring>

namespace wrally::formats {

namespace {

uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// A 565 colour to 8 bits a channel.
void Rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = uint8_t((c >> 11 & 31) * 255 / 31), out[1] = uint8_t((c >> 5 & 63) * 255 / 63), out[2] = uint8_t((c & 31) * 255 / 31);
}

// A BC1 colour block's 16 texels into `px` (4 bytes each, row by row); `opaque`: the 4-colour mode always (BC2/3).
void ColourBlock(const uint8_t* b, bool opaque, uint8_t px[16][4])
{
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t c[4][4] = {};
    Rgb565(c0, c[0]), Rgb565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = c[3][3] = 255;
    for (int i = 0; i < 3; ++i) {
        if (opaque || c0 > c1) {
            c[2][i] = uint8_t((2 * c[0][i] + c[1][i]) / 3), c[3][i] = uint8_t((c[0][i] + 2 * c[1][i]) / 3);
        } else {
            c[2][i] = uint8_t((c[0][i] + c[1][i]) / 2), c[3][i] = 0;
        }
    }
    if (!opaque && c0 <= c1) c[3][3] = 0;
    const uint32_t bits = U32(b + 4);
    for (int t = 0; t < 16; ++t) std::memcpy(px[t], c[bits >> (2 * t) & 3], 4);
}

} // namespace

std::string DecodeDds(const std::vector<uint8_t>& dds, Image& out)
{
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4)) return "not a .dds";
    const uint8_t* h = dds.data() + 4;
    const int height = int(U32(h + 8)), width = int(U32(h + 12));
    const uint32_t pfFlags = U32(h + 76), fourCC = U32(h + 80), bits = U32(h + 84);
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return "a bad size";
    const uint8_t* data = dds.data() + 128;
    const size_t left = dds.size() - 128;
    out.width = width, out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 0);

    auto is = [&](const char* cc) { return fourCC == U32(reinterpret_cast<const uint8_t*>(cc)); };
    if (pfFlags & 4) {   // compressed
        const int kind = is("DXT1") ? 1 : is("DXT3") ? 3 : is("DXT5") ? 5 : 0;
        if (!kind) return "a compression not read (only DXT1, DXT3, DXT5)";
        const size_t blockBytes = kind == 1 ? 8 : 16, bw = size_t(width + 3) / 4, bh = size_t(height + 3) / 4;
        if (left < bw * bh * blockBytes) return "cut short";
        for (size_t by = 0; by < bh; ++by)
            for (size_t bx = 0; bx < bw; ++bx) {
                const uint8_t* b = data + (by * bw + bx) * blockBytes;
                uint8_t px[16][4];
                ColourBlock(kind == 1 ? b : b + 8, kind != 1, px);
                if (kind == 3) {   // 4 bits of alpha a texel
                    for (int t = 0; t < 16; ++t) px[t][3] = uint8_t((b[t / 2] >> (t % 2 * 4) & 15) * 17);
                } else if (kind == 5) {   // two endpoints, 3-bit indices
                    uint8_t a[8] = {b[0], b[1]};
                    for (int i = 2; i < 8; ++i)
                        a[i] = b[0] > b[1] ? uint8_t(((8 - i) * b[0] + (i - 1) * b[1]) / 7)
                               : i < 6     ? uint8_t(((6 - i) * b[0] + (i - 1) * b[1]) / 5)
                                           : uint8_t(i == 6 ? 0 : 255);
                    uint64_t idx = 0;
                    for (int i = 0; i < 6; ++i) idx |= uint64_t(b[2 + i]) << (8 * i);
                    for (int t = 0; t < 16; ++t) px[t][3] = a[idx >> (3 * t) & 7];
                }
                for (int t = 0; t < 16; ++t) {
                    const size_t x = bx * 4 + t % 4, y = by * 4 + t / 4;
                    if (x < size_t(width) && y < size_t(height)) std::memcpy(&out.rgba[(y * width + x) * 4], px[t], 4);
                }
            }
        return "";
    }
    if ((pfFlags & 0x40) && bits == 32) {   // uncompressed: the masks say where the channels are
        const uint32_t masks[4] = {U32(h + 88), U32(h + 92), U32(h + 96), U32(h + 100)};
        if (left < size_t(width) * height * 4) return "cut short";
        auto shift = [](uint32_t m) { int s = 0; while (m && !(m & 1)) m >>= 1, ++s; return s; };
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            const uint32_t v = U32(data + i * 4);
            for (int c = 0; c < 4; ++c)
                out.rgba[i * 4 + c] = c == 3 && !(pfFlags & 1) ? 255 : masks[c] ? uint8_t((v & masks[c]) >> shift(masks[c])) : 255;
        }
        return "";
    }
    return "a pixel format not read";
}

} // namespace wrally::formats
