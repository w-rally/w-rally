#include "ngg_scene.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace ngg {

namespace {

void Put(std::vector<uint8_t>& o, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    o.insert(o.end(), b, b + n);
}
void PutU32(std::vector<uint8_t>& o, uint32_t v) { Put(o, &v, 4); }
void Pad4(std::vector<uint8_t>& o)
{
    while (o.size() % 4) o.push_back(0);
}

// A chunk: its header, the payload, padded to 4 (the size says the unpadded bytes).
void Chunk(std::vector<uint8_t>& o, uint32_t fourcc, const std::vector<uint8_t>& payload)
{
    PutU32(o, fourcc);
    PutU32(o, static_cast<uint32_t>(payload.size()));
    Put(o, payload.data(), payload.size());
    Pad4(o);
}
template <class T> std::vector<uint8_t> Bytes(const std::vector<T>& v)
{
    std::vector<uint8_t> o(v.size() * sizeof(T));
    if (!o.empty()) std::memcpy(o.data(), v.data(), o.size());
    return o;
}

bool SlotOk(uint32_t slot, size_t textures) { return slot == kNoTexture || slot < textures; }

struct Cursor {
    const uint8_t* p;
    size_t n, at = 0;
    bool Get(void* to, size_t k)
    {
        if (k > n - at) return false;
        std::memcpy(to, p + at, k);
        at += k;
        return true;
    }
    bool U32(uint32_t& v) { return Get(&v, 4); }
};

template <class T> void PutVec(std::vector<uint8_t>& o, const std::vector<T>& v)
{
    if (!v.empty()) Put(o, v.data(), v.size() * sizeof(T));
}

} // namespace

uint32_t PlantCount(const std::vector<uint8_t>& plants)
{
    if (plants.size() < sizeof(PlantTableHeader)) return 0;
    PlantTableHeader h;
    std::memcpy(&h, plants.data(), sizeof h);
    return h.plantCount;
}

std::string CheckModels(const ModelLibrary& m, size_t textureCount)
{
    for (size_t i = 0; i < m.materials.size(); ++i) {
        const Material& t = m.materials[i];
        for (uint32_t slot : {t.baseColour, t.detail, t.normal, t.orm, t.emissive})
            if (!SlotOk(slot, textureCount)) return "model material " + std::to_string(i) + " names texture " + std::to_string(slot);
    }
    for (size_t i = 0; i < m.submeshes.size(); ++i) {
        const ModelSubmesh& s = m.submeshes[i];
        const std::string at = "model submesh " + std::to_string(i);
        if (s.material >= m.materials.size()) return at + " has no material " + std::to_string(s.material);
        if (s.indexCount == 0 || s.indexCount % 3) return at + ": not whole triangles";
        if (uint64_t(s.firstIndex) + s.indexCount > m.indices.size()) return at + " runs past the indices";
        if (s.vertexCount == 0 || uint64_t(s.firstVertex) + s.vertexCount > m.vertices.size()) return at + " runs past the vertices";
        for (uint32_t k = s.firstIndex; k < s.firstIndex + s.indexCount; ++k)
            if (m.indices[k] >= s.vertexCount) return at + ": index " + std::to_string(k) + " past its vertices";
    }
    for (size_t i = 0; i < m.lods.size(); ++i) {
        const ModelLod& l = m.lods[i];
        if (l.submeshCount == 0 || uint64_t(l.firstSubmesh) + l.submeshCount > m.submeshes.size())
            return "model LOD " + std::to_string(i) + ": its submeshes run past them";
    }
    for (size_t i = 0; i < m.models.size(); ++i) {
        const ModelRecord& r = m.models[i];
        const std::string at = "model " + std::to_string(i);
        if (!std::memchr(r.name, 0, sizeof r.name) || !r.name[0]) return at + ": its name is empty or not terminated";
        for (size_t j = 0; j < i; ++j)
            if (!std::strcmp(m.models[j].name, r.name)) return at + ": the name " + r.name + " twice";
        if (r.lodCount == 0 || uint64_t(r.firstLod) + r.lodCount > m.lods.size()) return at + ": its LODs run past them";
        // The ray-only LOD (kModelLodRayOnly): one at most, the last, after a drawn one, named by rtLod, not a
        // billboard, no distance. The drawn LODs before it keep their rules.
        const ModelLod* L = m.lods.data() + r.firstLod;
        uint32_t rayOnly = 0;
        for (uint32_t k = 0; k < r.lodCount; ++k) rayOnly += (L[k].flags & kModelLodRayOnly) ? 1 : 0;
        if (rayOnly > 1) return at + ": two ray-only LODs";
        const uint32_t drawn = r.lodCount - rayOnly;
        if (rayOnly) {
            const ModelLod& ray = L[r.lodCount - 1];
            if (!(ray.flags & kModelLodRayOnly)) return at + ": its ray-only LOD is not its last";
            if (!drawn) return at + ": no drawn LOD before its ray-only one";
            if (ray.flags & kModelLodBillboard) return at + ": its ray-only LOD is also a billboard";
            if (ray.maxDistance != 0.0f) return at + ": its ray-only LOD has a distance";
            if (r.rtLod != r.lodCount - 1) return at + ": its rtLod does not name its ray-only LOD";
        }
        for (uint32_t k = 0; k < drawn; ++k)
            if (!(L[k].maxDistance > (k ? L[k - 1].maxDistance : 0.0f)))
                return at + (k ? ": its LOD distances do not increase" : ": its first LOD has no distance");
        if (r.rtLod >= r.lodCount || (m.lods[r.firstLod + r.rtLod].flags & kModelLodBillboard))
            return at + ": its ray-traced LOD is past its LODs or a billboard";
        if (r.fadeWidth < 0.0f) return at + ": a negative fade";
    }
    return "";
}

std::string CheckPlacements(const std::vector<Placement>& p, size_t modelCount, uint32_t plantCount)
{
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i].model >= modelCount) return "placement " + std::to_string(i) + " names no model " + std::to_string(p[i].model);
        if (plantCount != kNoPlant && p[i].plant != kNoPlant && p[i].plant >= plantCount)
            return "placement " + std::to_string(i) + " replaces no plant " + std::to_string(p[i].plant);
        if (!(p[i].scaleXZ > 0.0f) || !(p[i].scaleY > 0.0f)) return "placement " + std::to_string(i) + " has no size";
    }
    return "";
}

std::string Write(const SceneData& s, std::vector<uint8_t>& out)
{
    const size_t nv = s.vertices.size(), ni = s.indices.size();
    for (size_t i = 0; i < ni; ++i)
        if (s.indices[i] >= nv) return "index " + std::to_string(i) + " is past the " + std::to_string(nv) + " vertices";
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const Object& o = s.objects[i];
        if (o.materialId >= s.materials.size()) return "object " + std::to_string(i) + " has no material " + std::to_string(o.materialId);
        if (uint64_t(o.firstIndex) + o.indexCount > ni) return "object " + std::to_string(i) + " runs past the indices";
    }
    for (size_t i = 0; i < s.materials.size(); ++i) {
        const Material& m = s.materials[i];
        for (uint32_t slot : {m.baseColour, m.detail, m.normal, m.orm, m.emissive})
            if (!SlotOk(slot, s.textures.size())) return "material " + std::to_string(i) + " names texture " + std::to_string(slot);
    }
    if (!s.objectKeys.empty() && s.objectKeys.size() != s.objects.size())
        return "object keys: " + std::to_string(s.objectKeys.size()) + " for " + std::to_string(s.objects.size()) + " objects";
    for (const WeatherRecord& w : s.weather)
        if (!std::memchr(w.name, 0, sizeof w.name)) return "a weather preset's name is not terminated";
    if (s.variants.size() > kMaxVariants) return "more than 32 stage variants";
    for (const std::string& name : s.variants)
        if (name.empty() || name.size() >= sizeof(VariantRecord::name)) return "a stage variant's name is empty or over 15 bytes";
    for (const auto& a : s.variantAliases)
        if (a.first >= s.variants.size() || a.second.empty() || a.second.size() >= sizeof(VariantAlias::name))
            return "a stage variant alias names no variant, or is empty or over 31 bytes";
    if (!s.variantMasks.empty() && s.variantMasks.size() != s.objects.size())
        return "variant masks: " + std::to_string(s.variantMasks.size()) + " for " + std::to_string(s.objects.size()) + " objects";
    if (std::string e = CheckModels(s.models, s.textures.size()); !e.empty()) return e;
    if (!s.placements.empty() && s.models.empty()) return "placements without a model library";
    if (std::string e = CheckPlacements(s.placements, s.models.models.size(), s.plants.empty() ? 0u : PlantCount(s.plants));
        !e.empty())
        return e;

    std::vector<uint8_t> o;
    const bool spawn = s.hasSpawn;
    const uint32_t chunks = 6 + (spawn ? 1 : 0) + (s.weather.empty() ? 0 : 1) + (s.objectKeys.empty() ? 0 : 1) +
                            (s.tagNames.empty() ? 0 : 1) + (s.plants.empty() ? 0 : 1) +
                            (s.variants.empty() ? 0 : 1) + (s.variantMasks.empty() ? 0 : 1) +
                            (s.variantAliases.empty() ? 0 : 1) + (s.models.empty() ? 0 : 1) +
                            (s.placements.empty() ? 0 : 1);
    const FileHeader fh{kMagic, kFormatCurrent, chunks, 0};
    Put(o, &fh, sizeof fh);

    SceneHeader h{};
    for (int k = 0; k < 3; ++k) h.bboxMin[k] = FLT_MAX, h.bboxMax[k] = -FLT_MAX;
    for (const Vertex& v : s.vertices)
        for (int k = 0; k < 3; ++k) h.bboxMin[k] = std::min(h.bboxMin[k], v.pos[k]), h.bboxMax[k] = std::max(h.bboxMax[k], v.pos[k]);
    if (s.vertices.empty())
        for (int k = 0; k < 3; ++k) h.bboxMin[k] = h.bboxMax[k] = 0;
    h.vertexCount = static_cast<uint32_t>(nv);
    h.indexCount = static_cast<uint32_t>(ni);
    h.flags = s.flags;
    h.bakedScale = s.bakedScale;
    std::vector<uint8_t> p(sizeof h);
    std::memcpy(p.data(), &h, sizeof h);
    Chunk(o, ChunkSceneHeader(), p);
    Chunk(o, ChunkVertices(), Bytes(s.vertices));
    Chunk(o, ChunkIndices(), Bytes(s.indices));
    Chunk(o, ChunkMaterials(), Bytes(s.materials));
    Chunk(o, ChunkObjects(), Bytes(s.objects));
    // NGGT: a count, then per texture its name (not padded) and its DDS (padded to 4).
    p.clear();
    PutU32(p, static_cast<uint32_t>(s.textures.size()));
    for (const Texture& t : s.textures) {
        PutU32(p, static_cast<uint32_t>(t.name.size()));
        Put(p, t.name.data(), t.name.size());
        PutU32(p, static_cast<uint32_t>(t.dds.size()));
        Put(p, t.dds.data(), t.dds.size());
        p.insert(p.end(), (4 - t.dds.size() % 4) % 4, 0);   // by the DDS's own size, as readers skip it (not the chunk's)
    }
    Chunk(o, ChunkTextures(), p);
    if (spawn) {
        p.assign(sizeof(SpawnPoint), 0);
        std::memcpy(p.data(), &s.spawn, sizeof(SpawnPoint));
        Chunk(o, ChunkSpawn(), p);
    }
    if (!s.weather.empty()) {
        const WeatherChunkHeader wh{kWeatherVersion, static_cast<uint32_t>(s.weather.size()), sizeof(WeatherRecord), 0};
        p.assign(sizeof wh, 0);
        std::memcpy(p.data(), &wh, sizeof wh);
        Put(p, s.weather.data(), s.weather.size() * sizeof(WeatherRecord));
        Chunk(o, ChunkWeather(), p);
    }
    if (!s.objectKeys.empty()) {
        p.clear();
        PutU32(p, static_cast<uint32_t>(s.objectKeys.size()));
        Put(p, s.objectKeys.data(), s.objectKeys.size() * 4);
        Chunk(o, ChunkObjectKeys(), p);
    }
    if (!s.tagNames.empty()) Chunk(o, ChunkTagNames(), s.tagNames);
    if (!s.plants.empty()) Chunk(o, ChunkPlants(), s.plants);
    if (!s.variants.empty()) {      // (names and count checked above, before anything was written)
        p.clear();
        const VariantChunkHeader vh{1, static_cast<uint32_t>(s.variants.size()), sizeof(VariantRecord), 0};
        Put(p, &vh, sizeof vh);
        for (size_t k = 0; k < s.variants.size(); ++k) {
            VariantRecord r{};
            std::memcpy(r.name, s.variants[k].data(), s.variants[k].size());
            if (k < s.variantHasSpawn.size() && s.variantHasSpawn[k] && k < s.variantSpawns.size()) {
                r.flags |= kVariantHasSpawn;
                std::memcpy(r.spawnPosition, s.variantSpawns[k].position, sizeof r.spawnPosition);
                std::memcpy(r.spawnFacing, s.variantSpawns[k].facing, sizeof r.spawnFacing);
            }
            if (k < s.variantOffsets.size()) std::memcpy(r.offset, s.variantOffsets[k].data(), sizeof r.offset);
            Put(p, &r, sizeof r);
        }
        Chunk(o, ChunkVariants(), p);
    }
    if (!s.variantMasks.empty()) {
        p.clear();
        PutU32(p, static_cast<uint32_t>(s.variantMasks.size()));
        Put(p, s.variantMasks.data(), s.variantMasks.size() * 4);
        Chunk(o, ChunkVariantMasks(), p);
    }
    if (!s.variantAliases.empty()) {
        p.clear();
        const VariantAliasHeader ah{1, static_cast<uint32_t>(s.variantAliases.size()), sizeof(VariantAlias), 0};
        Put(p, &ah, sizeof ah);
        for (const auto& a : s.variantAliases) {
            VariantAlias r{};
            r.variant = a.first;
            std::memcpy(r.name, a.second.data(), a.second.size());
            Put(p, &r, sizeof r);
        }
        Chunk(o, ChunkVariantAliases(), p);
    }
    if (!s.models.empty()) {
        const ModelLibrary& m = s.models;
        const ModelChunkHeader mh{kModelVersion, uint32_t(m.models.size()), uint32_t(m.lods.size()), uint32_t(m.submeshes.size()),
                                  uint32_t(m.materials.size()), uint32_t(m.vertices.size()), uint32_t(m.indices.size()), 0};
        p.clear();
        Put(p, &mh, sizeof mh);
        PutVec(p, m.models);
        PutVec(p, m.lods);
        PutVec(p, m.submeshes);
        PutVec(p, m.materials);
        PutVec(p, m.vertices);
        PutVec(p, m.indices);
        Chunk(o, ChunkModels(), p);
    }
    if (!s.placements.empty()) {
        const PlacementChunkHeader ph{kPlacementVersion, uint32_t(s.placements.size()), sizeof(Placement), 0};
        p.clear();
        Put(p, &ph, sizeof ph);
        PutVec(p, s.placements);
        Chunk(o, ChunkPlacements(), p);
    }
    out.swap(o);
    return "";
}

std::string Read(const void* bytes, size_t n, SceneData& out)
{
    out = SceneData{};
    Cursor c{static_cast<const uint8_t*>(bytes), n};
    FileHeader fh;
    if (!c.Get(&fh, sizeof fh) || fh.magic != kMagic) return "not an .ngg (no NGG0 header)";
    if (fh.version != kFormatCurrent)
        return "format version " + std::to_string(fh.version) + " (this reads " + std::to_string(kFormatCurrent) + ")";
    out.version = fh.version;
    bool header = false;
    SceneHeader h{};
    for (uint32_t i = 0; i < fh.chunkCount; ++i) {
        ChunkHeader ch;
        if (!c.Get(&ch, sizeof ch)) return "the chunk table is truncated";
        if (ch.byteSize > n - c.at) return "a chunk runs past the file";
        const uint8_t* d = c.p + c.at;
        const size_t size = ch.byteSize;
        c.at += size + (4 - size % 4) % 4;
        c.at = std::min(c.at, n);
        auto take = [&](auto& v) {
            using T = typename std::decay_t<decltype(v)>::value_type;
            if (size % sizeof(T)) return false;
            v.resize(size / sizeof(T));
            if (size) std::memcpy(v.data(), d, size);
            return true;
        };
        const uint32_t f = ch.fourcc;
        if (f == ChunkSceneHeader()) {
            if (size < sizeof h) return "NGGH is too short";
            std::memcpy(&h, d, sizeof h);
            header = true;
        } else if (f == ChunkVertices()) {
            if (!take(out.vertices)) return "NGGV is not whole vertices";
        } else if (f == ChunkIndices()) {
            if (!take(out.indices)) return "NGGI is not whole indices";
        } else if (f == ChunkMaterials()) {
            if (!take(out.materials)) return "NGGM is not whole materials";
        } else if (f == ChunkObjects()) {
            if (!take(out.objects)) return "NGGO is not whole objects";
        } else if (f == ChunkTextures()) {
            Cursor t{d, size};
            uint32_t count = 0;
            if (!t.U32(count)) return "NGGT has no count";
            for (uint32_t k = 0; k < count; ++k) {
                Texture x;
                uint32_t len = 0, data = 0;
                if (!t.U32(len) || len > size - t.at) return "NGGT: a name runs past it";
                x.name.assign(reinterpret_cast<const char*>(d + t.at), len);
                t.at += len;
                if (!t.U32(data) || data > size - t.at) return "NGGT: a texture runs past it";
                x.dds.assign(d + t.at, d + t.at + data);
                t.at = std::min(size, t.at + data + (4 - data % 4) % 4);
                out.textures.push_back(std::move(x));
            }
        } else if (f == ChunkSpawn()) {
            if (size < sizeof(SpawnPoint)) return "NGGS is too short";
            std::memcpy(&out.spawn, d, sizeof(SpawnPoint));
            out.hasSpawn = true;
        } else if (f == ChunkWeather()) {
            WeatherChunkHeader wh;
            if (size < sizeof wh) return "NGGW is too short";
            std::memcpy(&wh, d, sizeof wh);
            if (wh.version != kWeatherVersion || wh.recordSize < kWeatherRecordMinSize ||
                sizeof wh + uint64_t(wh.count) * wh.recordSize > size)
                return "NGGW is not understood (version " + std::to_string(wh.version) + ")";
            for (uint32_t k = 0; k < wh.count; ++k) {
                // Over the defaults: a shorter (older) record leaves the newer fields at theirs.
                WeatherRecord r = DefaultWeatherRecord();
                std::memcpy(&r, d + sizeof wh + size_t(k) * wh.recordSize, wh.recordSize < sizeof r ? wh.recordSize : sizeof r);
                r.name[sizeof r.name - 1] = 0;
                out.weather.push_back(r);
            }
        } else if (f == ChunkObjectKeys()) {
            uint32_t count = 0;
            if (size < 4 || (std::memcpy(&count, d, 4), 4 + uint64_t(count) * 4 > size)) return "NGGK is truncated";
            out.objectKeys.resize(count);
            if (count) std::memcpy(out.objectKeys.data(), d + 4, size_t(count) * 4);
        } else if (f == ChunkTagNames()) {
            out.tagNames.assign(d, d + size);
        } else if (f == ChunkPlants()) {
            out.plants.assign(d, d + size);
        } else if (f == ChunkVariants()) {
            VariantChunkHeader vh{};
            if (size < sizeof vh) return "NGGR is truncated";
            std::memcpy(&vh, d, sizeof vh);
            if (vh.version != 1 || vh.recordSize < kVariantRecordV1Bytes || vh.count > kMaxVariants ||
                sizeof vh + uint64_t(vh.count) * vh.recordSize > size)
                return "NGGR is not version 1 or is truncated";
            for (uint32_t k = 0; k < vh.count; ++k) {
                VariantRecord r{};
                std::memcpy(&r, d + sizeof vh + size_t(k) * vh.recordSize, std::min<size_t>(vh.recordSize, sizeof r));
                r.name[sizeof r.name - 1] = 0;
                out.variants.push_back(r.name);
                const bool has = (r.flags & kVariantHasSpawn) != 0 && vh.recordSize >= sizeof r;
                out.variantHasSpawn.push_back(has ? 1 : 0);
                SpawnPoint sp{};
                if (has) std::memcpy(sp.position, r.spawnPosition, sizeof sp.position), std::memcpy(sp.facing, r.spawnFacing, sizeof sp.facing);
                out.variantSpawns.push_back(sp);
                std::array<float, 3> off{};
                if (vh.recordSize >= sizeof r) std::memcpy(off.data(), r.offset, sizeof r.offset);
                out.variantOffsets.push_back(off);
            }
        } else if (f == ChunkVariantAliases()) {
            VariantAliasHeader ah{};
            if (size < sizeof ah) return "NGGA truncated";
            std::memcpy(&ah, d, sizeof ah);
            if (ah.version != 1 || ah.recordSize < sizeof(VariantAlias) || size < sizeof ah + size_t(ah.count) * ah.recordSize)
                return "NGGA not understood";
            for (uint32_t k = 0; k < ah.count; ++k) {
                VariantAlias r{};
                std::memcpy(&r, d + sizeof ah + size_t(k) * ah.recordSize, sizeof r);
                r.name[sizeof r.name - 1] = 0;
                out.variantAliases.emplace_back(r.variant, r.name);
            }
        } else if (f == ChunkVariantMasks()) {
            uint32_t count = 0;
            if (size < 4 || (std::memcpy(&count, d, 4), 4 + uint64_t(count) * 4 > size)) return "NGGQ is truncated";
            out.variantMasks.resize(count);
            if (count) std::memcpy(out.variantMasks.data(), d + 4, size_t(count) * 4);
        } else if (f == ChunkModels()) {
            ModelChunkHeader mh{};
            if (size < sizeof mh) return "NGGD is truncated";
            std::memcpy(&mh, d, sizeof mh);
            if (mh.version < kModelVersionMin || mh.version > kModelVersion)
                return "NGGD version " + std::to_string(mh.version) + " (this reads " + std::to_string(kModelVersionMin) + "-" +
                       std::to_string(kModelVersion) + ")";
            const uint64_t need = sizeof mh + uint64_t(mh.modelCount) * sizeof(ModelRecord) + uint64_t(mh.lodCount) * sizeof(ModelLod) +
                                  uint64_t(mh.submeshCount) * sizeof(ModelSubmesh) + uint64_t(mh.materialCount) * sizeof(Material) +
                                  uint64_t(mh.vertexCount) * sizeof(Vertex) + uint64_t(mh.indexCount) * 4;
            if (need != size) return "NGGD is " + std::to_string(size) + " bytes, its header says " + std::to_string(need);
            size_t at = sizeof mh;
            auto grab = [&](auto& v, uint32_t count) {
                using T = typename std::decay_t<decltype(v)>::value_type;
                v.resize(count);
                if (count) std::memcpy(v.data(), d + at, size_t(count) * sizeof(T));
                at += size_t(count) * sizeof(T);
            };
            ModelLibrary& m = out.models;
            grab(m.models, mh.modelCount);
            grab(m.lods, mh.lodCount);
            grab(m.submeshes, mh.submeshCount);
            grab(m.materials, mh.materialCount);
            grab(m.vertices, mh.vertexCount);
            grab(m.indices, mh.indexCount);
            // Version 1 had no ray-only LOD: a v1 chunk carrying the flag was written by nobody this reader knows.
            if (mh.version < 2)
                for (const ModelLod& l : m.lods)
                    if (l.flags & kModelLodRayOnly) return "NGGD version 1 with a ray-only LOD";
        } else if (f == ChunkPlacements()) {
            PlacementChunkHeader ph{};
            if (size < sizeof ph) return "NGGE is truncated";
            std::memcpy(&ph, d, sizeof ph);
            if (ph.version != kPlacementVersion || ph.recordSize < sizeof(Placement) ||
                sizeof ph + uint64_t(ph.count) * ph.recordSize > size)
                return "NGGE is not version " + std::to_string(kPlacementVersion) + " or is truncated";
            out.placements.resize(ph.count);
            for (uint32_t k = 0; k < ph.count; ++k)
                std::memcpy(&out.placements[k], d + sizeof ph + size_t(k) * ph.recordSize, sizeof(Placement));
        }
        // (an unknown chunk is skipped: that is what keeps the format open)
    }
    if (!header) return "no NGGH";
    if (h.vertexCount != out.vertices.size() || h.indexCount != out.indices.size()) return "NGGH's counts disagree with NGGV / NGGI";
    std::memcpy(out.bboxMin, h.bboxMin, sizeof h.bboxMin);
    std::memcpy(out.bboxMax, h.bboxMax, sizeof h.bboxMax);
    out.flags = h.flags;
    out.bakedScale = h.bakedScale;
    if (!out.objectKeys.empty() && out.objectKeys.size() != out.objects.size()) return "NGGK is not one key per object";
    if (!out.variantMasks.empty() && out.variantMasks.size() != out.objects.size()) return "NGGQ is not one mask per object";
    if (std::string e = CheckModels(out.models, out.textures.size()); !e.empty()) return "NGGD: " + e;
    if (!out.placements.empty() && out.models.empty()) return "NGGE without NGGD";
    if (std::string e = CheckPlacements(out.placements, out.models.models.size(), out.plants.empty() ? 0u : PlantCount(out.plants));
        !e.empty())
        return "NGGE: " + e;
    return "";
}

} // namespace ngg
