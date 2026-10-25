#include "files.h"

#include <fstream>
#include <iterator>

namespace phys::data {

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
    if (const FileReader& r = Reader()) return r(path, out);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace phys::data
