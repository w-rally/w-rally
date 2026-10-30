#include "audio/sample.h"
#include "audio/files.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// stb_vorbis's declarations (its code: library stb-vorbis).
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

namespace audio {

namespace {

std::string ReadFile(const std::string& path, std::vector<uint8_t>& d)
{
    return ReadWholeFile(path, d) ? "" : "cannot open " + path;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

uint16_t U16(const std::vector<uint8_t>& d, size_t at)
{
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
}

void S16(const uint8_t* p, uint32_t count, std::vector<float>& out)
{
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        int16_t v;
        std::memcpy(&v, p + 2 * size_t(i), 2);
        out[i] = v / 32768.0f;
    }
}

} // namespace

std::string ReadWav(const std::string& path, Sample& out)
{
    std::vector<uint8_t> d;
    if (std::string e = ReadFile(path, d); !e.empty()) return e;
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return path + ": not a .wav";
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    for (size_t at = 12; at + 8 <= d.size();) {
        const uint32_t size = U32(d, at + 4);
        const size_t body = at + 8;
        if (!std::memcmp(d.data() + at, "fmt ", 4)) {
            format = U16(d, body), channels = U16(d, body + 2), rate = U32(d, body + 4), bits = U16(d, body + 14);
        } else if (!std::memcmp(d.data() + at, "data", 4)) {
            if (format != 1 || !channels || (bits != 8 && bits != 16)) return path + ": not 8 / 16-bit PCM";
            const uint32_t bytes = static_cast<uint32_t>(std::min<size_t>(size, d.size() - body));
            const uint32_t frames = bytes / (channels * bits / 8);
            out.rate = static_cast<float>(rate);
            out.data.assign(frames, 0.0f);
            for (uint32_t i = 0; i < frames; ++i) {
                float sum = 0;
                for (uint16_t c = 0; c < channels; ++c) {
                    const size_t p = body + (size_t(i) * channels + c) * (bits / 8);
                    if (bits == 16) {
                        int16_t v;
                        std::memcpy(&v, d.data() + p, 2);
                        sum += v / 32768.0f;
                    } else {
                        sum += (d[p] - 128) / 128.0f;
                    }
                }
                out.data[i] = sum / channels;
            }
            return "";
        }
        at = body + size + (size & 1);
    }
    return path + ": no data";
}

std::string ReadOgg(const std::string& path, Sample& out)
{
    std::vector<uint8_t> d;
    if (std::string e = ReadFile(path, d); !e.empty()) return e;
    int channels = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(d.data(), static_cast<int>(d.size()), &channels, &rate, &pcm);
    if (frames < 0 || !pcm || channels <= 0) {
        std::free(pcm);
        return path + ": not an Ogg Vorbis file";
    }
    out.rate = static_cast<float>(rate);
    out.data.assign(static_cast<size_t>(frames), 0.0f);
    for (int i = 0; i < frames; ++i) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) sum += pcm[size_t(i) * channels + c] / 32768.0f;
        out.data[size_t(i)] = sum / channels;
    }
    std::free(pcm);   // (stb_vorbis allocates with malloc)
    return "";
}

std::string ReadSound(const std::string& path, Sample& out)
{
    std::vector<uint8_t> d;
    if (std::string e = ReadFile(path, d); !e.empty()) return e;
    if (d.size() >= 4 && !std::memcmp(d.data(), "OggS", 4)) return ReadOgg(path, out);
    return ReadWav(path, out);
}

std::string ReadEng(const std::string& path, std::vector<EngineLoop>& out, std::string* warning)
{
    std::vector<uint8_t> d;
    if (std::string e = ReadFile(path, d); !e.empty()) return e;
    if (d.size() < 12 || std::memcmp(d.data(), "ENGI", 4)) return path + ": not an .eng";
    const uint32_t points = U32(d, 8);
    size_t at = 12 + 4 * size_t(points);
    out.clear();
    while (at + 36 <= d.size()) {
        if (std::memcmp(d.data() + at, "PART", 4) || std::memcmp(d.data() + at + 20, "WAVE", 4))
            return path + ": no PART / WAVE at " + std::to_string(at);
        EngineLoop l;
        l.rpm = static_cast<float>(U32(d, at + 8));
        l.load = U32(d, at + 12) / 100.0f;
        l.sample.rate = static_cast<float>(U32(d, at + 28));
        const uint32_t count = U32(d, at + 32);
        const size_t data = at + 36;
        if (data + 2 * size_t(count) > d.size()) return path + ": a loop runs past the file's end";
        S16(d.data() + data, count, l.sample.data);
        out.push_back(std::move(l));
        // The seek table after the samples; else (a mod's own layout) the next PART.
        const size_t table = data + 2 * size_t(count), next = table + 4 + 8 * size_t(U32(d, table));
        if (next + 4 <= d.size() && !std::memcmp(d.data() + next, "PART", 4)) {
            at = next;
            continue;
        }
        if (next + 4 >= d.size() && next <= d.size() + 4) break;   // the last loop (a few trailing bytes)
        static const char kPart[] = "PART";
        const auto it = std::search(d.begin() + table, d.end(), kPart, kPart + 4);
        if (it == d.end()) break;
        if (warning) *warning = path + ": loops not followed by the usual seek table (read on at the next PART)";
        at = static_cast<size_t>(it - d.begin());
    }
    return out.empty() ? path + ": no loops" : "";
}

} // namespace audio
