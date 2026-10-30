#include "formats/track_settings.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace wrally::formats {

namespace {

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

} // namespace

bool ReadTrackSettings(const std::string& path, std::vector<TrackSettingsSection>& out)
{
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            out.push_back(TrackSettingsSection{line.substr(1, close == std::string::npos ? std::string::npos : close - 1), {}});
            continue;
        }
        const size_t eq = line.find('=');
        if (out.empty() || eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.back().values[key] = Trim(line.substr(eq + 1));
    }
    return true;
}

std::string TrackSettingsMap(int map)
{
    char s[16];
    std::snprintf(s, sizeof s, "%02d", map);
    return s;
}

} // namespace wrally::formats
