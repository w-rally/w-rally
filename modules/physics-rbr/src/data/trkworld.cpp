#include "trkworld.h"

#include <cstring>

namespace phys::data {

namespace {

uint32_t Bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

TrkMeshRecord MeshRecord(const TrkCollisionMesh& m, size_t index, uint32_t slot)
{
    TrkMeshRecord r{};
    r.mesh = index;
    r.name = m.name;
    r.w[0] = slot;
    if (m.shape == 2) {
        r.w[1] |= 0x10000;
        // 0x20 bytes: the centre, the radius at +0x10 (the rest is never written: 0 here).
        r.shape = {m.shape4[0], m.shape4[1], m.shape4[2], 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        std::memcpy(&r.shape[4], &m.shape4w, 4);
    } else if (m.shape == 1) {
        r.w[1] |= 0x20000;
        // 0x50 bytes: the centre, the half size, then the rows (1, 0, 0) (0, 0, 1) (0, 1, 0) (w lanes never written).
        r.shape = {m.box[0], m.box[1], m.box[2], 0.0f, m.box[3], m.box[4], m.box[5], 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f,      0.0f,      1.0f,      0.0f, 0.0f,      1.0f,      0.0f,      0.0f};
    }
    r.w[3] = static_cast<uint32_t>(m.vertices.size() & 0xff) | static_cast<uint32_t>(m.faces.size()) << 16;
    r.w[12] = m.surface;
    for (const TrkVec3& v : m.vertices) r.vertices.insert(r.vertices.end(), {v.x, v.y, v.z, 0.0f});
    for (const TrkFace& f : m.faces)
        r.faces.insert(r.faces.end(), {static_cast<uint8_t>(f.v[0]), static_cast<uint8_t>(f.v[1]), static_cast<uint8_t>(f.v[2]), f.flags, 0, 0, 0});
    return r;
}

} // namespace

TrkWorld BuildTrkWorld(const TrkFile& trk)
{
    TrkWorld w;
    for (size_t i = 0; i < trk.meshes.size(); ++i)
        if (!trk.meshes[i].baked) w.meshes.push_back(MeshRecord(trk.meshes[i], i, static_cast<uint32_t>(10 + w.meshes.size())));
    // The non-baked meshes' instances in file order, then the baked ones (0x46e300 keeps them for the end).
    std::vector<TrkStuffRecord> baked;
    for (size_t i = 0; i < trk.meshes.size(); ++i) {
        const TrkCollisionMesh& m = trk.meshes[i];
        if (!m.baked)
            for (size_t j = 0; j < w.meshes.size(); ++j)
                if (w.meshes[j].mesh == i) w.events.push_back({true, static_cast<uint32_t>(j)});
        for (size_t k = 0; k < m.instances.size(); ++k) {
            const TrkInstance& in = m.instances[k];
            TrkStuffRecord r{};
            r.mesh = i;
            r.instance = k;
            r.id = in.id;
            r.meshRecord = -1;
            const float ident[4] = {0, 0, 0, 1};
            const float* q = m.baked ? ident : in.rotation;
            const uint32_t head[] = {Bits(in.translation.x), Bits(in.translation.y), Bits(in.translation.z), 0,
                                     Bits(q[0]), Bits(q[1]), Bits(q[2]), Bits(q[3]),
                                     Bits(in.scale.x), Bits(in.scale.y), Bits(in.scale.z), 0};
            std::memcpy(r.w, head, sizeof head);
            if (m.baked) {
                r.meshRecord = static_cast<int32_t>(w.bakedMeshes.size());
                w.bakedMeshes.push_back(MeshRecord(m, i, 0));
                r.w[0x44 / 4] = 0x30;
            } else {
                for (const TrkMeshRecord& mr : w.meshes)
                    if (mr.mesh == i) r.w[0x34 / 4] = mr.w[0];
            }
            r.w[0x38 / 4] = static_cast<uint32_t>(TrkDefKind(m.type));
            if (!m.baked) w.events.push_back({false, static_cast<uint32_t>(w.stuff.size())});
            (m.baked ? baked : w.stuff).push_back(r);
        }
    }
    for (size_t j = 0; j < baked.size(); ++j) w.events.push_back({false, static_cast<uint32_t>(w.stuff.size() + j)});
    w.stuff.insert(w.stuff.end(), baked.begin(), baked.end());
    return w;
}

} // namespace phys::data
