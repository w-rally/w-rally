// ---------------------------------------------------------------------------
// wrally/iface/audio.h - the Audio module interface (WAudio.dll).
//
// Built on plugin-rbr/audio (doc/sound-engine-plan.md): the engine and car
// sounds are DRIVEN BY THE PHYSICS' RECORDS - the firings, revs, boost, gear
// changes, wheel slip and surfaces - not by a handful of parameters the game
// distils from them. So the car's interface is the record stream itself: the
// game forwards every record it gets from the physics (records.h) plus the
// driver layer's DriverTick, and the module does what audio::Audio::Connect
// does today with a signals::Bus.
//
// Everything else is ordinary game audio: one-shots and loops on buses (UI,
// co-driver, world), a listener from the camera, pause, volumes.
//
// The module owns its output device and its mixing thread.
//
// THREADING:
//   - submitRecord: from the game's sim thread (where the physics emits),
//     concurrently with everything else. It queues; it never blocks on the
//     mixer.
//   - every other call: from one game thread (the main thread).
//   - renderOffline: only on an instance created with kAudioOffline, from the
//     thread that calls submitRecord - the same run gives the same samples
//     (a replay rendered to WAV).
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/common.h"

namespace wrally::iface {

constexpr uint32_t kAudioInterfaceVer = 1;

struct AudioModule;
typedef uint32_t CarVoiceHandle;   // one car's sound (engine, drivetrain, tyres, wind)
typedef uint32_t SoundHandle;      // a loaded sound (.wav/.ogg), 0 none
typedef uint32_t PlayHandle;       // one playing instance of a sound

// AudioCaps (ModuleInfo::caps)
constexpr uint64_t kAudioCapRbrBanks    = 1ull << 0;   // RBR's audio banks and Cars/<car>.eng
constexpr uint64_t kAudioCapPositional  = 1ull << 1;   // sources placed around the listener
constexpr uint64_t kAudioCapOffline     = 1ull << 2;

// AudioInput::flags
constexpr uint32_t kAudioOffline = 1u << 0;   // no device: samples come out of renderOffline

enum class AudioBus : uint32_t { Master = 0, Car = 1, World = 2, CoDriver = 3, Ui = 4, Count = 5 };

#pragma pack(push, 4)

struct AudioDevice {
    char id[128];      // stable endpoint id
    char name[128];    // for the UI
};

struct AudioSettings {
    char     deviceId[128];                         // "" the system default
    float    volume[uint32_t(AudioBus::Count)];     // 0..1 per bus
    uint32_t muted;
    uint32_t speakers;                              // 2 stereo, 6 5.1, 0 the device's
};

// One car's sound. VFS paths; "" for the module's default.
struct CarSoundDesc {
    uint32_t    car;          // the physics' car index: records with this Stamp::car drive the voice
    const char* engine;       // Cars/<car>.eng (RBR), or a WRally engine sound set
    const char* bank;         // the sounds around it (RBR: Audio/...; WRally: a sound set manifest)
    const char* mix;          // the per-car mix (layers' volumes), the module's format
};

struct Listener {
    float    position[3];     // PHYSICS AXES (z up) - the same frame as the records
    float    forward[3], up[3];
    float    velocity[3];
    uint32_t interior;        // 1: the camera is inside the car (cockpit filter, engine through the bulkhead)
};

struct PlayDesc {
    AudioBus bus;
    float    gain;            // 1 as authored
    float    pitch;           // 1 as authored
    uint32_t loop;
    uint32_t positional;      // 1: at `position` (physics axes); 0: in the head
    float    position[3];
};

#pragma pack(pop)

struct AudioApi {
    void        (WRALLY_CALL* destroy)(AudioModule*);
    const char* (WRALLY_CALL* lastError)(AudioModule*);          // never null

    // ---- device
    uint32_t (WRALLY_CALL* deviceCount)(AudioModule*);
    Status   (WRALLY_CALL* device)(AudioModule*, uint32_t index, AudioDevice* out);
    Status   (WRALLY_CALL* applySettings)(AudioModule*, const AudioSettings*);   // reopens the device if it changed
    Status   (WRALLY_CALL* setPaused)(AudioModule*, uint32_t paused);           // the game's pause: the world's buses

    // ---- cars: fed by the physics' records
    Status (WRALLY_CALL* createCarVoice)(AudioModule*, const CarSoundDesc*, CarVoiceHandle* out);
    void   (WRALLY_CALL* destroyCarVoice)(AudioModule*, CarVoiceHandle);
    // Every record the physics emitted (records.h, by kind) and the game's
    // DriverTick, in the order they came. Unknown kinds are ignored.
    void   (WRALLY_CALL* submitRecord)(AudioModule*, uint32_t kind, const void* data, uint32_t bytes);

    // ---- everything else
    Status (WRALLY_CALL* loadSound)(AudioModule*, const char* path, SoundHandle* out);
    void   (WRALLY_CALL* releaseSound)(AudioModule*, SoundHandle);
    Status (WRALLY_CALL* play)(AudioModule*, SoundHandle, const PlayDesc*, PlayHandle* out);   // out may be null
    void   (WRALLY_CALL* stop)(AudioModule*, PlayHandle);
    Status (WRALLY_CALL* setPlayPosition)(AudioModule*, PlayHandle, const float position[3]);
    uint32_t (WRALLY_CALL* isPlaying)(AudioModule*, PlayHandle);

    // Once per rendered frame, from the camera.
    void (WRALLY_CALL* setListener)(AudioModule*, const Listener*);

    // kAudioOffline only: the next `frames` interleaved frames of `channels`
    // (AudioInput's), as far as the records submitted so far reach. Returns how
    // many frames it wrote.
    uint32_t (WRALLY_CALL* renderOffline)(AudioModule*, float* out, uint32_t frames);
};

struct AudioInput {
    StructHeader  header;     // kind Audio, kAudioInterfaceVer
    HostServices  host;
    uint32_t      flags;      // kAudio*
    uint32_t      sampleRate; // 0: the device's (offline: 48000)
    uint32_t      channels;   // offline only; 0: 2
    AudioSettings settings;
};

struct AudioOutput {
    StructHeader    header;
    ModuleInfo      info;
    const AudioApi* api;
    AudioModule*    instance;
};

} // namespace wrally::iface
