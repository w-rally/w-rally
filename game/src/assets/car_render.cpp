#include "assets/car_render.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "assets/car_model.h"
#include "core/math.h"
#include "render/renderer.h"

namespace wrally::assets {

using math::V3;

namespace {

// The car's body hull (its collision mesh, MrCarCollision: the body's frame, physics axes) as a flat-shaded mesh in
// the renderer's axes.
void HullMesh(const phys::data::RecMesh& m, std::vector<iface::Vertex>& v, std::vector<uint32_t>& idx)
{
    auto at = [&](uint8_t i) {
        float p[3];
        std::memcpy(p, &m.verts[4 * i], 12);
        return math::ToRender({p[0], p[1], p[2]});
    };
    for (size_t f = 0; f + 7 <= m.faces.size(); f += 7) {
        const V3 a = at(m.faces[f]), b = at(m.faces[f + 1]), c = at(m.faces[f + 2]);
        const V3 n = math::Norm(math::Cross(b - a, c - a));
        for (const V3& p : {a, b, c}) {
            iface::Vertex x{};
            x.pos[0] = p.x;
            x.pos[1] = p.y;
            x.pos[2] = p.z;
            x.normal[0] = n.x;
            x.normal[1] = n.y;
            x.normal[2] = n.z;
            x.colour = 0xff2040c0;   // ABGR: a red car
            idx.push_back(static_cast<uint32_t>(v.size()));
            v.push_back(x);
        }
    }
}

// A physics pose (a quaternion and a position, physics axes) as the renderer's row-major matrix (row vectors,
// translation in row 3): the rotation conjugated by the axis swap. The car's placement, and a part's.
void PoseRows(const float rotation[4], const float position[3], float* m)
{
    const V3 cols[3] = {math::ToRender(math::Rotate(rotation, {1, 0, 0})), math::ToRender(math::Rotate(rotation, {0, 0, 1})),
                        math::ToRender(math::Rotate(rotation, {0, 1, 0}))};   // the images of the renderer's x, y, z
    for (int r = 0; r < 3; ++r) {
        m[r * 4 + 0] = cols[r].x;
        m[r * 4 + 1] = cols[r].y;
        m[r * 4 + 2] = cols[r].z;
        m[r * 4 + 3] = 0;
    }
    const V3 t = math::ToRender({position[0], position[1], position[2]});
    m[12] = t.x;
    m[13] = t.y;
    m[14] = t.z;
    m[15] = 1;
}

// The inverse of a rigid row-major matrix (rotation rows, translation row): the rotation transposed, -t of it.
void RigidInverse(const float* m, float* out)
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[4 * i + j] = m[4 * j + i];
        out[4 * i + 3] = 0;
    }
    for (int j = 0; j < 3; ++j) out[12 + j] = -(m[12] * out[j] + m[13] * out[4 + j] + m[14] * out[8 + j]);
    out[15] = 1;
}

// A wheel's spin under its hub: about the knuckle's x by `spin`, the other way on the right wheels (their knuckles are
// turned half about z, PartMatrix) - so spin * hub is the spinning part's placement PartMatrix gave in one.
void SpinNode(float spin, bool right, float* out)
{
    const float c = std::cos(spin), s = (right ? -1.0f : 1.0f) * std::sin(spin);
    const float m[16] = {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1};
    std::memcpy(out, m, sizeof m);
}

// A car part's placement in the car's frame (the pose's: the centre of gravity), as the renderer's row-major matrix
// (row vectors), to be followed by the car's PoseMatrix. The model's frame is the car body's: a body part sits at -cog. A wheel's
// part (in its hub's frame, the model's axes) goes with the physics' wheel: its knuckle's rotation and hub, the spin
// about the knuckle's x for the parts turning with it; the right wheels' knuckles are turned half about z, which the
// model's right wheels already are (car_model.h), so that half turn is taken out first.
void PartMatrix(const float cog[3], const physics::Wheel* w, bool right, bool spins, float* out)
{
    V3 cols[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // physics axes: the images of x, y, z
    V3 t{-cog[0], -cog[1], -cog[2]};
    if (w) {
        const float c = spins ? std::cos(w->spin) : 1.0f, s = spins ? std::sin(w->spin) : 0.0f;
        const V3 local[3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};   // the spin about x
        const float f = right ? -1.0f : 1.0f;                   // the half turn about z (its own inverse)
        const V3 flipped[3] = {local[0] * f, local[1] * f, local[2]};
        for (int i = 0; i < 3; ++i) cols[i] = math::Rotate(w->rotation, flipped[i]);
        t = {w->position[0], w->position[1], w->position[2]};
    }
    // the renderer's axes (y and z swapped): the image of the renderer's axis i is the swapped image of physics' axis
    // swap(i)
    const V3 r[3] = {math::ToRender(cols[0]), math::ToRender(cols[2]), math::ToRender(cols[1])};
    for (int i = 0; i < 3; ++i) {
        out[4 * i + 0] = r[i].x;
        out[4 * i + 1] = r[i].y;
        out[4 * i + 2] = r[i].z;
        out[4 * i + 3] = 0;
    }
    const V3 tr = math::ToRender(t);
    out[12] = tr.x;
    out[13] = tr.y;
    out[14] = tr.z;
    out[15] = 1;
}

} // namespace

void PoseMatrix(const float rotation[4], const float position[3], float out[16]) { PoseRows(rotation, position, out); }

void PartNode(const float body[16], const physics::Part& p, const float car[16], float out[16])
{
    if (p.inCar && !std::memcmp(p.position, p.restPosition, 12) && !std::memcmp(p.rotation, p.restRotation, 16)) {
        std::memcpy(out, body, 64);
        return;
    }
    float rest[16], restInv[16], now[16], a[16];
    PoseRows(p.restRotation, p.restPosition, rest);
    RigidInverse(rest, restInv);
    PoseRows(p.rotation, p.position, now);
    math::Mul44(body, restInv, a);
    if (p.inCar) {
        math::Mul44(a, now, out);
        return;
    }
    float b[16], carInv[16];
    RigidInverse(car, carInv);
    math::Mul44(a, now, b);
    math::Mul44(b, carInv, out);
}

void CarRender::Create(render::Renderer& renderer, const std::string& sgc, const std::string& surface,
                       const std::vector<phys::data::RecMesh>& collision, const std::vector<physics::Part>& parts)
{
    iface::MaterialDesc paint = iface::DefaultMaterialDesc();
    paint.flags = iface::kMaterialTwoSided;
    paint.baseColourFactor[0] = 0.55f;
    paint.baseColourFactor[1] = 0.04f;
    paint.baseColourFactor[2] = 0.03f;
    paint.baseColourFactor[3] = 1.0f;
    paint.roughness = 0.35f;
    std::vector<iface::MaterialDesc> materials{paint};

    // The nodes: the body, the hubs, the spins under them, the parts that can come loose.
    m_partNodes = static_cast<uint32_t>(parts.size());
    std::vector<iface::ModelNode> nodes(kParts + m_partNodes);
    static const char* const kWheels[4] = {"lf", "rf", "lb", "rb"};
    for (size_t i = 0; i < nodes.size(); ++i) {
        iface::ModelNode& n = nodes[i];
        n.parent = -1;
        n.local[0] = n.local[5] = n.local[10] = n.local[15] = 1;
        if (i == kBody) {
            std::snprintf(n.name, sizeof n.name, "body");
        } else if (i < kSpin) {
            std::snprintf(n.name, sizeof n.name, "hub_%s", kWheels[i - kHub]);
        } else if (i < kParts) {
            std::snprintf(n.name, sizeof n.name, "spin_%s", kWheels[i - kSpin]);
            n.parent = static_cast<int32_t>(kHub + i - kSpin);
        } else {
            std::snprintf(n.name, sizeof n.name, "%s", parts[i - kParts].name);
        }
    }
    // A mesh's node: a wheel's (spinning or not); a part's when the mesh is the part's (e_<name>, e_int_<name>: a door's
    // inside goes with the door) or hangs under it in the .sgc's node tree; else the body's.
    auto partOf = [&](const std::string& name) -> int {
        for (uint32_t i = 0; i < m_partNodes; ++i)
            if (name == std::string("e_") + parts[i].name || name == std::string("e_int_") + parts[i].name) return static_cast<int>(i);
        return -1;
    };
    auto nodeOf = [&](const CarPart& part) -> uint32_t {
        if (part.wheel >= 0) return (part.spins ? kSpin : kHub) + static_cast<uint32_t>(part.wheel);
        if (const int i = partOf(part.name); i >= 0) return kParts + static_cast<uint32_t>(i);
        for (const std::string& p : part.parents)
            if (const int i = partOf(p); i >= 0) return kParts + static_cast<uint32_t>(i);
        return kBody;
    };
    // PHYS_DRIVE_PARTS: every mesh's node, with the node tree above it.
    const bool listParts = std::getenv("PHYS_DRIVE_PARTS") != nullptr;

    std::vector<std::vector<iface::Vertex>> vertices;
    std::vector<std::vector<uint32_t>> indices;
    std::vector<iface::ModelMesh> meshes;
    std::vector<uint32_t> perPart(m_partNodes, 0);
    auto add = [&](std::vector<iface::Vertex> v, std::vector<uint32_t> idx, uint32_t material, uint32_t node) {
        vertices.push_back(std::move(v));
        indices.push_back(std::move(idx));
        iface::ModelMesh mm{};
        mm.material = material;
        mm.node = node;
        meshes.push_back(mm);
        if (node >= kParts) ++perPart[node - kParts];
    };
    try {
        const CarModel model = ReadCarModel(sgc, surface);
        size_t textured = 0;
        for (const CarTexture& t : model.materials) {
            const iface::TextureHandle tex = t.dds.empty() ? 0 : renderer.CreateTexture(t.dds.data(), t.dds.size());
            textured += tex != 0;
            if (std::getenv("PHYS_DRIVE_MATERIALS"))
                std::printf("material %s: %zu bytes dds%s, diffuse %.2f %.2f %.2f\n", t.material.c_str(), t.dds.size(),
                            tex == 0 && !t.dds.empty() ? " (rejected)" : "", t.diffuse[0], t.diffuse[1], t.diffuse[2]);
            // One-sided, as the game draws them: the body is a shell whose inner faces (the texture's black) lie close
            // behind its paint, and drawn they show through it. The glass both ways, seen from inside too.
            iface::MaterialDesc m = iface::DefaultMaterialDesc();
            m.baseColour = tex;
            m.flags = t.transparent ? iface::kMaterialTwoSided | iface::kMaterialAlphaTest : 0u;
            if (!tex)
                for (int k = 0; k < 3; ++k) m.baseColourFactor[k] = t.diffuse[k];
            m.roughness = 0.45f;
            materials.push_back(m);
        }
        for (const CarPart& part : model.parts) {
            std::vector<iface::Vertex> v(part.vertices.size());
            for (size_t k = 0; k < v.size(); ++k) {
                std::memcpy(v[k].pos, part.vertices[k].pos, 12);
                std::memcpy(v[k].normal, part.vertices[k].normal, 12);
                std::memcpy(v[k].uv0, part.vertices[k].uv, 8);
                std::memcpy(v[k].uv1, part.vertices[k].uv, 8);
                v[k].colour = 0xffffffff;
            }
            size_t mi = 0;
            while (mi < model.materials.size() && model.materials[mi].material != part.material) ++mi;
            if (mi >= model.materials.size()) continue;
            const uint32_t node = nodeOf(part);
            if (listParts) {
                std::string above;
                for (const std::string& p : part.parents) above += " < " + p;
                std::printf("mesh %s -> node %s%s\n", part.name.c_str(), nodes[node].name, above.c_str());
            }
            add(std::move(v), part.indices, static_cast<uint32_t>(mi + 1), node);
        }
        std::printf("the car: %zu parts, %zu materials (%zu textured)\n", meshes.size(), model.materials.size(), textured);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "the car's model: %s\n", e.what());
    }
    if (meshes.empty() && !collision.empty()) {
        std::vector<iface::Vertex> v;
        std::vector<uint32_t> idx;
        HullMesh(collision.at(0), v, idx);
        add(std::move(v), std::move(idx), 0, kBody);
    }
    if (m_partNodes) {
        std::string line = "the car's loose parts (meshes each):";
        for (uint32_t i = 0; i < m_partNodes; ++i) line += std::string(" ") + parts[i].name + " " + std::to_string(perPart[i]);
        std::printf("%s\n", line.c_str());
    }
    for (size_t i = 0; i < meshes.size(); ++i) {
        meshes[i].vertices = vertices[i].data();
        meshes[i].vertexCount = static_cast<uint32_t>(vertices[i].size());
        meshes[i].indices = indices[i].data();
        meshes[i].indexCount = static_cast<uint32_t>(indices[i].size());
    }
    iface::ModelDesc d{};
    d.nodes = nodes.data(), d.nodeCount = static_cast<uint32_t>(nodes.size());
    d.meshes = meshes.data(), d.meshCount = static_cast<uint32_t>(meshes.size());
    d.materials = materials.data(), d.materialCount = static_cast<uint32_t>(materials.size());
    const iface::ModelHandle model = meshes.empty() ? 0 : renderer.CreateModel(d);
    m_instance = model ? renderer.CreateInstance(model) : 0;
    if (!m_instance) std::fprintf(stderr, "the car: %s\n", renderer.Error().c_str());
}

const iface::InstanceUpdate& CarRender::Place(const physics::Pose& pose, const physics::Wheel wheels[4], const float cog[3],
                                              const std::vector<physics::Part>& parts)
{
    float car[16];
    PoseRows(pose.rotation, pose.position, car);
    m_nodes.resize(16 * (kParts + m_partNodes));
    float* body = &m_nodes[16 * kBody];
    PartMatrix(cog, nullptr, false, false, body);
    for (int k = 0; k < 4; ++k) {
        const bool right = k == 1 || k == 3;
        PartMatrix(cog, &wheels[k], right, false, &m_nodes[16 * (kHub + k)]);
        SpinNode(wheels[k].spin, right, &m_nodes[16 * (kSpin + k)]);
    }
    for (uint32_t i = 0; i < m_partNodes; ++i) {
        float* node = &m_nodes[16 * (kParts + i)];
        if (i < parts.size()) PartNode(body, parts[i], car, node);
        else std::memcpy(node, body, 64);
    }
    m_update = {};
    m_update.instance = m_instance;
    m_update.visible = 1;
    std::memcpy(m_update.world, car, sizeof m_update.world);
    m_update.nodes = m_nodes.data();
    m_update.nodeCount = static_cast<uint32_t>(kParts + m_partNodes);
    return m_update;
}

} // namespace wrally::assets
