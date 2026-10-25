#include "audio/files.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace audio {

namespace {

FileReader& Reader()
{
    static FileReader r;
    return r;
}

} // namespace

void SetFileReader(FileReader reader) { Reader() = std::move(reader); }

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    out.clear();
    if (const FileReader& r = Reader(); r.read) return r.read(path, out);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool FileExists(const std::string& path)
{
    if (const FileReader& r = Reader(); r.exists) return r.exists(path);
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace audio
