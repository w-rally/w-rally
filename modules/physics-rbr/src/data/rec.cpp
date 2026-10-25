#include "rec.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace phys::data {

namespace {

constexpr uint32_t kWords(char t) { return t == 'C' ? 8 : t == 'P' ? 13 : t == 'G' ? 16 : 0; }

} // namespace

Recording ReadRecording(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> d;
    for (int c; (c = std::fgetc(f)) != EOF;) d.push_back(static_cast<uint8_t>(c));
    std::fclose(f);
    size_t at = 0;
    auto need = [&](size_t n) {
        if (at + n > d.size()) throw std::runtime_error(path + ": truncated");
    };
    auto u32 = [&]() {
        need(4);
        uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        at += 4;
        return v;
    };
    need(4);
    if (std::memcmp(d.data(), "PHR1", 4)) throw std::runtime_error(path + ": not a physics recording");
    at = 4;
    if (u32() != kRecVersion) throw std::runtime_error(path + ": unknown version");
    Recording r;
    r.stage = u32();
    for (uint32_t& t : r.tyres) t = u32();
    r.settings = u32();
    u32();
    for (;;) {
        if (at == d.size()) break;   // no 'E': the recording ended with the game (a crash); what is there is whole
        need(1);
        RecEvent e;
        e.type = static_cast<char>(d[at++]);
        if (e.type == 'E') break;
        if (e.type == 'K') {
            RecMesh m;
            for (uint32_t& w : m.w) w = u32();
            const uint32_t nv = u32();
            need(size_t(nv) * 16);
            m.verts.resize(size_t(nv) * 4);
            for (uint32_t& w : m.verts) w = u32();
            const uint32_t nf = u32();
            need(size_t(nf) * 7 + 1);
            m.faces.assign(d.begin() + at, d.begin() + at + size_t(nf) * 7);
            at += size_t(nf) * 7;
            const size_t n = d[at++];
            need(n);
            m.name.assign(reinterpret_cast<const char*>(d.data() + at), n);
            at += n;
            uint32_t size[3];
            for (uint32_t& s : size) s = u32();
            for (int k = 0; k < 3; ++k) {
                need(size[k]);
                m.tables[k].assign(d.begin() + at, d.begin() + at + size[k]);
                at += size[k];
            }
            r.carMeshes.push_back(std::move(m));
            continue;
        }
        if (e.type == 'A') {
            const uint32_t n = u32();
            need(size_t(n) * 12);
            r.anims.resize(n);
            for (auto& p : r.anims)
                for (uint32_t& w : p) w = u32();
            continue;
        }
        if (e.type == 'U') {
            r.setupGame = u32();
            r.tyreGame = u32();
            need(kRecSetupSize + kRecTyreTypeSize);
            r.setup.assign(d.begin() + at, d.begin() + at + kRecSetupSize);
            at += kRecSetupSize;
            r.tyreType.assign(d.begin() + at, d.begin() + at + kRecTyreTypeSize);
            at += kRecTyreTypeSize;
            continue;
        }
        if (e.type == 'S') {
            e.tick = u32();
            e.steps = u32();
        } else if (e.type == 'G') {
            need(1);
            const size_t n = d[at++];
            need(n);
            e.name.assign(reinterpret_cast<const char*>(d.data() + at), n);
            at += n;
        } else if (!kWords(e.type)) {
            throw std::runtime_error(path + ": bad record type");
        }
        for (uint32_t k = 0; k < kWords(e.type); ++k) e.words[k] = u32();
        r.events.push_back(std::move(e));
    }
    return r;
}

void WriteRecording(const std::string& path, const Recording& r)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    std::fwrite("PHR1", 1, 4, f);
    u32(kRecVersion);
    u32(r.stage);
    for (uint32_t t : r.tyres) u32(t);
    u32(r.settings);
    u32(0);
    if (r.setup.size() == kRecSetupSize && r.tyreType.size() == kRecTyreTypeSize) {
        std::fputc('U', f);
        u32(r.setupGame);
        u32(r.tyreGame);
        std::fwrite(r.setup.data(), 1, kRecSetupSize, f);
        std::fwrite(r.tyreType.data(), 1, kRecTyreTypeSize, f);
    }
    for (const RecMesh& m : r.carMeshes) {
        std::fputc('K', f);
        for (uint32_t w : m.w) u32(w);
        u32(static_cast<uint32_t>(m.verts.size() / 4));
        for (uint32_t w : m.verts) u32(w);
        u32(static_cast<uint32_t>(m.faces.size() / 7));
        std::fwrite(m.faces.data(), 1, m.faces.size(), f);
        std::fputc(static_cast<int>(m.name.size() & 0xff), f);
        std::fwrite(m.name.data(), 1, m.name.size() & 0xff, f);
        for (const auto& t : m.tables) u32(static_cast<uint32_t>(t.size()));
        for (const auto& t : m.tables) std::fwrite(t.data(), 1, t.size(), f);
    }
    if (!r.anims.empty()) {
        std::fputc('A', f);
        u32(static_cast<uint32_t>(r.anims.size()));
        for (const auto& p : r.anims)
            for (uint32_t w : p) u32(w);
    }
    for (const RecEvent& e : r.events) {
        std::fputc(e.type, f);
        if (e.type == 'S') {
            u32(e.tick);
            u32(e.steps);
        } else if (e.type == 'G') {
            std::fputc(static_cast<int>(e.name.size() & 0xff), f);
            std::fwrite(e.name.data(), 1, e.name.size() & 0xff, f);
        }
        for (uint32_t k = 0; k < kWords(e.type); ++k) u32(e.words[k]);
    }
    std::fputc('E', f);
    std::fclose(f);
}

bool ParseColMeshLine(const std::string& line, RecMesh& m)
{
    if (line.rfind("COLMESH ", 0) != 0) return false;
    m = RecMesh{};
    const char* p = line.c_str() + 8;
    char* end;
    std::strtoul(p, &end, 16);
    p = end;
    for (uint32_t& w : m.w) {
        w = std::strtoul(p, &end, 16);
        p = end;
    }
    auto hexBytes = [](const char*& t, std::vector<uint8_t>& out, size_t max) {
        while (*t == ' ') ++t;
        for (; out.size() < max && std::isxdigit(static_cast<unsigned char>(t[0])) && std::isxdigit(static_cast<unsigned char>(t[1])); t += 2) {
            const char hex[3] = {t[0], t[1], 0};
            out.push_back(static_cast<uint8_t>(std::strtoul(hex, nullptr, 16)));
        }
    };
    if (const char* v = std::strstr(p, "| v ")) {
        const uint32_t nv = std::strtoul(v + 4, &end, 10);
        p = end;
        for (uint32_t k = 0; k < 4 * nv; ++k) {
            m.verts.push_back(std::strtoul(p, &end, 16));
            p = end;
        }
    }
    if (const char* f = std::strstr(p, "| f ")) {
        const uint32_t nf = std::strtoul(f + 4, &end, 10);
        p = end;
        hexBytes(p, m.faces, size_t(nf) * 7);
    }
    if (const char* q = std::strchr(p, '"')) {
        const char* e = std::strrchr(q, '"');
        if (e > q) m.name.assign(q + 1, e);
        if (const char* t = std::strstr(e, "| tables ")) {
            t += 9;
            for (std::vector<uint8_t>& tab : m.tables) {
                hexBytes(t, tab, ~size_t(0));
                if (*t == '-') ++t;
            }
        }
    }
    return true;
}

} // namespace phys::data
