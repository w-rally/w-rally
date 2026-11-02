#include "assets/splash.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "formats/fnc.h"

namespace wrally::assets {

namespace {

std::vector<uint8_t> ReadFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return f ? std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}) : std::vector<uint8_t>{};
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Where the photo is in RBR's splash layout (0..1 of the .dds): right of the torn paper, between its torn edges - as
// measured on its own splashes and the community's, which follow its template.
constexpr float kU0 = 0.54f, kU1 = 1.0f, kV0 = 0.23f, kV1 = 0.75f;

} // namespace

std::string ReadSplash(const std::string& rbrPath, const std::string& path, formats::Image& out, float& aspect)
{
    // A URL: the server's (the TrackRegister's API) - fetched once that is there.
    if (!_strnicmp(path.c_str(), "http://", 7) || !_strnicmp(path.c_str(), "https://", 8)) return path + ": a URL - not fetched yet";
    std::string rel = path;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    std::vector<uint8_t> dds = ReadFile(std::filesystem::path(rbrPath) / rel);
    if (dds.empty()) {   // the original stages': in Textures/splash.dat (a CHELALIC archive, as fence.dat is)
        const std::string name = Lower(std::filesystem::path(rel).filename().string());
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
        formats::ReadFenceTextures(ReadFile(std::filesystem::path(rbrPath) / "Textures" / "splash.dat"), files);
        for (auto& [n, bytes] : files)
            if (Lower(n) == name) dds = std::move(bytes);
    }
    if (dds.empty()) return path + ": not found (loose or in Textures\\splash.dat)";
    formats::Image whole;
    if (std::string e = formats::DecodeDds(dds, whole); !e.empty()) return path + ": " + e;
    const int x0 = int(whole.width * kU0), x1 = int(whole.width * kU1), y0 = int(whole.height * kV0), y1 = int(whole.height * kV1);
    out.width = x1 - x0, out.height = y1 - y0;
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(&out.rgba[size_t(y) * out.width * 4], &whole.rgba[(size_t(y0 + y) * whole.width + x0) * 4], size_t(out.width) * 4);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;   // (a photo: whatever its alpha says)
    aspect = float(out.width) / out.height * (4.0f / 3.0f) * whole.height / whole.width;
    return "";
}

} // namespace wrally::assets
