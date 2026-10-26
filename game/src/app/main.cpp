// WRally (docs/design.md): the rally game - grown from phys-drive (docs/imported.md), the physics, the sound and the
// renderer modules (bin/modules), the game itself in wrally::app (window, options, the frame loop), wrally::session
// (one stage run), wrally::camera, wrally::input, wrally::rules and wrally::ui (the HUD). The command line: app/options.h.

#include <windows.h>

#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app/game.h"
#include "app/options.h"

namespace {

// WRally is a window application, so it has no console of its own. Its output goes where it was sent (a redirect, as
// CTest's), else to the console it was started from, else - started from Explorer - to WRally.log beside the exe.
// Returns that log's path ("" when there is a console or a redirect to tell instead).
std::string SetUpOutput()
{
    auto redirected = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    if (redirected(STD_OUTPUT_HANDLE)) return "";
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        std::printf("\n");   // off the prompt the console printed when it went on without us
        return "";
    }
    const std::string log = (wrally::app::ExeDir() / "WRally.log").string();
    std::FILE* f;
    if (freopen_s(&f, log.c_str(), "w", stdout)) return "";
    _dup2(_fileno(stdout), _fileno(stderr));   // one file, both streams, in order
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return log;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string log = SetUpOutput();
    const int code = wrally::app::RunGame(std::vector<std::string>(argv, argv + argc));
    // Started without a console, a failure would be silent: say where its reasons went.
    if (code && !log.empty()) {
        const std::string text = "WRally stopped (exit code " + std::to_string(code) + ").\n\nThe reason is in\n" + log;
        MessageBoxA(nullptr, text.c_str(), "WRally", MB_OK | MB_ICONERROR);
    }
    return code;
}
