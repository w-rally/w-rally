#include "audio/device.h"

#include <windows.h>
#include <xaudio2.h>

#include <array>
#include <vector>

#include "audio/audio.h"

namespace audio {

namespace {
constexpr uint32_t kFrames = 256;   // a buffer, 5.3 ms
constexpr int kBuffers = 3;
} // namespace

struct Device::Impl : IXAudio2VoiceCallback {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    Audio* audio = nullptr;
    std::array<std::vector<float>, kBuffers> buffers;
    int next = 0;
    bool com = false;

    void Submit()
    {
        std::vector<float>& b = buffers[next];
        next = (next + 1) % kBuffers;
        audio->Render(b.data(), kFrames);
        XAUDIO2_BUFFER x{};
        x.AudioBytes = static_cast<UINT32>(b.size() * sizeof(float));
        x.pAudioData = reinterpret_cast<const BYTE*>(b.data());
        voice->SubmitSourceBuffer(&x);
    }

    // XAudio2's thread: a buffer played, the next rendered into its place.
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { Submit(); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

Device::Device() : m(std::make_unique<Impl>()) {}

Device::~Device() { Close(); }

std::string Device::Open(Audio& audio)
{
    Close();
    m->com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(XAudio2Create(&m->xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return "XAudio2Create failed";
    if (FAILED(m->xa->CreateMasteringVoice(&m->master, 2, kRate))) return "no audio output device";
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = kRate * 8;
    if (FAILED(m->xa->CreateSourceVoice(&m->voice, &f, 0, XAUDIO2_DEFAULT_FREQ_RATIO, m.get()))) return "CreateSourceVoice failed";
    m->audio = &audio;
    for (std::vector<float>& b : m->buffers) b.assign(2 * kFrames, 0.0f);
    for (int i = 0; i < kBuffers; ++i) m->Submit();
    m->voice->Start();
    return "";
}

void Device::Close()
{
    if (m->voice) {
        m->voice->Stop();
        m->voice->DestroyVoice();   // waits for the callbacks
        m->voice = nullptr;
    }
    if (m->master) m->master->DestroyVoice(), m->master = nullptr;
    if (m->xa) m->xa->Release(), m->xa = nullptr;
    if (m->com) CoUninitialize(), m->com = false;
}

} // namespace audio
