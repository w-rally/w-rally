#pragma once
// FileHost - iface::HostServices over the game's content layers (wrally::vfs, docs/content-layers.md) and a cache root
// with one folder per module. A module's paths are the VFS's: an item's file (cars/<id>/<version>/...), a raw RBR file
// (rbr/...) or a plain one; an absolute path is read from the disk as it is (the command line's, for development).

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wrally/iface/common.h"
#include "wrally/vfs/vfs.h"

namespace wrally::modules {

class FileHost {
public:
    using LogFn = std::function<void(iface::LogLevel, const char*)>;

    FileHost(std::shared_ptr<const vfs::Vfs> content, std::filesystem::path cacheRoot, LogFn log = {});

    // The services for one module (`moduleId` names its cache folder). Valid while this FileHost lives.
    iface::HostServices Services(const std::string& moduleId);

    const vfs::Vfs& Content() const { return *m_content; }

private:
    struct Ctx {
        FileHost* host;
        std::string cacheDir;
    };
    static void WRALLY_CALL Log(void* ctx, iface::LogLevel level, const char* message);
    static void* WRALLY_CALL Alloc(void* ctx, size_t bytes, size_t alignment);
    static void WRALLY_CALL Free(void* ctx, void* p);
    static iface::Status WRALLY_CALL Read(void* ctx, const char* path, iface::Blob* out);
    static void WRALLY_CALL Release(void* ctx, iface::Blob* blob);
    static int32_t WRALLY_CALL Exists(void* ctx, const char* path);
    static const char* WRALLY_CALL CacheDir(void* ctx);
    static double WRALLY_CALL Now(void* ctx);

    // Where `path` is: through the VFS, or the disk's for an absolute one.
    std::optional<vfs::Resolved> Resolve(const char* path) const;

    std::shared_ptr<const vfs::Vfs> m_content;
    std::filesystem::path m_cache;
    LogFn m_log;
    std::vector<std::unique_ptr<Ctx>> m_ctx;
};

} // namespace wrally::modules
