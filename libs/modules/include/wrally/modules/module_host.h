#pragma once
// ModuleHost - finds the module DLLs and initialises the ones the game uses (design 2.4).
//
// Scan only ASKS: it loads each DLL, calls GetWInterfaceVer for every kind and unloads it again. Nothing is
// initialised and no device is created, so scanning a folder is cheap and safe on a DLL that turns out to be
// incompatible. Init loads the DLL for good (until the host goes away) and calls InitWInterface.
//
// The game uses a DLL only when the version it reports EQUALS the game's (ExpectedVersion): an incompatible module is
// listed, with why, and never initialised.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wrally/iface/common.h"

namespace wrally::modules {

// This build of the game, as InitWInterface's gameVersion.
constexpr uint32_t kGameVersion = iface::MakeVersion(0, 1, 0);

// The kinds the game knows, and the interface version it needs for each (0 for an unknown kind).
inline constexpr iface::ModuleKind kKinds[] = {iface::ModuleKind::Renderer, iface::ModuleKind::Physics,
                                               iface::ModuleKind::Audio};
uint32_t ExpectedVersion(iface::ModuleKind kind);
const char* KindName(iface::ModuleKind kind);
const char* StatusName(iface::Status status);

// One kind one DLL implements. A DLL implementing two kinds is two entries.
struct ModuleEntry {
    std::filesystem::path path;
    iface::ModuleKind kind = iface::ModuleKind::None;
    uint32_t interfaceVer = 0;   // what the DLL reported
    bool compatible = false;     // interfaceVer == ExpectedVersion(kind)
};

// A DLL that could not be asked at all: not loadable, or not a module.
struct ScanProblem {
    std::filesystem::path path;
    std::string error;
};

class ModuleHost {
public:
    ModuleHost();
    ~ModuleHost();
    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // Every *.dll in `dir` (not recursive); a single .dll path works too. Adds to what earlier scans found.
    void Scan(const std::filesystem::path& dir);

    const std::vector<ModuleEntry>& Entries() const { return m_entries; }
    const std::vector<ScanProblem>& Problems() const { return m_problems; }
    std::vector<const ModuleEntry*> Available(iface::ModuleKind kind) const;   // compatible ones

    // InitWInterface on a compatible entry: `in` / `out` are the kind's input and output structs (headers filled by the
    // caller). On failure `error` says why (the module's lastError is in its own table, which out has not got then).
    iface::Status Init(const ModuleEntry& entry, const iface::StructHeader* in, iface::StructHeader* out,
                       std::string* error = nullptr);

private:
    struct Loaded;
    std::vector<ModuleEntry> m_entries;
    std::vector<ScanProblem> m_problems;
    std::vector<std::unique_ptr<Loaded>> m_loaded;   // DLLs Init has kept
};

} // namespace wrally::modules
