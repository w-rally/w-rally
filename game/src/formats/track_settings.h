#pragma once
// RBR's Maps/TrackSettings.ini (wrally::formats): the stages' weathers as the game lights them, one section a
// condition, named "<map><variant>_<weather>_<sky>" with the map zero-padded to two digits - "41M_crisp_clear",
// "04E_hazy_partcloud", "108O_M_heavyfog_clear" (variant M, N or E; O_M, O_N or O_E for an overcast stage).
//
// Read as text, not through GetPrivateProfileString: one read for the whole file (757 sections), and values as
// written. Keys are lower-cased; values keep their text (some presets were written with decimal commas).

#include <map>
#include <string>
#include <vector>

namespace wrally::formats {

struct TrackSettingsSection {
    std::string name;
    std::map<std::string, std::string> values;   // key lower-cased -> value as written
};

// The file's sections in file order. False if it cannot be read.
bool ReadTrackSettings(const std::string& path, std::vector<TrackSettingsSection>& out);

// The section-name prefix of a map: "41", "04" (RBR writes the map number in two digits at least).
std::string TrackSettingsMap(int map);

} // namespace wrally::formats
