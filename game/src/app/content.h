#pragma once
// The game's content layers (docs/content-layers.md): the downloaded folder, W-Rally's content and the RBR folder, as
// one wrally::vfs::Vfs made from the settings.

#include <filesystem>
#include <memory>
#include <string>

#include "app/settings.h"
#include "wrally/vfs/vfs.h"

namespace wrally::app {

// W-Rally's own content (layer 2): <exe>\content as the game ships, else the source tree's content/ (a build run from
// its tree, where the content submodule is).
std::filesystem::path WRallyContentDir();

// The layers for these settings: DownloadDir(downloadPath) ("downloaded"), WRallyContentDir() ("w-rally") and rbrPath.
std::unique_ptr<vfs::Vfs> MakeVfs(const Settings& s);

// For the log: the folders and what was found in them, and the first of the notes.
std::string DescribeVfs(const vfs::Vfs& v);

} // namespace wrally::app
