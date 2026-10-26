#include "formats/lbs_geometry.h"

#include <cstring>
#include <stdexcept>

namespace wrally::formats {

namespace {

// The vertex layouts by type: where each channel sits, in words (-1 none). Every type has its position at 0.
struct Layout {
    uint32_t type;
    int normal, colour, uv0, uv1;
};
constexpr Layout kLayouts[] = {
    {3, -1, -1, -1, -1},   // position
    {4, -1, 3, -1, -1},    // + colour
    {6, -1, 3, 4, -1},     // + uv0
    {7, -1, 3, 4, -1},     // + a scalar (6)
    {8, -1, 3, 4, 6},      // the ground's: + uv1
    {9, -1, 3, 4, 6},      // + a scalar (8): grass sway
    {11, -1, 3, 4, 6},     // + uv2 (8), a scalar (10)
    {14, 3, 6, 7, 9},      // normal, colour, uv0..2, a scalar
    {17, 3, 6, 7, 9},      // as 14, + uv3 and a scalar
};

const Layout* LayoutOf(uint32_t type)
{
    for (const Layout& l : kLayouts)
        if (l.type == type) return &l;
    return nullptr;
}

struct Cursor {
    const uint8_t* d;
    size_t n, at = 0;
    const char* what = "";

    [[noreturn]] void Fail(const std::string& why) const
    {
        throw std::runtime_error(std::string(".lbs ") + what + ": " + why + " (at byte " + std::to_string(at) + ")");
    }
    void Need(size_t k) const
    {
        if (k > n - at) Fail("truncated");
    }
    uint32_t U32()
    {
        Need(4);
        uint32_t v;
        std::memcpy(&v, d + at, 4);
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
    uint8_t U8()
    {
        Need(1);
        return d[at++];
    }
    void Skip(size_t k)
    {
        Need(k);
        at += k;
    }
    std::string Str()
    {
        const size_t from = at;
        while (at < n && d[at]) ++at;
        if (at >= n) Fail("a string runs past the block");
        return std::string(reinterpret_cast<const char*>(d + from), at++ - from);
    }
    void Done() const
    {
        if (at != n) Fail(std::to_string(n - at) + " bytes left over - the layout is wrong somewhere");
    }

    // A mesh of `type`.
    LbsMesh Mesh(uint32_t type)
    {
        const Layout* l = LayoutOf(type);
        if (!l) Fail("a mesh of unknown vertex type " + std::to_string(type));
        LbsMesh m;
        m.type = type;
        const uint32_t field = U32();
        const uint64_t count = type == 3 ? uint64_t(field) * 3 : field - field % 3;
        if (count * 2 > n - at) Fail("a mesh's indices run past the block");
        std::vector<uint16_t> indices(static_cast<size_t>(count));
        if (count) std::memcpy(indices.data(), d + at, size_t(count) * 2);
        at += size_t(count) * 2;   // (whole triangles: an index count is a multiple of 3)
        if (type == 7 || type == 9) {
            if (U8() == 2) {   // a second index block: not drawn
                const uint32_t second = U32();
                Skip(size_t(second) * 2);
            }
        }
        const uint32_t vertices = U32();
        if (vertices == 0 || indices.empty()) return m;   // empty: no vertex data follows
        const size_t stride = size_t(type) * 4;
        if (uint64_t(vertices) * stride > n - at) Fail("a mesh's vertices run past the block");
        for (uint16_t i : indices)
            if (i >= vertices) Fail("a mesh's index " + std::to_string(i) + " is past its " + std::to_string(vertices) + " vertices");
        m.indices = std::move(indices);
        m.positions.resize(size_t(vertices) * 3);
        if (l->normal >= 0) m.normals.resize(size_t(vertices) * 3);
        if (l->colour >= 0) m.colours.resize(vertices);
        if (l->uv0 >= 0) m.uv0.resize(size_t(vertices) * 2);
        if (l->uv1 >= 0) m.uv1.resize(size_t(vertices) * 2);
        for (uint32_t v = 0; v < vertices; ++v) {
            const uint8_t* p = d + at + size_t(v) * stride;
            std::memcpy(&m.positions[size_t(v) * 3], p, 12);
            if (l->normal >= 0) std::memcpy(&m.normals[size_t(v) * 3], p + l->normal * 4, 12);
            if (l->colour >= 0) std::memcpy(&m.colours[v], p + l->colour * 4, 4);
            if (l->uv0 >= 0) std::memcpy(&m.uv0[size_t(v) * 2], p + l->uv0 * 4, 8);
            if (l->uv1 >= 0) std::memcpy(&m.uv1[size_t(v) * 2], p + l->uv1 * 4, 8);
        }
        at += size_t(vertices) * stride;
        return m;
    }

    // The variable attribute record of blocks (1,1) and (1,3): flags, an optional word for each of flags 16 and 32,
    // then three words. Its bytes.
    std::vector<uint8_t> Attributes(uint32_t& flags)
    {
        const size_t from = at;
        flags = U32();
        if (flags & 16) U32();
        if (flags & 32) U32();
        Skip(12);
        return std::vector<uint8_t>(d + from, d + at);
    }
};

void TextureNames(Cursor& c, LbsGeometry& g)
{
    for (uint32_t n = c.U32(), k = 0; k < n; ++k) {
        c.Skip(16);
        g.textureNames.push_back(c.Str());
    }
}

void Ground(Cursor& c, LbsGeometry& g)
{
    static const uint32_t kTypes[5] = {4, 8, 14, 11, 17};
    const uint32_t regions = c.U32();
    c.F32();
    if (regions > (1u << 20)) c.Fail("an absurd region count " + std::to_string(regions));
    for (uint32_t r = 0; r < regions; ++r) {
        LbsGroundRegion region;
        region.submeshes[0] = c.Mesh(kTypes[0]);
        c.Skip(32);
        for (int k = 1; k < 5; ++k) region.submeshes[k] = c.Mesh(kTypes[k]);
        const uint32_t chunks = c.U32();
        if (uint64_t(chunks) * 112 > c.n - c.at) c.Fail("a region's render chunks run past the block");
        for (uint32_t k = 0; k < chunks; ++k) {
            const size_t start = c.at;
            LbsGroundChunk ch;
            ch.submesh = c.I32() - 4;
            c.Skip(8);                  // shaders
            ch.firstIndex = c.U32();    // an index offset already
            ch.faceCount = c.U32();
            c.Skip(8);                  // vertex count, first vertex
            c.Skip(24);                 // box
            c.Skip(20);                 // shadow and specular maps, the texture count
            ch.texture1 = c.I32();
            ch.texture2 = c.I32();
            c.Skip(28);
            ch.visibility = c.U8();
            c.Skip(3);
            if (c.at - start != 112) c.Fail("a render chunk is not 112 bytes");
            region.chunks.push_back(ch);
        }
        c.Skip(24);                     // box
        g.ground.push_back(std::move(region));
    }
}

void Vegetation(Cursor& c, LbsGeometry& g)
{
    for (uint32_t regions = c.U32(), r = 0; r < regions; ++r) {
        if (c.I32() == 0) continue;     // an empty region carries nothing more
        for (int list = 0; list < 2; ++list)
            for (int32_t items = c.I32(), k = 0; k < items; ++k) {
                LbsPlaced p;
                const std::vector<uint8_t> a = c.Attributes(p.flags);
                const size_t words = a.size() / 4;
                uint32_t A, C;
                std::memcpy(&A, a.data() + (words - 3) * 4, 4);
                std::memcpy(&C, a.data() + (words - 1) * 4, 4);
                const uint32_t type = (p.flags & 16) ? 9 : (C & 0xff) / 4;   // flag 16 forces the vegetation layout
                c.I32();
                p.mesh = c.Mesh(type);
                c.Skip(24);             // box
                p.texture = static_cast<int>(A & 0xff);
                if (!p.mesh.Empty()) g.vegetation.push_back(std::move(p));
            }
    }
}

void Scenery(Cursor& c, LbsGeometry& g)
{
    c.Str();                            // the group's name
    for (uint32_t n = c.U32(), k = 0; k < n; ++k) {
        c.Skip(12);                     // a reference point, not a translation
        LbsPlaced p;
        const std::vector<uint8_t> a = c.Attributes(p.flags);
        if (a.size() < 12) c.Fail("a scenery attribute record is too short");
        p.mesh = c.Mesh(a[a.size() - 8] / 4u);   // (the type sits 8 bytes from the end here, 4 in (1,1))
        p.texture = a[8];
        if (!p.mesh.Empty()) g.scenery.push_back(std::move(p));
    }
}

void Movable(Cursor& c, LbsGeometry& g)
{
    for (uint32_t objects = c.U32(), o = 0; o < objects; ++o) {
        c.Str();                        // the object's name
        c.Skip(1);
        std::vector<LbsPlaced> subs;
        for (int32_t n = c.I32(), s = 0; s < n; ++s) {
            uint8_t a[20];
            c.Need(20);
            std::memcpy(a, c.d + c.at, 20);
            c.at += 20;
            LbsPlaced p;
            p.mesh = c.Mesh(a[12] / 4u);
            p.texture = a[8];
            subs.push_back(std::move(p));
        }
        for (uint32_t n = c.U32(), i = 0; i < n; ++i) {
            const int32_t id = c.I32();
            float m[16];
            for (float& v : m) v = c.F32();
            for (const LbsPlaced& s : subs) {
                if (s.mesh.Empty()) continue;
                LbsPlaced p = s;
                p.placed = true;
                std::memcpy(p.transform, m, sizeof m);
                p.instanceId = id;
                g.movable.push_back(std::move(p));
            }
        }
    }
}

} // namespace

LbsGeometry ReadLbsGeometry(const std::vector<uint8_t>& file)
{
    LbsGeometry g;
    for (size_t at = 0; file.size() - at >= 16;) {
        uint32_t h[4];   // version, type, subtype, size
        std::memcpy(h, file.data() + at, 16);
        const size_t payload = at + 16;
        if (h[3] > file.size() - payload)
            throw std::runtime_error(".lbs: block (" + std::to_string(h[1]) + "," + std::to_string(h[2]) + ") runs past the file");
        at = payload + h[3];
        Cursor c{file.data() + payload, h[3]};
        const uint32_t kind = h[1] * 100 + h[2];
        switch (kind) {
        case 23: c.what = "(0,23) texture names", TextureNames(c, g); break;
        case 100:
            if (h[0] == 88 || h[0] == 89) {
                g.notes.push_back("the ground is encrypted (version " + std::to_string(h[0]) + "): left out");
                continue;
            }
            if (h[0] != 8) {
                g.notes.push_back("the ground's version " + std::to_string(h[0]) + " is not read (8 is): left out");
                continue;
            }
            c.what = "(1,0) ground", Ground(c, g), c.Done();
            break;
        case 101: c.what = "(1,1) vegetation", Vegetation(c, g), c.Done(); break;
        case 103: c.what = "(1,3) scenery", Scenery(c, g), c.Done(); break;
        case 106: c.what = "(1,6) movable", Movable(c, g), c.Done(); break;
        default: break;   // (gameplay and the rest: not geometry)
        }
    }
    return g;
}

} // namespace wrally::formats
