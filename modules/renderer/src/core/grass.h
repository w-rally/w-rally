#pragma once
// GRASS (NGG design §2.6 "Grass"; Wallaby's vegetation plan, workstream 7). The file says WHERE grass grows - the
// NGGF chunk: a mask per ground texture and a paint layer (ngg_format.h) - and this draws it round the camera, with
// nothing stored per blade:
//
//   1. THE FIELD, a clipmap round the camera (kClip^2 texels of kTexel metres): the terrain drawn from above into it -
//      the ground's height, its colour (the main pass's blend of the two layers), and the density (the masks' blend
//      times the paint), the type and the height scale. Drawn again when the camera has moved kRedrawTexels.
//   2. THE SPAWN, a compute pass over the world's 0.5 m patches within reach: each patch's blades from hashes of the
//      patch, so a blade stays where it is as the camera moves; the patch culled to the frustum; the blades written as
//      instances, with the draws' indirect arguments - one LOD per patch, by distance: blades of 7 triangles, blades of
//      3, a card per clump.
//   3. THE DRAWS (Scene::DrawGrass): the scene's own prepass and main pass shaders (scene.hlsl VSGrass, the GRASS
//      variants), so the ray-traced visibility, the AO, TAA and the motion vectors take the blades as any surface.
//
// Grass receives the sun (traced or the cascades) and the trees' shadows, and casts nothing: it is in no shadow map
// and not in the BVH.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace ngg {
namespace core {

class Grass
{
public:
    // The clipmap: kClip texels a side, kTexel metres each (128 m: reach 50 m with the camera up to kRedrawTexels off
    // its centre).
    static constexpr uint32_t kClip = 512;
    static constexpr float    kTexel = 0.25f;
    static constexpr uint32_t kRedrawTexels = 16;   // 4 m
    static constexpr float    kPatch = 0.5f;        // the spawn's unit, metres
    static constexpr uint32_t kLods = 3;            // blades of 7 triangles, of 3, a card per clump
    static constexpr uint32_t kMaxTypes = 16;
    // Instances per LOD (32 bytes each): what the spawn may write; more are dropped (the readout says so).
    static constexpr uint32_t kCap[kLods] = { 160u * 1024u, 256u * 1024u, 96u * 1024u };

    // The chunk, checked (ngg_format.h CheckGrassChunk; a refusal is logged and draws no grass), the paint tiles of
    // stage variant `variant` (an index into NGGR; 0 without one) at its `offset`, uploaded. False: no grass.
    bool Load(ID3D12Device* dev, const uint8_t* payload, size_t bytes, uint32_t textureCount, uint32_t plantCount,
              uint32_t variant, const float offset[3]);
    void Reset();
    bool Loaded() const { return m_loaded; }
    // The NGGP plants whose cards the grass replaces (drawn when the grass is off).
    const std::vector<uint32_t>& ReplacedPlants() const { return m_replaced; }
    // The mask of a ground texture (an NGGT index), ~0 for none.
    uint32_t MaskOf(uint32_t texture) const { return texture < m_maskOf.size() ? m_maskOf[texture] : 0xFFFFFFFFu; }
    uint32_t TypeCount() const { return (uint32_t)m_types.size(); }
    const char* TypeName(uint32_t t) const { return t < m_typeNames.size() ? m_typeNames[t].c_str() : ""; }

    // What the field is drawn from: the static scene's buffers and the terrain's draws (layer 0, not alpha-tested).
    struct TerrainDraw
    {
        uint32_t firstIndex = 0, indexCount = 0;
        uint32_t material = 0;        // its five SRVs at heap start + material * 5
        uint32_t flags = 0;           // Scene::kDrawSingleStage where it has no second layer
        uint32_t baseMask = 0xFFFFFFFFu, detailMask = 0xFFFFFFFFu;
        float    factor[3] = { 1, 1, 1 };
        float    bmin[3] = {}, bmax[3] = {};
    };
    struct Terrain
    {
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        D3D12_INDEX_BUFFER_VIEW  ibv = {};
        ID3D12DescriptorHeap*    heap = nullptr;   // the scene's SRV heap
        UINT                     stride = 0;
        const std::vector<TerrainDraw>* draws = nullptr;
    };

    // The field and the spawn for this frame's camera, before the prepass: `reach` metres (0: nothing - the draws
    // then draw nothing), `density` a scale on the types' blades per m2 (NGG_GRASS_DENSITY). The list's heap and
    // root signature are left the grass's: the caller binds its own after.
    void Update(ID3D12GraphicsCommandList* list, const Terrain& terrain, const float* view, const float* proj,
                float reach, float density);
    bool Spawned() const { return m_spawned; }
    // The blades' instances (the draws' input slot 1) and indirect arguments, kLods DRAW records of 16 bytes - in
    // their draw states after Update.
    ID3D12Resource* Args() const { return m_args.Get(); }
    D3D12_VERTEX_BUFFER_VIEW InstanceView() const;
    ID3D12CommandSignature* CommandSignature() const { return m_cmdSig.Get(); }

    // Last frame's instance count per LOD (after the GPU wait: the frame is synchronous), and how many the caps
    // dropped. Read by ReadStats.
    void ReadStats();
    uint32_t Count(uint32_t lod) const { return lod < kLods ? m_counts[lod] : 0; }
    uint32_t Dropped() const { return m_dropped; }

private:
    bool CreateGpu(ID3D12Device* dev);
    void DrawField(ID3D12GraphicsCommandList* list, const Terrain& terrain);

    bool m_loaded = false, m_gpuTried = false, m_gpuOk = false, m_spawned = false;
    std::vector<uint8_t> m_payload;            // the chunk as read (the shaders address its texels by its offsets)
    struct TypeGpu                             // GrassType as the spawn reads it (grass.hlsl GrassTypeGpu)
    {
        float heightMin, heightMax, density, width;
        float dryness, spread, wind, seedHeads;
    };
    std::vector<TypeGpu> m_types;
    std::vector<std::string> m_typeNames;
    std::vector<uint32_t> m_maskOf;            // per NGGT texture: its mask's index, ~0
    std::vector<uint32_t> m_maskRec;           // per mask: dataOffset, width | height << 16, type
    std::vector<uint32_t> m_replaced;
    // The paint: a grid of tile slots over the tiles' extent, each a tile's dataOffset or ~0 (neutral).
    int32_t  m_gridX0 = 0, m_gridZ0 = 0;
    uint32_t m_gridW = 0, m_gridH = 0;
    std::vector<uint32_t> m_grid;
    float    m_cellSize = 0.5f;
    uint32_t m_tileCells = 64;
    float    m_offset[3] = { 0, 0, 0 };

    Microsoft::WRL::ComPtr<ID3D12Resource> m_payloadBuf, m_gridBuf, m_typeBuf;
    // The field: height (R32F), colour (sRGB) + the mask's dryness, density | height scale | type | covered.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_field[3], m_fieldDepth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap, m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_inst, m_args, m_argsInit, m_argsReadback;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_fieldRs, m_spawnRs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fieldPso, m_spawnPso, m_clampPso;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_cmdSig;
    D3D12_RESOURCE_STATES m_fieldState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES m_instState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool     m_fieldValid = false;
    int32_t  m_fieldX = 0, m_fieldZ = 0;       // the clipmap's centre, in texels of the world
    uint32_t m_counts[kLods] = {}, m_dropped = 0;
    bool     m_readbackPending = false;
    uint32_t m_frame = 0;
};

} // namespace core
} // namespace ngg
