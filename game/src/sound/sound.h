#pragma once
// wrally::sound::Sound - the game's side of an audio module (docs/design.md 5): finds the module DLL, initialises it,
// and forwards the car's records from the game's signals::Bus to it (AudioApi::submitRecord) - the physics' records and
// the game's DriverTick, every one, in order. The module turns them back into the same records, so the sound is what it
// was when the audio was linked into the game.
//
// Paths: under `root` (the RBR folder) - the module's VFS; the anti-lag bang folders are listed here, since the module
// cannot list through the VFS.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace signals {
class Bus;
}

namespace wrally::sound {

struct CarSound {
    uint32_t car = 0;
    std::string engine = "Focus03-08";     // audio/Cars/<engine>.eng, a path to an .eng, or "none" (the pulse engine)
    std::string bangs, idleBangs;          // folders of .wav: the anti-lag's bangs ("" the bank's)
    std::string mixText;                   // the mix file's text ("" the defaults)
};

class Sound {
public:
    Sound();
    ~Sound();
    Sound(const Sound&) = delete;
    Sound& operator=(const Sound&) = delete;

    // The module with id `moduleId` ("waudio") among the audio DLLs in `modulesDir`; `offline`: no device, the samples
    // from RenderOffline. Returns "" or what is wrong.
    std::string Open(const std::filesystem::path& modulesDir, const std::string& moduleId, const std::string& root,
                     bool offline = false);
    // The car's voice. Returns "" or what is wrong.
    std::string CreateCar(const CarSound& car);
    // Every record on `bus` to the module from now on (nullptr: none). The bus must outlive this Sound (or be
    // disconnected first): it disconnects from it as it goes.
    void Connect(signals::Bus* bus);
    // A changed mix (the file's text). Returns "" or what was wrong with it.
    std::string SetMix(const std::string& text);
    void SetMuted(bool muted);
    bool Muted() const { return m_muted; }
    // Offline: up to `frames` stereo frames (interleaved) rendered so far; returns how many.
    uint32_t RenderOffline(float* out, uint32_t frames);
    // What the module logged since the last call (its Info and Warn lines).
    std::string TakeLog();
    std::string ModuleName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    bool m_muted = false;
};

} // namespace wrally::sound
