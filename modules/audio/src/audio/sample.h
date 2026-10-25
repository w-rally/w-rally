#pragma once
// A sound in memory (mono float at its own rate) and the readers of the files it comes from: a .wav (PCM 8 / 16 bit,
// mono or stereo - mixed to mono) and RBR's .eng (an engine's loops by rpm and load).
//
// .eng (RBR, little-endian): "ENGI", u32 0x101, u32 n (the rpm points), n x u32 (the loads at each: 2); then per
// loop a "PART" {u32 0x100, u32 rpm, u32 load (0 off the throttle, 100 on), u32 0} and a "WAVE" {u32 0x100, u32 rate,
// u32 count, count x s16 mono, then a seek table: u32 m, m x {u32 sample, u32 0} (every 192 samples; unused)}. A few
// mods' files differ after the samples (other tables); the reader goes on at the next "PART" and says so.

#include <string>
#include <vector>

namespace audio {

struct Sample {
    std::vector<float> data;
    float rate = 48000;
    bool Empty() const { return data.empty(); }
};

// "" or what is wrong.
std::string ReadWav(const std::string& path, Sample& out);

struct EngineLoop {
    float rpm = 0;
    float load = 0;   // 0 off the throttle, 1 on
    Sample sample;
};
// The loops in the file's order. `warning`: what was odd but read past.
std::string ReadEng(const std::string& path, std::vector<EngineLoop>& out, std::string* warning = nullptr);

} // namespace audio
