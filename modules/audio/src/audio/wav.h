#pragma once
// A WAV file of 48 kHz stereo float samples, written as they come (Audio's offline sink).

#include <cstdint>
#include <cstdio>
#include <string>

namespace audio {

class WavWriter {
public:
    bool Open(const std::string& path, uint32_t rate)
    {
        m_rate = rate;
        m_frames = 0;
        m_file = std::fopen(path.c_str(), "wb");
        if (m_file) Header();
        return m_file != nullptr;
    }
    void Write(const float* stereo, uint32_t frames)
    {
        if (!m_file) return;
        m_frames += std::fwrite(stereo, 8, frames, m_file);
    }
    void Close()   // the sizes into the header
    {
        if (!m_file) return;
        std::fseek(m_file, 0, SEEK_SET);
        Header();
        std::fclose(m_file);
        m_file = nullptr;
    }
    ~WavWriter() { Close(); }
    uint64_t Frames() const { return m_frames; }

private:
    void Header()
    {
        const uint32_t data = static_cast<uint32_t>(m_frames * 8);
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, m_file); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, m_file); };
        std::fwrite("RIFF", 1, 4, m_file);
        u32(36 + data);
        std::fwrite("WAVEfmt ", 1, 8, m_file);
        u32(16);
        u16(3);   // IEEE float
        u16(2);
        u32(m_rate);
        u32(m_rate * 8);
        u16(8);
        u16(32);
        std::fwrite("data", 1, 4, m_file);
        u32(data);
    }

    std::FILE* m_file = nullptr;
    uint32_t m_rate = 48000;
    uint64_t m_frames = 0;
};

} // namespace audio
