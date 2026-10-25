#pragma once
// FileHost - iface::HostServices over plain folders: a data root the VFS paths resolve against and a cache root with
// one folder per module. The stand-in until wrally::vfs exists (design 6.1): no archives (an "a.rbz:member" path is
// ErrIo, said in the log), no layers.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wrally/iface/common.h"

namespace wrally::modules {

class FileHost {
public:
    using LogFn = std::function<void(iface::LogLevel, const char*)>;

    FileHost(std::filesystem::path dataRoot, std::filesystem::path cacheRoot, LogFn log = {});

    // The services for one module (`moduleId` names its cache folder). Valid while this FileHost lives.
    iface::HostServices Services(const std::string& moduleId);

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

    std::filesystem::path Resolve(const char* path) const;

    std::filesystem::path m_data, m_cache;
    LogFn m_log;
    std::vector<std::unique_ptr<Ctx>> m_ctx;
};

} // namespace wrally::modules
