// ---------------------------------------------------------------------------
// Instanced models (ngg_format.h NGGD / NGGE, NGG design §2.6): the model library loaded into the dynamic pool, the
// placements, their culling and level of detail, and the per-frame instance stream the passes draw them from.
//
// The passes themselves (the prepass, the main pass, the cascades) are in scene.cpp; they ask CullModels for a view's
// batches and draw each batch's submeshes with DrawIndexedInstanced, the stream bound to input slot 1.
// ---------------------------------------------------------------------------
#include "scene.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace ngg {
namespace core {

using Microsoft::WRL::ComPtr;

namespace {

// The cull's first level: placements binned by where they stand, in squares this wide (metres). On stage 41 a
// 64 m cell holds a few dozen trees, and a view's frustum rejects most cells whole.
const float kCellSize = 64.0f;
// LOD slots per model in a batch key (a model with more LODs is refused at load).
const uint32_t kMaxLods = 8;
// InstanceRec::flags (the VS's INST3.z).
const uint32_t kInstBillboard = 1u << 0;
const uint32_t kInstModel = 1u << 1;
// Records the stream starts with: the identity plus a stage's worth of trees in every view.
const size_t kInstInitial = 64 * 1024;

bool Outside(const float p[6][4], const float* bmin, const float* bmax)
{
    for (int i = 0; i < 6; ++i)
    {
        const float x = (p[i][0] >= 0.0f) ? bmax[0] : bmin[0];
        const float y = (p[i][1] >= 0.0f) ? bmax[1] : bmin[1];
        const float z = (p[i][2] >= 0.0f) ? bmax[2] : bmin[2];
        if (p[i][0] * x + p[i][1] * y + p[i][2] * z + p[i][3] < 0.0f) return true;
    }
    return false;
}

float BoxDistance(const float* bmin, const float* bmax, const float* eye)
{
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k)
    {
        const float c = (std::max)((std::max)(bmin[k] - eye[k], eye[k] - bmax[k]), 0.0f);
        d2 += c * c;
    }
    return sqrtf(d2);
}

int64_t CellKey(float x, float z)
{
    const int64_t ix = (int64_t)floorf(x / kCellSize), iz = (int64_t)floorf(z / kCellSize);
    return (ix << 32) ^ (iz & 0xFFFFFFFFll);
}

} // namespace

// The loaded library into the scene: materials appended to the table (with an alpha-tested twin for each opaque one,
// which a crossfading batch dithers through), each submesh a mesh of the dynamic pool, the placements of the drawn
// variant. `fileMaterials` is how many the file's NGGM had (the model materials follow them). False, with nothing
// added, when the chunks do not hold together - the cards then draw as before.
bool Scene::LoadModels(ID3D12Device* dev, const uint8_t* models, size_t modelBytes, const uint8_t* placements,
                       size_t placementBytes, uint32_t fileMaterials, std::vector<uint32_t>& replacedPlants)
{
    // The previous stage's meshes leave the pool (it is the scene's; nothing else holds them).
    for (const ModelDef& m : m_models)
        for (const ModelLodRt& l : m.lods)
            for (const ModelSub& s : l.subs)
                if (s.mesh && s.mesh <= m_dynMeshes.size())
                {
                    DynMesh& dm = m_dynMeshes[s.mesh - 1];
                    if (dm.refs) --dm.refs;
                    dm.alive = false;
                }
    m_models.clear();
    m_placements.clear();
    m_modelCells.clear();
    m_mainBatches.clear();
    m_mainBatchesValid = false;
    m_rtModelInstancesDirty = true;
    m_replacedPlants = m_replacedTris = 0;
    replacedPlants.clear();
    if (const char* e = std::getenv("NGG_MODEL_LOD_SCALE"))
        m_modelLodScale = (std::max)(0.05f, (float)atof(e));
    if (!models) return false;

    ModelChunkHeader h = {};
    if (modelBytes < sizeof(h)) { Log("scene: NGGD truncated - no models, the cards drawn"); return false; }
    memcpy(&h, models, sizeof(h));
    const uint64_t need = sizeof(h) + (uint64_t)h.modelCount * sizeof(ModelRecord) + (uint64_t)h.lodCount * sizeof(ModelLod) +
                          (uint64_t)h.submeshCount * sizeof(ModelSubmesh) + (uint64_t)h.materialCount * sizeof(Material) +
                          (uint64_t)h.vertexCount * sizeof(Vertex) + (uint64_t)h.indexCount * 4;
    if (h.version != kModelVersion || need != modelBytes)
    {
        Log("scene: NGGD version %u, %zu bytes (its header says %llu) - not understood, no models, the cards drawn", h.version,
            modelBytes, (unsigned long long)need);
        return false;
    }
    const ModelRecord*  rec  = reinterpret_cast<const ModelRecord*>(models + sizeof(h));
    const ModelLod*     lods = reinterpret_cast<const ModelLod*>(rec + h.modelCount);
    const ModelSubmesh* subs = reinterpret_cast<const ModelSubmesh*>(lods + h.lodCount);
    const Material*     mats = reinterpret_cast<const Material*>(subs + h.submeshCount);
    const Vertex*       vtx  = reinterpret_cast<const Vertex*>(mats + h.materialCount);
    const uint32_t*     ind  = reinterpret_cast<const uint32_t*>(vtx + h.vertexCount);

    // Checked whole before anything is added (ngg_scene.cpp CheckModels says the same in words).
    auto bad = [&](const char* why, uint32_t i) {
        Log("scene: NGGD: %s (%u) - no models, the cards drawn", why, i);
        return false;
    };
    for (uint32_t i = 0; i < h.submeshCount; ++i)
    {
        const ModelSubmesh& s = subs[i];
        if (s.material >= h.materialCount || !s.indexCount || s.indexCount % 3 ||
            (uint64_t)s.firstIndex + s.indexCount > h.indexCount || !s.vertexCount ||
            (uint64_t)s.firstVertex + s.vertexCount > h.vertexCount)
            return bad("a submesh runs past its arrays", i);
        for (uint32_t k = s.firstIndex; k < s.firstIndex + s.indexCount; ++k)
            if (ind[k] >= s.vertexCount) return bad("an index past its submesh", i);
    }
    for (uint32_t i = 0; i < h.lodCount; ++i)
        if (!lods[i].submeshCount || (uint64_t)lods[i].firstSubmesh + lods[i].submeshCount > h.submeshCount ||
            !(lods[i].maxDistance > 0.0f))
            return bad("a LOD runs past the submeshes or has no distance", i);
    for (uint32_t i = 0; i < h.modelCount; ++i)
    {
        const ModelRecord& r = rec[i];
        if (!r.lodCount || r.lodCount > kMaxLods || (uint64_t)r.firstLod + r.lodCount > h.lodCount)
            return bad("a model's LODs run past them (or are more than 8)", i);
        if (r.rtLod >= r.lodCount || (lods[r.firstLod + r.rtLod].flags & kModelLodBillboard))
            return bad("a model's ray-traced LOD is past its LODs or a billboard", i);
    }

    // Materials: as the file's (scene.cpp's `take`), every one marked a model's (its prepass keeps its own normals).
    const uint32_t matBase = (uint32_t)m_materials.size();
    for (uint32_t i = 0; i < h.materialCount; ++i)
    {
        const Material& m = mats[i];
        MaterialSlot s = {};
        s.tex[0] = m.baseColour; s.tex[1] = m.detail; s.tex[2] = m.normal; s.tex[3] = m.orm; s.tex[4] = m.emissive;
        s.flags = m.flags | kDrawModel;
        if (m.flags & kMaterialFoliage) s.flags |= kMaterialAlphaTest | kMaterialTwoSided | kDrawHashedAlpha;
        if (m.detail == kNoTexture) s.flags |= kDrawSingleStage;
        if (m.normal != kNoTexture) s.flags |= kDrawNormalMap;
        for (int k = 0; k < 4; ++k) s.baseColourFactor[k] = m.baseColourFactor[k];
        s.roughness = m.roughness; s.metallic = m.metallic; s.normalScale = m.normalScale;
        s.occlusionStrength = m.occlusionStrength; s.emissiveStrength = m.emissiveStrength; s.alphaCutoff = m.alphaCutoff;
        m_materials.push_back(s);
        for (int k = 2; k <= 3; ++k)          // normal and ORM maps are data, not colour (as the file's materials)
            if (s.tex[k] != kNoTexture)
            {
                if (s.tex[k] >= m_linearTextures.size()) m_linearTextures.resize(s.tex[k] + 1, 0);
                m_linearTextures[s.tex[k]] = 1;
            }
    }
    // The opaque ones' twins: alpha-tested with a cutoff nothing reaches, so a crossfading draw clips by its dither
    // alone (the opaque pipelines have no clip - their early depth test is the terrain's speed).
    std::vector<uint32_t> twin(h.materialCount, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < h.materialCount; ++i)
    {
        if (m_materials[matBase + i].flags & kMaterialAlphaTest) { twin[i] = matBase + i; continue; }
        MaterialSlot t = m_materials[matBase + i];
        t.flags |= kMaterialAlphaTest;
        t.alphaCutoff = -1.0f;
        twin[i] = (uint32_t)m_materials.size();
        m_materials.push_back(t);
    }
    (void)fileMaterials;

    // Meshes: a submesh each, held (refs) so Compact keeps them.
    size_t tris = 0, lodTotal = 0, subTotal = 0;
    m_models.resize(h.modelCount);
    for (uint32_t i = 0; i < h.modelCount; ++i)
    {
        const ModelRecord& r = rec[i];
        ModelDef& m = m_models[i];
        m.name.assign(r.name, strnlen(r.name, sizeof(r.name)));
        memcpy(m.bmin, r.bboxMin, sizeof(m.bmin));
        memcpy(m.bmax, r.bboxMax, sizeof(m.bmax));
        m.fade = (std::max)(0.0f, r.fadeWidth);
        m.rtLod = r.rtLod;
        m.lods.resize(r.lodCount);
        for (uint32_t l = 0; l < r.lodCount; ++l)
        {
            const ModelLod& L = lods[r.firstLod + l];
            ModelLodRt& lr = m.lods[l];
            lr.maxDist = L.maxDistance;
            lr.billboard = (L.flags & kModelLodBillboard) != 0;
            for (uint32_t k = 0; k < L.submeshCount; ++k)
            {
                const ModelSubmesh& s = subs[L.firstSubmesh + k];
                ModelSub ms;
                ms.mesh = AddMesh(dev, vtx + s.firstVertex, s.vertexCount, ind + s.firstIndex, s.indexCount);
                if (!ms.mesh) { Log("scene: NGGD: model %s's mesh was refused", m.name.c_str()); continue; }
                ++m_dynMeshes[ms.mesh - 1].refs;
                ms.material = matBase + s.material;
                ms.fadeMaterial = twin[s.material];
                ms.alpha = (m_materials[ms.material].flags & kMaterialAlphaTest) != 0;
                lr.subs.push_back(ms);
                tris += s.indexCount / 3;
                ++subTotal;
            }
            ++lodTotal;
        }
    }

    // Placements: the drawn variant's, moved into its frame.
    uint32_t variantBit = 0xFFFFFFFFu;
    for (size_t v = 0; v < m_variantNames.size(); ++v)
        if (m_variantNames[v] == m_variant) variantBit = 1u << v;
    size_t otherVariant = 0;
    if (placements)
    {
        PlacementChunkHeader ph = {};
        if (placementBytes >= sizeof(ph)) memcpy(&ph, placements, sizeof(ph));
        if (placementBytes < sizeof(ph) || ph.version != kPlacementVersion || ph.recordSize < sizeof(Placement) ||
            sizeof(ph) + (uint64_t)ph.count * ph.recordSize > placementBytes)
            Log("scene: NGGE version %u, %u record(s) of %u bytes in %zu - not understood, no placements", ph.version, ph.count,
                ph.recordSize, placementBytes);
        else
        {
            m_placements.reserve(ph.count);
            for (uint32_t k = 0; k < ph.count; ++k)
            {
                Placement p;
                memcpy(&p, placements + sizeof(ph) + (size_t)k * ph.recordSize, sizeof(p));
                if (p.model >= m_models.size() || !(p.scaleXZ > 0.0f) || !(p.scaleY > 0.0f)) continue;
                if (!(p.variants & variantBit)) { ++otherVariant; continue; }
                PlacementRt pr;
                pr.d.model = p.model;
                pr.d.plant = p.plant;
                for (int c = 0; c < 3; ++c) pr.d.position[c] = p.position[c] + m_variantOffset[c];
                pr.d.yaw = p.yaw;
                pr.d.scaleXZ = p.scaleXZ;
                pr.d.scaleY = p.scaleY;
                pr.d.tint = p.tint;
                PlacementTransform(pr);
                m_placements.push_back(pr);
                if (p.plant != kNoPlant) replacedPlants.push_back(p.plant);
            }
        }
    }
    std::sort(replacedPlants.begin(), replacedPlants.end());
    replacedPlants.erase(std::unique(replacedPlants.begin(), replacedPlants.end()), replacedPlants.end());
    m_modelCellsDirty = true;
    RebuildModelCells();
    Log("scene: models - %u model(s): %zu LODs, %zu submeshes, %zu triangles, %u material(s) (+%zu twins); "
        "%zu placement(s) (%zu in other variants), %zu replace a plant; LOD scale %.2f",
        h.modelCount, lodTotal, subTotal, tris, h.materialCount, m_materials.size() - matBase - h.materialCount,
        m_placements.size(), otherVariant, replacedPlants.size(), m_modelLodScale);
    return true;
}

void Scene::PlacementTransform(PlacementRt& p) const
{
    const PlacementDesc& d = p.d;
    const float c = cosf(d.yaw), s = sinf(d.yaw), sh = d.scaleXZ, sv = d.scaleY;
    // p' = A p + t, from ngg_format.h's row-vector definition (scale, yaw, translate).
    const float rows[3][4] = {
        {  sh * c, 0.0f, sh * s, d.position[0] },
        {  0.0f,   sv,   0.0f,   d.position[1] },
        { -sh * s, 0.0f, sh * c, d.position[2] },
    };
    memcpy(p.cols, rows, sizeof(rows));
    // The box: yaw-free, round the up axis through the base (a billboard turns within it too).
    const ModelDef& m = m_models[d.model];
    float r2 = 0.0f;
    for (int corner = 0; corner < 4; ++corner)
    {
        const float x = (corner & 1) ? m.bmax[0] : m.bmin[0], z = (corner & 2) ? m.bmax[2] : m.bmin[2];
        r2 = (std::max)(r2, x * x + z * z);
    }
    const float r = sqrtf(r2) * sh;
    p.bmin[0] = d.position[0] - r; p.bmax[0] = d.position[0] + r;
    p.bmin[2] = d.position[2] - r; p.bmax[2] = d.position[2] + r;
    p.bmin[1] = d.position[1] + (std::min)(0.0f, m.bmin[1] * sv);
    p.bmax[1] = d.position[1] + (std::max)(0.0f, m.bmax[1] * sv);
    for (int k = 0; k < 3; ++k) p.centre[k] = 0.5f * (p.bmin[k] + p.bmax[k]);
}

void Scene::RebuildModelCells()
{
    if (!m_modelCellsDirty) return;
    m_modelCellsDirty = false;
    m_modelCells.clear();
    std::unordered_map<int64_t, uint32_t> at;
    for (uint32_t i = 0; i < m_placements.size(); ++i)
    {
        const PlacementRt& p = m_placements[i];
        if (p.removed) continue;
        const int64_t key = CellKey(p.d.position[0], p.d.position[2]);
        auto it = at.find(key);
        if (it == at.end())
        {
            it = at.emplace(key, (uint32_t)m_modelCells.size()).first;
            ModelCell c;
            for (int k = 0; k < 3; ++k) c.bmin[k] = 1e30f, c.bmax[k] = -1e30f;
            c.reach = 0.0f;
            m_modelCells.push_back(std::move(c));
        }
        ModelCell& c = m_modelCells[it->second];
        c.items.push_back(i);
        for (int k = 0; k < 3; ++k)
        {
            c.bmin[k] = (std::min)(c.bmin[k], p.bmin[k]);
            c.bmax[k] = (std::max)(c.bmax[k], p.bmax[k]);
        }
        const ModelDef& m = m_models[p.d.model];
        c.reach = (std::max)(c.reach, m.lods.back().maxDist * p.d.scaleY * m_modelLodScale);
    }
}

uint32_t Scene::LivePlacementCount() const
{
    uint32_t n = 0;
    for (const PlacementRt& p : m_placements) n += p.removed ? 0 : 1;
    return n;
}

bool Scene::GetPlacement(uint32_t index, PlacementDesc* out) const
{
    if (index >= m_placements.size() || !out || m_placements[index].removed) return false;
    *out = m_placements[index].d;
    return true;
}

bool Scene::AddPlacements(const PlacementDesc* descs, uint32_t count, uint32_t* out)
{
    if (!descs && count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (descs[i].model >= m_models.size() || !(descs[i].scaleXZ > 0.0f) || !(descs[i].scaleY > 0.0f)) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        PlacementRt p;
        p.d = descs[i];
        p.d.plant = kNoPlant;            // which cards draw is the file's (the static geometry); an edit adds a model
        PlacementTransform(p);
        if (out) out[i] = (uint32_t)m_placements.size();
        m_placements.push_back(p);
    }
    m_modelCellsDirty = m_rtModelInstancesDirty = true;
    ++m_geomRevision;
    return true;
}

bool Scene::SetPlacements(const uint32_t* indices, const PlacementDesc* descs, uint32_t count)
{
    if ((!indices || !descs) && count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] >= m_placements.size() || m_placements[indices[i]].removed || descs[i].model >= m_models.size() ||
            !(descs[i].scaleXZ > 0.0f) || !(descs[i].scaleY > 0.0f))
            return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        PlacementRt& p = m_placements[indices[i]];
        const uint32_t plant = p.d.plant;
        p.d = descs[i];
        p.d.plant = plant;
        PlacementTransform(p);
    }
    m_modelCellsDirty = m_rtModelInstancesDirty = true;
    ++m_geomRevision;
    return true;
}

bool Scene::RemovePlacements(const uint32_t* indices, uint32_t count)
{
    if (!indices && count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] >= m_placements.size()) return false;
    for (uint32_t i = 0; i < count; ++i) m_placements[indices[i]].removed = true;   // (idempotent: undo hits it)
    m_modelCellsDirty = m_rtModelInstancesDirty = true;
    ++m_geomRevision;
    return true;
}

// ---------------------------------------------------------------- the stream

bool Scene::InstanceAlloc(ID3D12Device* dev, size_t records, uint8_t*& cpu, D3D12_GPU_VIRTUAL_ADDRESS& gpu)
{
    const size_t bytes = records * sizeof(InstanceRec);
    if (!m_instBuf || m_instUsed + bytes > m_instCap)
    {
        if (!dev) return false;
        size_t cap = (std::max)(m_instCap * 2, kInstInitial * sizeof(InstanceRec));
        while (cap < bytes) cap *= 2;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = cap;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buf;
        void* mapped = nullptr;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&buf))) ||
            FAILED(buf->Map(0, &none, &mapped)))
        {
            Log("scene: the instance stream (%zu bytes) could not be created", cap);
            return false;
        }
        // The old one may still be read by this frame's draws: kept until the next BeginFrame.
        if (m_instBuf) m_instRetired.push_back(m_instBuf);
        m_instBuf = buf;
        m_instMapped = static_cast<uint8_t*>(mapped);
        m_instCap = cap;
        m_instUsed = 0;
    }
    cpu = m_instMapped + m_instUsed;
    gpu = m_instBuf->GetGPUVirtualAddress() + m_instUsed;
    m_instUsed += bytes;
    return true;
}

void Scene::EnsureInstanceStream(ID3D12Device* dev)
{
    static_assert(sizeof(InstanceRec) == 64, "InstanceRec is the VS's INST0..INST3 (64 bytes)");
    if (m_instIdentity.BufferLocation) return;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    if (!InstanceAlloc(dev, 1, cpu, gpu)) return;
    InstanceRec id = {};
    id.c[0][0] = id.c[1][1] = id.c[2][2] = 1.0f;
    id.tint = 0xFFFFFFFFu;
    memcpy(cpu, &id, sizeof(id));
    m_instIdentity.BufferLocation = gpu;
    m_instIdentity.SizeInBytes = sizeof(InstanceRec);
    m_instIdentity.StrideInBytes = sizeof(InstanceRec);
}

void Scene::BeginFrame(ID3D12Device* dev)
{
    // The last frame has run (the frame is synchronous): its stream is free again, the retired buffers too.
    m_instRetired.clear();
    m_instUsed = 0;
    m_instIdentity = {};
    m_mainBatchesValid = false;
    if (m_dynDirty && dev) UploadDynamic(dev);
    EnsureInstanceStream(dev);

    m_modelStats = m_modelStatsBuild;
    m_modelStatsBuild = ModelStats();
    m_modelStatsBuild.placements = LivePlacementCount();
    if (!m_placements.empty() && (m_modelFrame++ % 600) == 0)
        Log("scene: models - %u placement(s), %u in view: LOD0 %u, LOD1 %u, LOD2 %u, billboard %u (%u crossfading); "
            "%u shadow-map instance(s); %u instanced draw(s); cull %.3f ms (CPU)",
            m_modelStats.placements, m_modelStats.visible, m_modelStats.perLod[0], m_modelStats.perLod[1],
            m_modelStats.perLod[2], m_modelStats.perLod[3], m_modelStats.fading, m_modelStats.shadowInstances,
            m_modelStats.draws, m_modelStats.cullMs);
}

// ------------------------------------------------------------------ the cull

void Scene::CullModels(ID3D12Device* dev, const float planes[6][4], const float eye[3], bool shadow,
                       std::vector<ModelBatch>& out)
{
    out.clear();
    if (m_placements.empty() || m_models.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    RebuildModelCells();
    EnsureInstanceStream(dev);

    const size_t keys = m_models.size() * kMaxLods * 2;
    std::vector<uint32_t> counts(keys, 0);
    m_cullScratch.clear();
    uint32_t visible = 0;
    auto emit = [&](const PlacementRt& p, uint32_t lod, float fade) {
        const ModelDef& m = m_models[p.d.model];
        const uint64_t key = ((uint64_t)p.d.model * kMaxLods + lod) * 2 + (fade != 0.0f ? 1 : 0);
        InstanceRec r;
        memcpy(r.c, p.cols, sizeof(r.c));
        r.tint = p.d.tint;
        r.fade = fade;
        r.flags = kInstModel | (m.lods[lod].billboard ? kInstBillboard : 0u);
        r.pad = 0;
        m_cullScratch.emplace_back(key, r);
        ++counts[key];
        if (!shadow)
        {
            ++m_modelStatsBuild.perLod[m.lods[lod].billboard ? 3 : (std::min)(lod, 2u)];
            if (fade != 0.0f) ++m_modelStatsBuild.fading;
        }
    };
    for (const ModelCell& c : m_modelCells)
    {
        if (Outside(planes, c.bmin, c.bmax)) continue;
        if (BoxDistance(c.bmin, c.bmax, eye) > c.reach) continue;
        for (uint32_t idx : c.items)
        {
            const PlacementRt& p = m_placements[idx];
            if (Outside(planes, p.bmin, p.bmax)) continue;
            const ModelDef& m = m_models[p.d.model];
            const float s = p.d.scaleY * m_modelLodScale;
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) d2 += (p.centre[k] - eye[k]) * (p.centre[k] - eye[k]);
            const float d = sqrtf(d2);
            uint32_t lod = 0;
            while (lod < m.lods.size() && d >= m.lods[lod].maxDist * s) ++lod;
            if (lod >= m.lods.size()) continue;
            ++visible;
            const float fw = m.fade * s, end = m.lods[lod].maxDist * s;
            if (shadow || fw <= 0.0f || d <= end - fw)
            {
                emit(p, lod, 0.0f);
                continue;
            }
            // The crossfade: t of the way through the band. This LOD drops the pixels whose dither is under t, the
            // next keeps exactly those - every pixel drawn once, by one of the two.
            const float t = (std::min)(1.0f, (std::max)(1e-4f, (d - (end - fw)) / fw));
            emit(p, lod, t);
            if (lod + 1 < m.lods.size()) emit(p, lod + 1, -t);
        }
    }
    if (!m_cullScratch.empty())
    {
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        if (InstanceAlloc(dev, m_cullScratch.size(), cpu, gpu))
        {
            std::vector<uint32_t> start(keys, 0);
            uint32_t at = 0;
            for (size_t k = 0; k < keys; ++k)
            {
                start[k] = at;
                if (!counts[k]) continue;
                ModelBatch b;
                b.model = (uint32_t)(k / 2 / kMaxLods);
                b.lod = (uint32_t)(k / 2 % kMaxLods);
                b.fading = (k & 1) != 0;
                b.count = counts[k];
                b.vbv.BufferLocation = gpu + (UINT64)at * sizeof(InstanceRec);
                b.vbv.SizeInBytes = counts[k] * (UINT)sizeof(InstanceRec);
                b.vbv.StrideInBytes = sizeof(InstanceRec);
                out.push_back(b);
                at += counts[k];
            }
            InstanceRec* dst = reinterpret_cast<InstanceRec*>(cpu);
            for (const auto& e : m_cullScratch) dst[start[e.first]++] = e.second;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_modelStatsBuild.cullMs += ms;
    if (shadow) m_modelStatsBuild.shadowInstances += (uint32_t)m_cullScratch.size();
    else m_modelStatsBuild.visible = visible;
}

void Scene::ModelDrawItems(const std::vector<ModelBatch>& batches, std::vector<DrawOrder>& order, bool shadow)
{
    size_t n = 0;
    for (const ModelBatch& b : batches) n += m_models[b.model].lods[b.lod].subs.size();
    m_modelDrawItems.clear();
    m_modelDrawItems.reserve(n);       // DrawOrder points into it: no reallocation from here on
    for (const ModelBatch& b : batches)
    {
        const ModelDef& m = m_models[b.model];
        const ModelLodRt& L = m.lods[b.lod];
        const float nearest = b.lod ? m.lods[b.lod - 1].maxDist : 0.0f;
        for (const ModelSub& s : L.subs)
        {
            if (!s.mesh || s.mesh > m_dynMeshes.size() || !m_dynMeshes[s.mesh - 1].alive) continue;
            const DynMesh& dm = m_dynMeshes[s.mesh - 1];
            DrawItem o = {};
            o.layerId = 0;
            // A crossfading batch's opaque submesh dithers through its alpha-tested twin (the shadow maps take no fade).
            o.materialId = (b.fading && !s.alpha && !shadow) ? s.fadeMaterial : s.material;
            o.matFlags = m_materials[o.materialId].flags;
            o.firstIndex = dm.firstIndex;
            o.indexCount = dm.indexCount;
            o.meshId = s.mesh;
            o.baseVertex = (int32_t)dm.firstVertex;
            for (int k = 0; k < 3; ++k) o.bmin[k] = o.bmin0[k] = -1e30f, o.bmax[k] = o.bmax0[k] = 1e30f;
            m_modelDrawItems.push_back(o);
            DrawOrder d;
            d.alphaTest = (o.matFlags & kMaterialAlphaTest) != 0;
            d.d2 = nearest * nearest;
            d.item = &m_modelDrawItems.back();
            d.inst = &b.vbv;
            d.instCount = b.count;
            order.push_back(d);
        }
    }
}

} // namespace core
} // namespace ngg
