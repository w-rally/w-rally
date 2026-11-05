// ---------------------------------------------------------------------------
// wrally/iface/common.h - what every W-Rally module DLL and the game share.
//
// THE CONTRACT ACROSS A DLL BOUNDARY IS PLAIN DATA. The game and its modules
// may be built by different compilers, CRTs and build configurations, so
// nothing here is a C++ class, an STL type, an exception or a reference count.
// What crosses is:
//   - fixed-width integers, floats, and POD structs with static_assert'ed sizes
//   - opaque handles (pointers to incomplete types) owned by the side that made them
//   - function tables (structs of function pointers) - the module's "vtable"
//   - UTF-8, NUL-terminated strings, valid only for the duration of the call
//     unless the function says otherwise
//
// Namespaces are fine: the exports are extern "C", and namespaces do not change
// the layout of a POD struct.
//
// THE TWO EXPORTS (every module DLL has exactly these):
//
//   uint32_t GetWInterfaceVer(ModuleKind kind)
//       The version of `kind`'s interface the DLL implements, 0 if it does not
//       implement that kind at all. Cheap and side-effect free: the game calls
//       it on every DLL it finds before deciding whether to touch it further.
//       The game uses a DLL only when the number EQUALS its own
//       k<Kind>InterfaceVer - there is no "newer is fine". Any change to an
//       interface struct or function table bumps its version by one.
//
//   Status InitWInterface(uint32_t gameVersion, const StructHeader* in, StructHeader* out)
//       Called once, after the version matched. `in` is the kind's input
//       struct (RendererInput, PhysicsInput, AudioInput), `out` the kind's output struct;
//       both start with a StructHeader the module checks (kind, version, size)
//       before reading or writing anything else. On Ok, `out` holds the
//       module's info, its function table and its instance.
//
// A DLL may implement several kinds (GetWInterfaceVer answers per kind); the
// game calls InitWInterface once per kind it uses.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define WRALLY_CALL __cdecl
#  define WRALLY_EXPORT extern "C" __declspec(dllexport)
#else
#  define WRALLY_CALL
#  define WRALLY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace wrally::iface {

// major.minor.patch packed into one word, comparable with <.
constexpr uint32_t MakeVersion(uint32_t major, uint32_t minor, uint32_t patch)
{
    return (major & 0x3ffu) << 22 | (minor & 0x3ffu) << 12 | (patch & 0xfffu);
}
constexpr uint32_t VersionMajor(uint32_t v) { return v >> 22; }
constexpr uint32_t VersionMinor(uint32_t v) { return v >> 12 & 0x3ffu; }
constexpr uint32_t VersionPatch(uint32_t v) { return v & 0xfffu; }

enum class ModuleKind : uint32_t {
    None     = 0,
    Renderer = 1,
    Physics  = 2,
    Audio    = 3,
    // Input, ... when a module of that kind exists - not before.
};

enum class Status : int32_t {
    Ok              = 0,
    ErrVersion      = -1,   // interface version in the header is not the module's
    ErrKind         = -2,   // header's kind is not one this module implements
    ErrStructSize   = -3,   // header's size is not this version's struct size
    ErrGameVersion  = -4,   // module refuses this game version (ModuleInfo::minGameVersion)
    ErrInvalidArg   = -5,
    ErrNotSupported = -6,   // valid request this module does not do (see its caps)
    ErrIo           = -7,   // a file the host's FileApi could not provide
    ErrFormat       = -8,   // a file that was there but not readable as what it should be
    ErrDevice       = -9,   // GPU / device failure (renderer)
    ErrOutOfMemory  = -10,
    ErrFault        = -11,  // the module's own state is broken; the instance must be destroyed
    ErrInternal     = -12,
};

// Every input and output struct of InitWInterface starts with this.
struct StructHeader {
    uint32_t   size;           // sizeof the whole struct, as the writer compiled it
    ModuleKind kind;
    uint32_t   interfaceVer;   // the k<Kind>InterfaceVer the writer was built against
    uint32_t   reserved;       // 0
};
static_assert(sizeof(StructHeader) == 16, "StructHeader layout");

template <class T> inline void InitHeader(T& s, ModuleKind kind, uint32_t ver)
{
    s.header.size = static_cast<uint32_t>(sizeof(T));
    s.header.kind = kind;
    s.header.interfaceVer = ver;
    s.header.reserved = 0;
}

// Bytes owned by whoever filled it; released with the matching release call.
struct Blob {
    const void* data;
    uint64_t    size;
    void*       owner;         // the filler's bookkeeping, opaque to the reader
};

enum class LogLevel : uint32_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

// ---------------------------------------------------------------------------
// HostServices - what the game lends every module.
//
// FILES GO THROUGH THE GAME. A module never opens a game data file by itself:
// the content is in layers (downloaded, W-Rally's, RBR's - docs/content-layers.md),
// each item per version over its bases, and only the game's VFS knows which copy
// wins. Paths are the VFS's: forward slashes, case-insensitive - an item's file
// "cars/s_i2003/physics/rbr/rbr/common.lsp", RBR's folder as it is under rbr/
// ("rbr/Maps/track-41_M.trk"). No archive members: physics.rbz is the VFS's own.
//
// Every function pointer is non-null. All of them are thread-safe.
// ---------------------------------------------------------------------------
struct FileApi {
    // The whole file into `out`. ErrIo if there is none.
    Status  (WRALLY_CALL* read)(void* ctx, const char* path, Blob* out);
    void    (WRALLY_CALL* release)(void* ctx, Blob* blob);
    int32_t (WRALLY_CALL* exists)(void* ctx, const char* path);          // 1 / 0
    // A directory where the module may write its own caches (shader cache,
    // converted data), created by the game, one per module id. UTF-8, valid
    // for the module's lifetime.
    const char* (WRALLY_CALL* cacheDir)(void* ctx);
};

struct HostServices {
    void* ctx;                 // passed back to every call below

    void (WRALLY_CALL* log)(void* ctx, LogLevel level, const char* message);

    // Allocations the game may account for. A module may use its own heap for
    // anything that never crosses the boundary; anything the GAME frees must
    // come from here.
    void* (WRALLY_CALL* alloc)(void* ctx, size_t bytes, size_t alignment);
    void  (WRALLY_CALL* free)(void* ctx, void* p);

    FileApi files;

    // Monotonic seconds, the game's clock - for the module's own timing logs.
    double (WRALLY_CALL* now)(void* ctx);
};

// Module capability bits live in each kind's header (RendererCaps, PhysicsCaps).
struct ModuleInfo {
    char     id[32];           // stable machine id: "wrenderer", "rbr", "wphysics". Folder for its cache, key in configs and replays.
    char     name[64];         // for the UI: "WRenderer", "RBR original", "WPhysics"
    uint32_t moduleVersion;    // MakeVersion
    uint32_t minGameVersion;   // the module refused anything older already; for the UI
    uint64_t buildHash;        // identifies the exact build: replays and online results record it
    uint64_t caps;             // the kind's capability bits
};
static_assert(sizeof(ModuleInfo) == 120, "ModuleInfo layout");

} // namespace wrally::iface

// --------------------------------------------------------------- the exports
extern "C" {
typedef uint32_t (WRALLY_CALL* PFN_GetWInterfaceVer)(wrally::iface::ModuleKind kind);
typedef wrally::iface::Status (WRALLY_CALL* PFN_InitWInterface)(uint32_t gameVersion,
                                                                const wrally::iface::StructHeader* in,
                                                                wrally::iface::StructHeader* out);
}

#define WRALLY_EXPORT_NAME_GET_VER "GetWInterfaceVer"
#define WRALLY_EXPORT_NAME_INIT    "InitWInterface"
