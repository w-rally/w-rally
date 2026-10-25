#include "dls.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace phys::data {

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    size_t at;
    uint32_t U32()
    {
        if (at + 4 > d.size()) throw std::runtime_error(".dls: a section runs past the file");
        uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        at += 4;
        return v;
    }
    int32_t I32() { return static_cast<int32_t>(U32()); }
    float F32()
    {
        const uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    void F(float* out, int n)
    {
        for (int i = 0; i < n; ++i) out[i] = F32();
    }
    // a count that has to fit in what is left of the file at `bytes` each
    uint32_t Count(size_t bytes)
    {
        const uint32_t n = U32();
        if (n > (d.size() - at) / bytes) throw std::runtime_error(".dls: a count runs past the file");
        return n;
    }
};

} // namespace

Dls ReadDls(const std::string& path)
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
    if (d.size() < 56 || std::memcmp(d.data(), "MINAATAD", 8) != 0) throw std::runtime_error(path + ": not a .dls");
    int32_t offsets[10];
    std::memcpy(offsets, d.data() + 16, 40);
    auto at = [&](int slot) -> Reader {
        if (offsets[slot] <= 0 || static_cast<size_t>(offsets[slot]) >= d.size()) return Reader{d, d.size()};
        return Reader{d, static_cast<size_t>(offsets[slot])};
    };
    auto present = [&](int slot) { return offsets[slot] > 0 && static_cast<size_t>(offsets[slot]) < d.size(); };

    Dls dls;
    if (present(2)) {
        Reader r = at(2);
        for (uint32_t n = r.Count(56), k = 0; k < n; ++k) {
            DlsCamera c;
            c.id = r.I32();
            r.I32();
            c.path = r.I32();
            c.animationSet = r.I32();
            r.F(c.position, 3);
            r.F(c.angles, 3);
            float scale[3];
            r.F(scale, 3);
            r.I32();
            dls.cameras.push_back(c);
        }
    }
    if (present(3)) {
        Reader r = at(3);
        for (uint32_t n = r.Count(24), k = 0; k < n; ++k) {
            const int32_t id = r.I32(), aim = r.I32();
            const float zoom = r.F32(), tilt = r.F32();
            r.F32();
            r.F32();
            for (DlsCamera& c : dls.cameras)
                if (c.id == id) {
                    c.aim = aim;
                    c.zoom = zoom;
                    c.tilt = tilt;
                }
        }
    }
    if (present(1)) {
        Reader r = at(1);
        for (uint32_t n = r.Count(12), k = 0; k < n; ++k) {
            DlsPath p;
            p.id = r.U32();
            p.animationSet = r.U32();
            for (uint32_t m = r.Count(44), j = 0; j < m; ++j) {
                DlsPath::Point q;
                r.U32();
                r.F(q.position, 3);
                float v[6];
                r.F(v, 6);
                q.distance = r.F32();
                p.points.push_back(q);
            }
            dls.paths.push_back(std::move(p));
        }
    }
    if (present(0)) {
        Reader r = at(0);
        for (uint32_t n = r.Count(8), k = 0; k < n; ++k) {
            const uint32_t offset = r.U32();
            r.U32();
            Reader e{d, offset};
            uint32_t L[17];
            for (uint32_t& v : L) v = e.U32();
            if (L[9] > d.size() || L[1] > (d.size() - L[9]) / 8) throw std::runtime_error(path + ": the event ids run past the file");
            Reader ids{d, L[9]};
            std::vector<int32_t> objects(L[1]);
            for (int32_t& o : objects) {
                o = ids.I32();
                ids.I32();
            }
            Reader t{d, L[10]};
            for (uint32_t j = 0; j < L[2]; ++j) {
                const int32_t event = t.I32();
                const float distance = t.F32();
                const uint32_t end = t.U32();
                if (event < 0 || static_cast<size_t>(event) >= objects.size()) continue;
                dls.triggers.push_back({objects[event], distance, end != 0});
            }
            if (L[14] <= d.size() && L[6] <= (d.size() - L[14]) / 12) {
                Reader pn{d, L[14]};
                for (uint32_t j = 0; j < L[6]; ++j) {
                    DlsPacenote n;
                    n.type = pn.I32();
                    n.flags = pn.I32();
                    n.distance = pn.F32();
                    dls.pacenotes.push_back(n);
                }
            }
            Reader ch{d, L[13]};
            for (uint32_t j = 0; j < L[5]; ++j) {
                DlsCameraChannel c;
                c.camera = ch.I32();
                c.type = ch.U32();
                const uint32_t keys = ch.Count(28);
                ch.U32();
                for (uint32_t q = 0; q < keys; ++q) {
                    ch.U32();
                    const float distance = ch.F32(), value = ch.F32();
                    ch.F32();
                    ch.F32();
                    ch.F32();
                    ch.U32();
                    c.keys.push_back({distance, value});
                }
                dls.channels.push_back(std::move(c));
            }
        }
    }
    return dls;
}

} // namespace phys::data
