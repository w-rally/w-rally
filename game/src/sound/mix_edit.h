#pragma once
// The sound mix (game/config/sound-mix.ini: the audio module's `name = number [solo]` lines) as values to set while
// driving - the Debug menu's and the mix panel's. The file's text is kept: the sound gets it with the values' lines
// after it (a later line wins), and Save writes the values into the file's own lines, its comments kept.

#include <string>
#include <vector>

namespace wrally::sound {

struct MixValue {
    enum Kind { Volume, Switch, Amount };   // a layer's volume (with its solo), on / off (1 / 0), a number
    const char* key;      // the file's name
    const char* label;
    Kind kind;
    float init, min, max;   // init: the module's default (audio::Mix's)
    float value = 0;
    bool solo = false;
};

class MixEdit {
public:
    MixEdit();
    // The values from a mix file's text: its lines over the defaults ("engine" sets both engine layers).
    void Load(const std::string& text);
    // What the sound gets: the file's text and the values' lines after it.
    std::string Text() const;
    // The values into the file at `path`: each one's line rewritten (its comment kept), a missing one not at its default
    // added at the end. Returns "" or what went wrong.
    std::string Save(const std::string& path);
    // The values back to the module's defaults (no line of the file's then counts: the sound gets them after it).
    void Defaults();

    std::vector<MixValue> values;

private:
    std::string m_text;   // the file's, as last loaded or saved
};

} // namespace wrally::sound
