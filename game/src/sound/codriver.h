#pragma once
// The co-driver (wrally::sound, docs/design.md 5 and 6.5): the stage's pacenotes read out ahead of the car. The calls
// are the game's logic - which, when - and the audio module only plays them (Sound's one-shots, the CoDriver bus).
//
// The notes: the stage's .dls (phys::data::DlsPacenote: a type, flags and a distance along the drive line). RBR's types
// (as Wallaby's pacenotes editor names them):
//   0 .. 11  the corners: hairpin, 90, K, medium, fast, easy - left, then right in the reverse order (6 easy right ..
//            11 hairpin right); 26 / 25 flat left / right
//   12 twisty, 13 a distance (the next note's distance from it), 16 over crest, 17 ford, 18 caution, 19 bump, 20 jump,
//   22 the finish, 27 bridge, 28 go straight, 29 .. 31 keep right / left / middle, 32 caution water
//   (21 the start and 23 a checkpoint are the stage clock's marks: not called)
// and its flags: 1 narrows, 2 wide out, 4 tightens, 32 don't cut, 64 cut, 128 tightens bad, 1024 long, 2048 plus,
// 4096 minus, 8192 maybe.
//
// The voice: RBR's, audio/CoDriver.ini naming each call's recordings (PACENOTE::<CALL>: Sounds=n, Snd0..: one picked at
// random each time) in audio/Speech/PaceNote/<snd>.ogg, and the distances' (30, 50, 70, 100, 150 .. 1000.ogg) there.
//
// A phrase: the call, its modifiers (maybe first, then plus / minus, long, narrows / wide out, tightens (bad), cut /
// don't cut), and a note within 25 m after it chained on with "into". Called when the car is the lead from it: the speed
// times the lead time, at least 60 m (the first notes while standing at the start). The phrases' sounds play one after
// another, never over each other.

#include <atomic>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "data/dls.h"

namespace wrally::sound {

class Sound;

class CoDriver {
public:
    ~CoDriver();   // (a load still running finished first)
    // The calls of `notes` with RBR's voice under `rbrRoot` (CoDriver.ini read from it; the sounds decoded and loaded
    // through `sound`, VFS paths) - on a loader thread, so the stage loads meanwhile: nothing is called until it is
    // done (Ready). `sound` must outlive this co-driver; null: no voice, the calls only shown (Upcoming).
    void LoadAsync(Sound* sound, std::string rbrRoot, std::vector<phys::data::DlsPacenote> notes);
    bool Ready() const { return m_ready.load(std::memory_order_acquire); }
    // Once ready, the load's report, once: the calls, the recordings, what is missing, how long it took ("" else).
    std::string TakeReport();
    // Each frame: the car at `distance` along the drive line at `speed` (m/s); `leadSeconds` how far ahead it calls.
    // `running`: the stage time runs (the stage clock's Running) - before GO and after the finish nothing is called
    // (what was already called still plays). Nothing before Ready.
    void Update(float distance, float speed, float leadSeconds, bool running);
    // (These three: once Ready.)
    size_t Phrases() const { return Ready() ? m_phrases.size() : 0; }
    size_t Recordings() const { return m_recordings; }
    // The first `count` calls, a line each (distance, words) - for the log.
    std::string Describe(size_t count) const;
    // The last phrase called, as text (for the readout).
    const std::string& LastCall() const { return m_last; }
    // The HUD's words (ui::DrawPacenotes draws them: the corners as L1 / R1 (flat) .. L7 / R7 (hairpin), the rest
    // short words, a distance its number): `current` the first phrase ahead (not yet passed by
    // 5 m) once it is spoken - from when its first word starts (no voice: when it is called), so the HUD and the voice
    // agree; `next` the one after it when the car is `nextSeconds` from it or less at its speed (only with a current).
    // "" not shown.
    void Upcoming(float distance, float speed, float nextSeconds, std::string& current, std::string& next) const;

private:
    struct Phrase {
        float distance = 0;
        std::vector<std::string> words;   // CoDriver.ini's call names, or a distance's number
    };
    // The load itself (the loader thread's): "" or what is missing (it still calls what it has).
    std::string Load(Sound* sound, const std::string& rbrRoot, const std::vector<phys::data::DlsPacenote>& notes);
    // A word's sound (0: none), a variant picked at random.
    uint32_t SoundOf(const std::string& word);

    // The loader writes everything below up to m_ready, then sets it; the game's thread reads them only after.
    std::thread m_loader;
    std::atomic<bool> m_ready{false};
    std::string m_report;
    bool m_reported = false;
    Sound* m_sound = nullptr;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> m_words;   // a call's (or a number's) loaded variants
    std::vector<Phrase> m_phrases;
    size_t m_next = 0;               // the first phrase not called yet
    struct Queued {
        uint32_t sound;   // 0: a word without a recording (marks its phrase spoken, plays nothing)
        size_t phrase;
    };
    std::deque<Queued> m_queue;      // the words still to say
    size_t m_spoken = 0;             // the phrases whose speech has started (the HUD's current call)
    uint32_t m_playing = 0;
    std::string m_last;
    size_t m_recordings = 0;
    std::mt19937 m_random{0x5eed};
};

} // namespace wrally::sound
