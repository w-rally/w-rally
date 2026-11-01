#include "sound/codriver.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "sound/sound.h"

namespace wrally::sound {

namespace {

// The call a note's type makes ("" none: a distance, a mark, an unknown type).
const char* CallOf(int32_t type)
{
    static const char* const kCorners[12] = {"HAIRPINLEFT", "90_LEFT",     "KLEFT",       "MEDIUMLEFT", "FASTLEFT",
                                             "EASYLEFT",    "EASYRIGHT",   "FASTRIGHT",   "MEDIUMRIGHT", "KRIGHT",
                                             "90_RIGHT",    "HAIRPINRIGHT"};
    if (type >= 0 && type < 12) return kCorners[type];
    switch (type) {
    case 12: return "TWISTY";
    case 16: return "OVERCREST";
    case 17: return "FORD";
    case 18: return "CAUTION";
    case 19: return "BUMP";
    case 20: return "JUMP";
    case 22: return "FINISH";
    case 25: return "FLATRIGHT";
    case 26: return "FLATLEFT";
    case 27: return "BRIDGE";
    case 28: return "GOSTRAIGHT";
    case 29: return "KEEPRIGHT";
    case 30: return "KEEPLEFT";
    case 31: return "KEEPMIDDLE";
    case 32: return "CAUTION_WATER";
    default: return "";
    }
}

constexpr int32_t kDistance = 13;
constexpr float kChain = 25.0f;     // a note this close after the last one is chained on with "into"
constexpr float kMinLead = 60.0f;   // metres: the least lead (standing at the start)
const int kNumbers[] = {30, 50, 70, 100, 150, 200, 300, 400, 500, 600, 700, 800, 900, 1000};   // the distances spoken

// A note's words: the call and its modifiers in the order spoken.
std::vector<std::string> WordsOf(const phys::data::DlsPacenote& n)
{
    std::vector<std::string> w;
    const char* call = CallOf(n.type);
    if (!*call) return w;
    const int32_t f = n.flags;
    if (f & 8192) w.push_back("MAYBE");
    w.push_back(call);
    if (f & 2048) w.push_back("PLUS");
    if (f & 4096) w.push_back("MINUS");
    if (f & 1024) w.push_back("LONG");
    if (f & 1) w.push_back("NARROWS");
    if (f & 2) w.push_back("WIDEOUT");
    if (f & 128) w.push_back("TIGHTENSBAD");
    else if (f & 4) w.push_back("TIGHTENS");
    if (f & 32) w.push_back("DONTCUT");
    else if (f & 64) w.push_back("CUT");
    return w;
}

// A word as the HUD takes it (ui::DrawPacenotes: a corner its block, the rest its plate): the corners as Wallaby's
// pacenotes editor names them - L1 / R1 flat .. L7 / R7 hairpin -, the rest a short word, a distance its number.
std::string ShortOf(const std::string& w)
{
    static const std::pair<const char*, const char*> kShort[] = {
        {"FLATLEFT", "L1"},     {"EASYLEFT", "L2"},     {"FASTLEFT", "L3"},       {"MEDIUMLEFT", "L4"},
        {"KLEFT", "L5"},        {"90_LEFT", "L6"},      {"HAIRPINLEFT", "L7"},    {"FLATRIGHT", "R1"},
        {"EASYRIGHT", "R2"},    {"FASTRIGHT", "R3"},    {"MEDIUMRIGHT", "R4"},    {"KRIGHT", "R5"},
        {"90_RIGHT", "R6"},     {"HAIRPINRIGHT", "R7"}, {"TWISTY", "twisty"},     {"OVERCREST", "crest"},
        {"FORD", "ford"},       {"CAUTION", "caution"}, {"BUMP", "bump"},         {"JUMP", "jump"},
        {"FINISH", "finish"},   {"BRIDGE", "bridge"},   {"GOSTRAIGHT", "straight"}, {"KEEPRIGHT", "keep R"},
        {"KEEPLEFT", "keep L"}, {"KEEPMIDDLE", "keep middle"}, {"CAUTION_WATER", "water"}, {"MAYBE", "maybe"},
        {"PLUS", "+"},          {"MINUS", "-"},         {"LONG", "long"},         {"NARROWS", "narrows"},
        {"WIDEOUT", "opens"},   {"TIGHTENS", "tightens"}, {"TIGHTENSBAD", "tightens bad"}, {"CUT", "cut"},
        {"DONTCUT", "don't cut"}, {"INTO", ">"}};
    if (!w.empty() && w[0] == '#') return w.substr(1);
    for (const auto& [word, text] : kShort)
        if (w == word) return text;
    return w;
}

std::string Display(const std::vector<std::string>& words)
{
    std::string s;
    for (const std::string& w : words) {
        const std::string t = ShortOf(w);
        // (A grade's + / - right on it: "L4+".)
        s += (s.empty() || t == "+" || t == "-" ? "" : " ") + t;
    }
    return s;
}

std::string Ini(const std::string& file, const std::string& section, const std::string& key)
{
    char v[256] = {};
    GetPrivateProfileStringA(section.c_str(), key.c_str(), "", v, sizeof v, file.c_str());
    return v;
}

} // namespace

std::string CoDriver::Load(Sound* sound, const std::string& rbrRoot, const std::vector<phys::data::DlsPacenote>& notes)
{
    m_sound = sound;
    m_words.clear();
    m_phrases.clear();
    m_queue.clear();
    m_next = 0;
    m_spoken = 0;
    m_playing = 0;
    std::string missing;
    const std::string ini = rbrRoot + "\\audio\\CoDriver.ini";
    const bool voice = sound && GetFileAttributesA(ini.c_str()) != INVALID_FILE_ATTRIBUTES;   // (none: the calls shown only)
    if (sound && !voice) missing = "no " + ini + ": no voice ";

    // The calls' recordings, as CoDriver.ini names them.
    std::vector<char> names(64 * 1024);
    const DWORD n = voice ? GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), ini.c_str()) : 0;
    size_t loaded = 0;
    for (DWORD at = 0; at < n && names[at];) {
        const std::string section(&names[at]);
        at += static_cast<DWORD>(section.size()) + 1;
        if (section.rfind("PACENOTE::", 0) != 0) continue;
        std::vector<uint32_t> variants;
        const int count = std::atoi(Ini(ini, section, "Sounds").c_str());
        for (int k = 0; k < count; ++k) {
            const std::string snd = Ini(ini, section, "Snd" + std::to_string(k));
            if (snd.empty()) continue;
            std::string e;
            if (const uint32_t h = sound->LoadSound("audio/Speech/PaceNote/" + snd + ".ogg", &e)) variants.push_back(h), ++loaded;
            else missing += snd + " ";
        }
        m_words.emplace_back(section.substr(10), std::move(variants));
    }
    for (int d : kNumbers) {
        if (!voice) break;
        std::vector<uint32_t> v;
        if (const uint32_t h = sound->LoadSound("audio/Speech/PaceNote/" + std::to_string(d) + ".ogg")) v.push_back(h), ++loaded;
        m_words.emplace_back("#" + std::to_string(d), std::move(v));
    }

    // The phrases: each note's words, a close next note chained on with "into", a distance note its number.
    std::vector<phys::data::DlsPacenote> sorted = notes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    size_t lastTarget = SIZE_MAX;   // the call the last distance said how far it is to
    for (size_t i = 0; i < sorted.size(); ++i) {
        const phys::data::DlsPacenote& note = sorted[i];
        Phrase p;
        p.distance = note.distance;
        if (note.type == kDistance) {
            // The distance to the next call - once: a stage often has a second distance note before the same call
            // (stage 41 at 1363 m, right after the hairpin, and 1397 m), and the first is the one said with the corner.
            float next = -1;
            size_t target = SIZE_MAX;
            for (size_t j = i + 1; j < sorted.size() && next < 0; ++j)
                if (*CallOf(sorted[j].type)) next = sorted[j].distance, target = j;
            if (target == lastTarget) continue;
            lastTarget = target;
            const float gap = next - note.distance;
            int number = 0;
            for (int d : kNumbers)
                if (gap >= d) number = d;
            if (next < 0 || !number) continue;
            p.words.push_back("#" + std::to_string(number));
        } else {
            p.words = WordsOf(note);
            if (p.words.empty()) continue;
            float last = note.distance;
            while (i + 1 < sorted.size() && *CallOf(sorted[i + 1].type) && sorted[i + 1].distance - last < kChain) {
                const std::vector<std::string> more = WordsOf(sorted[++i]);
                last = sorted[i].distance;
                p.words.push_back("INTO");
                p.words.insert(p.words.end(), more.begin(), more.end());
            }
        }
        m_phrases.push_back(std::move(p));
    }
    m_recordings = loaded;
    return missing.empty() ? std::string() : "missing recordings: " + missing;
}

std::string CoDriver::Describe(size_t count) const
{
    std::string s;
    char line[48];
    for (size_t i = 0; i < m_phrases.size() && i < count; ++i) {
        std::snprintf(line, sizeof line, "  %7.0f m  ", m_phrases[i].distance);
        s += line;
        for (const std::string& w : m_phrases[i].words) s += w + " ";
        s += "\n";
    }
    return s;
}

uint32_t CoDriver::SoundOf(const std::string& word)
{
    for (const auto& [name, variants] : m_words)
        if (name == word && !variants.empty())
            return variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(m_random)];
    return 0;
}

CoDriver::~CoDriver()
{
    if (m_loader.joinable()) m_loader.join();
}

void CoDriver::LoadAsync(Sound* sound, std::string rbrRoot, std::vector<phys::data::DlsPacenote> notes)
{
    if (m_loader.joinable()) m_loader.join();
    m_ready.store(false, std::memory_order_relaxed);
    m_reported = false;
    m_loader = std::thread([this, sound, root = std::move(rbrRoot), list = std::move(notes)] {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string missing = Load(sound, root, list);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char line[192];
        std::snprintf(line, sizeof line, "co-driver: %zu calls from %zu pacenotes, %zu recordings, loaded in %.0f ms%s",
                      m_phrases.size(), list.size(), m_recordings, ms, missing.empty() ? "" : "; ");
        m_report = line + missing;
        m_ready.store(true, std::memory_order_release);
    });
}

std::string CoDriver::TakeReport()
{
    if (!Ready() || m_reported) return "";
    m_reported = true;
    return m_report;
}

void CoDriver::Update(float distance, float speed, float leadSeconds, bool running)
{
    if (!Ready()) return;
    const float lead = std::max(kMinLead, speed * leadSeconds);
    for (; running && m_next < m_phrases.size() && m_phrases[m_next].distance - distance <= lead; ++m_next) {
        const Phrase& p = m_phrases[m_next];
        if (p.distance < distance - 5.0f) continue;   // already passed (a run started further on)
        m_last.clear();
        // its first word marks the phrase spoken when it starts (a word without a recording: 0, nothing played)
        for (const std::string& w : p.words) {
            m_queue.push_back({m_sound ? SoundOf(w) : 0u, m_next});
            m_last += (m_last.empty() ? "" : " ") + w;
        }
    }
    while (m_queue.size() > 24) m_queue.pop_front();   // (far behind: the oldest dropped)
    while (!m_queue.empty() && !(m_sound && m_sound->Playing(m_playing))) {
        const Queued q = m_queue.front();
        m_queue.pop_front();
        m_spoken = std::max(m_spoken, q.phrase + 1);
        if (q.sound) m_playing = m_sound->Play(q.sound, Sound::Bus::CoDriver);
    }
}

void CoDriver::Upcoming(float distance, float speed, float nextSeconds, std::string& current, std::string& next) const
{
    current.clear();
    next.clear();
    if (!Ready()) return;
    // The first phrase ahead (passed by 5 m: gone) once its speech has started, and the next by the time to it at the
    // speed (standing: never).
    size_t i = 0;
    while (i < m_phrases.size() && m_phrases[i].distance < distance - 5.0f) ++i;
    if (i >= m_phrases.size() || i >= m_spoken) return;
    current = Display(m_phrases[i].words);
    if (i + 1 >= m_phrases.size()) return;
    const float ahead = std::max(0.0f, m_phrases[i + 1].distance - distance);
    if (speed > 0.1f ? ahead / speed <= nextSeconds : ahead <= 0.0f) next = Display(m_phrases[i + 1].words);
}

} // namespace wrally::sound
