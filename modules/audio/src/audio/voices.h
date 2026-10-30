#pragma once
// The game's one-shot sounds (wrally/iface/audio.h loadSound / play): samples loaded once, played as instances on the
// buses - the co-driver's calls, the UI's - and mixed into the device's output after the car's sound (Audio::Render).
// The game's thread loads and plays, the device's thread mixes; one lock between them, held briefly.
//
// Each instance: its sample read at its own rate times its pitch (linearly interpolated: speech at 11 kHz to 48 kHz),
// its gain times its bus's, looped or once. Not placed yet (no positional sources: the mix is mono into both channels).
// Paused, the world's buses (car, world, co-driver) hold where they are; the UI's plays on.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/sample.h"

namespace audio {

class Voices {
public:
    enum Bus : uint32_t { kMaster = 0, kCar = 1, kWorld = 2, kCoDriver = 3, kUi = 4, kBuses = 5 };

    // A sample kept for playing; its id (0 never).
    uint32_t Add(Sample s);
    void Remove(uint32_t sound);
    // An instance of `sound` on `bus`; its id (0: no such sound). Stopping or finishing frees it.
    uint32_t Play(uint32_t sound, Bus bus, float gain, float pitch, bool loop);
    void Stop(uint32_t play);
    bool Playing(uint32_t play) const;
    // Each bus's gain (the master's applies on top), and the world's buses held.
    void SetGain(Bus bus, float gain);
    void SetPaused(bool paused);

    // The device's thread: the instances added into `out` (interleaved stereo, `rate` frames a second).
    void Mix(float* out, uint32_t frames, float rate);

private:
    struct Instance {
        uint32_t id = 0;
        std::shared_ptr<const Sample> sample;
        Bus bus = kUi;
        float gain = 1, step = 1;   // step: source frames per output frame, before the rate
        bool loop = false;
        double position = 0;
    };
    mutable std::mutex m_lock;
    std::vector<std::pair<uint32_t, std::shared_ptr<const Sample>>> m_sounds;
    std::vector<Instance> m_playing;
    std::array<float, kBuses> m_gain{1, 1, 1, 1, 1};
    bool m_paused = false;
    uint32_t m_nextSound = 1, m_nextPlay = 1;
};

} // namespace audio
