#include "audio/voices.h"

#include <algorithm>
#include <cmath>

namespace audio {

uint32_t Voices::Add(Sample s)
{
    std::lock_guard<std::mutex> g(m_lock);
    const uint32_t id = m_nextSound++;
    m_sounds.emplace_back(id, std::make_shared<const Sample>(std::move(s)));
    return id;
}

void Voices::Remove(uint32_t sound)
{
    std::lock_guard<std::mutex> g(m_lock);
    // (A playing instance keeps its sample alive to its end.)
    m_sounds.erase(std::remove_if(m_sounds.begin(), m_sounds.end(), [&](const auto& s) { return s.first == sound; }),
                   m_sounds.end());
}

uint32_t Voices::Play(uint32_t sound, Bus bus, float gain, float pitch, bool loop)
{
    std::lock_guard<std::mutex> g(m_lock);
    const auto it = std::find_if(m_sounds.begin(), m_sounds.end(), [&](const auto& s) { return s.first == sound; });
    if (it == m_sounds.end() || it->second->Empty()) return 0;
    Instance i;
    i.id = m_nextPlay++;
    i.sample = it->second;
    i.bus = bus < kBuses ? bus : kUi;
    i.gain = gain;
    i.step = i.sample->rate * std::max(pitch, 0.01f);
    i.loop = loop;
    m_playing.push_back(std::move(i));
    return m_playing.back().id;
}

void Voices::Stop(uint32_t play)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_playing.erase(std::remove_if(m_playing.begin(), m_playing.end(), [&](const Instance& i) { return i.id == play; }),
                    m_playing.end());
}

bool Voices::Playing(uint32_t play) const
{
    std::lock_guard<std::mutex> g(m_lock);
    return std::any_of(m_playing.begin(), m_playing.end(), [&](const Instance& i) { return i.id == play; });
}

void Voices::SetGain(Bus bus, float gain)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (bus < kBuses) m_gain[bus] = gain;
}

void Voices::SetPaused(bool paused)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_paused = paused;
}

void Voices::Mix(float* out, uint32_t frames, float rate)
{
    std::lock_guard<std::mutex> g(m_lock);
    for (Instance& i : m_playing) {
        if (m_paused && i.bus != kUi) continue;
        const std::vector<float>& d = i.sample->data;
        const double step = i.step / rate, end = static_cast<double>(d.size());
        const float gain = i.gain * m_gain[i.bus] * m_gain[kMaster];
        for (uint32_t f = 0; f < frames; ++f) {
            if (i.position >= end) {
                if (!i.loop) break;
                i.position = std::fmod(i.position, end);
            }
            const size_t k = static_cast<size_t>(i.position);
            const float u = static_cast<float>(i.position - double(k));
            const float a = d[k], b = k + 1 < d.size() ? d[k + 1] : (i.loop ? d[0] : 0.0f);
            const float v = (a + (b - a) * u) * gain;
            out[2 * f] += v;
            out[2 * f + 1] += v;
            i.position += step;
        }
    }
    m_playing.erase(std::remove_if(m_playing.begin(), m_playing.end(),
                                   [](const Instance& i) { return !i.loop && i.position >= double(i.sample->data.size()); }),
                    m_playing.end());
}

} // namespace audio
