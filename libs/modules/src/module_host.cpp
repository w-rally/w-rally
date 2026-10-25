#include "wrally/modules/module_host.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <system_error>

#include "wrally/iface/audio.h"
#include "wrally/iface/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::modules {

using iface::ModuleKind;
using iface::Status;

uint32_t ExpectedVersion(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::Renderer: return iface::kRendererInterfaceVer;
    case ModuleKind::Physics: return iface::kPhysicsInterfaceVer;
    case ModuleKind::Audio: return iface::kAudioInterfaceVer;
    default: return 0;
    }
}

const char* KindName(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::Renderer: return "renderer";
    case ModuleKind::Physics: return "physics";
    case ModuleKind::Audio: return "audio";
    default: return "none";
    }
}

const char* StatusName(Status s)
{
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::ErrVersion: return "ErrVersion";
    case Status::ErrKind: return "ErrKind";
    case Status::ErrStructSize: return "ErrStructSize";
    case Status::ErrGameVersion: return "ErrGameVersion";
    case Status::ErrInvalidArg: return "ErrInvalidArg";
    case Status::ErrNotSupported: return "ErrNotSupported";
    case Status::ErrIo: return "ErrIo";
    case Status::ErrFormat: return "ErrFormat";
    case Status::ErrDevice: return "ErrDevice";
    case Status::ErrOutOfMemory: return "ErrOutOfMemory";
    case Status::ErrFault: return "ErrFault";
    case Status::ErrInternal: return "ErrInternal";
    }
    return "Status(?)";
}

namespace {

std::string LastErrorText()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

// The module's own folder first, so its dependencies (WRenderer's dxcompiler.dll) load from beside it.
HMODULE LoadModule(const std::filesystem::path& path, std::string* error)
{
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m && error) *error = "LoadLibrary: " + LastErrorText();
    return m;
}

bool IsDll(const std::filesystem::path& p)
{
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".dll";
}

} // namespace

struct ModuleHost::Loaded {
    std::filesystem::path path;
    HMODULE dll = nullptr;
    ~Loaded()
    {
        if (dll) FreeLibrary(dll);
    }
};

ModuleHost::ModuleHost() = default;
ModuleHost::~ModuleHost() = default;

void ModuleHost::Scan(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> dlls;
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir, ec)) {
        dlls.push_back(dir);
    } else {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file() && IsDll(e.path())) dlls.push_back(e.path());
        if (ec) m_problems.push_back({dir, "cannot list: " + ec.message()});
    }
    std::sort(dlls.begin(), dlls.end());

    for (const auto& path : dlls) {
        std::string error;
        HMODULE m = LoadModule(path, &error);
        if (!m) {
            m_problems.push_back({path, error});
            continue;
        }
        auto getVer = reinterpret_cast<PFN_GetWInterfaceVer>(GetProcAddress(m, WRALLY_EXPORT_NAME_GET_VER));
        const bool hasInit = GetProcAddress(m, WRALLY_EXPORT_NAME_INIT) != nullptr;
        if (!getVer || !hasInit) {
            m_problems.push_back({path, getVer ? "no InitWInterface export" : "not a module (no GetWInterfaceVer)"});
        } else {
            bool any = false;
            for (ModuleKind kind : kKinds) {
                const uint32_t v = getVer(kind);
                if (!v) continue;
                any = true;
                m_entries.push_back({path, kind, v, v == ExpectedVersion(kind)});
            }
            if (!any) m_problems.push_back({path, "implements no kind this game knows"});
        }
        FreeLibrary(m);
    }
}

std::vector<const ModuleEntry*> ModuleHost::Available(ModuleKind kind) const
{
    std::vector<const ModuleEntry*> out;
    for (const ModuleEntry& e : m_entries)
        if (e.kind == kind && e.compatible) out.push_back(&e);
    return out;
}

Status ModuleHost::Init(const ModuleEntry& entry, const iface::StructHeader* in, iface::StructHeader* out,
                        std::string* error)
{
    auto fail = [error](Status s, std::string why) {
        if (error) *error = std::move(why);
        return s;
    };
    if (!entry.compatible)
        return fail(Status::ErrVersion, std::string(KindName(entry.kind)) + " interface " +
                                            std::to_string(entry.interfaceVer) + ", the game needs " +
                                            std::to_string(ExpectedVersion(entry.kind)));
    if (!in || !out || in->kind != entry.kind || out->kind != entry.kind)
        return fail(Status::ErrInvalidArg, "the input / output headers are not this entry's kind");

    Loaded* loaded = nullptr;
    for (auto& l : m_loaded)
        if (l->path == entry.path) loaded = l.get();
    if (!loaded) {
        std::string why;
        HMODULE m = LoadModule(entry.path, &why);
        if (!m) return fail(Status::ErrIo, why);
        m_loaded.push_back(std::make_unique<Loaded>());
        loaded = m_loaded.back().get();
        loaded->path = entry.path;
        loaded->dll = m;
    }
    auto init = reinterpret_cast<PFN_InitWInterface>(GetProcAddress(loaded->dll, WRALLY_EXPORT_NAME_INIT));
    if (!init) return fail(Status::ErrInternal, "InitWInterface vanished");
    const Status s = init(kGameVersion, in, out);
    if (s != Status::Ok) return fail(s, std::string("InitWInterface: ") + StatusName(s));
    return s;
}

} // namespace wrally::modules
