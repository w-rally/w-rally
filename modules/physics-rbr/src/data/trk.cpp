#include "trk.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace phys::data {

namespace {

class Cursor {
public:
    Cursor(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    template <class T> T Get()
    {
        Need(sizeof(T));
        T v;
        std::memcpy(&v, p_ + at_, sizeof v);
        at_ += sizeof v;
        return v;
    }
    std::string CString()
    {
        const void* z = std::memchr(p_ + at_, 0, n_ - at_);
        if (!z) throw std::runtime_error("trk: unterminated string");
        const size_t len = static_cast<const uint8_t*>(z) - (p_ + at_);
        std::string s(reinterpret_cast<const char*>(p_ + at_), len);
        at_ += len + 1;
        return s;
    }
    TrkVec3 Vec3() { return {Get<float>(), Get<float>(), Get<float>()}; }
    size_t Left() const { return n_ - at_; }
    void Need(size_t k) const
    {
        if (n_ - at_ < k) throw std::runtime_error("trk: truncated block");
    }

private:
    const uint8_t* p_;
    size_t n_, at_ = 0;
};

TrkCollisionMesh ReadMesh(Cursor& c)
{
    TrkCollisionMesh m{};
    m.name = c.CString();
    m.type = c.Get<int32_t>();
    m.surface = c.Get<uint8_t>();
    m.baked = c.Get<uint8_t>();
    m.shape = c.Get<int32_t>();
    if (m.shape == 1) {
        for (float& f : m.box) f = c.Get<float>();
    } else if (m.shape == 2) {
        for (float& f : m.shape4) f = c.Get<float>();
        m.shape4w = c.Get<uint32_t>();
    }
    const uint32_t nv = c.Get<uint32_t>();
    c.Need(size_t(nv) * 12);
    m.vertices.resize(nv);
    for (TrkVec3& v : m.vertices) v = c.Vec3();
    const uint32_t nf = c.Get<uint32_t>();
    c.Need(size_t(nf) * 13);
    m.faces.resize(nf);
    for (TrkFace& f : m.faces) {
        f.flags = c.Get<uint8_t>();
        for (uint32_t& i : f.v) {
            i = c.Get<uint32_t>();
            if (i >= nv) throw std::runtime_error("trk: face index out of range in " + m.name);
        }
    }
    const uint32_t ni = c.Get<uint32_t>();
    c.Need(size_t(ni) * 44);
    m.instances.resize(ni);
    for (TrkInstance& in : m.instances) {
        in.id = c.Get<int32_t>();
        in.translation = c.Vec3();
        in.scale = c.Vec3();
        for (float& q : in.rotation) q = c.Get<float>();
    }
    return m;
}

} // namespace

TrkFile ReadTrk(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("trk: cannot open " + path);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TrkFile out;
    size_t at = 0;
    while (at + 16 <= d.size()) {
        int32_t h[4];
        std::memcpy(h, d.data() + at, 16);
        const size_t size = static_cast<uint32_t>(h[3]);
        if (h[0] != 8 || size > d.size() - at - 16) throw std::runtime_error("trk: bad block header");
        const uint8_t* p = d.data() + at + 16;
        if (h[1] == 3 && h[2] == 22) {
            Cursor c(p, size);
            const uint32_t n = c.Get<uint32_t>();
            for (uint32_t i = 0; i < n; ++i) out.meshes.push_back(ReadMesh(c));
            if (c.Left()) throw std::runtime_error("trk: collision block not fully consumed");
        } else if (h[1] == 3 && h[2] == 20) {
            out.driveLine.assign(p, p + size);
        }
        at += 16 + size;
    }
    if (at != d.size()) throw std::runtime_error("trk: trailing bytes");
    return out;
}

int32_t TrkDefKind(int32_t type)
{
    switch (type) {   // 0x46e300's switch (types 4, 5 and the rest fall to GENERIC)
    case 1: return 25;
    case 2: return 24;
    case 3: return 13;
    case 6: return 21;
    case 7: return 28;
    case 8: return 30;
    case 9: return 31;
    case 10: return 32;
    default: return 1;
    }
}

bool TrkHasObjectHandle(int32_t type) { return type != 0 && type != 5; }

} // namespace phys::data
