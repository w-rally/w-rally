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
#include <string>
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

// Installed by the installer (its install.ini beside the exe).
inline bool Installed()
{
    std::error_code e;
    return std::filesystem::exists(InstallIni(), e);
}

inline std::filesystem::path UserDir()
{
    static const std::filesystem::path dir = [] {
        std::error_code e;
        const char* local = std::getenv("LOCALAPPDATA");
        if (!local || !Installed()) return ExeFolder();
        std::filesystem::path d = std::filesystem::path(local) / "W-Rally";
        std::filesystem::create_directories(d, e);
        return d;
    }();
    return dir;
}

// The downloaded layer's folder (docs/content-layers.md): `setting` (download_path - the installer's in install.ini, a
// user's own in W-Rally.ini) with its %VARIABLES% expanded, relative to the exe's folder; "" its default:
// %LOCALAPPDATA%\W-Rally\Downloads installed, else <exe>\Downloads. Not made here: the first write makes it.
inline std::filesystem::path DownloadDir(const std::string& setting)
{
    if (setting.empty()) {
        const char* local = std::getenv("LOCALAPPDATA");
        return Installed() && local ? std::filesystem::path(local) / "W-Rally" / "Downloads" : ExeFolder() / "Downloads";
    }
    std::string expanded(ExpandEnvironmentStringsA(setting.c_str(), nullptr, 0), '\0');
    expanded.resize(ExpandEnvironmentStringsA(setting.c_str(), expanded.data(), static_cast<DWORD>(expanded.size())));
    while (!expanded.empty() && expanded.back() == '\0') expanded.pop_back();
    const std::filesystem::path p(expanded);
    return p.is_absolute() ? p : ExeFolder() / p;
}

} // namespace wrally::app
