#include "assets/fences.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#include "formats/fnc.h"
#include "render/renderer.h"

namespace wrally::assets {

using math::V3;

namespace {

constexpr float kPostHeight = 1.1f, kPostBelow = 0.15f, kPostHalfWidth = 0.03f;
constexpr float kAttach = 1.0f / kPostHeight;   // the tape's top edge, up the post: 1 m over its foot (RBR's)
// RBR's fence STYLES (the exe's table at 0x7d81b0, 0x2c bytes a style; its strip builder 0x604610), a group's by its
// .fnc header's first word: the band's height, the texture's u across a whole segment post to post (u0 .. u1: the
// tile repeated so many times between two posts, however far apart they are) and its v down the band, and how many
// strands one under another, `step` apart. 0 and 1: a tape (14 or 28 tiles a segment); 2 and 3: a net, 0.9 m of it,
// 32 or 64 cells along and 27 rows down (track-42_M's orange net, style 3 over 6 m segments: cells 9 x 3.3 cm);
// 12 and 13: two strands of barbed wire. Styles 4 to 11 are the posts' (a sub-rectangle of the post's texture).
struct Style {
    float height, u0, v0, u1, v1;
    int strands;
    float step;
};
constexpr Style kStyles[] = {
    {0.074f, 0, 0, 14, 1, 1, 0},     {0.074f, 0, 0, 28, 1, 1, 0},      // 0, 1: tape
    {0.9f, 0, 0, 32, 27.12f, 1, 0},  {0.9f, 0, 0, 64, 27.12f, 1, 0},   // 2, 3: net
};
constexpr Style kWire[] = {{0.074f, 0, 0, 14, 1, 2, 0.4f}, {0.074f, 0, 0, 28, 1, 2, 0.4f}};   // 12, 13
const Style& StyleOf(int32_t style)
{
    if (style >= 0 && style < 4) return kStyles[style];
    if (style == 12 || style == 13) return kWire[style - 12];
    return kStyles[0];   // (another: a tape)
}
constexpr float kStep = 1.0f / 240.0f;   // the fences' own step
constexpr float kMargin = 0.03f;         // the car's box grown by this
// A post: a spring back to upright (per second squared a metre of lean, and its damping), falling past half its height.
constexpr float kPostSpring = 90.0f, kPostDamping = 7.0f, kFallAt = 0.5f, kFallAccel = 14.0f, kDownAt = 0.97f;
// A tape: its middle's spring back to the line, slack to 3 % over its length, its pull on the posts, snapping at 1.5 x.
constexpr float kTapeSpring = 50.0f, kTapeDamping = 5.0f, kSlack = 1.03f, kTension = 120.0f, kBreakAt = 1.5f;

float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Flat(V3 a) { return {a.x, 0.0f, a.z}; }

std::vector<uint8_t> ReadBytes(const std::string& path)
{
    std::vector<uint8_t> b;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        b.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        b.resize(std::fread(b.data(), 1, b.size(), f));
        std::fclose(f);
    }
    return b;
}

iface::Vertex Vert(V3 p, V3 n, float u, float v)
{
    iface::Vertex x{};
    x.pos[0] = p.x, x.pos[1] = p.y, x.pos[2] = p.z;
    x.normal[0] = n.x, x.normal[1] = n.y, x.normal[2] = n.z;
    x.uv0[0] = x.uv1[0] = u;
    x.uv0[1] = x.uv1[1] = v;
    x.colour = 0xffffffff;
    return x;
}

// A post in its node's frame: x and z across (metres), y from its foot (0) to its top (1); the texture's height over
// all of it, its width round each side.
void PostMesh(std::vector<iface::Vertex>& v, std::vector<uint32_t>& idx)
{
    const float w = kPostHalfWidth, y0 = -kPostBelow / kPostHeight;
    const V3 corners[4] = {{-w, 0, -w}, {w, 0, -w}, {w, 0, w}, {-w, 0, w}};
    for (int s = 0; s < 4; ++s) {
        const V3 a = corners[s], b = corners[(s + 1) % 4];
        const V3 n = math::Norm(math::Cross(V3{0, 1, 0}, b - a));   // (outwards)
        const uint32_t base = static_cast<uint32_t>(v.size());
        v.push_back(Vert({a.x, y0, a.z}, n, 0, 1));
        v.push_back(Vert({b.x, y0, b.z}, n, 1, 1));
        v.push_back(Vert({b.x, 1, b.z}, n, 1, 0));
        v.push_back(Vert({a.x, 1, a.z}, n, 0, 0));
        for (uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) idx.push_back(base + k);
    }
    const uint32_t base = static_cast<uint32_t>(v.size());
    for (const V3& c : corners) v.push_back(Vert({c.x, 1, c.z}, {0, 1, 0}, 0.5f, 0.02f));
    for (uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) idx.push_back(base + k);
}

// Half a tape in its node's frame: x along it (0 .. 1), y across the band (0 its top edge, -1 its bottom), facing z;
// u from `u0` to `u1`, v as the style has it; a strand under another its `step` down (in bands).
void TapeMesh(float u0, float u1, const Style& st, std::vector<iface::Vertex>& v, std::vector<uint32_t>& idx)
{
    for (int k = 0; k < st.strands; ++k) {
        const float y = -float(k) * st.step / st.height;
        const uint32_t base = static_cast<uint32_t>(v.size());
        v.push_back(Vert({0, y, 0}, {0, 0, 1}, u0, st.v0));
        v.push_back(Vert({1, y, 0}, {0, 0, 1}, u1, st.v0));
        v.push_back(Vert({1, y - 1, 0}, {0, 0, 1}, u1, st.v1));
        v.push_back(Vert({0, y - 1, 0}, {0, 0, 1}, u0, st.v1));
        // Wound as the posts are, to the side its normal faces: the renderer's two-sided materials turn a back face's
        // normal round, and wound the other way the tape's turned into the wall behind it - every AO ray blocked, the
        // tape black in the shade.
        for (uint32_t i : {0u, 2u, 1u, 0u, 3u, 2u}) idx.push_back(base + i);
    }
}

// A post's node: its sides across, its axis foot to top (the mesh's y 0 .. 1), at its foot.
void PostNode(V3 foot, V3 top, float* m)
{
    const V3 axis = top - foot;
    const V3 x = math::Norm(math::Cross(axis, V3{0, 0, 1})), z = math::Norm(math::Cross(x, axis));
    const V3 rows[4] = {x, axis, z, foot};
    for (int r = 0; r < 4; ++r) m[4 * r] = rows[r].x, m[4 * r + 1] = rows[r].y, m[4 * r + 2] = rows[r].z, m[4 * r + 3] = r == 3;
}

// Half a tape's node: from `a` to `b`, the band (`band` metres) hanging below that line, facing sideways.
void TapeNode(V3 a, V3 b, float band, float* m)
{
    const V3 along = b - a;
    V3 side = math::Cross(along, V3{0, 1, 0});
    if (Length(side) < 1e-4f) side = math::Cross(along, V3{1, 0, 0});   // (hanging straight down)
    side = math::Norm(side);
    const V3 up = math::Norm(math::Cross(side, along)) * band;
    const V3 rows[4] = {along, up, side, a};
    for (int r = 0; r < 4; ++r) m[4 * r] = rows[r].x, m[4 * r + 1] = rows[r].y, m[4 * r + 2] = rows[r].z, m[4 * r + 3] = r == 3;
}

// A snapped tape's end: hanging from `from` towards `towards` (the other post), as far as half the tape reaches, its
// end not under the ground at `ground`.
V3 Hanging(V3 from, V3 towards, float length, float ground)
{
    const float drop = std::max(0.0f, std::min(length * 0.95f, from.y - (ground + 0.03f)));
    const float out = std::sqrt(std::max(0.0f, length * length - drop * drop));
    V3 dir = Flat(towards - from);
    dir = Length(dir) > 1e-4f ? math::Norm(dir) : V3{1, 0, 0};
    return from + dir * out - V3{0, drop, 0};
}

// A tile's mips keep its LINES: each level's alpha the MOST of the 2 x 2 texels it stands for, a level up - not their
// mean. A net's frame is a line a texel or two wide (fence_tile01: 31 % of its texels pass the alpha test, 170, RBR's
// AlphaRef); the shipped mips average it, from mip 2 its sides fall under the cut while its top stays, and from mip 3
// nothing passes - the net a few metres off broke into dots, then went. Taken the most of, a line stays a line at every
// level and the net far off is a whole orange band (as a fine mesh reads there). The colour mips are the shipped ones:
// the cell's inside is orange too, so the band has no dark fringe. DXT3 only (every fence.dat texture): its alpha is
// 4 bits a texel, rewritten in place. A texture opaque through (a post) is left as it was.
void KeepLines(std::vector<uint8_t>& dds)
{
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) || std::memcmp(dds.data() + 84, "DXT3", 4)) return;
    uint32_t h = 0, w = 0, mips = 0;
    std::memcpy(&h, dds.data() + 12, 4);
    std::memcpy(&w, dds.data() + 16, 4);
    std::memcpy(&mips, dds.data() + 28, 4);
    std::vector<uint8_t> above;   // the level above's alphas, row by row
    uint32_t aw = 0, ah = 0;
    size_t at = 128;
    for (uint32_t m = 0; m < std::max(mips, 1u); ++m, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
        const size_t blocks = size_t((w + 3) / 4) * ((h + 3) / 4);
        if (at + blocks * 16 > dds.size()) return;
        std::vector<uint8_t> level(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                // Its nibble: block (x / 4, y / 4), texel (x & 3, y & 3) in it, two a byte.
                const int i = int((y & 3) * 4 + (x & 3)), shift = (i & 1) * 4;
                uint8_t& byte = dds[at + (size_t(y / 4) * ((w + 3) / 4) + x / 4) * 16 + i / 2];
                uint8_t a = (byte >> shift) & 15;
                if (m > 0) {
                    a = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx)
                            a = std::max(a, above[size_t(std::min(2 * y + dy, ah - 1)) * aw + std::min(2 * x + dx, aw - 1)]);
                    byte = uint8_t((byte & ~(15 << shift)) | (a << shift));
                }
                level[size_t(y) * w + x] = a;
            }
        if (m == 0 && std::all_of(level.begin(), level.end(), [](uint8_t a) { return a == 15; })) return;   // (opaque)
        above.swap(level);
        aw = w, ah = h;
        at += blocks * 16;
    }
}

} // namespace

std::string Fences::Create(render::Renderer& renderer, const std::string& fncPath, const std::string& game)
{
    const std::vector<uint8_t> bytes = ReadBytes(fncPath);
    if (bytes.empty()) return "";   // (no .fnc: no fences)
    formats::Fnc fnc;
    if (std::string e = formats::ReadFnc(bytes, fnc); !e.empty()) return e;
    if (fnc.groups.empty()) return "";
    std::vector<std::pair<std::string, std::vector<uint8_t>>> textures;
    const std::string dat = formats::ReadFenceTextures(ReadBytes(game + "/Textures/fence.dat"), textures);
    if (!dat.empty()) std::printf("fences: %s - drawn untextured\n", dat.c_str());
    std::map<std::string, iface::TextureHandle> made;   // by type
    auto texture = [&](int32_t type) -> iface::TextureHandle {
        if (type < 0 || static_cast<size_t>(type) >= fnc.types.size()) return 0;
        const std::string& name = fnc.types[static_cast<size_t>(type)];
        if (const auto it = made.find(name); it != made.end()) return it->second;
        iface::TextureHandle t = 0;
        for (const auto& [file, dds] : textures)
            if (!_stricmp(file.c_str(), (name + ".dds").c_str())) {
                std::vector<uint8_t> kept = dds;   // (alpha-tested: its mips keep its lines)
                KeepLines(kept);
                t = renderer.CreateTexture(kept.data(), kept.size());
            }
        if (!t) std::printf("fences: no texture %s\n", name.c_str());
        return made[name] = t;
    };

    size_t nets = 0;
    std::vector<iface::Vertex> postV;
    std::vector<uint32_t> postI;
    PostMesh(postV, postI);
    for (const formats::FncGroup& fg : fnc.groups) {
        if (fg.items.empty()) continue;
        Group g{};
        g.posts = static_cast<uint32_t>(m_posts.size());
        g.tapes = static_cast<uint32_t>(m_tapes.size());
        g.lo = g.hi = V3{fg.items[0].point[0], fg.items[0].point[1], fg.items[0].point[2]};
        for (size_t i = 0; i < fg.items.size(); ++i) {
            Post p;
            p.foot = {fg.items[i].point[0], fg.items[i].point[1], fg.items[i].point[2]};
            g.lo = {std::min(g.lo.x, p.foot.x), std::min(g.lo.y, p.foot.y), std::min(g.lo.z, p.foot.z)};
            g.hi = {std::max(g.hi.x, p.foot.x), std::max(g.hi.y, p.foot.y + kPostHeight), std::max(g.hi.z, p.foot.z)};
            m_posts.push_back(p);
            // A tape to the next post (a group's last item, its box a speck, has none; nor a gap no tape would span).
            if (i + 1 < fg.items.size()) {
                const float* q = fg.items[i + 1].point;
                const float d = Length(V3{q[0], q[1], q[2]} - p.foot);
                if (d > 0.2f && d < 15.0f) {
                    m_tapes.push_back({g.posts + static_cast<uint32_t>(i), g.posts + static_cast<uint32_t>(i + 1), d});
                }
            }
        }
        g.postCount = static_cast<uint32_t>(m_posts.size()) - g.posts;
        g.tapeCount = static_cast<uint32_t>(m_tapes.size()) - g.tapes;

        // Its model: the tape's material (0), the post's (1); the posts' nodes, then the tapes' halves.
        iface::MaterialDesc tape = iface::DefaultMaterialDesc(), post = iface::DefaultMaterialDesc();
        tape.baseColour = texture(fg.tile);
        // Its style (RBR's): a tape, a net or a wire - the band, and the tile's repeats along and down it.
        const Style& style = StyleOf(fg.words[0]);
        g.band = style.height;
        nets += style.height > 0.5f;
        tape.flags = iface::kMaterialAlphaTest | iface::kMaterialTwoSided;
        tape.roughness = 0.55f;
        if (!tape.baseColour) tape.baseColourFactor[1] = tape.baseColourFactor[2] = 0.1f;   // (a red tape)
        post.baseColour = texture(fg.post);
        post.flags = iface::kMaterialTwoSided;   // (whichever way the renderer winds them)
        post.roughness = 0.85f;
        if (!post.baseColour) post.baseColourFactor[0] = 0.45f, post.baseColourFactor[1] = 0.35f, post.baseColourFactor[2] = 0.25f;
        const iface::MaterialDesc materials[2] = {tape, post};
        const uint32_t nodeCount = g.postCount + 2 * g.tapeCount;
        std::vector<iface::ModelNode> nodes(nodeCount);
        std::vector<iface::ModelMesh> meshes(nodeCount);
        std::vector<std::vector<iface::Vertex>> tapeV(2 * g.tapeCount);
        std::vector<std::vector<uint32_t>> tapeI(2 * g.tapeCount);
        for (uint32_t n = 0; n < nodeCount; ++n) {
            iface::ModelNode& node = nodes[n];
            node.parent = -1;
            iface::ModelMesh& mesh = meshes[n];
            mesh.node = n;
            if (n < g.postCount) {
                const Post& p = m_posts[g.posts + n];
                PostNode(p.foot, Top(p), node.local);
                std::snprintf(node.name, sizeof node.name, "post %u", n);
                mesh.vertices = postV.data(), mesh.vertexCount = static_cast<uint32_t>(postV.size());
                mesh.indices = postI.data(), mesh.indexCount = static_cast<uint32_t>(postI.size());
                mesh.material = 1;
            } else {
                const uint32_t h = n - g.postCount;
                const Tape& t = m_tapes[g.tapes + h / 2];
                const V3 a = Attach(m_posts[t.a]), b = Attach(m_posts[t.b]), mid = Middle(t);
                if (h % 2 == 0) TapeNode(a, mid, g.band, node.local);
                else TapeNode(mid, b, g.band, node.local);
                std::snprintf(node.name, sizeof node.name, "tape %u %s", h / 2, h % 2 ? "b" : "a");
                const float uMid = 0.5f * (style.u0 + style.u1);   // (the tape's halves: a half of the segment's u each)
                TapeMesh(h % 2 ? uMid : style.u0, h % 2 ? style.u1 : uMid, style, tapeV[h], tapeI[h]);
                mesh.vertices = tapeV[h].data(), mesh.vertexCount = static_cast<uint32_t>(tapeV[h].size());
                mesh.indices = tapeI[h].data(), mesh.indexCount = static_cast<uint32_t>(tapeI[h].size());
                mesh.material = 0;
            }
        }
        iface::ModelDesc d{};
        d.nodes = nodes.data(), d.nodeCount = nodeCount;
        d.meshes = meshes.data(), d.meshCount = nodeCount;
        d.materials = materials, d.materialCount = 2;
        const iface::ModelHandle model = renderer.CreateModel(d);
        g.instance = model ? renderer.CreateInstance(model) : 0;
        if (!g.instance) return "a fence's model: " + renderer.Error();
        m_groups.push_back(g);
    }
    if (nets) std::printf("fences: %zu of %zu groups nets\n", nets, m_groups.size());
    m_nodes.resize(16 * (m_posts.size() + 2 * m_tapes.size()));
    return "";
}

void Fences::SetCar(const phys::data::RecMesh& hull)
{
    m_hasCar = false;
    for (size_t i = 0; i + 4 <= hull.verts.size(); i += 4) {
        float p[3];
        std::memcpy(p, &hull.verts[i], 12);
        for (int k = 0; k < 3; ++k) {
            m_hullLo[k] = m_hasCar ? std::min(m_hullLo[k], p[k]) : p[k];
            m_hullHi[k] = m_hasCar ? std::max(m_hullHi[k], p[k]) : p[k];
        }
        m_hasCar = true;
    }
}

V3 Fences::Top(const Post& p) const
{
    const float h2 = std::max(0.0f, kPostHeight * kPostHeight - Dot(p.lean, p.lean));
    return p.foot + V3{p.lean.x, std::sqrt(h2), p.lean.z};
}

V3 Fences::Attach(const Post& p) const { return p.foot + (Top(p) - p.foot) * kAttach; }

V3 Fences::Middle(const Tape& t) const { return (Attach(m_posts[t.a]) + Attach(m_posts[t.b])) * 0.5f + t.bulge; }

bool Fences::Push(V3 point, V3& out) const
{
    // Into the car's frame (physics axes): the point less its position, turned back.
    const V3 p = V3{point.x, point.z, point.y} - V3{m_position[0], m_position[1], m_position[2]};
    const float back[4] = {-m_rotation[0], -m_rotation[1], -m_rotation[2], m_rotation[3]};
    const V3 l = math::Rotate(back, p);
    const float at[3] = {l.x, l.y, l.z};
    for (int k = 0; k < 3; ++k)
        if (at[k] < m_lo[k] - kMargin || at[k] > m_hi[k] + kMargin) return false;
    // Out along the car's motion (the face it leaves by first) - or, when it hardly moves, the shortest way.
    const V3 vw = V3{m_velocity.x, m_velocity.z, m_velocity.y};
    float d[3] = {0, 0, 0};
    if (Length(vw) > 0.5f) {
        const V3 vl = math::Norm(math::Rotate(back, vw));
        const float dir[3] = {vl.x, vl.y, vl.z};
        float t = 1e9f;
        for (int k = 0; k < 3; ++k)
            if (std::fabs(dir[k]) > 1e-4f) t = std::min(t, ((dir[k] > 0 ? m_hi[k] + kMargin : m_lo[k] - kMargin) - at[k]) / dir[k]);
        for (int k = 0; k < 3; ++k) d[k] = dir[k] * t;
    } else {
        float best = 1e9f;
        int axis = 0;
        float move = 0;
        for (int k = 0; k < 3; ++k) {
            const float up = m_hi[k] + kMargin - at[k], down = m_lo[k] - kMargin - at[k];
            if (up < best) best = up, axis = k, move = up;
            if (-down < best) best = -down, axis = k, move = down;
        }
        d[axis] = move;
    }
    out = math::ToRender(math::Rotate(m_rotation, V3{d[0], d[1], d[2]}));
    return true;
}

void Fences::StepGroup(Group& g, float h)
{
    bool moving = false;
    // The tapes: the car drags their middles; stretched, they pull their posts in, and past kBreakAt they snap.
    for (uint32_t i = g.tapes; i < g.tapes + g.tapeCount; ++i) {
        Tape& t = m_tapes[i];
        if (t.broken) continue;
        const V3 a = Attach(m_posts[t.a]), b = Attach(m_posts[t.b]);
        const V3 mid = (a + b) * 0.5f + t.bulge;
        bool hit = false;
        const std::pair<V3, float> samples[3] = {{mid, 1.0f}, {(a + mid) * 0.5f, 0.5f}, {(mid + b) * 0.5f, 0.5f}};
        for (const auto& [q, w] : samples) {
            V3 d;
            if (m_hasCar && Push(q, d)) {
                t.bulge = t.bulge + d * (1.0f / w);
                hit = true;
                break;
            }
        }
        if (hit) {
            t.velocity = m_velocity;
        } else {
            t.velocity = t.velocity + (t.bulge * -kTapeSpring - t.velocity * kTapeDamping) * h;
            t.bulge = t.bulge + t.velocity * h;
        }
        if (!hit && Dot(t.velocity, t.velocity) < 1e-6f) {   // (at rest)
            t.velocity = {};
            if (Dot(t.bulge, t.bulge) < 1e-8f) t.bulge = {};
        }
        const V3 m = (a + b) * 0.5f + t.bulge;
        const float length = Length(m - a) + Length(b - m);
        if (length > t.rest * kBreakAt) {
            t.broken = true;
            t.bulge = t.velocity = {};
            moving = true;
        } else if (length > t.rest * kSlack) {
            const float pull = kTension * (length - t.rest * kSlack);
            const V3 da = Flat(m - a), db = Flat(m - b);
            if (Length(da) > 1e-4f) m_posts[t.a].pull = m_posts[t.a].pull + math::Norm(da) * pull;
            if (Length(db) > 1e-4f) m_posts[t.b].pull = m_posts[t.b].pull + math::Norm(db) * pull;
        }
        moving = moving || hit || Dot(t.velocity, t.velocity) > 0;
    }
    // The posts: shoved by the car (their top along with it), leaning back on a spring, or falling once past kFallAt.
    for (uint32_t i = g.posts; i < g.posts + g.postCount; ++i) {
        Post& p = m_posts[i];
        const V3 top = Top(p);
        bool hit = false;
        for (const float f : {1.0f, 0.65f, 0.3f}) {
            V3 d;
            if (m_hasCar && Push(p.foot + (top - p.foot) * f, d)) {
                p.lean = p.lean + Flat(d) * (1.0f / f);
                p.velocity = Flat(m_velocity);
                hit = true;
                break;
            }
        }
        const float lean = Length(p.lean);
        if (lean > kFallAt * kPostHeight) p.down = true;
        V3 accel = p.pull;
        p.pull = {};
        if (!p.down) accel = accel - p.lean * kPostSpring - p.velocity * kPostDamping;
        else if (lean > 1e-4f) accel = accel + p.lean * (kFallAccel / lean) - p.velocity * 1.5f;
        p.velocity = Flat(p.velocity + accel * h);
        p.lean = p.lean + p.velocity * h;
        const float most = kDownAt * kPostHeight, now = Length(p.lean);
        if (now > most) {
            p.lean = p.lean * (most / now);
            const V3 out = p.lean * (1.0f / most);
            const float outward = Dot(p.velocity, out);
            if (outward > 0) p.velocity = p.velocity - out * outward;   // (on the ground: no further)
            if (p.down && !hit) p.velocity = {};   // (lying: the ground holds it, the tapes' pull too)
        }
        // At rest: hardly moving (upright again, or held leaning by a tape's pull).
        if (!hit && Dot(p.velocity, p.velocity) < 1e-6f) {
            p.velocity = {};
            if (!p.down && Dot(p.lean, p.lean) < 1e-8f) p.lean = {};
        }
        moving = moving || hit || Dot(p.velocity, p.velocity) > 0;
    }
    g.moving = g.moving || moving;
}

void Fences::Place(Group& g)
{
    float* nodes = &m_nodes[16 * (g.posts + 2 * g.tapes)];
    for (uint32_t i = 0; i < g.postCount; ++i) {
        const Post& p = m_posts[g.posts + i];
        PostNode(p.foot, Top(p), nodes + 16 * i);
    }
    for (uint32_t i = 0; i < g.tapeCount; ++i) {
        const Tape& t = m_tapes[g.tapes + i];
        const Post &pa = m_posts[t.a], &pb = m_posts[t.b];
        const V3 a = Attach(pa), b = Attach(pb);
        float* half = nodes + 16 * (g.postCount + 2 * i);
        if (!t.broken) {
            const V3 m = Middle(t);
            TapeNode(a, m, g.band, half);
            TapeNode(m, b, g.band, half + 16);
        } else {
            TapeNode(a, Hanging(a, b, t.rest * 0.5f, pa.foot.y), g.band, half);
            TapeNode(Hanging(b, a, t.rest * 0.5f, pb.foot.y), b, g.band, half + 16);
        }
    }
    iface::InstanceUpdate u{};
    u.instance = g.instance;
    u.visible = 1;
    u.world[0] = u.world[5] = u.world[10] = u.world[15] = 1;
    u.nodes = nodes;
    u.nodeCount = g.postCount + 2 * g.tapeCount;
    m_updates.push_back(u);
}

const std::vector<iface::InstanceUpdate>& Fences::Step(double seconds, const physics::Pose& pose, const float cog[3])
{
    m_updates.clear();
    std::memcpy(m_rotation, pose.rotation, 16);
    std::memcpy(m_position, pose.position, 12);
    m_velocity = math::ToRender({pose.velocity[0], pose.velocity[1], pose.velocity[2]});
    for (int k = 0; k < 3; ++k) m_lo[k] = m_hullLo[k] - cog[k], m_hi[k] = m_hullHi[k] - cog[k];   // (the car's frame)
    // Which groups the car can reach: its box's reach round its position (render axes).
    float reach = 0;
    for (int k = 0; k < 3; ++k) reach = std::max({reach, std::fabs(m_lo[k]), std::fabs(m_hi[k])});
    reach = reach * 1.8f + 0.5f;
    const V3 car = math::ToRender({pose.position[0], pose.position[1], pose.position[2]});
    m_acc = std::min(m_acc + seconds, 0.2);
    while (m_acc >= kStep) {
        m_acc -= kStep;
        for (Group& g : m_groups) {
            const bool near = m_hasCar && car.x > g.lo.x - reach && car.x < g.hi.x + reach && car.y > g.lo.y - reach &&
                              car.y < g.hi.y + reach && car.z > g.lo.z - reach && car.z < g.hi.z + reach;
            bool resting = true;   // (nothing of it moves: no need to step it)
            for (uint32_t i = g.posts; resting && i < g.posts + g.postCount; ++i)
                resting = (m_posts[i].down || Dot(m_posts[i].lean, m_posts[i].lean) == 0) && Dot(m_posts[i].velocity, m_posts[i].velocity) == 0;
            for (uint32_t i = g.tapes; resting && i < g.tapes + g.tapeCount; ++i)
                resting = Dot(m_tapes[i].bulge, m_tapes[i].bulge) == 0 && Dot(m_tapes[i].velocity, m_tapes[i].velocity) == 0;
            if (near || !resting) StepGroup(g, kStep);
        }
    }
    for (Group& g : m_groups) {
        if (!g.moving) continue;
        Place(g);
        g.moving = false;
    }
    return m_updates;
}

} // namespace wrally::assets
