#pragma once
// Every file the sound reads goes through here (as the physics' data/files.h): by default the filesystem; WAudio.dll
// installs the game's VFS (HostServices::files), so a bank's paths are the game's VFS paths. One reader per process,
// set before the first load.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audio {

struct FileReader {
    std::function<bool(const std::string& path, std::vector<uint8_t>& out)> read;   // false: no such file
    std::function<bool(const std::string& path)> exists;
};

// An empty reader: the filesystem again.
void SetFileReader(FileReader reader);

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out);
bool FileExists(const std::string& path);

} // namespace audio
