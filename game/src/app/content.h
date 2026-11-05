#pragma once
// The game's content layers (docs/content-layers.md): the downloaded folder, W-Rally's content and the RBR folder, as
// one wrally::vfs::Vfs made from the settings.

#include <filesystem>
#include <memory>
#include <string>

#include "app/options.h"
#include "app/settings.h"
#include "wrally/vfs/vfs.h"

namespace wrally::app {

// W-Rally's own content (layer 2): <exe>\content as the game ships, else the source tree's content/ (a build run from
// its tree, where the content submodule is).
std::filesystem::path WRallyContentDir();

// The layers for these settings: DownloadDir(downloadPath) ("downloaded"), WRallyContentDir() ("w-rally") and rbrPath.
std::unique_ptr<vfs::Vfs> MakeVfs(const Settings& s);
// The same with another RBR folder (a stage run's: its command line's).
std::unique_ptr<vfs::Vfs> MakeVfs(const Settings& s, const std::string& rbr);

// --track: the stage's files (.trk, .col, .mat, .fnc, .lbs, .dls) from the track item `o.track` ("<id>[@<version>]"),
// each through its versions, on disk. Returns "" (nothing to do, or done) or why it could not; `o.track` gets the
// version found.
std::string ApplyTrack(Options& o, const vfs::Vfs& content);

// For the log: the folders and what was found in them, and the first of the notes.
std::string DescribeVfs(const vfs::Vfs& v);

} // namespace wrally::app
