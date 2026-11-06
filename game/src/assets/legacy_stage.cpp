#include "assets/legacy_stage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>

#include "assets/legacy_weather.h"
#include "data/lsp.h"
#include "formats/dds_image.h"
#include "formats/lbs_geometry.h"
#include "ngg_scene.h"

namespace wrally::assets {

namespace {

using formats::LbsMesh;
using formats::LbsPlaced;

constexpr uint32_t kTerrain = 0, kVegetation = 1, kScenery = 2, kMovable = 3;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Whether a DDS texture has any transparency in its top level: DXT1's punch-through, DXT2-5's alpha below 255, an
// uncompressed format's alpha bits. Unknown layouts count as transparent (alpha test kept: the safe side).
bool HasTransparency(const std::string& dds)
{
    if (dds.size() < 128 || dds.compare(0, 4, "DDS ")) return true;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(dds.data());
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, d + at, 4);
        return v;
    };
    const uint32_t height = u32(12), width = u32(16), pfFlags = u32(80), fourcc = u32(84), bits = u32(88), aMask = u32(104);
    const size_t blocks = size_t(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4);
    const uint8_t* p = d + 128;
    const size_t avail = dds.size() - 128;
    auto is = [&](const char* f) { return !std::memcmp(&fourcc, f, 4); };
    if ((pfFlags & 4) && is("DXT1")) {
        for (size_t b = 0; b < blocks && (b + 1) * 8 <= avail; ++b) {
            const uint8_t* k = p + b * 8;
            const uint16_t c0 = uint16_t(k[0] | k[1] << 8), c1 = uint16_t(k[2] | k[3] << 8);
            if (c0 > c1) continue;   // four colours, no transparency
            uint32_t idx;
            std::memcpy(&idx, k + 4, 4);
            for (int i = 0; i < 16; ++i)
                if (((idx >> (2 * i)) & 3) == 3) return true;
        }
        return false;
    }
    if ((pfFlags & 4) && (is("DXT2") || is("DXT3"))) {
        for (size_t b = 0; b < blocks && (b + 1) * 16 <= avail; ++b)
            for (int i = 0; i < 8; ++i)
                if (p[b * 16 + i] != 0xff) return true;
        return false;
    }
    if ((pfFlags & 4) && (is("DXT4") || is("DXT5"))) {
        for (size_t b = 0; b < blocks && (b + 1) * 16 <= avail; ++b) {
            const uint8_t* k = p + b * 16;
            const int a0 = k[0], a1 = k[1];
            int pal[8] = {a0, a1};
            if (a0 > a1)
                for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else {
                for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                pal[6] = 0, pal[7] = 255;
            }
            uint64_t idx = 0;
            for (int i = 0; i < 6; ++i) idx |= uint64_t(k[2 + i]) << (8 * i);
            for (int i = 0; i < 16; ++i)
                if (pal[(idx >> (3 * i)) & 7] < 255) return true;
        }
        return false;
    }
    if (pfFlags & 4) return true;            // another compressed format: not measured
    if (!(pfFlags & 1) || !aMask) return false;   // no alpha bits
    const size_t bytes = bits / 8;
    if (bytes < 1 || bytes > 4) return true;
    const size_t pixels = std::min(size_t(width) * height, avail / bytes);
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, p + i * bytes, bytes);
        if ((v & aMask) != aMask) return true;
    }
    return false;
}

// The stage's textures by name, from its archive: <root>/[<surface>/][<age>/]<name>.dds, the wanted surface first.
class Textures {
public:
    Textures(const std::string& rbz, int surface)
    {
        std::error_code ec;
        if (!std::filesystem::exists(rbz, ec)) return;
        m_root = Lower(std::filesystem::path(rbz).stem().string());
        for (auto& [name, bytes] : phys::data::ReadZip(rbz)) {
            std::string key = Lower(name);
            std::replace(key.begin(), key.end(), '\\', '/');
            m_files[key] = std::move(bytes);
        }
        static const char* const kSurfaces[3] = {"dry", "damp", "wet"};
        m_surfaces = {"", kSurfaces[std::clamp(surface, 0, 2)]};
        for (const char* s : kSurfaces)
            if (m_surfaces[1] != s) m_surfaces.push_back(s);
    }
    bool Open() const { return !m_files.empty(); }
    // Null when the archive has no such texture.
    const std::string* Find(const std::string& textureName) const
    {
        const std::string stem = Lower(std::filesystem::path(textureName).stem().string());
        for (const char* age : {"", "new", "normal", "worn"})
            for (const std::string& surface : m_surfaces) {
                std::string key = m_root + "/";
                if (!surface.empty()) key += surface + "/";
                if (*age) key += std::string(age) + "/";
                key += stem + ".dds";
                if (auto it = m_files.find(key); it != m_files.end()) return &it->second;
            }
        return nullptr;
    }

private:
    std::string m_root;
    std::vector<std::string> m_surfaces;
    std::map<std::string, std::string> m_files;
};

class Builder {
public:
    Builder(const formats::LbsGeometry& g, const Textures& textures) : m_g(g), m_textures(textures) {}

    ngg::SceneData scene;
    size_t missing = 0;

    // A mesh's vertices and indices appended (placed by `m` when given: row vectors, translation in 12..14). Returns
    // its first index.
    uint32_t Mesh(const LbsMesh& mesh, const float* m)
    {
        const uint32_t firstVertex = static_cast<uint32_t>(scene.vertices.size());
        const uint32_t firstIndex = static_cast<uint32_t>(scene.indices.size());
        const size_t vc = mesh.VertexCount();
        // The faces' normals (area-weighted), for a layout with none.
        std::vector<double> acc;
        if (mesh.normals.empty()) {
            acc.assign(vc * 3, 0.0);
            for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                const uint16_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
                const float* pa = &mesh.positions[a * 3];
                const float* pb = &mesh.positions[b * 3];
                const float* pc = &mesh.positions[c * 3];
                const double u[3] = {double(pb[0]) - pa[0], double(pb[1]) - pa[1], double(pb[2]) - pa[2]};
                const double v[3] = {double(pc[0]) - pa[0], double(pc[1]) - pa[1], double(pc[2]) - pa[2]};
                const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                for (uint16_t i : {a, b, c})
                    for (int k = 0; k < 3; ++k) acc[i * 3 + k] += n[k];
            }
        }
        for (size_t i = 0; i < vc; ++i) {
            ngg::Vertex v{};
            double p[3], n[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = mesh.positions[i * 3 + k];
                n[k] = mesh.normals.empty() ? acc[i * 3 + k] : mesh.normals[i * 3 + k];
            }
            if (m) {
                double q[3], r[3];
                for (int k = 0; k < 3; ++k) {
                    q[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
                    r[k] = n[0] * m[k] + n[1] * m[4 + k] + n[2] * m[8 + k];   // rotation only: RBR's placements are rigid
                }
                std::memcpy(p, q, sizeof p);
                std::memcpy(n, r, sizeof n);
            }
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; ++k) {
                v.pos[k] = static_cast<float>(p[k]);
                v.normal[k] = len > 1e-12 ? static_cast<float>(n[k] / len) : (k == 1 ? 1.0f : 0.0f);
            }
            if (!mesh.uv0.empty()) std::memcpy(v.uv0, &mesh.uv0[i * 2], 8);
            const std::vector<float>& uv1 = mesh.uv1.empty() ? mesh.uv0 : mesh.uv1;   // no second set: the first again
            if (!uv1.empty()) std::memcpy(v.uv1, &uv1[i * 2], 8);
            // D3DCOLOR 0xAARRGGBB to the .ngg's 0xAABBGGRR. The baked light stays only as the A/B reference; its
            // brightest channel measures the bake's scale (SceneHeader::bakedScale).
            if (!mesh.colours.empty()) {
                const uint32_t c = mesh.colours[i];
                v.colour = (c & 0xff00ff00u) | (c & 0xffu) << 16 | (c >> 16 & 0xffu);
                ++m_brightest[std::max({c >> 16 & 0xffu, c >> 8 & 0xffu, c & 0xffu})];
            } else {
                v.colour = 0xffffffffu;
            }
            scene.vertices.push_back(v);
        }
        for (uint16_t i : mesh.indices) scene.indices.push_back(firstVertex + i);
        return firstIndex;
    }

    // A vegetation object's connected pieces as PLANTS (the plant table, NGGP): RBR's trees and tufts are crossed
    // flat cards, and without plants the renderer lights each card by its own plane - cardboard - and a card's rays
    // hit the cards crossing it. A piece is the triangles sharing a vertex position (a tree's crossed cards share
    // their crossing edge, and separate plants do not touch - design 7.2b). Topology only: no tags (what a piece IS
    // stays Wallaby's), nothing merged, pieces wider than 50 m (the renderer's kPlantMaxWidth) left out. The object's
    // triangles are reordered by piece, so each piece is one part and the draw is unchanged.
    void Plants(uint32_t object)
    {
        const ngg::Object o = scene.objects[object];
        const uint32_t tris = o.indexCount / 3;
        uint32_t* idx = scene.indices.data() + o.firstIndex;
        std::vector<uint32_t> parent(tris);
        for (uint32_t t = 0; t < tris; ++t) parent[t] = t;
        auto find = [&](uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> at;   // a position (1 cm) -> a triangle on it
        auto cm = [](float v) { return static_cast<int64_t>(std::floor(v * 100.0f + 0.5f)); };
        for (uint32_t t = 0; t < tris; ++t)
            for (int k = 0; k < 3; ++k) {
                const float* p = scene.vertices[idx[t * 3 + k]].pos;
                const auto [it, fresh] = at.emplace(std::make_tuple(cm(p[0]), cm(p[1]), cm(p[2])), t);
                if (!fresh) parent[find(t)] = find(it->second);
            }
        // The triangles by piece, in the order the pieces first appear.
        std::map<uint32_t, uint32_t> rank;
        std::vector<uint32_t> pieceOf(tris);
        for (uint32_t t = 0; t < tris; ++t) pieceOf[t] = rank.emplace(find(t), static_cast<uint32_t>(rank.size())).first->second;
        std::vector<uint32_t> order(tris);
        for (uint32_t t = 0; t < tris; ++t) order[t] = t;
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return pieceOf[a] < pieceOf[b]; });
        std::vector<uint32_t> sorted(o.indexCount);
        for (uint32_t t = 0; t < tris; ++t)
            for (int k = 0; k < 3; ++k) sorted[t * 3 + k] = idx[order[t] * 3 + k];
        std::copy(sorted.begin(), sorted.end(), idx);
        for (uint32_t from = 0; from < tris;) {
            uint32_t to = from;
            ngg::PlantRecord r{};
            for (int j = 0; j < 3; ++j) r.bboxMin[j] = 1e30f, r.bboxMax[j] = -1e30f;
            for (; to < tris && pieceOf[order[to]] == pieceOf[order[from]]; ++to)
                for (int k = 0; k < 3; ++k) {
                    const float* p = scene.vertices[idx[to * 3 + k]].pos;
                    for (int j = 0; j < 3; ++j) r.bboxMin[j] = std::min(r.bboxMin[j], p[j]), r.bboxMax[j] = std::max(r.bboxMax[j], p[j]);
                }
            if (std::max(r.bboxMax[0] - r.bboxMin[0], r.bboxMax[2] - r.bboxMin[2]) <= 50.0f) {
                m_parts.push_back({static_cast<uint32_t>(m_plants.size()), object, o.firstIndex + from * 3, (to - from) * 3});
                m_plants.push_back(r);
            }
            from = to;
        }
    }

    void Object(uint32_t layer, uint32_t material, uint32_t firstIndex, uint32_t indexCount, int32_t key)
    {
        scene.objects.push_back({layer, material, firstIndex, indexCount});
        m_keys.push_back(key);
    }

    // The material of textures `t0`, `t1` (-1 none) with `flags` (alpha-tested: no second layer).
    uint32_t Material(int t0, int t1, uint32_t flags)
    {
        if (flags & ngg::kMaterialAlphaTest) t1 = -1;
        const auto key = std::make_tuple(t0, t1, flags);
        if (auto it = m_materials.find(key); it != m_materials.end()) return it->second;
        ngg::Material m = ngg::DefaultMaterial();
        m.baseColour = Slot(t0);
        m.detail = Slot(t1);
        m.flags = flags;
        const uint32_t id = static_cast<uint32_t>(scene.materials.size());
        scene.materials.push_back(m);
        m_materials[key] = id;
        return id;
    }

    // Whether `mesh` samples only opaque texels of texture `t`: a wall, a rock, a house of RBR's vegetation block,
    // which holds them beside the trees under the same flags - only the texture tells them apart (RBR's
    // ObjectTexturePlateOpaque, transparent only in its unused corners, against the cards' cut-out outlines). Each
    // triangle's UV footprint at 15 points, each the least alpha of its 2x2 bilinear footprint times the vertex
    // alpha, against RBR's AlphaRef (170). Unreadable: not solid (the alpha test kept, the safe side).
    bool Solid(const LbsMesh& mesh, int t)
    {
        if (mesh.uv0.empty()) return false;
        auto it = m_images.find(t);
        if (it == m_images.end()) {
            formats::Image im;
            const std::string* dds = t >= 0 && t < int(m_g.textureNames.size()) ? m_textures.Find(m_g.textureNames[t]) : nullptr;
            if (!dds || !formats::DecodeDds(std::vector<uint8_t>(dds->begin(), dds->end()), im).empty()) im = {};
            it = m_images.emplace(t, std::move(im)).first;
        }
        const formats::Image& im = it->second;
        if (im.width <= 0 || im.height <= 0) return false;
        auto alpha = [&](int x, int y) {
            x %= im.width, y %= im.height;
            if (x < 0) x += im.width;
            if (y < 0) y += im.height;
            return im.rgba[(size_t(y) * im.width + x) * 4 + 3];
        };
        for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
            const uint16_t v[3] = {mesh.indices[k], mesh.indices[k + 1], mesh.indices[k + 2]};
            // The grid as fine as the triangle is large on the texture: a point every 16 texels along its longest
            // edge, 4 to 32 steps (a big card's small gaps between leaves not stepped over).
            float edge = 0.0f;
            for (int e = 0; e < 3; ++e)
                edge = std::max(edge, std::hypot((mesh.uv0[v[(e + 1) % 3] * 2] - mesh.uv0[v[e] * 2]) * im.width,
                                                 (mesh.uv0[v[(e + 1) % 3] * 2 + 1] - mesh.uv0[v[e] * 2 + 1]) * im.height));
            const int steps = std::isfinite(edge) ? std::clamp(static_cast<int>(std::ceil(edge / 16.0f)), 4, 32) : 4;
            for (int a = 0; a <= steps; ++a)
                for (int b = 0; a + b <= steps; ++b) {
                    const float w[3] = {1.0f - float(a + b) / steps, float(a) / steps, float(b) / steps};
                    float u = 0, vv = 0, va = 0;
                    for (int i = 0; i < 3; ++i) {
                        u += w[i] * mesh.uv0[v[i] * 2];
                        vv += w[i] * mesh.uv0[v[i] * 2 + 1];
                        va += w[i] * (mesh.colours.empty() ? 255.0f : float(mesh.colours[v[i]] >> 24));
                    }
                    if (!std::isfinite(u) || !std::isfinite(vv) || std::fabs(u) > 1e4f || std::fabs(vv) > 1e4f) return false;
                    const int x0 = static_cast<int>(std::floor(u * im.width - 0.5f)), y0 = static_cast<int>(std::floor(vv * im.height - 0.5f));
                    const int lo = std::min(std::min(alpha(x0, y0), alpha(x0 + 1, y0)), std::min(alpha(x0, y0 + 1), alpha(x0 + 1, y0 + 1)));
                    if (lo * va < 170.0f * 255.0f) return false;
                }
        }
        return true;
    }

    // Whether texture `t`'s artwork has alpha to test (no texture: keep the test).
    bool HasAlpha(int t)
    {
        if (auto it = m_alpha.find(t); it != m_alpha.end()) return it->second;
        const std::string* dds = t >= 0 && t < int(m_g.textureNames.size()) ? m_textures.Find(m_g.textureNames[t]) : nullptr;
        return m_alpha[t] = !dds || HasTransparency(*dds);
    }

    void Finish()
    {
        if (std::any_of(m_keys.begin(), m_keys.end(), [](int32_t k) { return k >= 0; })) scene.objectKeys = m_keys;
        // The plant table (NGGP): its header, the plants, their parts; no tags (NGGG left out).
        if (!m_plants.empty()) {
            const ngg::PlantTableHeader h{static_cast<uint32_t>(m_plants.size()), static_cast<uint32_t>(m_parts.size()), 0, 0};
            auto put = [&](const void* p, size_t n) { scene.plants.insert(scene.plants.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
            put(&h, sizeof h);
            put(m_plants.data(), m_plants.size() * sizeof(ngg::PlantRecord));
            put(m_parts.data(), m_parts.size() * sizeof(ngg::PlantPart));
        }
        // The bake's scale: 255 over its 99th percentile (a few stray bright vertices would read a dim bake as full).
        uint64_t total = 0;
        for (uint64_t n : m_brightest) total += n;
        int p99 = 255;
        if (total)
            for (uint64_t seen = 0, v = 0; v < 256; ++v)
                if ((seen += m_brightest[v]) >= total * 99 / 100) {
                    p99 = static_cast<int>(v);
                    break;
                }
        scene.bakedScale = p99 > 0 ? std::min(255.0f / p99, 8.0f) : 1.0f;
    }

private:
    uint32_t Slot(int t)
    {
        if (t < 0 || t >= int(m_g.textureNames.size())) return ngg::kNoTexture;
        if (auto it = m_slots.find(t); it != m_slots.end()) return it->second;
        const std::string* dds = m_textures.Find(m_g.textureNames[t]);
        uint32_t slot = ngg::kNoTexture;
        if (dds) {
            slot = static_cast<uint32_t>(scene.textures.size());
            scene.textures.push_back({m_g.textureNames[t], std::vector<uint8_t>(dds->begin(), dds->end())});
        } else {
            ++missing;
        }
        return m_slots[t] = slot;
    }

    const formats::LbsGeometry& m_g;
    const Textures& m_textures;
    std::map<std::tuple<int, int, uint32_t>, uint32_t> m_materials;
    std::map<int, uint32_t> m_slots;
    std::map<int, bool> m_alpha;
    std::map<int, formats::Image> m_images;   // (Solid's: the decoded textures)
    std::vector<int32_t> m_keys;
    uint64_t m_brightest[256] = {};
    std::vector<ngg::PlantRecord> m_plants;
    std::vector<ngg::PlantPart> m_parts;
};

} // namespace

std::string ImportLegacyStage(const std::string& lbsPath, int surface, LegacyImport& out)
{
    out = LegacyImport{};
    std::ifstream f(lbsPath, std::ios::binary);
    if (!f) return "cannot open " + lbsPath;
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    formats::LbsGeometry g;
    try {
        g = formats::ReadLbsGeometry(file);
    } catch (const std::exception& e) {
        return lbsPath + ": " + e.what();
    }
    if (g.ground.empty() && g.scenery.empty() && g.vegetation.empty()) return lbsPath + ": no geometry to draw";

    const std::filesystem::path lbs(lbsPath);
    const std::string rbz = (lbs.parent_path() / (lbs.stem().string() + "_textures.rbz")).string();
    std::unique_ptr<Textures> textures;
    try {
        textures = std::make_unique<Textures>(rbz, surface);
    } catch (const std::exception& e) {
        out.log += "textures: " + std::string(e.what()) + "\n";
        textures = std::make_unique<Textures>("", surface);
    }
    if (!textures->Open()) out.log += "textures: no " + rbz + " - drawn untextured\n";

    Builder b(g, *textures);
    // The ground: each region's sub-meshes once, then its chunks as objects (RBR draws visibility 1 and 2 only; the
    // rest is hidden geometry, darker patches over the road if drawn).
    size_t hidden = 0;
    for (const formats::LbsGroundRegion& r : g.ground) {
        uint32_t first[5] = {};
        bool has[5] = {};
        for (int s = 0; s < 5; ++s)
            if (!r.submeshes[s].Empty()) first[s] = b.Mesh(r.submeshes[s], nullptr), has[s] = true;
        for (const formats::LbsGroundChunk& ch : r.chunks) {
            if (ch.submesh < 0 || ch.submesh > 4 || !has[ch.submesh] || !ch.faceCount || ch.texture1 < 0) continue;
            if (ch.visibility != 1 && ch.visibility != 2) {
                ++hidden;
                continue;
            }
            const LbsMesh& m = r.submeshes[ch.submesh];
            if (uint64_t(ch.firstIndex) + uint64_t(ch.faceCount) * 3 > m.indices.size())
                return lbsPath + ": a ground chunk runs past its mesh";
            b.Object(kTerrain, b.Material(ch.texture1, ch.texture2, 0), first[ch.submesh] + ch.firstIndex, ch.faceCount * 3, -1);
        }
    }
    // Vegetation: alpha-tested, two-sided, one texture (both slots the same: the blend a no-op).
    // Each object's connected pieces its plants (Builder::Plants). The block's solids - walls, rocks, houses, which
    // sample only opaque texels (Builder::Solid) - are scenery: not alpha-tested, not plants, not lit as foliage.
    size_t solids = 0;
    for (const LbsPlaced& p : g.vegetation) {
        const uint32_t first = b.Mesh(p.mesh, nullptr), count = static_cast<uint32_t>(p.mesh.indices.size());
        if (b.Solid(p.mesh, p.texture)) {
            b.Object(kScenery, b.Material(p.texture, -1, ngg::kMaterialTwoSided), first, count, -1);   // (one layer: uv1 is not theirs)
            ++solids;
            continue;
        }
        b.Object(kVegetation, b.Material(p.texture, p.texture, ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided), first, count, -1);
        b.Plants(static_cast<uint32_t>(b.scene.objects.size() - 1));
    }
    // Scenery (world space) and the movable objects (each placement its own geometry, keyed): alpha-tested only where
    // the texture has alpha to test.
    for (const auto& [layer, list] : {std::make_pair(kScenery, &g.scenery), std::make_pair(kMovable, &g.movable)})
        for (const LbsPlaced& p : *list) {
            const uint32_t flags = ngg::kMaterialTwoSided | (b.HasAlpha(p.texture) ? ngg::kMaterialAlphaTest : 0);
            b.Object(layer, b.Material(p.texture, p.texture, flags), b.Mesh(p.mesh, p.placed ? p.transform : nullptr),
                     static_cast<uint32_t>(p.mesh.indices.size()), p.placed ? p.instanceId : -1);
        }
    b.Finish();
    b.scene.flags |= ngg::kSceneFlagLegacyImport;   // (the renderer's legacy profile for its cards)
    // The stage's weathers, TrackSettings.ini's sections of its map translated (assets/legacy_weather.h), so the menu's
    // or --weather's choice is drawn: the sky, the fog, the clouds.
    out.log += LegacyWeather((lbs.parent_path() / "TrackSettings.ini").string(), lbs.stem().string(), b.scene.weather);

    if (const std::string e = ngg::Write(b.scene, out.ngg); !e.empty()) return lbsPath + ": the .ngg: " + e;
    const ngg::SceneData& s = b.scene;
    char line[320];
    std::snprintf(line, sizeof line,
                  "imported %s: %zu ground regions, %zu vegetation (%zu solid), %zu scenery, %zu movable meshes -> %zu objects, %zu "
                  "triangles, %zu materials, %zu textures (%zu missing), %zu hidden ground chunks left out, baked scale %.2f\n",
                  lbs.filename().string().c_str(), g.ground.size(), g.vegetation.size(), solids, g.scenery.size(), g.movable.size(),
                  s.objects.size(), s.indices.size() / 3, s.materials.size(), s.textures.size(), b.missing, hidden, s.bakedScale);
    out.log += line;
    for (const std::string& n : g.notes) out.log += n + "\n";
    return "";
}

} // namespace wrally::assets
