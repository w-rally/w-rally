#include "app/content.h"

#include <system_error>

#include "app/paths.h"

namespace wrally::app {

std::filesystem::path WRallyContentDir()
{
    std::error_code e;
    const std::filesystem::path beside = ExeFolder() / "content";
    if (std::filesystem::is_directory(beside, e)) return beside;
    return (std::filesystem::path(WRALLY_SOURCE_DIR) / "content").make_preferred();
}

std::unique_ptr<vfs::Vfs> MakeVfs(const Settings& s)
{
    return std::make_unique<vfs::Vfs>(std::vector<vfs::Layer>{{DownloadDir(s.downloadPath), "downloaded"},
                                                              {WRallyContentDir(), "w-rally"}},
                                      std::filesystem::path(s.rbrPath));
}

std::string DescribeVfs(const vfs::Vfs& v)
{
    std::error_code e;
    auto folder = [&](const std::filesystem::path& p) {
        return p.string() + (std::filesystem::is_directory(p, e) ? "" : " (none yet)");
    };
    std::string out = "content layers:";
    for (const vfs::Layer& l : v.Layers()) out += "\n  " + l.name + ": " + folder(l.root);
    out += "\n  rbr: " + (v.RbrRoot().empty() ? std::string("(not set)") : folder(v.RbrRoot()));
    out += "\n  " + std::to_string(v.Ids(vfs::Kind::Track).size()) + " tracks, " +
           std::to_string(v.Ids(vfs::Kind::Car).size()) + " cars";
    // the notes: how many, and the first few (each RBR map without an id in rbr-tracks.json is one)
    const std::string& log = v.Log();
    size_t lines = 0, shown = 0, at = 0;
    std::string first;
    while (at < log.size()) {
        const size_t end = std::min(log.find('\n', at), log.size());
        if (shown < 3) first += "\n    " + log.substr(at, end - at), ++shown;
        ++lines, at = end + 1;
    }
    if (lines) out += ", " + std::to_string(lines) + " notes:" + first + (lines > shown ? "\n    ..." : "");
    return out + "\n";
}

} // namespace wrally::app
