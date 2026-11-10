// A PLACEHOLDER TREE, generated in code, put in place of a stage's tree cards (vegetation plan, workstream 2's proof):
// one model in the .ngg's library (NGGD) and a placement (NGGE) per plant the file tags "tree-card", each replacing its
// plant. It stands in for workstream 4's Blender models until they exist; nothing in it comes from RBR (the shapes and
// the textures are made here), the plants give only where each tree stands and how big it is.
//
// The model: a conifer 10 m tall at scale 1 - a tapering trunk and tiers of needle cones - in three levels of detail:
//   LOD0  trunk 8 sides, 7 tiers of 18 segments (142 triangles)      to 50 m
//   LOD1  trunk 4 sides, 4 tiers of 8 segments (40 triangles)        to 150 m
//   LOD2  the far billboard, one quad with a picture of the tree       to 900 m
// (distances at scale 1: a placement's scale with its height), crossfading over 6 m. The ray tracer takes LOD1 by
// default (--rt-lod 0 for LOD0): measured on stage 41, its rays' alpha tests through 7 needle tiers cost 16.3 ms of
// ray tracing against 12.6 ms through 4 (8.8 ms with RBR's cards) - NGG design 5.0w.
#include "placeholder_tree.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

const float kPi = 3.14159265f;
const float kHeight = 10.0f;      // the model's height at scale 1
const float kRadius = 2.4f;       // its lowest tier's radius

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float Rand01(uint32_t a, uint32_t b) { return float(Hash(a * 9781u + Hash(b)) & 0xFFFFFF) / 16777216.0f; }

// Value noise, tileable over `period` cells.
float Noise(float x, float y, int period, uint32_t seed)
{
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    auto at = [&](int i, int j) {
        return Rand01(uint32_t(((i % period) + period) % period) * 131u + uint32_t(((j % period) + period) % period), seed);
    };
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

struct Rgba { uint8_t r, g, b, a; };

// BC1 (DXT1) of an RGBA image, w and h multiples of 4. A block with any texel under alpha 128 uses the three-colour
// mode, index 3 transparent (1-bit alpha: what an alpha-tested texture needs).
std::vector<uint8_t> EncodeBc1(const std::vector<Rgba>& img, int w, int h)
{
    auto pack = [](int r, int g, int b) { return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); };
    auto unpack = [](uint16_t c, int out[3]) {
        out[0] = ((c >> 11) & 31) * 255 / 31; out[1] = ((c >> 5) & 63) * 255 / 63; out[2] = (c & 31) * 255 / 31;
    };
    std::vector<uint8_t> out;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            Rgba px[16];
            bool transparent = false;
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (int k = 0; k < 16; ++k) {
                px[k] = img[size_t(by + k / 4) * w + bx + k % 4];
                if (px[k].a < 128) { transparent = true; continue; }
                const int c[3] = {px[k].r, px[k].g, px[k].b};
                for (int i = 0; i < 3; ++i) lo[i] = std::min(lo[i], c[i]), hi[i] = std::max(hi[i], c[i]);
            }
            if (lo[0] > hi[0]) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
            uint16_t c0 = pack(hi[0], hi[1], hi[2]), c1 = pack(lo[0], lo[1], lo[2]);
            // Four-colour mode needs c0 > c1, three-colour (with transparency) c0 <= c1.
            if (transparent ? c0 > c1 : c0 < c1) std::swap(c0, c1);
            if (!transparent && c0 == c1) { if (c1) --c1; else ++c0; }
            int p[4][3];
            unpack(c0, p[0]);
            unpack(c1, p[1]);
            const int levels = transparent ? 3 : 4;
            for (int i = 0; i < 3; ++i) {
                if (transparent) p[2][i] = (p[0][i] + p[1][i]) / 2;
                else p[2][i] = (2 * p[0][i] + p[1][i]) / 3, p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
            }
            uint32_t bits = 0;
            for (int k = 0; k < 16; ++k) {
                uint32_t best = 0;
                if (transparent && px[k].a < 128) best = 3;
                else {
                    int bestD = 1 << 30;
                    for (int l = 0; l < levels; ++l) {
                        const int dr = px[k].r - p[l][0], dg = px[k].g - p[l][1], db = px[k].b - p[l][2];
                        const int d = dr * dr + dg * dg + db * db;
                        if (d < bestD) bestD = d, best = uint32_t(l);
                    }
                }
                bits |= best << (2 * k);
            }
            out.push_back(uint8_t(c0)); out.push_back(uint8_t(c0 >> 8));
            out.push_back(uint8_t(c1)); out.push_back(uint8_t(c1 >> 8));
            for (int k = 0; k < 4; ++k) out.push_back(uint8_t(bits >> (8 * k)));
        }
    return out;
}

std::vector<uint8_t> Dds(const std::vector<Rgba>& img, int w, int h)
{
    std::vector<uint8_t> dds(128, 0);
    std::memcpy(dds.data(), "DDS ", 4);
    const uint32_t header[] = {124, 0x81007, uint32_t(h), uint32_t(w), uint32_t(w * h / 2), 0, 1};
    std::memcpy(dds.data() + 4, header, sizeof header);
    const uint32_t pf[] = {32, 4};
    std::memcpy(dds.data() + 76, pf, sizeof pf);
    std::memcpy(dds.data() + 84, "DXT1", 4);
    const uint32_t caps = 0x1000;
    std::memcpy(dds.data() + 108, &caps, 4);
    const std::vector<uint8_t> blocks = EncodeBc1(img, w, h);
    dds.insert(dds.end(), blocks.begin(), blocks.end());
    return dds;
}

// Needles: sprays of dark strokes slanting down from a twig, gaps between them (alpha), lighter tips.
std::vector<uint8_t> NeedleTexture()
{
    const int n = 256;
    std::vector<Rgba> img(size_t(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float u = x / float(n), v = y / float(n);
            // Eight sprays across, each a band of needles leaning from its centre line.
            const float band = u * 8.0f;
            const float across = std::fabs(band - std::floor(band) - 0.5f) * 2.0f;          // 0 on a twig, 1 between
            const float needle = 0.5f + 0.5f * std::sin((v * 48.0f + across * 6.0f) * 2.0f * kPi);
            const float rough = Noise(u * 32.0f, v * 32.0f, 32, 7u);
            const float cover = (1.0f - across * 0.85f) * (0.55f + 0.45f * needle) + (rough - 0.5f) * 0.5f;
            const bool opaque = cover > 0.45f && v > 0.03f;
            const float shade = 0.55f + 0.3f * rough + 0.25f * (1.0f - across);
            img[size_t(y) * n + x] = {uint8_t(std::min(255.0f, 34 * shade)), uint8_t(std::min(255.0f, 68 * shade)),
                                      uint8_t(std::min(255.0f, 30 * shade)), uint8_t(opaque ? 255 : 0)};
        }
    return Dds(img, n, n);
}

std::vector<uint8_t> BarkTexture()
{
    const int n = 64;
    std::vector<Rgba> img(size_t(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float f = Noise(x / 4.0f, y / 16.0f, 16, 3u) * 0.7f + Noise(x / 2.0f, y / 2.0f, 32, 5u) * 0.3f;
            const float s = 0.6f + 0.5f * f;
            img[size_t(y) * n + x] = {uint8_t(78 * s), uint8_t(58 * s), uint8_t(42 * s), 255};
        }
    return Dds(img, n, n);
}

// The billboard's picture: the same tree seen from the side - the tiers as ragged triangles, the trunk below.
std::vector<uint8_t> BillboardTexture()
{
    const int w = 128, h = 256;
    std::vector<Rgba> img(size_t(w) * h, Rgba{30, 52, 26, 0});
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = (x + 0.5f) / w * 2.0f * kRadius - kRadius;    // metres across
            const float py = (1.0f - (y + 0.5f) / h) * kHeight;             // metres up
            bool leaf = false;
            for (int t = 0; t < 7; ++t) {
                const float base = 1.6f + t * 1.1f, top = base + 2.6f;
                const float r = kRadius * (1.0f - t / 8.0f);
                if (py < base || py > top) continue;
                const float half = r * (top - py) / (top - base);
                const float edge = 1.0f + (Noise(px * 6.0f, py * 6.0f, 64, 11u) - 0.5f) * 0.35f;
                if (std::fabs(px) < half * edge) leaf = true;
            }
            const bool trunk = !leaf && std::fabs(px) < 0.16f && py < 9.0f;
            const float shade = 0.7f + 0.3f * Noise(px * 5.0f, py * 5.0f, 64, 13u) + 0.15f * (px / kRadius);
            if (leaf) img[size_t(y) * w + x] = {uint8_t(30 * shade), uint8_t(62 * shade), uint8_t(27 * shade), 255};
            else if (trunk) img[size_t(y) * w + x] = {70, 52, 38, 255};
        }
    return Dds(img, w, h);
}

ngg::Vertex V(float x, float y, float z, float nx, float ny, float nz, float u, float v)
{
    ngg::Vertex o{};
    o.pos[0] = x, o.pos[1] = y, o.pos[2] = z;
    const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
    o.normal[0] = nx / l, o.normal[1] = ny / l, o.normal[2] = nz / l;
    o.uv0[0] = u, o.uv0[1] = v;
    o.uv1[0] = u, o.uv1[1] = v;
    o.colour = 0xFFFFFFFFu;
    return o;
}

// One submesh into the library: its vertices and indices appended, the record returned.
ngg::ModelSubmesh Submesh(ngg::ModelLibrary& lib, uint32_t material, const std::vector<ngg::Vertex>& v,
                          const std::vector<uint32_t>& i)
{
    ngg::ModelSubmesh s{};
    s.material = material;
    s.firstVertex = uint32_t(lib.vertices.size());
    s.vertexCount = uint32_t(v.size());
    s.firstIndex = uint32_t(lib.indices.size());
    s.indexCount = uint32_t(i.size());
    lib.vertices.insert(lib.vertices.end(), v.begin(), v.end());
    lib.indices.insert(lib.indices.end(), i.begin(), i.end());
    return s;
}

void Trunk(int sides, std::vector<ngg::Vertex>& v, std::vector<uint32_t>& idx)
{
    const float r0 = 0.20f, r1 = 0.05f, top = 9.2f;
    for (int k = 0; k <= sides; ++k) {
        const float a = 2.0f * kPi * k / sides, c = std::cos(a), s = std::sin(a);
        v.push_back(V(r0 * c, 0.0f, r0 * s, c, 0.05f, s, float(k) / sides, 0.0f));
        v.push_back(V(r1 * c, top, r1 * s, c, 0.05f, s, float(k) / sides, 4.0f));
    }
    for (int k = 0; k < sides; ++k) {
        const uint32_t a = 2 * k, b = a + 2;
        idx.insert(idx.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
}

// Tiers of needle cones, apex up, each ring `segs` around; normals lean out and up as a cone's do.
void Tiers(int tiers, int segs, std::vector<ngg::Vertex>& v, std::vector<uint32_t>& idx)
{
    for (int t = 0; t < tiers; ++t) {
        const float f = tiers > 1 ? float(t) / (tiers - 1) : 0.0f;
        const float base = 1.6f + f * 6.6f, top = base + 2.6f + (tiers < 5 ? 0.9f : 0.0f);
        const float r = kRadius * (1.0f - f * 0.82f);
        const float droop = 0.35f;   // the rim hangs below the cone's line
        const uint32_t apex = uint32_t(v.size());
        for (int k = 0; k <= segs; ++k) {
            const float a = 2.0f * kPi * (k + 0.5f * t) / segs, c = std::cos(a), s = std::sin(a);
            v.push_back(V(0.0f, top, 0.0f, c, 0.8f, s, 3.0f * k / segs, 1.0f));
            v.push_back(V(r * c, base - droop, r * s, c, 0.8f, s, 3.0f * k / segs, 0.0f));
        }
        for (int k = 0; k < segs; ++k) {
            const uint32_t a = apex + 2 * k;
            idx.insert(idx.end(), {a, a + 1, a + 3});
        }
    }
}

struct PlantBox { float bmin[3], bmax[3]; uint32_t firstTag, tagCount; };

} // namespace

std::string AddPlaceholderTrees(ngg::SceneData& s, PlaceholderReport& report, uint32_t rtLod)
{
    report = PlaceholderReport{};
    if (!s.models.empty()) return "the file already has a model library";
    // The tags (NGGG) and plants (NGGP), as ngg_format.h lays them out.
    std::vector<std::string> tags;
    if (s.tagNames.size() >= 4) {
        uint32_t n = 0;
        std::memcpy(&n, s.tagNames.data(), 4);
        size_t at = 4;
        for (uint32_t i = 0; i < n && at + 4 <= s.tagNames.size(); ++i) {
            uint32_t len = 0;
            std::memcpy(&len, s.tagNames.data() + at, 4);
            at += 4;
            if (at + len > s.tagNames.size()) break;
            tags.emplace_back(reinterpret_cast<const char*>(s.tagNames.data() + at), len);
            at += len + (4 - len % 4) % 4;
        }
    }
    if (s.plants.size() < sizeof(ngg::PlantTableHeader)) return "the file has no plant table (NGGP)";
    ngg::PlantTableHeader ph;
    std::memcpy(&ph, s.plants.data(), sizeof ph);
    const size_t need = sizeof ph + size_t(ph.plantCount) * sizeof(ngg::PlantRecord) + size_t(ph.partCount) * sizeof(ngg::PlantPart) +
                        size_t(ph.tagRefCount) * 4;
    if (s.plants.size() < need) return "the plant table is truncated";
    const auto* rec = reinterpret_cast<const ngg::PlantRecord*>(s.plants.data() + sizeof ph);
    const auto* refs = reinterpret_cast<const uint32_t*>(s.plants.data() + sizeof ph + size_t(ph.plantCount) * sizeof(ngg::PlantRecord) +
                                                         size_t(ph.partCount) * sizeof(ngg::PlantPart));
    uint32_t treeTag = ~0u;
    for (size_t i = 0; i < tags.size(); ++i)
        if (tags[i] == "tree-card") treeTag = uint32_t(i);
    if (treeTag == ~0u) return "the file tags no plant tree-card";

    // Textures, after the file's.
    const uint32_t tex = uint32_t(s.textures.size());
    s.textures.push_back({"placeholder-needles.dds", NeedleTexture()});
    s.textures.push_back({"placeholder-bark.dds", BarkTexture()});
    s.textures.push_back({"placeholder-conifer-billboard.dds", BillboardTexture()});

    ngg::ModelLibrary& lib = s.models;
    ngg::Material needles = ngg::DefaultMaterial(), bark = ngg::DefaultMaterial(), board = ngg::DefaultMaterial();
    needles.baseColour = tex;
    needles.flags = ngg::kMaterialFoliage | ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided;
    needles.alphaCutoff = 0.5f;
    needles.roughness = 0.8f;
    bark.baseColour = tex + 1;
    bark.flags = ngg::kMaterialTwoSided;
    bark.roughness = 0.9f;
    board = needles;
    board.baseColour = tex + 2;
    lib.materials = {needles, bark, board};

    std::vector<ngg::Vertex> v;
    std::vector<uint32_t> i;
    auto lodOf = [&](int trunkSides, int tiers, int segs, float maxDistance) {
        ngg::ModelLod l{};
        l.firstSubmesh = uint32_t(lib.submeshes.size());
        v.clear(), i.clear();
        Trunk(trunkSides, v, i);
        lib.submeshes.push_back(Submesh(lib, 1, v, i));
        report.trianglesPerLod.push_back(uint32_t(i.size() / 3));
        v.clear(), i.clear();
        Tiers(tiers, segs, v, i);
        lib.submeshes.push_back(Submesh(lib, 0, v, i));
        report.trianglesPerLod.back() += uint32_t(i.size() / 3);
        l.submeshCount = 2;
        l.maxDistance = maxDistance;
        lib.lods.push_back(l);
    };
    lodOf(8, 7, 18, 50.0f);
    lodOf(4, 4, 8, 150.0f);
    {   // the billboard: a quad across x, up y
        v = {V(-kRadius, 0, 0, 0, 0, 1, 0, 1), V(kRadius, 0, 0, 0, 0, 1, 1, 1), V(kRadius, kHeight, 0, 0, 0, 1, 1, 0),
             V(-kRadius, kHeight, 0, 0, 0, 1, 0, 0)};
        i = {0, 2, 1, 0, 3, 2};
        ngg::ModelLod l{};
        l.firstSubmesh = uint32_t(lib.submeshes.size());
        lib.submeshes.push_back(Submesh(lib, 2, v, i));
        l.submeshCount = 1;
        l.maxDistance = 900.0f;
        l.flags = ngg::kModelLodBillboard;
        lib.lods.push_back(l);
        report.trianglesPerLod.push_back(2);
    }
    ngg::ModelRecord m{};
    std::strcpy(m.name, "placeholder-conifer");
    m.firstLod = 0;
    m.lodCount = 3;
    m.bboxMin[0] = -kRadius, m.bboxMin[1] = 0, m.bboxMin[2] = -kRadius;
    m.bboxMax[0] = kRadius, m.bboxMax[1] = kHeight, m.bboxMax[2] = kRadius;
    m.fadeWidth = 6.0f;
    m.rtLod = rtLod < 2 ? rtLod : 1;
    lib.models.push_back(m);

    // A placement per tree card: the base mid-plant on its lowest point, the height and footprint the plant's, a turn
    // and a slight tint of its own (from its index, so the file is the same every time).
    for (uint32_t p = 0; p < ph.plantCount; ++p) {
        bool tree = false;
        for (uint32_t k = 0; k < rec[p].tagCount; ++k) {
            const uint32_t r = rec[p].firstTagRef + k;
            if (r < ph.tagRefCount && refs[r] == treeTag) tree = true;
        }
        if (!tree) continue;
        const float* lo = rec[p].bboxMin;
        const float* hi = rec[p].bboxMax;
        const float height = hi[1] - lo[1], foot = std::max(hi[0] - lo[0], hi[2] - lo[2]);
        if (!(height > 0.3f) || !(foot > 0.1f)) { ++report.skipped; continue; }
        ngg::Placement q{};
        q.model = 0;
        q.plant = p;
        q.position[0] = 0.5f * (lo[0] + hi[0]);
        q.position[1] = lo[1];
        q.position[2] = 0.5f * (lo[2] + hi[2]);
        q.yaw = Rand01(p, 1u) * 2.0f * kPi;
        q.scaleY = height / kHeight;
        q.scaleXZ = std::clamp(foot / (2.0f * kRadius), 0.4f * q.scaleY, 2.5f * q.scaleY);
        const float t = 0.88f + 0.24f * Rand01(p, 2u), g = 0.92f + 0.16f * Rand01(p, 3u);
        auto c = [](float x) { return uint32_t(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
        q.tint = c(t * 0.97f) | (c(t * g) << 8) | (c(t * 0.95f) << 16) | 0xFF000000u;
        q.variants = 0xFFFFFFFFu;
        s.placements.push_back(q);
        report.minHeight = report.placements ? std::min(report.minHeight, height) : height;
        report.maxHeight = std::max(report.maxHeight, height);
        ++report.placements;
    }
    return "";
}
