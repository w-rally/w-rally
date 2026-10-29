#include "formats/fnc.h"

#include <cstring>

namespace wrally::formats {

std::string ReadFnc(const std::vector<uint8_t>& data, Fnc& out)
{
    out = {};
    size_t at = 0;
    auto take = [&](void* dst, size_t n) {
        if (at + n > data.size()) return false;
        std::memcpy(dst, data.data() + at, n);
        at += n;
        return true;
    };
    int32_t version = 0;
    uint32_t groups = 0;
    if (!take(&version, 4) || version != 2) return "";
    if (!take(&groups, 4)) return ".fnc: cut short";
    if (groups > 100000) return ".fnc: " + std::to_string(groups) + " groups";
    out.groups.resize(groups);
    for (FncGroup& g : out.groups) {
        uint32_t items = 0;
        if (!take(&items, 4) || !take(g.words, 8) || !take(&g.tile, 4) || !take(&g.post, 4) || !take(g.box, 24))
            return ".fnc: cut short";
        if (items > (data.size() - at) / 40) return ".fnc: cut short";
        g.items.resize(items);
        for (FncItem& i : g.items)
            if (!take(i.point, 12) || !take(i.centre, 12) || !take(i.halfSize, 12) || !take(&i.colour, 4)) return ".fnc: cut short";
    }
    uint32_t types = 0, recordSize = 0;
    if (!take(&types, 4) || !take(&recordSize, 4)) return ".fnc: no type table";
    if (recordSize == 0 || recordSize > 256 || types > (data.size() - at) / recordSize) return ".fnc: a bad type table";
    for (uint32_t k = 0; k < types; ++k) {
        const char* const name = reinterpret_cast<const char*>(data.data() + at);
        out.types.emplace_back(name, strnlen(name, recordSize));
        at += recordSize;
    }
    return "";
}

std::string ReadFenceTextures(const std::vector<uint8_t>& data, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out)
{
    out.clear();
    if (data.size() < 16 || std::memcmp(data.data(), "CHELALIC", 8)) return "fence.dat: not a CHELALIC archive";
    uint32_t base = 0;
    std::memcpy(&base, data.data() + 12, 4);
    if (base > data.size()) return "fence.dat: a bad header";
    // The directory: each name ending ".dds" behind its length, its offset and size the record's first two words.
    for (size_t p = 16; p + 8 <= base; ++p) {
        uint32_t n = 0;
        std::memcpy(&n, data.data() + p, 4);
        if (n < 5 || n > 64 || p + 4 + n > base) continue;
        const char* const name = reinterpret_cast<const char*>(data.data() + p + 4);
        const size_t len = strnlen(name, n);
        if (len < 5 || len == n || _stricmp(name + len - 4, ".dds")) continue;
        bool printable = true;
        for (size_t k = 0; k < len; ++k) printable = printable && name[k] > 32 && name[k] < 127;
        if (!printable) continue;
        uint32_t offset = 0, size = 0;
        std::memcpy(&offset, data.data() + p - 16, 4);
        std::memcpy(&size, data.data() + p - 12, 4);
        if (size < 128 || uint64_t(base) + offset + size > data.size() || std::memcmp(data.data() + base + offset, "DDS ", 4)) continue;
        out.emplace_back(std::string(name, len), std::vector<uint8_t>(data.begin() + base + offset, data.begin() + base + offset + size));
        p += 3 + n;
    }
    if (out.empty()) return "fence.dat: no textures";
    return "";
}

} // namespace wrally::formats
