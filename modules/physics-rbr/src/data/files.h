#pragma once
// Every file the physics reads goes through here, whole, by path. By default that is the filesystem; a host installs its
// own reader - WPhysicsRBR.dll the game's VFS (HostServices::files), so the paths are the VFS's and physics.rbz,
// loose files and later mod layers resolve the way the game resolves them.
//
// One reader per process (the module's), set before the first load. Writing (data/rec.h's WriteRecording) stays on the
// filesystem: the physics never writes.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace phys::data {

// Fills `out` with the file's bytes; false if there is none.
using FileReader = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

// nullptr: the filesystem again.
void SetFileReader(FileReader reader);

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out);

} // namespace phys::data
