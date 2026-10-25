// WAudio.dll - the car's sound (audio/audio.h) behind wrally/iface/audio.h (docs/design.md 5).
//
// The records the game forwards (submitRecord: the physics' and the game's DriverTick) are turned back into signals
// records and emitted on the module's own bus, which audio::Audio listens to exactly as it listened to the game's bus
// when it was linked in - so the same records give the same samples. Real time: XAudio2 pulls from the device's thread
// (audio/device.h). Offline (kAudioOffline): every StepEnd renders its ticks' samples on the submitting thread into a
// buffer that renderOffline drains.
//
// Files: through the game's HostServices::files (audio/files.h's reader) - the bank's paths are VFS paths.
//
// One car voice (audio::Audio plays one car); the game's one-shots (loadSound / play) are not in yet.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/bank.h"
#include "audio/device.h"
#include "audio/files.h"
#include "audio/mix.h"
#include "signals/bus.h"
#include "signals/convert.h"
#include "wrally/iface/audio.h"

using namespace wrally::iface;

namespace wrally::iface {

struct AudioModule {
    HostServices host;
    std::string error;
    bool offline = false;
    AudioSettings settings{};
    bool paused = false;
    audio::Bank bank;
    std::unique_ptr<audio::Audio> sound;
    audio::Device device;
    signals::Bus bus;
    CarVoiceHandle voice = 0;          // the one voice (0: none yet)
    std::vector<float> rendered;       // offline: interleaved stereo, not yet taken by renderOffline
    size_t taken = 0;                  // of `rendered`, in floats
};

} // namespace wrally::iface

namespace {

constexpr uint32_t kModuleVersion = MakeVersion(0, 1, 0);

void Log(AudioModule& m, LogLevel level, const std::string& text)
{
    if (m.host.log) m.host.log(m.host.ctx, level, text.c_str());
}

void InstallReader(AudioModule& m)
{
    const HostServices host = m.host;
    audio::FileReader r;
    r.read = [host](const std::string& path, std::vector<uint8_t>& out) {
        Blob b{};
        if (host.files.read(host.ctx, path.c_str(), &b) != Status::Ok) return false;
        const auto* p = static_cast<const uint8_t*>(b.data);
        out.assign(p, p + b.size);
        host.files.release(host.ctx, &b);
        return true;
    };
    r.exists = [host](const std::string& path) { return host.files.exists(host.ctx, path.c_str()) != 0; };
    audio::SetFileReader(std::move(r));
}

std::string Str(const char* s) { return s ? s : ""; }

// The settings into the playing sound: one gain (the master bus times the car's), muted while muted or paused.
void Apply(AudioModule& m)
{
    if (!m.sound) return;
    m.sound->volume = m.settings.volume[uint32_t(AudioBus::Master)] * m.settings.volume[uint32_t(AudioBus::Car)];
    m.sound->muted = m.settings.muted != 0 || m.paused;
}

std::vector<std::string> Paths(const char* const* list, uint32_t n)
{
    std::vector<std::string> out;
    for (uint32_t i = 0; list && i < n; ++i) out.push_back(Str(list[i]));
    return out;
}

Status SetMix(AudioModule& m, const char* text)
{
    audio::Mix mix;
    const std::string e = mix.Parse(Str(text), "the mix");
    m.sound->SetMix(mix);
    Log(m, LogLevel::Info, e + "mix:" + mix.Describe() + "\n");
    if (!e.empty()) return m.error = e, Status::ErrFormat;
    return Status::Ok;
}

void WRALLY_CALL Destroy(AudioModule* m)
{
    if (!m) return;
    m->device.Close();
    if (m->sound && m->voice) m->sound->Disconnect(m->bus);
    m->sound.reset();
    audio::SetFileReader({});
    delete m;
}

const char* WRALLY_CALL LastError(AudioModule* m) { return m ? m->error.c_str() : "no module"; }

uint32_t WRALLY_CALL DeviceCount(AudioModule* m) { return m && !m->offline ? 1 : 0; }

Status WRALLY_CALL GetDevice(AudioModule* m, uint32_t index, AudioDevice* out)
{
    if (!m || !out || m->offline || index != 0) return Status::ErrInvalidArg;
    *out = {};
    std::snprintf(out->id, sizeof out->id, "default");
    std::snprintf(out->name, sizeof out->name, "The system's default output");
    return Status::Ok;
}

Status WRALLY_CALL ApplySettings(AudioModule* m, const AudioSettings* s)
{
    if (!m || !s) return Status::ErrInvalidArg;
    m->settings = *s;
    Apply(*m);
    if (s->deviceId[0] && std::strcmp(s->deviceId, "default"))
        return m->error = "only the default device so far", Status::ErrNotSupported;
    return Status::Ok;
}

Status WRALLY_CALL SetPaused(AudioModule* m, uint32_t paused)
{
    if (!m) return Status::ErrInvalidArg;
    m->paused = paused != 0;
    Apply(*m);
    return Status::Ok;
}

Status WRALLY_CALL CreateCarVoice(AudioModule* m, const CarSoundDesc* d, CarVoiceHandle* out)
{
    if (out) *out = 0;
    if (!m) return Status::ErrInvalidArg;
    m->error.clear();
    if (!d || !out) return m->error = "createCarVoice: null argument", Status::ErrInvalidArg;
    if (m->voice) return m->error = "one car voice at a time", Status::ErrNotSupported;
    if (!Str(d->bank).empty()) return m->error = "only RBR's sound set (CarSoundDesc::bank \"\")", Status::ErrNotSupported;

    // The bank: RBR's set around the engine, unless the engine is "none" (S0's pulse engine alone).
    const std::string engine = Str(d->engine).empty() ? "Focus03-08" : Str(d->engine);
    if (engine != "none") {
        std::string missing = m->bank.LoadRbr("", engine);
        if (d->bangCount) missing += m->bank.LoadBangFiles(Paths(d->bangs, d->bangCount));
        if (d->idleBangCount) missing += m->bank.LoadBangFiles(Paths(d->idleBangs, d->idleBangCount), true);
        char t[160];
        std::snprintf(t, sizeof t, "sounds: %zu engine loops (%s)%s", m->bank.engine.size(), engine.c_str(),
                      missing.empty() ? "\n" : "; missing or odd:\n");
        Log(*m, LogLevel::Info, t + missing);
        m->sound->SetBank(&m->bank);
    }
    m->sound->Connect(m->bus, d->car);
    m->voice = 1;
    if (Str(d->mix).size()) SetMix(*m, d->mix);
    if (!m->offline)
        if (const std::string e = m->device.Open(*m->sound); !e.empty()) Log(*m, LogLevel::Warn, "no sound: " + e + "\n");
    *out = m->voice;
    return Status::Ok;
}

void WRALLY_CALL DestroyCarVoice(AudioModule* m, CarVoiceHandle v)
{
    if (!m || !v || v != m->voice) return;
    m->device.Close();
    m->sound->Disconnect(m->bus);
    m->voice = 0;
}

void WRALLY_CALL SubmitRecord(AudioModule* m, uint32_t kind, const void* data, uint32_t bytes)
{
    if (m && data) signals::EmitFromIface(m->bus, kind, data, bytes);
}

Status WRALLY_CALL SetCarMix(AudioModule* m, CarVoiceHandle v, const char* mix)
{
    if (!m || !v || v != m->voice) return Status::ErrInvalidArg;
    return SetMix(*m, mix);
}

Status WRALLY_CALL LoadSound(AudioModule* m, const char*, SoundHandle* out)
{
    if (out) *out = 0;
    if (m) m->error = "loadSound: not in yet";
    return Status::ErrNotSupported;
}
void WRALLY_CALL ReleaseSound(AudioModule*, SoundHandle) {}
Status WRALLY_CALL Play(AudioModule* m, SoundHandle, const PlayDesc*, PlayHandle* out)
{
    if (out) *out = 0;
    if (m) m->error = "play: not in yet";
    return Status::ErrNotSupported;
}
void WRALLY_CALL Stop(AudioModule*, PlayHandle) {}
Status WRALLY_CALL SetPlayPosition(AudioModule*, PlayHandle, const float*) { return Status::ErrNotSupported; }
uint32_t WRALLY_CALL IsPlaying(AudioModule*, PlayHandle) { return 0; }
void WRALLY_CALL SetListener(AudioModule*, const Listener*) {}   // one mono mix so far (no placed sources)

uint32_t WRALLY_CALL RenderOffline(AudioModule* m, float* out, uint32_t frames)
{
    if (!m || !m->offline || !out) return 0;
    const size_t have = (m->rendered.size() - m->taken) / 2;
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(have, frames));
    std::memcpy(out, m->rendered.data() + m->taken, sizeof(float) * 2 * n);
    m->taken += 2 * size_t(n);
    if (m->taken == m->rendered.size()) m->rendered.clear(), m->taken = 0;
    return n;
}

const AudioApi kApi = {&Destroy,         &LastError,    &DeviceCount,     &GetDevice,     &ApplySettings,
                       &SetPaused,       &CreateCarVoice, &DestroyCarVoice, &SubmitRecord, &SetCarMix,
                       &LoadSound,       &ReleaseSound, &Play,            &Stop,          &SetPlayPosition,
                       &IsPlaying,       &SetListener,  &RenderOffline};

} // namespace

WRALLY_EXPORT uint32_t WRALLY_CALL GetWInterfaceVer(ModuleKind kind)
{
    return kind == ModuleKind::Audio ? kAudioInterfaceVer : 0;
}

WRALLY_EXPORT Status WRALLY_CALL InitWInterface(uint32_t gameVersion, const StructHeader* in, StructHeader* out)
{
    if (!in || !out) return Status::ErrInvalidArg;
    if (in->kind != ModuleKind::Audio || out->kind != ModuleKind::Audio) return Status::ErrKind;
    if (in->interfaceVer != kAudioInterfaceVer || out->interfaceVer != kAudioInterfaceVer) return Status::ErrVersion;
    if (in->size != sizeof(AudioInput) || out->size != sizeof(AudioOutput)) return Status::ErrStructSize;
    if (gameVersion < MakeVersion(0, 1, 0)) return Status::ErrGameVersion;
    try {
        const auto& input = *reinterpret_cast<const AudioInput*>(in);
        auto& output = *reinterpret_cast<AudioOutput*>(out);
        if (!input.host.files.read || !input.host.files.release || !input.host.files.exists) return Status::ErrInvalidArg;
        // audio::Audio renders 48 kHz stereo; the device's rate is XAudio2's to convert.
        if (input.sampleRate && input.sampleRate != audio::kRate) return Status::ErrNotSupported;
        if (input.channels && input.channels != 2) return Status::ErrNotSupported;

        auto m = std::make_unique<AudioModule>();
        m->host = input.host;
        m->offline = (input.flags & kAudioOffline) != 0;
        m->settings = input.settings;
        m->sound = std::make_unique<audio::Audio>(m->offline ? audio::Audio::Mode::Offline : audio::Audio::Mode::RealTime);
        if (m->offline) {
            AudioModule* raw = m.get();
            m->sound->sink = [raw](const float* stereo, uint32_t frames) {
                raw->rendered.insert(raw->rendered.end(), stereo, stereo + 2 * size_t(frames));
            };
        }
        Apply(*m);
        InstallReader(*m);

        output.info = {};
        std::snprintf(output.info.id, sizeof output.info.id, "waudio");
        std::snprintf(output.info.name, sizeof output.info.name, "WAudio");
        output.info.moduleVersion = kModuleVersion;
        output.info.minGameVersion = MakeVersion(0, 1, 0);
        output.info.buildHash = WAUDIO_BUILD_HASH;
        output.info.caps = kAudioCapRbrBanks | kAudioCapOffline;
        output.api = &kApi;
        output.instance = m.release();
        return Status::Ok;
    } catch (...) {
        return Status::ErrInternal;
    }
}
