#include "lbs.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace phys::data {

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    size_t at, end;
    void Need(size_t n) const
    {
        if (at + n > end) throw std::runtime_error(".lbs: a script block is truncated");
    }
    uint32_t U32()
    {
        Need(4);
        uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        at += 4;
        return v;
    }
    float F32()
    {
        const uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    std::string Str()
    {
        const size_t from = at;
        while (at < end && d[at]) ++at;
        if (at == end) throw std::runtime_error(".lbs: a script block's string runs past it");
        return std::string(reinterpret_cast<const char*>(d.data() + from), at++ - from);
    }
};

} // namespace

LbsScripts ReadLbsScripts(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> d;
    std::fseek(f, 0, SEEK_END);
    d.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const size_t got = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (got != d.size()) throw std::runtime_error("cannot read " + path);
    LbsScripts s;
    for (size_t at = 0; d.size() - at >= 16;) {
        uint32_t h[4];
        std::memcpy(h, d.data() + at, 16);
        const size_t payload = at + 16;
        if (payload + h[3] > d.size()) throw std::runtime_error(path + ": a block runs past the file");
        Reader r{d, payload, payload + h[3]};
        if (h[1] == 2 && h[2] == 14) {
            for (uint32_t n = r.U32(), k = 0; k < n; ++k) {
                LbsScriptObject o;
                o.name = r.Str();
                o.id = r.U32();
                o.variant = r.U32();
                o.group = r.U32();
                o.loadScript = r.Str();
                o.runScript = r.Str();
                for (float* v : {o.position, o.rotation, o.scale, o.boxCentre, o.boxHalfSize})
                    for (int i = 0; i < 3; ++i) v[i] = r.F32();
                o.colour = r.U32();
                s.objects.push_back(std::move(o));
            }
            if (r.at != r.end) throw std::runtime_error(path + ": the script objects do not fill their block");
        } else if (h[1] == 2 && h[2] == 11) {
            if (h[3] != 88) throw std::runtime_error(path + ": the car info is not 88 bytes");
            for (float& v : s.carMatrix) v = r.F32();
            for (float& v : s.carPosition) v = r.F32();
            r.F32();
            for (int i = 0; i < 3; ++i) r.F32();
            for (float& v : s.carAngles) v = r.F32();
            s.hasCar = true;
        } else if (h[1] == 2 && h[2] == 13) {
            for (uint32_t n = r.U32(), k = 0; k < n; ++k) {
                LbsScripts::DrivePoint p;
                for (float& v : p.position) v = r.F32();
                for (int i = 0; i < 3; ++i) r.F32();
                p.distance = r.F32();
                s.driveLine.push_back(p);
            }
            if (r.at != r.end) throw std::runtime_error(path + ": the drive line does not fill its block");
        } else if (h[1] == 2 && h[2] == 15) {
            for (uint32_t n = r.U32(), k = 0; k < n; ++k) {
                LbsScriptGroup g;
                g.name = r.Str();
                g.id = r.U32();
                g.a = r.U32();
                g.b = r.U32();
                s.groups.push_back(std::move(g));
            }
            if (r.at != r.end) throw std::runtime_error(path + ": the script groups do not fill their block");
        }
        at = payload + h[3];
    }
    return s;
}

} // namespace phys::data
