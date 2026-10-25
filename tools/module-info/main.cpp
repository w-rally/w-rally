// module-info - what a module DLL (or a folder of them) says about itself, as the game sees it.
//
//   module-info <dll or folder>... [--init] [--data <dir>]
//
// For each DLL: the kinds it implements, the interface version it reports for each and whether the game can use it.
// --init also initialises every compatible physics and audio module (a renderer needs a window: skipped) with a
// FileHost over --data (default: the current folder), prints its ModuleInfo and destroys it again.
//
// Exit code: 0 all fine, 1 a DLL could not be asked or an --init failed, 2 usage.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "wrally/iface/audio.h"
#include "wrally/iface/physics.h"
#include "wrally/iface/renderer.h"
#include "wrally/modules/file_host.h"
#include "wrally/modules/module_host.h"

using namespace wrally;
using iface::ModuleKind;
using iface::Status;

namespace {

std::string Version(uint32_t v)
{
    return std::to_string(iface::VersionMajor(v)) + "." + std::to_string(iface::VersionMinor(v)) + "." +
           std::to_string(iface::VersionPatch(v));
}

void PrintInfo(const iface::ModuleInfo& i)
{
    std::printf("      id %s, \"%s\", version %s (needs game %s), build %016llx, caps %016llx\n", i.id, i.name,
                Version(i.moduleVersion).c_str(), Version(i.minGameVersion).c_str(),
                static_cast<unsigned long long>(i.buildHash), static_cast<unsigned long long>(i.caps));
}

template <class In, class Out>
bool InitAndDestroy(modules::ModuleHost& host, const modules::ModuleEntry& e, modules::FileHost& files, In& in, Out& out)
{
    iface::InitHeader(in, e.kind, e.interfaceVer);
    iface::InitHeader(out, e.kind, e.interfaceVer);
    in.host = files.Services(e.path.stem().string());
    std::string error;
    if (host.Init(e, &in.header, &out.header, &error) != Status::Ok) {
        std::printf("      init FAILED: %s\n", error.c_str());
        return false;
    }
    PrintInfo(out.info);
    if (!out.api || !out.instance) {
        std::printf("      init FAILED: Ok without a table or an instance\n");
        return false;
    }
    out.api->destroy(out.instance);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::filesystem::path> targets;
    bool init = false;
    std::filesystem::path data = std::filesystem::current_path();
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--init")) init = true;
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else targets.emplace_back(argv[i]);
    }
    if (targets.empty()) {
        std::fprintf(stderr, "usage: module-info <dll or folder>... [--init] [--data <dir>]\n");
        return 2;
    }

    std::printf("game %s; interfaces: renderer %u, physics %u, audio %u\n", Version(modules::kGameVersion).c_str(),
                iface::kRendererInterfaceVer, iface::kPhysicsInterfaceVer, iface::kAudioInterfaceVer);
    modules::ModuleHost host;
    for (const auto& t : targets) host.Scan(t);

    bool ok = host.Problems().empty();
    for (const auto& p : host.Problems()) std::printf("%s: %s\n", p.path.string().c_str(), p.error.c_str());

    modules::FileHost files(data, data / "cache");
    for (const auto& e : host.Entries()) {
        std::printf("%s: %s interface %u - %s\n", e.path.filename().string().c_str(), modules::KindName(e.kind),
                    e.interfaceVer, e.compatible ? "compatible" : "INCOMPATIBLE");
        if (!e.compatible) {
            std::printf("      the game needs %s interface %u\n", modules::KindName(e.kind), modules::ExpectedVersion(e.kind));
            continue;
        }
        if (!init) continue;
        if (e.kind == ModuleKind::Physics) {
            iface::PhysicsInput in{};
            iface::PhysicsOutput out{};
            in.config = "";
            ok &= InitAndDestroy(host, e, files, in, out);
        } else if (e.kind == ModuleKind::Audio) {
            iface::AudioInput in{};
            iface::AudioOutput out{};
            in.flags = iface::kAudioOffline;   // no device opened by a listing tool
            ok &= InitAndDestroy(host, e, files, in, out);
        } else {
            std::printf("      init skipped: a renderer needs a window\n");
        }
    }
    if (host.Entries().empty() && host.Problems().empty()) std::printf("no module DLLs found\n");
    return ok ? 0 : 1;
}
