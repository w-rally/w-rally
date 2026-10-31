#pragma once
// Where W-Rally's own files are (wrally::app). The game's files are the exe's folder; what the game writes (W-Rally.ini,
// W-Rally.log, the last drive's .phr, the modules' cache) goes to the user folder:
// - installed (the installer's install.ini beside the exe - Program Files is not the user's to write):
//   %LOCALAPPDATA%\W-Rally, made when first asked for;
// - otherwise (a build run from its tree, a folder copied anywhere): the exe's folder, as before.
// Header only: the modules' hosts (physics, render, sound) use it without the app's other code.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace wrally::app {

inline std::filesystem::path ExeFolder()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

// The installer's settings (rbr_path = the RBR folder chosen at install), read before the user's own.
inline std::filesystem::path InstallIni() { return ExeFolder() / "install.ini"; }

inline std::filesystem::path UserDir()
{
    static const std::filesystem::path dir = [] {
        std::error_code e;
        const char* local = std::getenv("LOCALAPPDATA");
        if (!local || !std::filesystem::exists(InstallIni(), e)) return ExeFolder();
        std::filesystem::path d = std::filesystem::path(local) / "W-Rally";
        std::filesystem::create_directories(d, e);
        return d;
    }();
    return dir;
}

} // namespace wrally::app
