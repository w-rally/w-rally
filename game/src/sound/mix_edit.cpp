#include "sound/mix_edit.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace wrally::sound {

namespace {

// A value as the file writes it: up to three decimals, no trailing zeros.
std::string Number(float v)
{
    char t[32];
    std::snprintf(t, sizeof t, "%.3f", v);
    std::string s = t;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string Line(const MixValue& v) { return std::string(v.key) + " = " + Number(v.value) + (v.solo ? " solo" : ""); }

// A line's name, value and solo (`name = number [solo]`, before any '#'). False when it is not one.
bool Parse(const std::string& line, std::string& name, float& value, bool& solo)
{
    std::istringstream in(line.substr(0, line.find('#')));
    std::string eq, flag;
    if (!(in >> name >> eq >> value) || eq != "=") return false;
    solo = (in >> flag) && flag == "solo";
    return true;
}

} // namespace

MixEdit::MixEdit()
{
    using K = MixValue::Kind;
    // The module's names and defaults (modules/audio's audio/mix.h).
    values = {{"master", "Master", K::Amount, 1, 0, 2},
              {"engine.grains", "Engine as grains (off: pitched loops)", K::Switch, 1, 0, 1},
              {"engine.rpmBlend", "Crossfade between revs", K::Amount, 1, 0, 1},
              {"engine.loadBlend", "Crossfade off / on throttle", K::Amount, 1, 0, 1},
              {"engine.grainJumps", "Grain jumps per loop pass", K::Amount, 2, 0, 10},
              {"engine.idleWander", "Idle wander", K::Amount, 1, 0, 3},
              {"antiLag.bangs", "Anti-lag bangs (share of firings)", K::Amount, 0.4f, 0, 1},
              {"antiLag.idleBangs", "Anti-lag bangs at standstill", K::Amount, 0.6f, 0, 1},
              {"engine.on", "Engine on throttle", K::Volume, 1, 0, 2},
              {"engine.off", "Engine off throttle", K::Volume, 1, 0, 2},
              {"turbo", "Turbo", K::Volume, 1, 0, 2},
              {"valve", "Blow-off valve", K::Volume, 1, 0, 2},
              {"pops", "Pops", K::Volume, 1, 0, 2},
              {"bangs", "Bangs", K::Volume, 1, 0, 2},
              {"gear", "Gear change", K::Volume, 1, 0, 2},
              {"dogring", "Dog ring", K::Volume, 1, 0, 2},
              {"transmission", "Gear whine", K::Volume, 1, 0, 2},
              {"roll", "Tyres rolling", K::Volume, 1, 0, 2},
              {"skid", "Tyres sliding", K::Volume, 1, 0, 2},
              {"wind", "Wind", K::Volume, 1, 0, 2}};
    Defaults();
}

void MixEdit::Defaults()
{
    for (MixValue& v : values) v.value = v.init, v.solo = false;
}

void MixEdit::Load(const std::string& text)
{
    m_text = text;
    Defaults();
    std::istringstream f(text);
    std::string line, name;
    float value;
    bool solo;
    while (std::getline(f, line)) {
        if (!Parse(line, name, value, solo)) continue;
        for (MixValue& v : values)
            if (name == v.key || (name == "engine" && (std::string(v.key) == "engine.on" || std::string(v.key) == "engine.off")))
                v.value = value, v.solo = solo;
    }
}

std::string MixEdit::Text() const
{
    std::string s = m_text + "\n# the Debug menu's\n";
    for (const MixValue& v : values) s += Line(v) + "\n";
    return s;
}

std::string MixEdit::Save(const std::string& path)
{
    std::istringstream f(m_text);
    std::string out, line, name;
    float value;
    bool solo;
    std::vector<bool> written(values.size());
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (Parse(line, name, value, solo)) {
            // The value's own text replaced; the comment after it stays where it was (or a space after the value).
            const size_t hash = line.find('#');
            auto with = [&](const std::string& text) {
                if (hash == std::string::npos) return text;
                return text + std::string(hash > text.size() ? hash - text.size() : 1, ' ') + line.substr(hash);
            };
            if (name == "engine") {
                // Both engine layers: kept while they agree, else the line goes and each gets its own (at the end).
                const MixValue *a = nullptr, *b = nullptr;
                size_t ia = 0, ib = 0;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (std::string(values[i].key) == "engine.on") a = &values[i], ia = i;
                    if (std::string(values[i].key) == "engine.off") b = &values[i], ib = i;
                }
                if (a && b && a->value == b->value && a->solo == b->solo) {
                    out += with("engine = " + Number(a->value) + (a->solo ? " solo" : "")) + "\n";
                    written[ia] = written[ib] = true;
                }
                continue;
            }
            size_t i = 0;
            while (i < values.size() && name != values[i].key) ++i;
            if (i < values.size()) {
                out += with(Line(values[i])) + "\n";
                written[i] = true;
                continue;
            }
        }
        out += line + "\n";
    }
    bool header = false;
    for (size_t i = 0; i < values.size(); ++i) {
        const MixValue& v = values[i];
        if (written[i] || (v.value == v.init && !v.solo)) continue;
        if (!header) out += "\n# from the Debug menu\n", header = true;
        out += Line(v) + "\n";
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) return "cannot write " + path;
    file << out;
    if (!file) return "cannot write " + path;
    m_text = out;
    return "";
}

} // namespace wrally::sound
