#include "native/stage_collision.h"

#include <xmmintrin.h>

#include <cassert>
#include <cstring>

#include "num/f32.h"
#include "num/math.h"
#include "num/x87.h"   // jmp:: branch helpers

namespace phys::native {

using data::ColNode;
using data::ColRootHeader;
using data::ColTreeHeader;
using num::F32;
using num::RayHit;
using data::kNodeBankShift;
using data::kNodeFlagBranchStart;
using data::kNumFacesMask;
namespace jmp = num::jmp;

namespace {
float FromBits(uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
F32 Int(uint32_t i) { return F32(static_cast<float>(static_cast<int32_t>(i))); }   // cvtsi2ss
template <class T> T Read(const std::vector<uint8_t>& file, size_t at)
{
    T v;
    std::memcpy(&v, file.data() + at, sizeof v);
    return v;
}

constexpr int kStack = 256;   // the original's walk stack (no bound checked there)

// The face's two materials and its light and blend at barycentrics (u, v), into out: `uv` blends a material's uv
// nibbles (each query has its own order).
template <class Uv> void FaceValues(const ColFaceBr& face, const MaterialMaps& maps, F32 u, F32 v, Uv uv, GroundHit& out)
{
    const F32 k31 = F32(FromBits(0x3d042108));   // 1/31
    out.lightFlag = face.light >> 15;
    out.blendFlag = face.blend >> 15;
    const F32 w = F32(1.0f) - (v + u);
    // Three 5-bit values (vertex 0 in bits 0-4) blended at the hit: ((c2 v + c1 u) + c0 w) / 31.
    auto packed5 = [&](uint16_t c) { return (((Int((c >> 10) & 31) * v + Int((c >> 5) & 31) * u) + Int(c & 31) * w) * k31).f(); };
    out.light = packed5(face.light);
    for (int m = 0; m < 2; ++m) {
        float su, sv;
        uv(face.uv[m], u, v, w, su, sv);
        out.material[m] = maps.Sample(face.mat[m], su, sv);
    }
    out.blend = packed5(face.blend);
}

// -dot(v0, n) as the original's fld; fchs; fstp: the sign bit flipped.
float Plane(const Vec3& v0, const Vec3& n)
{
    const float dot = num::Dot3(v0, n);
    uint32_t bits;
    std::memcpy(&bits, &dot, 4);
    bits ^= 0x80000000u;
    float p;
    std::memcpy(&p, &bits, 4);
    return p;
}

// MapSample's axis: i = cvttss2si(float(size) * coord); wrapped (i % size, or size - (-i) % size below 0, the original's
// off-by-one) or clamped to [0, size - 1].
int32_t Axis(uint32_t size, float coord, bool wrap)
{
    const int32_t i = _mm_cvtt_ss2si(_mm_set_ss(_mm_cvtss_f32(_mm_mul_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), static_cast<int>(size)), _mm_set_ss(coord)))));
    const int32_t n = static_cast<int32_t>(size);
    if (wrap) {
        if (i >= 0) return i % n;
        return n - static_cast<int32_t>(0u - static_cast<uint32_t>(i)) % n;   // -INT_MIN wraps to INT_MIN, as idiv's
    }
    if (i < 0) return 0;
    return i > n - 1 ? n - 1 : i;
}
} // namespace

std::string MaterialMaps::Load(std::vector<uint8_t> file, uint32_t weather)
{
    m_file = std::move(file);
    m_maps = {};
    m_count = 0;
    const std::vector<uint8_t>& d = m_file;
    if (d.size() < 4) return "the .mat is too short";
    size_t at = 4;
    const int32_t weathers = Read<int32_t>(d, 0);
    for (int32_t w = 0; w < weathers; ++w) {
        while (at < d.size() && d[at]) ++at;   // the weather's name
        ++at;
        if (at + 4 > d.size()) return "the .mat ends in a weather";
        const int32_t n = Read<int32_t>(d, at);
        at += 4;
        for (int32_t j = 0; j < n; ++j) {
            if (at + 8 > d.size()) return "the .mat ends in a map";
            const int32_t width = Read<int32_t>(d, at), height = Read<int32_t>(d, at + 4);
            at += 8;
            if (static_cast<uint32_t>(w) == weather && j < 256) {
                // CMaterialMaps' handler (0x520190) stores the width byte as a whole word: the height and the flags are 0,
                // so a map is sampled along its first row only, clamped (RBR's own behaviour, kept).
                m_maps[j] = {static_cast<uint8_t>(width), 0, 0, d.data() + at};
                m_count = static_cast<uint32_t>(n);
            }
            at += static_cast<size_t>(width) * static_cast<size_t>(height);
            if (at > d.size()) return "the .mat ends in a map's bytes";
        }
    }
    m_loaded = true;
    return "";
}

uint8_t MaterialMaps::Sample(uint8_t index, float u, float v) const
{
    if (!m_loaded || index >= m_count) return 0;
    const Map& map = m_maps[index];
    if (!map.data) return 0;
    const int32_t i = Axis(map.width, u, (map.flags & 1) != 0);
    // The original never loads v: the second axis is sampled at u too (kept; v unused).
    (void)v;
    const int32_t j = Axis(map.height, u, (map.flags & 2) != 0);
    return map.data[static_cast<int32_t>(map.height) * j + i];
}

std::string StageCollision::Load(std::vector<uint8_t> file)
{
    m_file = std::move(file);
    m_brakeWall = 0;
    m_rootVersion = 0, m_water = {}, m_river = {};
    m_nodes.clear();
    m_root = -1;
    m_banks = {};
    m_bankCursor = 0;
    if (m_file.size() < 0x10 || Read<uint32_t>(m_file, 0) != 0x5237434f) return "not an R7CO .col";
    LoadTree(Read<uint32_t>(m_file, 4));
    const uint32_t branches = Read<uint32_t>(m_file, 8), list = Read<uint32_t>(m_file, 0xc);
    for (uint32_t i = 0; i < branches; ++i) LoadTree(Read<uint32_t>(m_file, list + 4 * i));
    return m_root < 0 ? "the .col has no root tree" : "";
}

uint32_t StageCollision::AllocBank(const Vec3* vertices)
{
    // AllocBank (0x4fd630): the first free of the 1024 from the cursor on.
    const uint32_t start = m_bankCursor;
    if (m_banks[start]) {
        for (;;) {
            m_bankCursor = (m_bankCursor + 1) & 0x3ff;
            if (m_bankCursor == start || !m_banks[m_bankCursor]) break;
        }
    }
    const uint32_t i = m_bankCursor;
    m_banks[i] = vertices;
    m_bankCursor = (m_bankCursor + 1) & 0x3ff;
    return i;
}

void StageCollision::LoadTree(size_t header)
{
    // TreeLoad (0x4fdff0) for the collision's part: the vertex bank, then the tree by its type. (A root's brake wall,
    // water and grid are the world's, not loaded here.)
    const ColTreeHeader h = Read<ColTreeHeader>(m_file, header);
    uint32_t bank = 0;
    if (h.numVertices) bank = AllocBank(reinterpret_cast<const Vec3*>(m_file.data() + header + h.vertexOffset));
    const size_t tree = header + h.treeOffset;
    if (h.type == ColTreeHeader::kTypeDefaultBrakeWall) {   // the root header's brake wall block
        const ColRootHeader r = Read<ColRootHeader>(m_file, header + sizeof h);
        if (h.version == 1) {
            if (h.vertexOffset > sizeof h) m_brakeWall = header + sizeof h;
        } else if (h.version >= 2 && h.version <= 4 && r.brakeWallOffset) {
            m_brakeWall = header + r.brakeWallOffset;
        }
        m_rootVersion = h.version;
        if (h.version == 3 || h.version == 4) {
            m_water = {r.numWaterSurfaces ? header + r.waterOffset : 0, r.numWaterSurfaces};
            m_river = {h.version == 4 && r.numRiverSurfaces ? header + r.riverOffset : 0, h.version == 4 ? r.numRiverSurfaces : 0u};
        }
    }
    switch (h.type) {
    case ColTreeHeader::kTypeDefaultBrakeWall:
    case ColTreeHeader::kTypeDefault:
        // The root: relocated down to the branches' placeholders (any flag ends the walk).
        m_root = static_cast<int32_t>(m_nodes.size());
        m_nodes.emplace_back();
        FillNode(m_root, tree, tree, 0xffe00000, bank);
        break;
    case ColTreeHeader::kTypeBranch:
        if (m_root < 0 || !h.numVertices) break;
        {
            const int32_t top = static_cast<int32_t>(m_nodes.size());
            m_nodes.emplace_back();
            FillNode(top, tree, tree, 0, bank);
            Graft(h.traversalId, top);
        }
        break;
    default:   // a processed branch (a file loaded twice) or an unknown type: nothing
        break;
    }
}

void StageCollision::FillNode(int32_t slot, size_t at, size_t base, uint32_t endMark, uint32_t bank)
{
    // InitTreeRec (0x4fd6b0) into node `slot`: a leaf gets its faces and bank, an inner node its two children.
    const ColNode f = Read<ColNode>(m_file, at);
    m_nodes[slot].box = f.bbox;
    m_nodes[slot].numFaces = f.numFaces;
    if (f.numFaces & endMark) return;   // a placeholder: left as the file has it
    if (f.numFaces & kNumFacesMask) {
        m_nodes[slot].numFaces = (f.numFaces & 0x3fffff) | bank << kNodeBankShift;
        m_nodes[slot].faces = reinterpret_cast<const ColFaceBr*>(m_file.data() + base + f.faces);
    } else if (f.children) {
        const int32_t c = static_cast<int32_t>(m_nodes.size());
        m_nodes.resize(m_nodes.size() + 2);
        m_nodes[slot].children = c;
        FillNode(c, base + f.children, base, endMark, bank);
        FillNode(c + 1, base + f.children + sizeof(ColNode), base, endMark, bank);
    }
}

void StageCollision::Graft(uint32_t trav, int32_t branch)
{
    // The branch's top node into the slot its traversal names under the root (NODE_FLAG_BRANCHSTART cleared).
    int32_t level = static_cast<int32_t>(trav >> 24);
    if (!level) return;
    int32_t at = m_root;
    while (--level > 0) {
        if (m_nodes[at].children < 0) return;
        at = m_nodes[at].children + static_cast<int32_t>(trav & 1);
        trav >>= 1;
    }
    if (m_nodes[at].children < 0) return;
    const int32_t slot = m_nodes[at].children + static_cast<int32_t>(trav & 1);
    m_nodes[slot] = m_nodes[branch];
    m_nodes[slot].numFaces &= ~kNodeFlagBranchStart;
}

template <class Test> void StageCollision::PushChildren(const Node& n, const Node** stack, int& top, Test test) const
{
    // The children that pass the test, first then second (so the second is visited first).
    if (n.children < 0) return;
    for (int k = 0; k < 2; ++k) {
        const Node& c = m_nodes[n.children + k];
        if (!(c.numFaces & kNodeFlagBranchStart) && test(c.box)) {
            assert(top < kStack);
            stack[top++] = &c;
        }
    }
}

void StageCollision::BoxQuery(const Vec3& pos, const Vec3& ext, std::vector<const Node*>& leaves) const
{
    leaves.clear();
    const Node* root = Root();
    if (!root) return;
    const Node* stack[kStack];
    int top = 0;
    stack[top++] = root;
    do {
        const Node* n = stack[--top];
        if (n->numFaces & kNumFacesMask) {
            leaves.push_back(n);
            continue;
        }
        PushChildren(*n, stack, top, [&](const AABBox& b) { return num::BoxOverlap(b, pos, ext) != 0; });
    } while (top);
}

const uint8_t* StageCollision::BrakeWallBlock() const
{
    if (!m_brakeWall || Read<uint32_t>(m_file, m_brakeWall + 4) == 0) return nullptr;
    return m_file.data() + m_brakeWall;
}

void StageCollision::MinMaxQuery(const Vec3& mn, const Vec3& mx, std::vector<const Node*>& leaves) const
{
    const F32 half = F32(0.5f);
    const Vec3 pos = {((F32(mx.x) + F32(mn.x)) * half).f(), ((F32(mx.y) + F32(mn.y)) * half).f(), ((F32(mx.z) + F32(mn.z)) * half).f()};
    const Vec3 ext = {((F32(mx.x) - F32(mn.x)) * half).f(), ((F32(mx.y) - F32(mn.y)) * half).f(), ((F32(mx.z) - F32(mn.z)) * half).f()};
    BoxQuery(pos, ext, leaves);
}

bool StageCollision::SegmentQuery(const MaterialMaps& maps, const Vec3& p0, const Vec3& p1, GroundHit& out) const
{
    const Node* root = Root();
    if (!root) return false;
    const F32 k15 = F32(FromBits(0x3d888889));   // 1/15
    const Vec3 o = p0;
    const Vec3 d = {(F32(p1.x) - F32(p0.x)).f(), (F32(p1.y) - F32(p0.y)).f(), (F32(p1.z) - F32(p0.z)).f()};
    Vec3 h;
    num::Scale3(d, 0.5f, h);
    RayHit best, hit;   // `hit` one record reused for every triangle, as the original's local
    best.t = FromBits(0x3f8ccccd);   // 1.1
    const ColFaceBr* bestFace = nullptr;
    const Vec3* bestBank = nullptr;
    const Node* stack[kStack];
    int top = 0;
    stack[top++] = root;
    do {
        const Node* n = stack[--top];
        const uint32_t count = n->numFaces & kNumFacesMask;
        if (!count) {
            PushChildren(*n, stack, top, [&](const AABBox& b) { return num::SegmentBoxOverlap(b, o, h) != 0; });
            continue;
        }
        const Vec3* bank = Bank(*n);
        if (!bank) continue;
        for (uint32_t i = 0; i < count; ++i) {
            const ColFaceBr& t = n->faces[i];
            if (!num::RayTriangle(o, d, bank[t.idxVertex[0]], bank[t.idxVertex[1]], bank[t.idxVertex[2]], hit)) continue;
            if (jmp::BE(hit.t, 0.0f)) continue;     // comiss t, 0.0; jbe
            if (jmp::BE(best.t, hit.t)) continue;   // comiss best.t, t; jbe
            std::memcpy(&best.normal, &hit.normal, sizeof best.normal);   // bit copies (mov / movss)
            std::memcpy(&best.t, &hit.t, 4);
            std::memcpy(&best.u, &hit.u, 4);
            std::memcpy(&best.v, &hit.v, 4);
            bestFace = &t;
            bestBank = bank;
        }
    } while (top);
    if (jmp::B(1.0f, best.t)) return false;   // comiss 1.0, t; jb
    num::Normalize3Approx(best.normal);
    out.normal = best.normal;
    const ColFaceBr face = *bestFace;
    out.plane = Plane(bestBank[face.idxVertex[0]], best.normal);
    const F32 tt = F32(best.t);
    out.point = {(F32(d.x) * tt + F32(o.x)).f(), (F32(d.y) * tt + F32(o.y)).f(), (F32(d.z) * tt + F32(o.z)).f()};
    std::memcpy(&out.t, &best.t, 4);
    // The uv nibbles in this query's own order: ((c2 v + c0 w) + c1 u) / 15.
    FaceValues(face, maps, F32(best.u), F32(best.v), [&](const uint8_t* c, F32 u, F32 v, F32 w, float& su, float& sv) {
        su = (((Int(c[2] & 15) * v + Int(c[0] & 15) * w) + Int(c[1] & 15) * u) * k15).f();
        sv = (((Int(c[2] >> 4) * v + Int(c[0] >> 4) * w) + Int(c[1] >> 4) * u) * k15).f();
    }, out);
    return true;
}

bool StageCollision::LeavesRaycast(const MaterialMaps& maps, const Vec3& p0, const Vec3& p1, const std::vector<const Node*>& leaves,
                                   GroundHit& out) const
{
    const F32 k15 = F32(FromBits(0x3d888889));   // 1/15
    const Vec3 p = p0;
    const Vec3 d = {(F32(p1.x) - F32(p0.x)).f(), (F32(p1.y) - F32(p0.y)).f(), (F32(p1.z) - F32(p0.z)).f()};
    Vec3 h;
    num::Scale3(d, 0.5f, h);
    float best = 2.0f, bu = 0, bv = 0;
    Vec3 n = {};
    const ColFaceBr* bestFace = nullptr;
    const Vec3* bestBank = nullptr;
    RayHit hit;
    for (const Node* node : leaves) {
        const Vec3* bank = Bank(*node);
        if (!bank || !num::SegmentBoxOverlap(node->box, p, h)) continue;
        const uint32_t tris = node->numFaces & kNumFacesMask;
        for (uint32_t k = 0; k < tris; ++k) {
            const ColFaceBr& t = node->faces[k];
            if (!num::RayTriangle(p, d, bank[t.idxVertex[0]], bank[t.idxVertex[1]], bank[t.idxVertex[2]], hit)) continue;
            if (jmp::B(hit.t, 0.0f)) continue;    // comiss t, 0.0; jb
            if (jmp::BE(best, hit.t)) continue;   // comiss best, t; jbe
            std::memcpy(&n, &hit.normal, sizeof n);
            std::memcpy(&best, &hit.t, 4);
            std::memcpy(&bu, &hit.u, 4);
            std::memcpy(&bv, &hit.v, 4);
            bestFace = &t;
            bestBank = bank;
        }
    }
    if (jmp::B(1.0f, best)) return false;   // comiss 1.0, best; jb
    num::Normalize3Approx(n);
    out.normal = n;
    const ColFaceBr face = *bestFace;
    out.plane = Plane(bestBank[face.idxVertex[0]], n);
    const F32 tt = F32(best);
    out.point = {(F32(d.x) * tt + F32(p.x)).f(), (F32(d.y) * tt + F32(p.y)).f(), (F32(d.z) * tt + F32(p.z)).f()};
    std::memcpy(&out.t, &best, 4);
    // The uv nibbles: ((c0 w + c1 u) + c2 v) / 15.
    FaceValues(face, maps, F32(bu), F32(bv), [&](const uint8_t* c, F32 u, F32 v, F32 w, float& su, float& sv) {
        su = (((Int(c[0] & 15) * w + Int(c[1] & 15) * u) + Int(c[2] & 15) * v) * k15).f();
        sv = (((Int(c[0] >> 4) * w + Int(c[1] >> 4) * u) + Int(c[2] >> 4) * v) * k15).f();
    }, out);
    return true;
}

bool StageCollision::LeavesPointQuery(const MaterialMaps& maps, const Vec3& p, const std::vector<const Node*>& leaves, GroundHit& out) const
{
    const F32 k15 = F32(FromBits(0x3d888889));   // 1/15
    float best = 2.0f, bu = 0, bv = 0;
    Vec3 point = {}, n = {};
    const ColFaceBr* bestFace = nullptr;
    const Vec3* bestBank = nullptr;
    RayHit hit;
    for (const Node* node : leaves) {
        const Vec3* bank = Bank(*node);
        if (!bank) continue;
        const uint32_t tris = node->numFaces & kNumFacesMask;
        for (uint32_t k = 0; k < tris; ++k) {
            const ColFaceBr& t = node->faces[k];
            if (!num::PointTriangle(p, bank[t.idxVertex[0]], bank[t.idxVertex[1]], bank[t.idxVertex[2]], hit)) continue;
            if (!jmp::A(best, hit.t)) continue;   // comiss best, t; jbe
            std::memcpy(&best, &hit.t, 4);
            std::memcpy(&point, hit.unknown0, sizeof point);
            std::memcpy(&bu, &hit.u, 4);
            std::memcpy(&n, &hit.normal, sizeof n);
            bestFace = &t;
            bestBank = bank;
            std::memcpy(&bv, &hit.v, 4);
        }
    }
    if (!bestFace) return false;
    out.normal = n;   // as PointTriangle left it (normalised there)
    const ColFaceBr face = *bestFace;
    out.plane = Plane(bestBank[face.idxVertex[0]], n);
    out.point = point;
    out.t = 0.0f;
    // The uv nibbles: ((c1 u + c2 v) + c0 w) / 15.
    FaceValues(face, maps, F32(bu), F32(bv), [&](const uint8_t* c, F32 u, F32 v, F32 w, float& su, float& sv) {
        su = (((Int(c[1] & 15) * u + Int(c[2] & 15) * v) + Int(c[0] & 15) * w) * k15).f();
        sv = (((Int(c[1] >> 4) * u + Int(c[2] >> 4) * v) + Int(c[0] >> 4) * w) * k15).f();
    }, out);
    return true;
}

std::vector<StageCollision::Quads> StageCollision::WaterQuads(uint32_t wetness) const
{
    std::vector<Quads> out;
    auto add = [&](const std::pair<size_t, uint32_t>& q) {
        if (q.first && q.second) out.push_back({reinterpret_cast<const num::Vec4*>(m_file.data() + q.first), q.second});
    };
    if (m_rootVersion == 3) add(m_water);
    if (m_rootVersion == 4) {
        if (wetness == 2) add(m_water);
        add(m_river);
    }
    return out;
}

} // namespace phys::native
