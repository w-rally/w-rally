#include "wrally/modules/file_host.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <malloc.h>
#include <memory>
#include <vector>

namespace wrally::modules {

using iface::Status;

FileHost::FileHost(std::filesystem::path dataRoot, std::filesystem::path cacheRoot, LogFn log)
    : m_data(std::move(dataRoot)), m_cache(std::move(cacheRoot)), m_log(std::move(log))
{
    if (!m_log)
        m_log = [](iface::LogLevel level, const char* m) {
            static const char* const kNames[] = {"trace", "debug", "info", "warn", "error"};
            std::fprintf(stderr, "[%s] %s\n", kNames[static_cast<uint32_t>(level) % 5], m);
        };
}

iface::HostServices FileHost::Services(const std::string& moduleId)
{
    auto ctx = std::make_unique<Ctx>();
    ctx->host = this;
    std::error_code ec;
    const std::filesystem::path dir = m_cache / moduleId;
    std::filesystem::create_directories(dir, ec);
    const std::u8string u8 = dir.u8string();
    ctx->cacheDir.assign(u8.begin(), u8.end());

    iface::HostServices s{};
    s.ctx = ctx.get();
    s.log = &Log;
    s.alloc = &Alloc;
    s.free = &Free;
    s.files.read = &Read;
    s.files.release = &Release;
    s.files.exists = &Exists;
    s.files.cacheDir = &CacheDir;
    s.now = &Now;
    m_ctx.push_back(std::move(ctx));
    return s;
}

std::filesystem::path FileHost::Resolve(const char* path) const
{
    return m_data / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path ? path : "")));
}

void WRALLY_CALL FileHost::Log(void* ctx, iface::LogLevel level, const char* message)
{
    static_cast<Ctx*>(ctx)->host->m_log(level, message ? message : "");
}

void* WRALLY_CALL FileHost::Alloc(void*, size_t bytes, size_t alignment)
{
    return _aligned_malloc(bytes ? bytes : 1, alignment < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignment);
}

void WRALLY_CALL FileHost::Free(void*, void* p)
{
    _aligned_free(p);
}

Status WRALLY_CALL FileHost::Read(void* ctx, const char* path, iface::Blob* out)
{
    if (!out) return Status::ErrInvalidArg;
    *out = {};
    FileHost& h = *static_cast<Ctx*>(ctx)->host;
    if (!path || std::strchr(path, ':')) {
        h.m_log(iface::LogLevel::Warn, (std::string("no archive support yet: ") + (path ? path : "(null)")).c_str());
        return Status::ErrIo;
    }
    std::ifstream f(h.Resolve(path), std::ios::binary | std::ios::ate);
    if (!f) return Status::ErrIo;
    const std::streamoff size = f.tellg();
    auto* bytes = new std::vector<uint8_t>(static_cast<size_t>(size));
    f.seekg(0);
    if (size && !f.read(reinterpret_cast<char*>(bytes->data()), size)) {
        delete bytes;
        return Status::ErrIo;
    }
    out->data = bytes->data();
    out->size = static_cast<uint64_t>(size);
    out->owner = bytes;
    return Status::Ok;
}

void WRALLY_CALL FileHost::Release(void*, iface::Blob* blob)
{
    if (!blob) return;
    delete static_cast<std::vector<uint8_t>*>(blob->owner);
    *blob = {};
}

int32_t WRALLY_CALL FileHost::Exists(void* ctx, const char* path)
{
    if (!path || std::strchr(path, ':')) return 0;
    std::error_code ec;
    return std::filesystem::is_regular_file(static_cast<Ctx*>(ctx)->host->Resolve(path), ec) ? 1 : 0;
}

const char* WRALLY_CALL FileHost::CacheDir(void* ctx)
{
    return static_cast<Ctx*>(ctx)->cacheDir.c_str();
}

double WRALLY_CALL FileHost::Now(void*)
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace wrally::modules
