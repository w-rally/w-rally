#pragma once
// The sound card: an XAudio2 source voice (48 kHz stereo float) fed from Audio::Render on XAudio2's thread, a few
// short buffers queued (about 16 ms).

#include <memory>
#include <string>

namespace audio {

class Audio;

class Device {
public:
    Device();
    ~Device();
    // Starts playing `audio` (Mode::RealTime). Returns "" or what failed.
    std::string Open(Audio& audio);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace audio
