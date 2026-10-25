#include "sound/sound.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "signals/bus.h"
#include "signals/convert.h"
#include "wrally/iface/audio.h"
#include "wrally/modules/file_host.h"
#include "wrally/modules/module_host.h"

namespace wrally::sound {

using iface::Status;

struct Sound::Impl {
    modules::ModuleHost host;
    std::unique_ptr<modules::FileHost> files;
    iface::AudioOutput out{};
    iface::AudioSettings settings{};
    iface::CarVoiceHandle voice = 0;
    signals::Bus* bus = nullptr;
    std::vector<signals::Bus::Connection> connections;
    std::mutex logLock;   // the module may log from its device's thread
    std::string log;

    ~Impl()
    {
        Disconnect();
        if (voice) out.api->destroyCarVoice(out.instance, voice);
        if (out.instance) out.api->destroy(out.instance);
    }

    void Disconnect()
    {
        if (bus)
            for (signals::Bus::Connection c : connections) bus->Disconnect(c);
        connections.clear();
        bus = nullptr;
    }

    template <class T> void Forward()
    {
        connections.push_back(bus->Connect<T>([this](const T& r) {
            const auto o = signals::ToIface(r);
            out.api->submitRecord(out.instance, T::kKind, &o, static_cast<uint32_t>(sizeof o));
        }));
    }
};

namespace {

// The .wav files in `dir` in the folder's order - as audio::Bank::LoadBangs listed them when the audio was linked in.
std::vector<std::string> WavFiles(const std::string& dir)
{
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".wav") files.push_back(e.path().string());
    return files;
}

} // namespace

Sound::Sound() = default;
Sound::~Sound() = default;

std::string Sound::Open(const std::filesystem::path& modulesDir, const std::string& moduleId, const std::string& root,
                        bool offline)
{
    m = std::make_unique<Impl>();
    m->host.Scan(modulesDir);
    Impl* impl = m.get();
    m->files = std::make_unique<modules::FileHost>(root, modulesDir.parent_path().parent_path() / "cache",
                                                   [impl](iface::LogLevel level, const char* text) {
                                                       if (level < iface::LogLevel::Info) return;
                                                       std::lock_guard<std::mutex> lock(impl->logLock);
                                                       impl->log += text;
                                                   });
    // The defaults audio::Audio had when linked: 0.8 overall, not muted.
    for (float& v : m->settings.volume) v = 1.0f;
    m->settings.volume[uint32_t(iface::AudioBus::Master)] = 0.8f;

    std::string tried;
    for (const modules::ModuleEntry* e : m->host.Available(iface::ModuleKind::Audio)) {
        iface::AudioInput in{};
        iface::AudioOutput out{};
        iface::InitHeader(in, iface::ModuleKind::Audio, iface::kAudioInterfaceVer);
        iface::InitHeader(out, iface::ModuleKind::Audio, iface::kAudioInterfaceVer);
        in.host = m->files->Services(moduleId);
        in.flags = offline ? iface::kAudioOffline : 0u;
        in.settings = m->settings;
        std::string error;
        if (m->host.Init(*e, &in.header, &out.header, &error) != Status::Ok) {
            tried += "\n  " + e->path.filename().string() + ": " + error;
            continue;
        }
        if (moduleId == out.info.id) {
            m->out = out;
            break;
        }
        tried += "\n  " + e->path.filename().string() + ": id " + out.info.id;
        out.api->destroy(out.instance);
    }
    if (!m->out.instance) {
        for (const modules::ModuleEntry& e : m->host.Entries())
            if (!e.compatible && e.kind == iface::ModuleKind::Audio)
                tried += "\n  " + e.path.filename().string() + ": interface " + std::to_string(e.interfaceVer) +
                         ", the game needs " + std::to_string(iface::kAudioInterfaceVer);
        return "no audio module \"" + moduleId + "\" in " + modulesDir.string() + (tried.empty() ? "" : ":") + tried;
    }
    return "";
}

std::string Sound::CreateCar(const CarSound& car)
{
    if (!m || !m->out.instance) return "no audio module";
    std::vector<std::string> bangs, idle;
    std::string missing;
    for (auto [dir, list] : {std::pair{&car.bangs, &bangs}, std::pair{&car.idleBangs, &idle}}) {
        if (dir->empty()) continue;
        *list = WavFiles(*dir);
        if (list->empty()) missing += *dir + ": no .wav\n";
    }
    std::vector<const char*> bangPtrs, idlePtrs;
    for (const std::string& s : bangs) bangPtrs.push_back(s.c_str());
    for (const std::string& s : idle) idlePtrs.push_back(s.c_str());
    iface::CarSoundDesc d{};
    d.car = car.car;
    d.engine = car.engine.c_str();
    d.bank = "";
    d.mix = car.mixText.c_str();
    d.bangs = bangPtrs.data(), d.bangCount = static_cast<uint32_t>(bangPtrs.size());
    d.idleBangs = idlePtrs.data(), d.idleBangCount = static_cast<uint32_t>(idlePtrs.size());
    if (m->out.api->createCarVoice(m->out.instance, &d, &m->voice) != Status::Ok)
        return m->out.api->lastError(m->out.instance);
    if (!missing.empty()) {
        std::lock_guard<std::mutex> lock(m->logLock);
        m->log += missing;
    }
    return "";
}

void Sound::Connect(signals::Bus* bus)
{
    if (!m || !m->out.instance) return;
    m->Disconnect();
    m->bus = bus;
    if (!bus) return;
    m->Forward<signals::CarInfo>();
    m->Forward<signals::EngineInfo>();
    m->Forward<signals::EngineTick>();
    m->Forward<signals::DriveTick>();
    m->Forward<signals::WheelsTick>();
    m->Forward<signals::BodyTick>();
    m->Forward<signals::StepEnd>();
    m->Forward<signals::GearChange>();
    m->Forward<signals::Collision>();
    m->Forward<signals::DriverTick>();
}

std::string Sound::SetMix(const std::string& text)
{
    if (!m || !m->voice) return "no car voice";
    if (m->out.api->setCarMix(m->out.instance, m->voice, text.c_str()) != Status::Ok)
        return m->out.api->lastError(m->out.instance);
    return "";
}

void Sound::SetMuted(bool muted)
{
    m_muted = muted;
    if (!m || !m->out.instance) return;
    m->settings.muted = muted ? 1u : 0u;
    m->out.api->applySettings(m->out.instance, &m->settings);
}

uint32_t Sound::RenderOffline(float* out, uint32_t frames)
{
    return m && m->out.instance ? m->out.api->renderOffline(m->out.instance, out, frames) : 0;
}

std::string Sound::TakeLog()
{
    if (!m) return "";
    std::lock_guard<std::mutex> lock(m->logLock);
    std::string s;
    s.swap(m->log);
    return s;
}

std::string Sound::ModuleName() const
{
    if (!m || !m->out.instance) return "none";
    return std::string(m->out.info.name) + " (" + m->out.info.id + ")";
}

} // namespace wrally::sound
