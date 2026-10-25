// ---------------------------------------------------------------------------
// wrally/iface/renderer.h - the Renderer module interface (WRenderer.dll).
//
// Built on NGG (plugin-rbr/ngg): ngg-core is the implementation, and this is
// ngg-api reshaped from an EDITOR's interface into a GAME's. The two differ in
// one way that decides the shape: the editor edits a scene call by call, the
// game describes a whole frame once per frame. So most of the per-object
// setters of ngg_api.h become one FrameDesc handed to renderFrame - one call
// across the boundary per frame instead of hundreds, and no renderer-side state
// that can drift from the game's.
//
// THE RENDERER KNOWS NOTHING ABOUT RBR - the rule ngg-core already keeps (NGG
// design 8.0). It draws .ngg scenes and models it is given. Reading RBR's
// .lbs / .sgc / .dds and turning them into that is the game's assets layer
// (design doc, "Assets"), which is what lets legacy and new tracks share one
// renderer path.
//
// Conventions (all of them NGG's):
//   - render axes: y up, as RBR's renderer and the .ngg (physics is z up)
//   - matrices row-major, row-vector (v * M), translation in row 3
//   - projection conventional (near 0, far 1); reverse-Z is applied inside
//   - colours 0xAABBGGRR
//   - handles: nonzero; 0 is "none"
//
// THREADING: one thread - the game's render thread - makes every call on a
// Renderer and everything it created. The module may use threads of its own.
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/common.h"

namespace wrally::iface {

// 2: FrameDesc::weatherPreset, RenderSettings::shadows, RenderStats as the
//    renderer's readout, sceneKeyStats; models and textures belong to the
//    loaded scene.
constexpr uint32_t kRendererInterfaceVer = 2;

struct Renderer;                     // the module's instance, opaque
typedef uint32_t SceneHandle;        // a loaded .ngg stage
typedef uint32_t TextureHandle;
typedef uint32_t ModelHandle;        // geometry + materials + node tree (a car)
typedef uint32_t InstanceHandle;     // one placed model

// RendererCaps (ModuleInfo::caps)
constexpr uint64_t kRendererCapRayTracing = 1ull << 0;   // the adapter can, and the module will
constexpr uint64_t kRendererCapVr         = 1ull << 1;   // OpenXR (NGG design 1.8)
constexpr uint64_t kRendererCapHdrOutput  = 1ull << 2;

// RendererInput::flags
constexpr uint32_t kRendererDebugLayer   = 1u << 0;
constexpr uint32_t kRendererNoRayTracing = 1u << 1;

#pragma pack(push, 4)

// Same layout as ngg::Vertex / ngg_vertex (44 bytes).
struct Vertex {
    float    pos[3];
    float    normal[3];
    float    uv0[2];
    float    uv1[2];
    uint32_t colour;
};
static_assert(sizeof(Vertex) == 44, "Vertex - keep in step with ngg_format.h");

// Metallic-roughness, as ngg_material_desc / ngg::Material v3. Texture slots
// are TextureHandles from createTexture, 0 for an empty slot (which binds
// white, so a factor alone is a complete material).
constexpr uint32_t kMaterialAlphaTest = 1u << 0;
constexpr uint32_t kMaterialTwoSided  = 1u << 1;
constexpr uint32_t kMaterialBlend     = 1u << 2;   // alpha-blended (car glass) - not drawn blended yet: WRenderer ignores it

struct MaterialDesc {
    TextureHandle baseColour, detail, normal, orm, emissive;
    uint32_t      flags;
    float         baseColourFactor[4];
    float         roughness, metallic, normalScale, occlusionStrength, emissiveStrength, alphaCutoff;
};
static_assert(sizeof(MaterialDesc) == 64, "MaterialDesc layout");

// What a material is before anything is said about it: ngg::DefaultMaterial - a
// dielectric, fairly rough, opaque white surface.
inline MaterialDesc DefaultMaterialDesc()
{
    MaterialDesc m{};
    m.baseColourFactor[0] = m.baseColourFactor[1] = m.baseColourFactor[2] = m.baseColourFactor[3] = 1.0f;
    m.roughness = 0.75f;
    m.metallic = 0.0f;
    m.normalScale = 1.0f;
    m.occlusionStrength = 1.0f;
    m.emissiveStrength = 0.0f;
    m.alphaCutoff = 170.0f / 255.0f;
    return m;
}

// As ngg_weather_desc: physical values only.
struct WeatherDesc {
    float sunDirection[3];   // the direction light travels, unit length
    float sunIntensity;
    float turbidity;
    float groundAlbedo;
    float groundColour[3];
    float cloudCover;
    float fogVisibility;     // m; 0 no fog
    float fogHeight;
    float rain, snow;        // 0..1
    float exposure;
};
static_assert(sizeof(WeatherDesc) == 60, "WeatherDesc layout");

// ------------------------------------------------------------------ models
//
// A model is a node tree with meshes on the nodes - what a car is (.sgc today,
// a car .ngg later). The game animates it by node transforms each frame
// (wheels, steering, doors, detached parts); the renderer never interprets node
// names. A mesh is drawn at node(world) = local * parent(world) * ... * the
// instance's world.
struct ModelMesh {
    const Vertex*   vertices;
    uint32_t        vertexCount;
    const uint32_t* indices;      // mesh-relative
    uint32_t        indexCount;
    uint32_t        material;     // index into ModelDesc::materials
    uint32_t        node;         // index into ModelDesc::nodes
};

struct ModelNode {
    int32_t parent;               // index, -1 for a root; a parent comes before its children
    float   local[16];            // rest transform relative to the parent
    char    name[32];             // for logs and debug views only
};

struct ModelDesc {
    const ModelNode*    nodes;
    uint32_t            nodeCount;
    const ModelMesh*    meshes;
    uint32_t            meshCount;
    const MaterialDesc* materials;
    uint32_t            materialCount;
};

// ------------------------------------------------------------------- frame

struct Camera {
    float view[16];
    float proj[16];               // conventional depth; the renderer reverses it
    float nearZ, farZ;
    float fovY;                   // radians, for LOD and culling decisions
    uint32_t cut;                 // 1 when the camera jumped (cut, new camera): TAA history is dropped
};

// One placed model this frame. `nodes`, if given, overrides every node's
// transform (nodeCount must equal the model's); null keeps the rest pose.
struct InstanceUpdate {
    InstanceHandle instance;
    uint32_t       visible;       // 1 / 0
    float          world[16];
    const float*   nodes;         // nodeCount x 16 floats, relative to the parent
    uint32_t       nodeCount;
};

// A stage object the physics moved (cone, sign, fence): every scene object
// carrying this .trk instance id (NGGK, ngg_object_key) is drawn with `delta`,
// a WORLD-SPACE delta from its authored placement (identity: where the file has it).
// Kept until changed: a key not in this frame's list stays as it was last set.
struct StageObjectUpdate {
    int32_t  key;
    uint32_t visible;
    float    delta[16];
};

// 2D: the game's whole UI (menus, HUD, debug UI) as one indexed triangle list
// in pixels, drawn after the tonemap. Shaped like Dear ImGui's draw data, so
// ImGui and the game's own widgets submit the same way.
struct UiVertex {
    float    pos[2];              // pixels from the top-left
    float    uv[2];
    uint32_t colour;
};
static_assert(sizeof(UiVertex) == 20, "UiVertex - ImDrawVert's layout");
struct UiCommand {
    TextureHandle texture;        // 0: white; else one from createTextureRgba
    float         clip[4];        // x0, y0, x1, y1 pixels
    uint32_t      firstIndex;
    uint32_t      indexCount;
    uint32_t      vertexOffset;
};
struct UiDrawList {
    const UiVertex*  vertices;
    uint32_t         vertexCount;
    const uint32_t*  indices;
    uint32_t         indexCount;
    const UiCommand* commands;
    uint32_t         commandCount;
};

// World-space lines (debug: drive line, contact points, triggers), as
// ngg_set_overlay_lines' segments: consecutive pairs are one line.
struct LineVertex {
    float    pos[3];
    uint32_t colour;
};

constexpr uint32_t kFrameNoWorld = 1u << 0;   // menus without a stage: UI only

struct FrameDesc {
    uint32_t                 flags;
    SceneHandle              scene;           // 0: none
    Camera                   camera;
    // >= 0: the scene's weather preset of that index, exactly as the .ngg has
    // it (NGGW; 0 its default). -1: `weather`.
    int32_t                  weatherPreset;
    WeatherDesc              weather;
    float                    time;            // seconds, the game's: animated materials, wind
    const InstanceUpdate*    instances;
    uint32_t                 instanceCount;
    const StageObjectUpdate* stageObjects;
    uint32_t                 stageObjectCount;
    const LineVertex*        lines;
    uint32_t                 lineVertexCount;
    UiDrawList               ui;
};

// -------------------------------------------------------------- settings

enum class WindowMode : uint32_t { Windowed = 0, Borderless = 1, Fullscreen = 2 };

struct RenderSettings {
    WindowMode mode;
    uint32_t   width, height;     // 0, 0: the window's client size
    uint32_t   refreshHz;         // fullscreen only; 0 the desktop's
    uint32_t   vsync;
    uint32_t   quality;           // 0 low, 1 medium, 2 high (ngg Quality tiers)
    uint32_t   rayTracing;        // use it when available
    uint32_t   taa;
    uint32_t   autoExposure;
    uint32_t   shadows;           // sun shadows at all
    float      renderScale;       // 1 native; < 1 upscaled
};

// What the last finished frame did - ngg_stats, the renderer's own readout.
struct RenderStats {
    float    passMs, envMs, aerialMs;      // the main pass; its environment and aerial-table parts (GPU ms)
    float    rtMs, aoMs, denoiseMs;        // the ray tracing: all of it, the AO's part, the denoise
    uint32_t rtAvailable, rtActive;        // the adapter can ray trace; this frame's shadows were ray traced
    uint32_t rtPixels, rtShadowed;         // pixels traced, of them in shadow
    uint32_t quality;                      // 0 low, 1 medium, 2 high
    uint32_t taa, shadows, ambientOcclusion, autoExposure;
    uint32_t debugView;
    float    exposure;                     // applied last frame
    float    meteredEv, adaptedEv;         // auto exposure: log2 luminance, this frame / adapted
    char     rtFallback[96];               // why RT is not active, "" when it is
};

#pragma pack(pop)

// ------------------------------------------------------------- the table
//
// Valid from a successful InitWInterface until `destroy`. Every function
// tolerates a null renderer and returns ErrInvalidArg rather than faulting.
//
// ONE SCENE AT A TIME: textures and models live in the loaded scene, and
// loading another (or unloading it) releases them - their handles are invalid
// from then on. UI textures (createTextureRgba) are the renderer's and stay.
struct RendererApi {
    void        (WRALLY_CALL* destroy)(Renderer*);
    const char* (WRALLY_CALL* lastError)(Renderer*);           // never null; valid until the next call

    Status (WRALLY_CALL* applySettings)(Renderer*, const RenderSettings*);
    Status (WRALLY_CALL* resize)(Renderer*, uint32_t width, uint32_t height);   // the window's WM_SIZE

    // ---- stage: one .ngg, from memory (the game's VFS read it, or the
    //      assets layer built it from a legacy RBR stage).
    Status   (WRALLY_CALL* loadScene)(Renderer*, const void* ngg, uint64_t bytes, SceneHandle* out);
    void     (WRALLY_CALL* unloadScene)(Renderer*, SceneHandle);
    Status   (WRALLY_CALL* sceneBounds)(Renderer*, SceneHandle, float outMin[3], float outMax[3]);
    // How many of the scene's objects carry a key (NGGK: they follow the
    // physics' solids), and how many keys there are.
    Status   (WRALLY_CALL* sceneKeyStats)(Renderer*, SceneHandle, uint32_t* keyedObjects, uint32_t* keys);
    // The weather presets the .ngg carries (NGGW); index 0 is its default.
    uint32_t (WRALLY_CALL* weatherPresetCount)(Renderer*, SceneHandle);
    Status   (WRALLY_CALL* weatherPreset)(Renderer*, SceneHandle, uint32_t index,
                                          WeatherDesc* out, char* name, uint32_t nameBytes);
    Status   (WRALLY_CALL* findWeatherPreset)(Renderer*, SceneHandle, const char* name, uint32_t* outIndex);

    // ---- resources
    Status (WRALLY_CALL* createTexture)(Renderer*, const void* dds, uint64_t bytes, TextureHandle* out);
    // RGBA8, for the UI's font atlas and generated images - drawn by the UI pass only.
    Status (WRALLY_CALL* createTextureRgba)(Renderer*, const void* rgba, uint32_t width, uint32_t height,
                                            TextureHandle* out);
    void   (WRALLY_CALL* releaseTexture)(Renderer*, TextureHandle);
    // Copies everything it needs during the call.
    Status (WRALLY_CALL* createModel)(Renderer*, const ModelDesc*, ModelHandle* out);
    void   (WRALLY_CALL* releaseModel)(Renderer*, ModelHandle);           // refcounted by instances
    Status (WRALLY_CALL* createInstance)(Renderer*, ModelHandle, InstanceHandle* out);
    void   (WRALLY_CALL* destroyInstance)(Renderer*, InstanceHandle);

    // ---- the frame: everything between "here is a camera" and "the image is
    //      in the back buffer". Instances not in `instances` keep last frame's
    //      state. present shows it.
    Status (WRALLY_CALL* renderFrame)(Renderer*, const FrameDesc*);
    Status (WRALLY_CALL* present)(Renderer*);

    // ---- diagnostics
    Status (WRALLY_CALL* stats)(Renderer*, RenderStats* out);
    Status (WRALLY_CALL* captureFrame)(Renderer*, const char* path);    // the next rendered frame, .bmp
    Status (WRALLY_CALL* setDebugView)(Renderer*, int32_t mode);         // 0 off
    const char* (WRALLY_CALL* debugViewName)(int32_t mode);              // null past the last
};

struct RendererInput {
    StructHeader header;          // kind Renderer, kRendererInterfaceVer
    HostServices host;
    void*        window;          // HWND: the game's; the renderer makes its swap chain on it
    uint32_t     width, height;
    uint32_t     flags;           // kRenderer*
    uint32_t     adapterLuidLow;  // 0, 0: the renderer picks
    int32_t      adapterLuidHigh;
    RenderSettings settings;      // the starting ones
};

struct RendererOutput {
    StructHeader       header;
    ModuleInfo         info;
    const RendererApi* api;
    Renderer*          instance;
};

} // namespace wrally::iface
