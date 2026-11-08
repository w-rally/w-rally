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

} // namespace

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
    if (!s.variantMasks.empty() && s.variantMasks.size() != s.objects.size())
        return "variant masks: " + std::to_string(s.variantMasks.size()) + " for " + std::to_string(s.objects.size()) + " objects";

    std::vector<uint8_t> o;
    const bool spawn = s.hasSpawn;
    const uint32_t chunks = 6 + (spawn ? 1 : 0) + (s.weather.empty() ? 0 : 1) + (s.objectKeys.empty() ? 0 : 1) +
                            (s.tagNames.empty() ? 0 : 1) + (s.plants.empty() ? 0 : 1) +
                            (s.variants.empty() ? 0 : 1) + (s.variantMasks.empty() ? 0 : 1);
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
            }
        } else if (f == ChunkVariantMasks()) {
            uint32_t count = 0;
            if (size < 4 || (std::memcpy(&count, d, 4), 4 + uint64_t(count) * 4 > size)) return "NGGQ is truncated";
            out.variantMasks.resize(count);
            if (count) std::memcpy(out.variantMasks.data(), d + 4, size_t(count) * 4);
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
    return "";
}

} // namespace ngg
