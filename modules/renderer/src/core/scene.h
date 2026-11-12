// ---------------------------------------------------------------------------
// Scene - loads a .ngg file and draws it with the camera the caller supplies.
//
// Deliberately a single opaque draw with a directional light, no culling, no
// streaming. Its first job was to settle the camera contract with RBR -
// convention, handedness, units - which had to be right before any amount of
// real geometry was worth loading. That is now settled, and this is the surface
// the real renderer grows behind.
//
// THE CAMERA CONTRACT, stated once here because every client must honour it:
// view and proj are row-major and meant for ROW-VECTOR multiplication (v * M),
// which is D3D9's convention, because RBR's matrices are used unchanged. A
// client that builds matrices itself (the viewer, the editor) must match that
// convention rather than the HLSL default, or the picture is subtly wrong in a
// way that looks like a geometry bug.
//
// Reverse-Z is applied INSIDE Record: hand it a conventional near->0/far->1
// projection and it does the flip. Clients must not pre-flip.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>
#include <vector>
#include <string>

#include "../format/ngg_format.h"   // for the file's Vertex layout
#include "sky.h"                    // SkyParams, shared with the sky pass
#include "shadow.h"
#include "env.h"
#include "probe.h"
#include "aerial.h"
#include "lights.h"
#include "atmosphere_ms.h"
#include "weather.h"

namespace ngg {
namespace core {

class Scene
{
public:
    // Builds the pipeline and uploads the file. Returns false and logs on any
    // failure; the caller then renders its own fallback, so a bad asset cannot
    // leave the screen black with no explanation.
    bool Load(ID3D12Device* device, ID3D12CommandQueue* queue,
              const wchar_t* path, DXGI_FORMAT rtvFormat, UINT width, UINT height);

    // Same, from bytes already in memory. Load() is this plus a file read.
    //
    // The editor needs it: Wallaby serialises the scene it is holding and hands
    // over the pointer, so opening a project costs no temp file and no disk round
    // trip (design §8). It is for OPENING a project - editing pushes deltas
    // instead, or the disk cost simply moves to PCIe.
    bool LoadFromMemory(ID3D12Device* device, ID3D12CommandQueue* queue,
                        const void* data, size_t bytes,
                        DXGI_FORMAT rtvFormat, UINT width, UINT height);
    // The materials' texture LOD bias, from the next Load on (it is in the root
    // signature's samplers): log2(render / output) under an upscaler, so the
    // textures it reconstructs are picked at the output's texel density; 0
    // native.
    void SetMipBias(float bias)           { m_mipBias = bias; }

    // Dynamic content counts: an editor can open an empty project and start
    // adding, and a scene that draws must not report itself as not ready.
    bool Ready() const { return m_indexCount > 0 || !m_dynIndices.empty(); }

    // See the camera contract above.
    void Record(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const float* view, const float* proj, UINT width, UINT height);

    // DEPTH + NORMAL PREPASS. Optional; call before Record with the same view
    // and proj. Draws the geometry once with a trivial shader into the depth
    // buffer and the normal buffer, so that (a) a ray tracer has both in the
    // SAME frame - a shadow term one frame behind is wrong the moment the
    // camera moves - and (b) the main pass, testing GREATER_EQUAL against
    // depth it already has, shades every pixel exactly once. The second is the
    // classic Z-prepass win and it is why this is not RT-only plumbing.
    // False when it could not run (no prepass pipeline): the main pass then
    // writes the depth itself, and nothing else has this frame's targets.
    bool RecordPrepass(ID3D12GraphicsCommandList* list,
                       const float* view, const float* proj, UINT width, UINT height);
    ID3D12Resource* NormalBuffer() const { return m_normal.Get(); }
    // The prepass's motion target, RGBA16F, in RENDER_TARGET between frames
    // like the normal buffer: per pixel of a moving object (one that has moved
    // since it was placed, standing still or not), xy = last frame's NDC minus
    // this frame's, z = last frame's reverse-Z depth, w = 1; w = 0 everywhere
    // else (reproject by the camera).
    // Only meaningful in a frame that ran RecordPrepass.
    ID3D12Resource* MotionBuffer() const { return m_motion.Get(); }
    // And the moving objects' GLASS, which the motion target sees through (its pixels are what is behind it): RG16F,
    // its own NDC offset to last frame where a moving object's glass is in front, kNoGlass (8, 8) elsewhere. In
    // RENDER_TARGET between frames, as the motion target.
    ID3D12Resource* GlassMotionBuffer() const { return m_glassMotion.Get(); }
    ID3D12Resource* PlantBuffer() const { return m_plantTarget.Get(); }

    // Where the sun visibility comes from: 0 = the cascaded shadow map,
    // 1 = the ray tracer's texture (SetRtVisibility, traced this frame from the
    // prepass). In RT mode the caller should skip RenderShadows entirely -
    // that, not the lookup, is where the map's cost is.
    void SetShadowSource(int src) { m_shadowSource = src ? 1 : 0; }
    int  ShadowSource() const { return m_shadowSource; }

    // World-space bounds of the loaded scene, from the file header. The viewer
    // uses them to place its initial camera; without them a 11 km stage opens
    // with the camera inside a hillside.
    void Bounds(float* outMin3, float* outMax3) const;

    // A point ON the ground for a viewer to start at. False if no scene is
    // loaded. Stand-in for the car's start position until the converter carries
    // it into the file - see the note on ComputeFocusPoint.
    bool FocusPoint(float* out3) const;

    // Cycling wraps HERE, not in the *Name lookups: those answer "what is mode N
    // called" and return null past the end, which is what lets a caller enumerate
    // them without knowing the count. C++ truncates toward zero, so a bare `%`
    // leaves -1 as -1 -- not a mode, and now not a name either.
    static int WrapMode(int mode, int count)
    {
        int m = mode % count;
        return m < 0 ? m + count : m;
    }

    // Runtime-only draw flags, ORed into a draw's material flags above the
    // format's own. Never written to a file: both are DERIVED from which
    // texture slots a material fills, so the file states each fact once rather
    // than twice in a way that can disagree with itself.
    //
    // Public because the shader carries mirrored #defines and the static_asserts
    // tying the two together sit at file scope in scene.cpp.
    // kDrawModel: an instanced model's material (its prepass keeps its own normal, not a card's plane);
    // kDrawHashedAlpha: its alpha edge dithered about the cutoff (kMaterialFoliage).
    enum { kDrawSingleStage = 1u << 8, kDrawNormalMap = 1u << 9, kDrawModel = 1u << 10, kDrawHashedAlpha = 1u << 11 };

    // One material's five texture slots, in NGGM order: baseColour, detail,
    // normal, orm, emissive. Contiguous in the SRV heap, so a single descriptor
    // table root points at all of them and the shader sees t0..t4.
    enum { kMaterialSrvCount = 5 };
    struct MaterialSlot
    {
        uint32_t tex[kMaterialSrvCount];
        uint32_t flags;
        float    baseColourFactor[4];
        float    roughness, metallic, normalScale;
        float    occlusionStrength, emissiveStrength, alphaCutoff;
        float    clearCoat;   // 0..1 (MaterialDesc::clearCoat); the .ngg's materials have none
    };

    // Debug views - isolate one shader input so an artefact can be ATTRIBUTED
    // rather than guessed at. See kDebugModeNames for the list.
    enum { kDebugCount = 16 };
    static const char* DebugModeName(int mode);
    void SetDebugMode(int mode) { m_debugMode = WrapMode(mode, kDebugCount); }
    int  DebugMode() const { return m_debugMode; }

    // Aerial perspective uses the SAME atmosphere parameters as the sky pass,
    // and takes them from the caller rather than owning a second copy - two
    // sets of sun parameters would drift and the horizon is where that shows.
    void SetAtmosphere(const SkyParams& p, bool enabled)
    {
        m_atmosphere = p;
        m_aerial = enabled;
    }
    bool Aerial() const { return m_aerial; }

    // HOW aerial perspective is evaluated, not whether. false (the default) reads
    // the per-frame froxel table (aerial.h); true marches the atmosphere per
    // pixel as it always used to. Kept as the REFERENCE the table is judged
    // against - same integral, so any visible difference is a table defect -
    // and as the fallback where the table could not be created.
    void SetAerialMarch(bool march) { m_aerialMarch = march; }
    bool AerialMarch() const { return m_aerialMarch; }
    bool AerialLutReady() const { return m_aerialLut.Ready(); }

    // Mip chains for textures that shipped without one (mipgen.h, design §2.7).
    // 0 = off (as shipped), 1 = plain box filter (the A/B reference - foliage
    // thins out with distance), 2 = coverage-preserving (default). Applied at
    // load, so it takes effect on the next Load.
    void SetMipFallback(int mode) { m_mipFallback = mode; }
    int  MipFallback() const { return m_mipFallback; }

    // GPU time of the last MAIN pass (not the prepass), and of the aerial-table
    // fill at its start. Timestamps, because whole-frame fps cannot attribute
    // anything: the viewer's frame is synchronous, so fps is CPU recording plus
    // GPU execution plus readbacks, and a pass can halve without fps moving.
    // One frame late (the result is read at the next Record), 0 until then.
    double LastPassMs() const   { return m_passMs; }
    double LastAerialMs() const { return m_aerialMs; }
    double LastEnvMs() const    { return m_envMs; }      // environment + irradiance (+ MS table when dirty)
    // GPU time of the last RenderShadows - the cascaded maps, all four passes.
    // 0 while RT is the shadow source, because then they are not rendered.
    double LastShadowMapMs() const { return m_shadowMapMs; }

    // Baked vs dynamic surface lighting. Mode 0 is RBR's own behaviour and is
    // kept as the A/B reference, not as a fallback - the bake cannot follow a
    // moving sun, which is what the whole overhaul is for.
    enum { kLightModeCount = 2 };
    static const char* LightModeName(int mode);
    void SetLightMode(int mode) { m_lightMode = WrapMode(mode, kLightModeCount); }
    int  LightMode() const { return m_lightMode; }

    // The LEGACY PROFILE: how a scene W-Rally's .lbs importer made (kSceneFlagLegacyImport) lights its alpha-tested
    // geometry - RBR's cards, untagged, which modern lighting shows up as cardboard. Other scenes are never touched.
    //   0 off: as any scene
    //   1 A, baked: the card lit by RBR's bake (its vertex colour) times the dynamic light of an upward surface
    //   2 B, occlusion: dynamic light on normals bent up, the bake's brightness as occlusion of the ambient
    // In 1 and 2 a card's sun and AO rays skip every card (only opaque geometry shadows it).
    enum { kLegacyProfileCount = 3 };
    void SetLegacyProfile(int mode) { m_legacyProfile = WrapMode(mode, kLegacyProfileCount); }
    int  LegacyProfile() const { return m_legacyProfile; }
    // How wet the surfaces are, 0 dry .. 1 wet (design §5.2): porous ones darker, all glossier, and above ~0.5
    // puddles on the ground layer. The road's state, set per frame by the client.
    void  SetWetness(float w) { m_wetness = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w; }
    // The car whose paint reflects the world (probe.h): its objects sample the cube probe instead of the sky, and the
    // probe is drawn from `centre` around it. count 0: none (the sky alone). Per frame, by the client's focus.
    void  SetReflectionFocus(const uint32_t* objects, uint32_t count, const float centre[3]);
    float Wetness() const { return m_wetness; }
    bool LegacyImport() const { return m_legacyImport; }

    // Smooth split normals at load. FALLBACK for assets whose normals were
    // never meant for shading - see WeldNormals. Takes effect on the next Load.
    void SetWeldNormals(bool on) { m_weldNormals = on; }
    bool WeldingNormals() const { return m_weldNormals; }

    // Shade foliage as a rounded volume instead of as crossed flat cards.
    // Also a load-time fallback; see SphericalFoliageNormals.
    void SetFoliageNormals(bool on) { m_foliageNormals = on; }
    bool FoliageNormals() const { return m_foliageNormals; }

    // Pixel probe: ask one pixel to report its intermediate lighting terms.
    // Set it, render a frame, then call LogProbe once the GPU has been waited
    // on. Numbers by name, so a lighting question stops being a colour-matching
    // exercise. Complements a real shader debugger (RenderDoc) rather than
    // replacing it - this one needs no capture and knows our term names.
    void SetProbe(int x, int y) { m_probeX = x; m_probeY = y; m_probeOn = true; }
    void LogProbe();

    // Cascaded shadows. The Scene owns the geometry, so it does the drawing;
    // ShadowMaps owns the targets and the fitting. Call RenderShadows BEFORE
    // Record, with the same view/proj.
    void SetShadowMaps(ShadowMaps* s) { m_shadows = s; m_shadowSrvReady = false; }
    // THE LOCAL LIGHTS' SHADOW MAPS (lights.h Lights::ShadowCount) - the cascades' path, where no ray tracer shadows
    // them: one perspective depth map per chosen spot or beam, a slice of `maps` each (its four "cascades", Update
    // never called). ResetLocalShadows at each frame's start; RenderLocalShadows after PrepareLights draws them and
    // tells the lights' shading to read them this frame (a null list: the share alone). `aoShare`: the screen-space
    // AO's share of the local lights' diffuse (0 by day).
    void SetLocalShadowMaps(ShadowMaps* maps) { m_localShadows = maps; }
    void ResetLocalShadows() { m_localShadowsDrawn = 0; m_localAoShare = 0.0f; m_localMaskWritten = nullptr; }
    void RenderLocalShadows(ID3D12GraphicsCommandList* list, float aoShare);
    uint32_t LocalShadowsDrawn() const { return m_localShadowsDrawn; }
    // The MOVED-SHADOW MASK the deferred lights wrote this frame (z: a moving object's shadow from a mapped lamp, now
    // or in the last three frames), for TAA as the ray tracer's lights' distrust; null when there is none.
    ID3D12Resource* LocalShadowMask() const { return m_localShadowsDrawn ? m_localMaskWritten : nullptr; }
    double LocalShadowMs() const { return m_localShadowMs; }

    // The prefiltered sky and its irradiance (env.h). OWNED and rebuilt by the
    // Scene at the start of every main pass, from the SkyParams SetAtmosphere
    // gives it - the same ones the sky is drawn with. It used to be the
    // caller's, and only the viewer was a caller: the game host and the editor
    // never built one, so in-game there was no ambient specular and the sky
    // ambient fell back to the per-pixel march. Owning it, like the aerial
    // table, is what makes every client get the same lighting.
    void SetAmbientSpecular(bool on) { m_ambientSpecular = on; }
    const Environment& Env() const { return m_ownEnv; }

    // Lighting balance (design §3.5). false: the established balance, with
    // sunDiffuse/skyAmbient and the albedo*E diffuse convention. true: physical
    // Lambert (albedo/PI) and GGX with no extra factors, the sun at its model
    // irradiance, the sky from the irradiance map, the ground bounce
    // analytic - i.e. what the atmosphere model itself says.
    //
    // PHYSICAL IS THE DEFAULT since multiple scattering landed (5.0j): measured
    // in the lab, physical + multiple scattering gives 10.6:1 sun:sky on flat
    // ground at a 45 degree sun - clean clear air - where the established
    // balance gives 3.2-4.2:1, an overcast-to-hazy look. The established path
    // stays one key away for one round of in-game comparison, then goes.
    void SetPhysicalLighting(bool on) { m_physical = on; }
    bool PhysicalLighting() const { return m_physical; }

    // Multiple scattering in the atmosphere (atmosphere_ms.h, design §3.2).
    // The Sky has its own table and its own switch - keep the two in step.
    void SetMultipleScattering(bool on) { m_msOn = on; }
    bool MultipleScattering() const { return m_msOn; }
    const AtmosphereMs& Ms() const { return m_ms; }

    // Last frame's ray-traced sun visibility (rt.h), R8_UNORM, already in
    // PIXEL_SHADER_RESOURCE. Null when there is none. Read by the debug views
    // that put it beside the shadow map - the point of the probe's first ray
    // is that it can be checked against something we already have.
    //
    // The descriptor is rewritten on EVERY set, never keyed on the pointer.
    // Keying on it was a use-after-free: when the ray tracer recreates its
    // targets (a rebuild, a resize) the new texture can be allocated at the
    // address the old one had - same pointer, so no rewrite, and the main pass
    // read the FREED texture's memory. A page fault, then a GPU hang (TDR),
    // caught by DRED on the first object move (design §10.5). Two descriptor
    // writes a frame cost nothing; a pointer is not an identity.
    void SetRtVisibility(ID3D12Resource* tex) { m_rtVis = tex; m_rtSrvReady = false; }
    void ClearRtVisibility() { m_rtVis = nullptr; m_rtSrvReady = false; }

    // This frame's ray-traced ambient occlusion (rt.h, denoised), applied to
    // the ambient terms. Null when there is none; SetAmbientOcclusion toggles
    // its use without unbinding it, for A/B.
    // `view`: the format it is read through, for a typeless one (Gtao's); UNKNOWN reads it as its own.
    void SetRtAo(ID3D12Resource* tex, DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN)   // see SetRtVisibility
    {
        m_rtAo = tex;
        m_rtAoView = view;
        m_aoSrvReady = false;
    }
    void ClearRtAo() { m_rtAo = nullptr; m_aoSrvReady = false; }

    // LOCAL LIGHTS (lights.h, design §5.5): this frame's lights, their clusters for this view (before the ray
    // tracer reads them), and the ray tracer's denoised shadowed fraction of their light (Rt::FilteredLights; none:
    // unshadowed).
    void SetLights(const std::vector<LightDesc>& lights) { m_lights.Set(lights); }
    void PrepareLights(ID3D12GraphicsCommandList* list, const float* view, const float* proj, UINT width, UINT height)
    {
        m_lights.Build(list, view, proj, width, height);
    }
    const Lights& GetLights() const { return m_lights; }
    void SetRtLights(ID3D12Resource* tex) { m_rtLights = tex; }
    void ClearRtLights() { m_rtLights = nullptr; }
    void SetAmbientOcclusion(bool on) { m_aoOn = on; }
    bool AmbientOcclusion() const { return m_aoOn; }
    bool AmbientSpecular() const { return m_ambientSpecular; }
    void RenderShadows(ID3D12GraphicsCommandList* list,
                       const float* view, const float* proj, const float* sunDirTravel);
    // --- editor: per-object visibility (design §8, tier 1) -----------------
    //
    // Hiding is a DRAW-LIST edit and costs no GPU work, which is what makes an
    // editor's delete and undo equally cheap: the geometry never leaves the
    // buffer, so putting it back is one flag. Freeing storage on delete would
    // make undo the expensive direction, which is backwards.
    //
    // Index is into the object array as loaded, i.e. the .ngg NGGO order.
    bool SetObjectVisible(uint32_t index, bool visible);

    // Moves one object, as a WORLD-SPACE delta: .ngg bakes placements into the
    // vertices, so there is no local space to place it in. Identity restores it
    // to where the file put it. Rotating about the object's own centre is the
    // caller's job to compose, which is what ObjectBounds is for.
    //
    // Row-major with translation in row 3 -- the convention everywhere in .ngg.
    bool SetObjectTransform(uint32_t index, const float xform[16]);

    // World bounds, after any transform. The editor needs these for a pivot and
    // for framing the camera on a selection.
    bool ObjectBounds(uint32_t index, float* outMin3, float* outMax3) const;
    bool ObjectVisible(uint32_t index) const;
    uint32_t ObjectCount() const { return (uint32_t)m_objects.size(); }
    // An object's source key (the file's NGGK: RBR's movable instance id, the physics' solid), -1 for none - objects
    // added after the load have none.
    int32_t ObjectKey(uint32_t index) const { return index < m_objectKeys.size() ? m_objectKeys[index] : -1; }
    uint32_t VisibleObjectCount() const;

    // --- stage variants (NGGR/NGGQ, ngg_format.h) -----------------------------
    // Which variant the NEXT load draws: RBR's letter ("M", "O"), or a stage name ending in it ("track-41_M"); null
    // or empty for the file's main one. A file with variants then draws only that variant's objects - the others
    // are loaded and marked removed, so object indices, keys and plants stay the file's. A file without variants
    // ignores it.
    void SetVariant(const char* variant) { m_variantRequest = variant ? variant : ""; }
    // What the last load drew: the variant's name, empty for a file without variants.
    const std::string& Variant() const { return m_variant; }
    const std::vector<std::string>& VariantNames() const { return m_variantNames; }

    // --- editor: selection highlight (design §8, tier 1) -------------------
    //
    // The renderer draws this, not the host, for one reason: the depth buffer
    // is here. A wire drawn by WinForms on top of the viewport would show
    // through the hillside in front of it, which is worse than no highlight -
    // it puts the object in the wrong place.
    //
    // INVISIBLE OBJECTS ARE SKIPPED. Hiding is how deletion is expressed
    // (SetObjectVisible), so a deleted object still trailing a wire would be
    // the "handle that quietly draws nothing" confusion running backwards.
    bool SetSelection(const uint32_t* indices, uint32_t count);
    uint32_t SelectionCount() const { return (uint32_t)m_selection.size(); }

    // Built once from the device, not per stage: it depends on the OUTPUT
    // format rather than on any scene content, and a selection must survive a
    // track reload. Call before the first RenderSelection.
    bool CreateSelectionPipeline(ID3D12Device* device, DXGI_FORMAT outputFormat);

    // Draws the highlight AFTER the tonemap, straight into the caller's target.
    //
    // After, not into the HDR buffer, for two reasons that both show on screen:
    // an overlay routed through the tonemap is not the colour the editor asked
    // for, and a one-pixel wire fed to TAA ghosts behind itself whenever the
    // camera moves. It still tests against the scene's depth, which is the part
    // that has to be in here.
    //
    // view/proj are the UNJITTERED pair, as handed to Record.
    //
    // Draws the selection wires AND the overlay layers, in one call because they
    // share the counter buffer: zeroing it per half would make each read the
    // other's leftovers.
    void RenderEditorOverlay(ID3D12GraphicsCommandList* list,
                             D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                             const float* view, const float* proj, UINT width, UINT height);

    // Wire pixels the last frame actually wrote, split by whether they passed
    // the depth test. The renderer counting its own output is the only way to
    // tell "the highlight is drawing" from "the highlight is being drawn and
    // rejected" from outside -- both look like an empty screen.
    void ReadOverlayStats();
    uint32_t SelectionPixelsVisible() const  { return m_selPixVisible; }
    uint32_t SelectionPixelsOccluded() const { return m_selPixOccluded; }
    uint32_t OverlayPixelsVisible() const    { return m_ovPixVisible; }
    uint32_t OverlayPixelsOccluded() const   { return m_ovPixOccluded; }

    // --- editor: overlay line layers (design §8) ---------------------------
    //
    // Everything a track editor draws that is not track geometry is a line: the
    // drive line, trigger volumes, camera markers, bounding boxes, manipulator
    // handles. Measured in Wallaby, its gizmos are 7 LineStrip draws, 4
    // TriangleStrip and 3 TriangleList -- so lines plus per-vertex colour is
    // very nearly the whole requirement.
    //
    // DESCRIBED, NOT COMMANDED. The caller replaces a whole layer when it
    // changes and the renderer keeps it; there is no per-frame submission and no
    // render-state API. That is the difference between this and the D3D9
    // immediate-mode model it replaces, and it is why an ngg-core backend can
    // satisfy it while it could never satisfy SetRenderState.
    //
    // Segments, not strips: consecutive PAIRS of vertices are one line each. One
    // topology means no state to get wrong, and a strip is trivially expanded by
    // the caller.
    struct OverlayVertex { float pos[3]; uint32_t colour; };   // 0xAABBGGRR

    enum {
        kOverlayNoDepth = 0x1,      // draw through geometry, for manipulators
        // Labels only: Label::pos.xy is PIXELS from the top-left and pos[2] is
        // ignored. For a HUD - frame time, material values, whatever the caller
        // wants on screen - which the world-anchored form cannot express, since
        // a HUD has no world position to anchor to. Implies no depth test.
        kOverlayScreenSpace = 0x2,
        // Labels only: draw the glyphs twice, the lower copy black and offset
        // by a pixel. White text is unreadable over a sunlit road or a bright
        // sky, which is exactly the case a readout has to survive.
        kOverlayTextShadow = 0x4,
    };

    bool SetOverlayLines(ID3D12Device* device, uint32_t layer,
                         const OverlayVertex* verts, uint32_t count, uint32_t flags);
    void ClearOverlays();
    uint32_t OverlayVertexCount() const;

    // --- editor: world-space text labels ------------------------------------
    //
    // The last thing the editor draws that the renderer could not. Object names
    // and ids sit at world positions, and they cannot be painted by the host on
    // top of the viewport: a native child window with a swap chain wins the
    // airspace fight, so host-drawn text either flickers or never appears.
    //
    // THE FONT COMES FROM THE HOST, deliberately. Embedding one would put glyph
    // data in the renderer and freeze a size and a character set into it; the
    // host already has a font rasteriser and the same content-in, drawing-out
    // split the rest of this API uses. A monospace grid keeps the mapping from
    // character to cell arithmetic rather than a table.
    bool SetFont(ID3D12Device*, ID3D12CommandQueue*, const void* rgba,
                 uint32_t width, uint32_t height,
                 uint32_t columns, uint32_t rows, uint32_t firstChar);
    bool HasFont() const { return m_font != nullptr; }

    struct Label
    {
        float    pos[3];        // world anchor
        uint32_t colour;        // 0xAABBGGRR
        uint32_t textOffset;    // byte offset into the caller's blob, NUL-terminated
    };

    // Replaces one label layer. Sizes are in PIXELS and stay that size at any
    // distance, because a name you cannot read at 200 m is not a label.
    bool SetLabels(ID3D12Device*, uint32_t layer, const Label* labels, uint32_t count,
                   const char* textBlob, uint32_t blobBytes,
                   float pixelHeight, uint32_t flags);
    uint32_t LabelGlyphCount() const;

    uint32_t LabelPixelsVisible() const  { return m_labPixVisible; }
    uint32_t LabelPixelsOccluded() const { return m_labPixOccluded; }

    // --- editor: the dynamic pool (design §8, tier 2) ----------------------
    //
    // Everything added after the load lives BESIDE the static scene, in its own
    // vertex and index buffers, so placing one prop does not re-upload a 50 MB
    // track. That is the whole reason this exists rather than "rebuild and
    // reload", which is the temp-file mistake with the cost moved to PCIe.
    //
    // HANDLE CONVENTIONS, because they differ and the difference is deliberate:
    //   - mesh, texture, material handles are 1-BASED, so 0 is invalid and a
    //     zeroed struct means "nothing", as the header promises.
    //   - instance handles are the object INDEX, 0-based, because tier 1 already
    //     established that and the editor addresses static objects the same way.
    //     Failure is kUnknownInstance, not 0.
    enum : uint32_t { kUnknownInstance = 0xFFFFFFFFu };

    uint32_t AddMesh(ID3D12Device*, const ::ngg::Vertex* verts, uint32_t vertexCount,
                     const uint32_t* indices, uint32_t indexCount);
    // `linear`: data (a normal or ORM map), read as stored; else colour, decoded from sRGB.
    uint32_t AddTexture(ID3D12Device*, ID3D12CommandQueue*, const void* dds, size_t bytes, bool linear = false);
    uint32_t AddMaterial(ID3D12Device*, uint32_t tex0, uint32_t tex1, uint32_t flags);
    uint32_t AddInstance(uint32_t mesh, uint32_t material, uint32_t layer,
                         const float xform[16]);

    // Removing an instance is a draw-list edit: no GPU work, so undo costs the
    // same nothing. The slot is a tombstone rather than being reused, because
    // renumbering would invalidate every handle the editor is holding.
    bool RemoveInstance(uint32_t index);
    bool RemoveMesh(uint32_t mesh);           // refuses while instances reference it
    bool Compact(ID3D12Device*);              // reclaims mesh storage, never renumbers

    // The load-time single-stage fallback. Off means the asset is drawn exactly
    // as the file describes it, garbage uv1 included - which is the only way to
    // see what the fix is actually worth. Takes effect on the next Load.
    void SetSingleStageFix(bool on) { m_singleStageFix = on; }
    bool SingleStageFix() const { return m_singleStageFix; }

    void SetCull(bool on) { m_cull = on; }
    bool Cull() const { return m_cull; }
    void SetShadowsOn(bool on) { m_shadowsOn = on; }
    bool ShadowsOn() const { return m_shadowsOn; }

    // --- editor: live material editing (design §2.3) ------------------------
    //
    // Scalars take effect on the NEXT FRAME with no rebuild of anything: the
    // material travels to the shader as root constants, read from m_materials
    // in the draw loop. That is what makes a slider in the editor feel like a
    // slider rather than like a reconversion, and it is the reason the material
    // is root constants at all.
    //
    // Changing a TEXTURE slot is the expensive case - it rewrites the SRV heap -
    // and SetMaterial does it only when a slot actually changed.
    uint32_t MaterialCount() const { return (uint32_t)m_materials.size(); }
    bool GetMaterial(uint32_t index, MaterialSlot* out) const;
    bool SetMaterial(ID3D12Device* device, uint32_t index, const MaterialSlot& m);
    // Many materials added or changed at once (a car's): the SRV heap - a new heap and every material's descriptors -
    // rebuilt once at the end rather than at each AddMaterial / SetMaterial. Between the two nothing may draw.
    void BeginMaterialBatch() { m_materialBatch = true; }
    bool EndMaterialBatch(ID3D12Device* device);

    // The material's base-colour texture name, which is how RBR names materials
    // anyway - an editor listing "material 82" helps nobody.
    const char* MaterialName(uint32_t index) const;

    // Which material an object draws with, so clicking a surface can select the
    // material that produced it.
    bool ObjectMaterial(uint32_t index, uint32_t* out) const;

    void OnResize(ID3D12Device* device, UINT width, UINT height);

    // TAA plumbing. The offset is in NDC and comes from PostFx::Jitter; the sky
    // pass must be given the SAME value, or the world anti-aliases against a
    // static background and the horizon crawls.
    void SetJitter(float ndcX, float ndcY) { m_jitterX = ndcX; m_jitterY = ndcY; }

    // The last Record's camera: reverse-Z included, TAA jitter EXCLUDED.
    // PostFx reprojects with this rather than rebuilding it. The jitter is left
    // out on purpose - a history reprojected through it follows the jitter
    // instead of staying on the pixel grid, and the image shakes. See the note
    // where this is filled in.
    const float* LastViewProj() const { return m_lastViewProj; }

    // The same matrix WITH the jitter: what the depth buffer was actually
    // rasterised with. TAA must not use this (5.0c), but anything that
    // reconstructs a real surface point from depth must - a ray origin half a
    // pixel off the surface is centimetres at a grazing angle, which is the
    // same size as the bias meant to clear it.
    const float* LastViewProjJittered() const { return m_lastViewProjJittered; }

    // The depth buffer, for passes that read it. Owned here because the Scene
    // is what draws into it.
    ID3D12Resource* Depth() const { return m_depth.Get(); }

    // The static geometry, for a ray-tracing acceleration structure to be built
    // over (rt.h). Only the file's own buffers: the dynamic editor pool is
    // separate and moves, and the whole reason one BLAS suffices is that this
    // half does not.
    ID3D12Resource* StaticVertexBuffer() const { return m_vb.Get(); }
    ID3D12Resource* StaticIndexBuffer() const  { return m_ib.Get(); }
    uint32_t StaticVertexCount() const { return m_vbv.SizeInBytes / (m_vbv.StrideInBytes ? m_vbv.StrideInBytes : 1); }
    // The dynamic pool's vertices as last uploaded (FlushDynamic), for the ray tracer's any-hit view.
    ID3D12Resource* DynamicVertexBuffer() const { return m_dynVb.Get(); }
    uint32_t DynamicVertexCount() const { return (uint32_t)m_dynVerts.size(); }
    // A DescribeRt key that is the world's (scene.cpp kRtWorld: kind 0 in the top byte).
    static bool IsRtWorldKey(uint64_t key) { return (key >> 56) == 0; }
    // The vertex stride the BLASes read positions with (both pools share the layout).
    uint32_t RtVertexStride() const { return m_vbv.StrideInBytes ? m_vbv.StrideInBytes : (uint32_t)sizeof(Vertex); }
    uint32_t StaticVertexStride() const { return m_vbv.StrideInBytes; }
    uint32_t StaticIndexCount() const { return m_indexCount; }

    // Everything a ray tracer needs to answer "does this ray hit a LEAF, or
    // only the card it is painted on" - which is the difference between a tree
    // casting the shadow of its foliage and casting a solid block.
    //
    // Built here rather than in rt.cpp because it is all format knowledge: which
    // objects are alpha-tested, which layer is the far backdrop that casts no
    // shadow, where uv0 and the vertex alpha sit in the 44-byte vertex, which
    // texture is a material's base colour. rt.cpp then knows only buffers.
    //
    // Two index buffers over the ONE vertex buffer: opaque triangles and
    // alpha-tested triangles, each contiguous, because a BLAS geometry is a
    // contiguous index range and RBR's objects interleave the two freely.
    // ---- the ray tracer's view of the scene (design §4.2) ------
    //
    // MESHES and INSTANCES, like the rasteriser's draw list rather than one
    // baked soup. Each mesh has a stable KEY, so the ray tracer can keep a BLAS
    // per key and rebuild only what changed:
    //   - the WORLD: every static object at its authored position, one BLAS.
    //     Its key hashes which static objects are NOT in it, so hiding,
    //     removing or moving one changes the key and nothing else does;
    //   - a MOVED static object: its own BLAS at its authored position, placed
    //     by its instance transform - so dragging it is a TLAS-only change;
    //   - an editor-added DYNAMIC mesh: one BLAS, as many instances as use it.
    struct RtMeshData
    {
        uint64_t        key = 0;
        ID3D12Resource* vb = nullptr;        // positions are the first 12 bytes
        UINT64          vbOffset = 0;        // bytes
        uint32_t        vertexCount = 0;
        uint32_t        vbSelect = 0;        // 0 = static buffer, 1 = dynamic pool
        uint32_t        vertexBase = 0;      // added to its indices in the any-hit test
        std::vector<uint32_t> opaque;        // indices, relative to vertexBase
        std::vector<uint32_t> alpha;         // non-opaque: the any-hit test runs
        std::vector<uint32_t> alphaMat;      // material per alpha triangle; ~0 = the instance's
        std::vector<uint32_t> alphaPlant;    // plant per alpha triangle (FindPlants); ~0 = none
    };
    struct RtInstanceDesc
    {
        uint64_t mesh = 0;
        float    xform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // row-vector, like the draws
        uint32_t material = 0xFFFFFFFFu;     // for meshes whose alphaMat is ~0
        bool     forceOpaque = false;        // a dynamic mesh drawn with an opaque material
        // It went further than kRtMovedMin (scene.cpp) since last frame: a ray
        // that hits it sees an occluder that was elsewhere, so the denoiser
        // must not trust the past.
        bool     moved = false;
        // One of the reflection focus's objects (SetReflectionFocus): its own bit in the instance mask
        // (kRtMaskFocus), so the focus's reflection rays (Shade's ReflOpen) see the car and nothing else.
        bool     focus = false;
        // Its transform last frame, so the denoiser can tell how far what a
        // ray hit moved (rt.cpp Allow).
        float    prevXform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };
    struct RtShared
    {
        ID3D12Resource* staticVb = nullptr;  uint32_t staticVertexCount = 0;
        ID3D12Resource* dynVb = nullptr;     uint32_t dynVertexCount = 0;
        uint32_t        stride = 0;
        // Per material: { uint texture; float cutoff; } - what an alpha test needs.
        Microsoft::WRL::ComPtr<ID3D12Resource> materials;  uint32_t materialCount = 0;
        std::vector<ID3D12Resource*> textures;             // null-free; missing -> white
    };
    void DescribeRt(std::vector<uint64_t>& meshes, std::vector<RtInstanceDesc>& instances) const;
    // A static object in the world's BLAS: drawn, not moved, and not keyed (a
    // keyed object is the physics' and gets a BLAS of its own from the load).
    bool InRtWorld(size_t i) const
    {
        const DrawItem& o = m_objects[i];
        return !o.removed && o.visible && o.identity && ObjectKey(static_cast<uint32_t>(i)) < 0;
    }
    bool BuildRtMesh(uint64_t key, RtMeshData& out) const;
    bool BuildRtShared(ID3D12Device* device, RtShared& out) const;
    // The dynamic pool's upload is lazy (the next pass does it); a BVH built
    // from it needs it now. True when it uploaded: the pool's buffer is a new one (the old is freed).
    bool FlushDynamic(ID3D12Device* device) { if (!m_dynDirty) return false; UploadDynamic(device); return true; }

    // Bumped by every change to WHAT geometry exists or where (load, add,
    // remove, hide, move, compact). The Renderer re-describes the scene to the
    // ray tracer when it moves (Renderer::KeepRtCurrent).
    uint32_t GeometryRevision() const { return m_geomRevision; }
    // The ray tracer's TLAS this frame (0: none - no ray tracing, or not active): the reflection focus's pixels
    // trace their reflection against the car's own instances in it (Shade's ReflOpen).
    void SetReflectionTlas(D3D12_GPU_VIRTUAL_ADDRESS tlas) { m_reflTlas = tlas; }
    // TLAS instance masks: every instance has kRtMaskAll's bits; the reflection focus's also kRtMaskFocus.
    static constexpr uint32_t kRtMaskAll = 0x01, kRtMaskFocus = 0x02;

    // ---- instanced models (NGGD / NGGE, ngg_format.h; NGG design §2.6) ----------
    //
    // A model's LOD meshes live in the dynamic pool (one mesh per submesh, so the ray tracer's BLAS per mesh is a BLAS
    // per model submesh), its materials in the scene's table. Its placements are drawn with GPU instancing: per pass
    // and view (the camera; each shadow cascade) the placements are culled on the CPU - a grid of cells first, then per
    // placement - and the survivors' transforms go into a per-frame instance stream, one batch per model LOD, each
    // batch one DrawIndexedInstanced per submesh. A placement in a LOD's crossfade band is in both LODs' batches,
    // dithered to its share.
    //
    // PLACEMENT HANDLES are 0-based indices into the table as loaded (NGGE order) then as added; removing leaves a
    // tombstone, as RemoveInstance does, so a handle an editor holds stays valid.
    struct PlacementDesc
    {
        uint32_t model = 0;                  // into the loaded library
        uint32_t plant = 0xFFFFFFFFu;        // the plant it replaces (load only; an edit cannot change which cards draw)
        float    position[3] = { 0, 0, 0 };  // the model's base, in the scene's space (the drawn variant's)
        float    yaw = 0.0f;
        float    scaleXZ = 1.0f, scaleY = 1.0f;
        uint32_t tint = 0xFFFFFFFFu;
    };
    uint32_t ModelCount() const { return (uint32_t)m_models.size(); }
    const char* ModelName(uint32_t model) const { return model < m_models.size() ? m_models[model].name.c_str() : nullptr; }
    uint32_t PlacementCount() const { return (uint32_t)m_placements.size(); }       // with the removed
    uint32_t LivePlacementCount() const;
    bool GetPlacement(uint32_t index, PlacementDesc* out) const;
    // Edits in BATCHES: one call for many placements (a brush stroke, a selection dragged), the cells rebuilt once.
    // Add: `out` gets the new handles (count of them). Set: each index gets its desc (its model may change). Remove:
    // tombstones. All false (and nothing changed) if any index or model is out of range.
    bool AddPlacements(const PlacementDesc* descs, uint32_t count, uint32_t* out);
    bool SetPlacements(const uint32_t* indices, const PlacementDesc* descs, uint32_t count);
    bool RemovePlacements(const uint32_t* indices, uint32_t count);
    // What the last frame drew of them: placements visible (main view), instances per LOD (crossfades counted in
    // both), the billboards among them, the shadow casters' instances (all cascades), the cull's CPU time.
    struct ModelStats
    {
        uint32_t placements = 0, visible = 0, fading = 0;
        uint32_t perLod[4] = { 0, 0, 0, 0 };   // LOD 0..2, 3 = the billboard (whichever index it has)
        uint32_t shadowInstances = 0, draws = 0;
        double   cullMs = 0.0;
    };
    const ModelStats& LastModelStats() const { return m_modelStats; }
    // Once per frame, before any pass: the instance stream starts over (the frame is synchronous, so last frame's
    // has been consumed) and last frame's statistics are logged every few seconds.
    void BeginFrame(ID3D12Device* dev);

    // The weather presets the FILE carries (NGGW, design §5.1) - the only ones
    // a competition should use. Empty for a file converted before the chunk
    // existed. The first is the track's default.
    const std::vector<WeatherPreset>& WeatherPresets() const { return m_weather; }
    const WeatherPreset* FindWeather(const char* name) const;
    // The camera position the last pass was recorded with (world space).
    const float* LastCameraPos() const { return m_lastCamPos; }

private:
    bool CreateDepth(ID3D12Device* device, UINT width, UINT height);
    bool EnsurePipelines(ID3D12Device* device, DXGI_FORMAT rtvFormat);   // made once per format and mip bias
    bool CreatePipelines(ID3D12Device* device, DXGI_FORMAT rtvFormat, float mipBias);
    bool UploadTextures(ID3D12Device* device, ID3D12CommandQueue* queue,
                        const uint8_t* chunk, size_t bytes);

    void ComputeFocusPoint(const struct ::ngg::Vertex* verts, uint32_t count);
    bool CreateProbe(ID3D12Device* device);
    static void WeldNormals(std::vector<::ngg::Vertex>& verts);
    // Load-time fallback for kMaterialSingleStage, in the same shape as
    // WeldNormals: measure first, act only where the data is unambiguous, say
    // what it did, and stand down by itself once the converter sets the flag.
    void MeasureSolidCards(const ::ngg::Vertex* verts, uint32_t vertexCount,
                           const uint8_t* indexData, uint32_t indexCount,
                           const uint8_t* texData, size_t texBytes);
    void MeasureSingleStage(const ::ngg::Vertex* verts, uint32_t vertexCount,
                            const uint8_t* indexData, uint32_t indexCount);
    void SphericalFoliageNormals(std::vector<::ngg::Vertex>& verts,
                                 const uint8_t* indexData, uint32_t indexCount);
    // The PLANTS inside the alpha-tested objects: one object is often a whole
    // stand of trees, and a plant is the 2-3 crossed cards of one of them.
    // Fills m_plants and m_triPlant; see FindPlants in scene.cpp.
    void FindPlants(const ::ngg::Vertex* verts, uint32_t vertexCount,
                    const uint8_t* indexData, uint32_t indexCount);
    // The same from the file's NGGG/NGGP chunks; false when it has none.
    // `untaggedCards`: every plant is old card vegetation, tags or none (an automatic legacy import: its plants are
    // its vegetation's connected pieces, untagged).
    bool ReadPlants(const uint8_t* tags, size_t tagBytes,
                    const uint8_t* table, size_t tableBytes, uint32_t indexCount, bool untaggedCards);

    // Bounds are computed at LOAD from the vertex data - the renderer-side
    // fallback promised in design §2.7. The converter should eventually emit
    // them, because only it can also choose the grouping.
    struct DrawItem
    {
        uint32_t layerId, materialId, firstIndex, indexCount;
        // The material's flags as this draw should USE them, which is not
        // always what the file says: MeasureSingleStage can add
        // kMaterialSingleStage per object when the asset predates the converter
        // emitting it. Per object rather than per material because a material
        // can be shared by geometry that disagrees -- on Joux Verte
        // ObjectTexturePlateTransparent has 23% of its references unusable and
        // 77% fine, and flagging the whole material would break the 77%.
        uint32_t matFlags = 0;
        // Kept apart from matFlags because it came from the GEOMETRY, not from
        // the material: an unrelated material edit recomposes matFlags, and
        // this is what survives it. See Scene::SetMaterial.
        bool     measuredSingleStage = false;
        // Alpha-tested by its material, but every texel it samples is opaque
        // (MeasureSolidCards): a wall, a rock, a house of RBR's vegetation
        // block. Drawn as a solid - no alpha test, no foliage lighting, never
        // a plant. From the geometry and the texture, so it survives a
        // material edit as measuredSingleStage does.
        bool     measuredSolid = false;
        // Culling bounds, in world space AFTER the transform. The untransformed
        // pair is kept alongside because moving an object has to recompute these
        // from the original, not from the last result -- accumulating would drift
        // the box away from the geometry one drag at a time.
        float    bmin[3], bmax[3];
        float    bmin0[3], bmax0[3];
        bool     visible = true;      // editor hide; see SetObjectVisible
        bool     identity = true;     // skip binding the matrix in the common case
        float    xform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        // The transform the LAST frame drew with, for the motion target; see
        // the end of RecordPass, which advances it.
        bool     prevIdentity = true;
        float    prevXform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        // It has moved since it was placed (the car, its wheels and glass): its pixels are a moving object's in the
        // motion target from then on, still or not - see RecordPass's movedOf.
        bool     dynamic = false;
        // 0 = the static scene's shared buffers, otherwise a 1-based index into
        // m_dynMeshes. The draw loop binds whichever pair this names, and static
        // content stays exactly as fast as it was.
        uint32_t meshId = 0;
        int32_t  baseVertex = 0;      // dynamic meshes index from their own base
        bool     removed = false;     // tombstone; see RemoveInstance
        bool     probeFocus = false;  // the reflection focus's: reflects the cube probe (SetReflectionFocus)
    };

    // One entry per added mesh. The vertices themselves are suballocated from
    // the shared dynamic buffers, so fifty cones are fifty instances of one
    // mesh and one upload.
    struct DynMesh
    {
        uint32_t firstVertex = 0, vertexCount = 0;
        uint32_t firstIndex = 0, indexCount = 0;
        uint32_t refs = 0;            // instances pointing at it
        bool     alive = true;
        float    bmin[3] = { 0,0,0 }, bmax[3] = { 0,0,0 };
    };

    // Layer 2 is RBR's FAR SCENERY - the distant painted backdrop
    // (MB_SUPERBOWL on Joux Verte, scene1/scene2 on track-108). Measured: 4-7
    // objects per stage. It is kilometres away and is not part of the lit
    // world, so it neither casts nor receives shadows.
    enum { kLayerFarScenery = 2 };

    // Clears all per-stage state. Called at the top of Load, because loading a
    // second track must replace the first rather than accumulate onto it.
    void ResetScene();
    void ApplyVariant(const uint8_t* variants, size_t variantBytes, const uint8_t* masks, size_t maskBytes,
                      const uint8_t* aliases, size_t aliasBytes);
    std::vector<int32_t> m_objectKeys;   // NGGK, parallel to the file's objects
    std::string              m_variantRequest;   // SetVariant: applies at the next load
    std::string              m_variant;          // what the last load drew
    std::vector<std::string> m_variantNames;     // NGGR, the main one first
    float                    m_variantOffset[3] = {0, 0, 0};   // the drawn variant's world, relative to the file's

    // The CPU copy is the source of truth for the dynamic pool: growth,
    // compaction and bounds all read it, and an UPLOAD heap is write-combined
    // memory that must never be read back.
    std::vector<DynMesh>                                      m_dynMeshes;
    std::vector<::ngg::Vertex>                                m_dynVerts;
    std::vector<uint32_t>                                     m_dynIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource>                    m_dynVb, m_dynIb;
    D3D12_VERTEX_BUFFER_VIEW                                  m_dynVbv = {};
    D3D12_INDEX_BUFFER_VIEW                                   m_dynIbv = {};
    bool                                                      m_dynDirty = false;
    uint32_t                                                  m_whiteSlot = 0;

    bool UploadDynamic(ID3D12Device*);
    bool RebuildSrvHeap(ID3D12Device*);
    bool m_materialBatch = false;    // BeginMaterialBatch: the SRV heap's rebuild waits for EndMaterialBatch
    bool m_srvStale = false;         // a rebuild was put off by the batch

    std::vector<DrawItem>                                     m_objects;
    std::vector<MaterialSlot>                                 m_materials;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>       m_textures;
    // Per texture index: true if some material binds it to a normal or ORM
    // slot, i.e. it holds numbers rather than colours and must skip the sRGB
    // decode. Filled from the material table, which is why materials are
    // parsed before textures are uploaded.
    std::vector<uint8_t>                                      m_linearTextures;
    // Kept for the editor: a material list reading "material 82" helps nobody,
    // and the base-colour texture name is how RBR identifies a material anyway.
    std::vector<std::string>                                  m_textureNames;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>              m_srvHeap;
    UINT                                                      m_srvStride = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;
    // Same shader with clip() enabled, for foliage. A separate PSO rather
    // than a branch: alpha test forces late depth-write, and paying that on
    // the terrain would cost far more than the state change saves.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoAlphaTest;
    // Depth-only variants for the shadow passes. The alpha-tested one exists so
    // a tree casts the shadow of its leaves rather than of its cards.
    // Depth + normal prepass, both material variants (see RecordPrepass).
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoPrepass;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoPrepassAlpha;
    // The above with back faces culled, for the one-sided materials:
    // [prepass][alpha test][mirroring transform]. Null: drawn two-sided.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoCulled[2][2][2];
    // The main pass's foliage after a prepass: the prepass's depth tested, not written, so tested BEFORE the shader
    // despite its clip(). [forward, deferred][cull] none (two-sided), back, front (a mirroring transform).
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoAlphaKept[2][3];
    // The blended materials' (kMaterialBlend, glass), drawn after the opaque: [cull] none (two-sided), back, front
    // (a mirroring transform). Premultiplied (ONE, INV_SRC_ALPHA), depth tested, not written.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoBlend[3];
    // A moving object's glass, in the prepass: its own motion into the glass target (PSGlassMotion). [cull] as above.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoPrepassGlass[3];
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_normal;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_plantTarget;      // R32_UINT, plant + 1
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_glassMotion;      // GlassMotionBuffer
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_normalRtvHeap;   // normal, motion, plant, the glass's motion
    // The deferred local lights (PSDeferredLights): the surface's targets (normal, roughness and kind; albedo and
    // metallic) and their RTVs; the opaque world's pipelines that write them ([alpha test][mirroring transform] the
    // culled ones); the full-screen pass. Used while there are local lights.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_gbuf[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gbufRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoG, m_psoAlphaTestG, m_psoCulledG[2][2], m_psoDeferred;
    // The reflection focus's opaque draws with their own reflection rays (ReflOpen, shader model 6.5): [forward,
    // deferred][cull none, back, front]. Only these carry the ray query - in every pipeline, its state cost the whole
    // main pass ~1 ms on the development GPU.
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoRefl[2][3];
    bool                     m_deferredReady = false;
    bool                     m_pipeBuilt = false, m_pipeOk = false;
    DXGI_FORMAT              m_pipeFormat = DXGI_FORMAT_UNKNOWN;
    float                    m_pipeBias = 0.0f;         // the mip bias the pipelines were made for
    UINT                     m_dsvStride = 0;                   // the read-only depth view is the DSV heap's second
    // A pass's opaque draws in the order they are drawn (RecordPass: the solid, then the alpha-tested, each near to
    // far); kept to reuse its storage. A model batch's draws carry their instances (`inst`, `instCount`); the rest
    // draw the stream's identity instance.
    struct DrawOrder { bool alphaTest; float d2; const DrawItem* item; const D3D12_VERTEX_BUFFER_VIEW* inst = nullptr;
                       uint32_t instCount = 1; };

    // --- instanced models (see the public half) ---
    struct ModelSub { uint32_t mesh = 0, material = 0, fadeMaterial = 0; bool alpha = false; };
    struct ModelLodRt { std::vector<ModelSub> subs; float maxDist = 0.0f; bool billboard = false; };
    struct ModelDef
    {
        std::string name;
        std::vector<ModelLodRt> lods;
        float bmin[3] = {}, bmax[3] = {};
        float fade = 0.0f;
        uint32_t rtLod = 0;
    };
    struct PlacementRt
    {
        PlacementDesc d;
        bool  removed = false;
        float cols[3][4] = {};                 // the transform as three rows of p' = A p + t (the instance stream's)
        float bmin[3] = {}, bmax[3] = {};      // world box
        float centre[3] = {};
    };
    // The per-instance record of the instance stream (slot 1, PER_INSTANCE_DATA): VSIn's INST0..3.
    struct InstanceRec { float c[3][4]; uint32_t tint; float fade; uint32_t flags; uint32_t pad; };
    // One view's culled batches: per (model, LOD, solid or crossfading) a run of the stream.
    struct ModelBatch
    {
        uint32_t model, lod;
        bool     fading;
        uint32_t count;
        D3D12_VERTEX_BUFFER_VIEW vbv;
    };
    struct ModelCell { float bmin[3], bmax[3]; float reach; std::vector<uint32_t> items; };
    std::vector<ModelDef>    m_models;
    std::vector<PlacementRt> m_placements;
    std::vector<ModelCell>   m_modelCells;
    bool                     m_modelCellsDirty = false;
    float                    m_modelLodScale = 1.0f;     // NGG_MODEL_LOD_SCALE
    uint32_t                 m_replacedPlants = 0, m_replacedTris = 0;
    ModelStats               m_modelStats, m_modelStatsBuild;
    uint64_t                 m_modelFrame = 0;
    // The instance stream: an upload buffer written as views are culled; the frame's first record (offset 0) is the
    // identity every non-instanced draw uses. Grown by a new buffer (the old kept to the frame's end).
    Microsoft::WRL::ComPtr<ID3D12Resource>              m_instBuf;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_instRetired;
    uint8_t*                 m_instMapped = nullptr;
    size_t                   m_instCap = 0, m_instUsed = 0;
    D3D12_VERTEX_BUFFER_VIEW m_instIdentity = {};
    std::vector<ModelBatch>  m_mainBatches;              // the camera's, culled by the prepass (or the main pass)
    bool                     m_mainBatchesValid = false;
    std::vector<DrawItem>    m_modelDrawItems;           // per pass: a DrawItem per batch submesh
    std::vector<std::pair<uint64_t, InstanceRec>> m_cullScratch;
    bool LoadModels(ID3D12Device* dev, const uint8_t* models, size_t modelBytes, const uint8_t* placements,
                    size_t placementBytes, uint32_t fileMaterials, std::vector<uint32_t>& replacedPlants);
    void EnsureInstanceStream(ID3D12Device* dev);
    // The ray tracer's instances of the placements (a TLAS instance per placement and submesh of its ray-traced
    // LOD), kept until a placement changes: DescribeRt runs every frame the car moves.
    mutable std::vector<RtInstanceDesc> m_rtModelInstances;
    mutable std::vector<uint64_t>       m_rtModelMeshes;
    mutable bool                        m_rtModelInstancesDirty = true;
    mutable uint32_t                    m_rtModelGen = 0xFFFFFFFFu;
    std::vector<DrawOrder>              m_shadowOrder;      // a cascade's model draws (RenderShadows)
    void PlacementTransform(PlacementRt& p) const;
    void RebuildModelCells();
    bool InstanceAlloc(ID3D12Device* dev, size_t records, uint8_t*& cpu, D3D12_GPU_VIRTUAL_ADDRESS& gpu);
    // Culls the placements against `planes` from `eye` (LODs and fades by the distance from `lodEye`; a shadow view
    // takes the camera's), writes the survivors to the stream and returns its batches.
    void CullModels(ID3D12Device* dev, const float planes[6][4], const float eye[3], bool shadow,
                    std::vector<ModelBatch>& out);
    // The DrawItems of `batches` for a pass: one per batch submesh; `fadeSolid` - a crossfading batch's opaque submesh
    // through its alpha-tested twin, which dithers.
    void ModelDrawItems(const std::vector<ModelBatch>& batches, std::vector<DrawOrder>& order, bool shadow);
    void DescribeModelsRt(std::vector<uint64_t>& meshes, std::vector<RtInstanceDesc>& instances) const;
    std::vector<DrawOrder>   m_drawOrder;
    UINT                     m_prepassRtvStride = 0;
    bool                     m_prepassThisFrame = false;
    int                      m_shadowSource = 0;
    void RecordPass(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                    const float* view, const float* proj, UINT width, UINT height,
                    bool prepass);
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoShadow;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoShadowAlpha;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoLocalShadow, m_psoLocalShadowAlpha;   // RenderLocalShadows
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_localShadowCB[4];
    ShadowMaps*              m_localShadows = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_localFlag;      // the casters' kind, a slice per map
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_localFlagRtv;
    UINT                     m_localFlagRtvStride = 0;
    bool                     m_localFlagInRt = true;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_localMask[2];   // the moved-shadow mask, written / read in turn
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_localMaskRtv;
    D3D12_RESOURCE_STATES    m_localMaskState[2] = { D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET };
    uint64_t                 m_localMaskFrame = 0;
    bool                     m_localMaskFresh = true;
    ID3D12Resource*          m_localMaskWritten = nullptr;
    uint32_t                 m_localShadowsDrawn = 0;
    float                    m_localAoShare = 0.0f;
    double                   m_localShadowMs = 0.0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_localShadowTimer;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_localShadowTimerReadback;
    bool                     m_localShadowTimerPending = false;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vb;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_ib;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_depth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    // Kept alive until the GPU has consumed the frame; see the note at the end
    // of Scene::Record.
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_frameCB;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_prepassCB;   // see RecordPass
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_shadowCB[ShadowMaps::kCascades];

    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW  m_ibv = {};
    uint32_t                 m_indexCount = 0;

    // Test-scene placement: the grid follows the camera in XZ (snapped) with a
    // height captured once, so it is visible wherever a stage happens to be in
    // RBR's world space. Goes away with real converted geometry.
    bool                     m_haveAnchor = false;
    float                    m_anchorY = 0.0f;
    float                    m_lastCam[3] = { 0.0f, 0.0f, 0.0f };
    bool                     m_loggedMatrix = false;
    // Measured per asset by the converter - see SceneHeader::bakedScale.
    // Not a constant: it depends on which editor baked the track.
    float                    m_bakedScale = 1.0f;
    float                    m_mipBias = 0.0f;

    enum { kProbeSlots = 16 };  // 15 reported groups + the FIRED marker
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probe;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeZero;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeReadback;
    int                      m_probeX = 0, m_probeY = 0;
    bool                     m_probeOn = false;

    // Selection overlay. Separate root signature and PSOs from the main pass:
    // it draws into a different format, with a different depth comparison, and
    // reads no textures at all.
    std::vector<uint32_t>                        m_selection;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_selRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoSelVisible;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoSelOccluded;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_selCount;      // 2 uints, UAV
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_selCountZero;  // upload, all zero
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_selCountReadback;

    // One buffer per layer, replaced wholesale when the layer changes. The frame
    // loop is synchronous, so a replaced buffer cannot still be in flight.
    struct OverlayLayer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vb;
        D3D12_VERTEX_BUFFER_VIEW               vbv = {};
        uint32_t                               count = 0;
        uint32_t                               flags = 0;
    };
    std::vector<OverlayLayer>                    m_overlays;

    // Labels: one buffer of textured quads per layer, built at submit time so the
    // caller's strings need not outlive the call.
    struct LabelLayer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vb;
        D3D12_VERTEX_BUFFER_VIEW               vbv = {};
        uint32_t                               count = 0;   // vertices
        uint32_t                               flags = 0;
    };
    std::vector<LabelLayer>                      m_labels;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_font;
    uint32_t                                     m_fontColumns = 0, m_fontRows = 0;
    uint32_t                                     m_fontFirstChar = 32;
    UINT                                         m_fontSrvSlot = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_labelRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoLabel;         // in front
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoLabelOccluded;  // behind, faded
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoLabelNoDepth;
    uint32_t                 m_labPixVisible = 0, m_labPixOccluded = 0;

    void DrawLabels(ID3D12GraphicsCommandList*, const float* viewProj, UINT width, UINT height);
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoOvVisible;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoOvOccluded;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_psoOvNoDepth;
    uint32_t                 m_selPixVisible = 0, m_selPixOccluded = 0;
    uint32_t                 m_ovPixVisible = 0, m_ovPixOccluded = 0;

    void DrawSelection(ID3D12GraphicsCommandList*, const float* viewProj);
    void DrawOverlays(ID3D12GraphicsCommandList*, const float* viewProj);

    ID3D12Resource*          m_rtVis = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_reflTlas = 0;      // SetReflectionTlas
    bool                     m_rayQuery = false;  // m_psoRefl built (the adapter has inline ray queries)
    ID3D12Resource*          m_rtAo = nullptr;
    DXGI_FORMAT              m_rtAoView = DXGI_FORMAT_UNKNOWN;
    UINT                     m_aoSrvSlot = 0;
    bool                     m_aoSrvReady = false;
    // The froxel aerial-perspective table: two SRVs (in-scatter, transmittance)
    // in consecutive slots. Fixed-size volumes, so written once per heap.
    AerialLut                m_aerialLut;
    // Local lights: their buffers and clusters, the ray tracer's shadowed fraction, and the four heap slots the
    // scene shader reads them from (t12 lights, t13 clusters, t14 indices, t15 the fraction).
    Lights                   m_lights;
    bool                     m_lightsReady = false;
    ID3D12Resource*          m_rtLights = nullptr;
    UINT                     m_lightSrvSlot = 0;
    UINT                     m_aerialSrvSlot = 0;
    bool                     m_aerialSrvReady = false;
    bool                     m_aoOn = true;
    UINT                     m_rtSrvSlot = 0;
    bool                     m_rtSrvReady = false;
    UINT64                   m_rtSrvLoggedW = 0;        // the size and format the log last reported (the log only)
    UINT                     m_rtSrvLoggedH = 0;
    DXGI_FORMAT              m_rtSrvLoggedFormat = DXGI_FORMAT_UNKNOWN;

    Environment              m_ownEnv;
    Environment*             m_env = &m_ownEnv;
    // The cube probe around the car (probe.h, SetReflectionFocus): its focus's objects, where it is drawn from, the
    // face drawn next, the faces drawn since the focus came (all six before it is read), its SRV (t19) and the faces'
    // constant buffers, alive until the frame has run.
    ReflectionProbe          m_reflProbe;
    std::vector<uint32_t>    m_probeObjects;
    float                    m_probeCentre[3] = {};
    int                      m_probeNextFace = 0;
    int                      m_probeFacesDrawn = 0;
    UINT                     m_reflSrvSlot = 0;
    bool                     m_reflSrvReady = false;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_probeCBs;
    double                   m_probeMsSum = 0.0;   // its GPU time, summed for the log's mean
    int                      m_probeMsFrames = 0;
    UINT                     m_irrSrvSlot = 0;
    bool                     m_irrSrvReady = false;
    bool                     m_physical = true;    // the default since 5.0j
    uint32_t                 m_geomRevision = 0;
    std::vector<WeatherPreset> m_weather;
    uint32_t                 m_rtGeneration = 0;     // bumped by load and compact: mesh keys change
    std::vector<uint32_t>    m_staticIndices;       // CPU copy - the IB lives in an upload heap
    // A plant's box in the static (authored) space; see FindPlants.
    // `crossed`: old crossed-card vegetation, shaded as one volume - tagged "tree-card",
    // "bush-card" or "grass-card" in the file, or found so by FindPlants.
    // `shadow`: what the plant casts (kPlantShadow*): grass nothing, a bush a lighter, softer shadow, a tree all of
    // it - its tags' (grass-card, bush-card), or for an automatic legacy import its height (kGrassHeight, kBushHeight).
    enum { kPlantShadowCast = 0, kPlantShadowNone = 1, kPlantShadowSoft = 2 };
    struct Plant { float bmin[3], bmax[3]; bool crossed; uint32_t firstTag = 0, tagCount = 0; uint32_t shadow = kPlantShadowCast; };
    std::vector<Plant>       m_plants;
    std::vector<uint32_t>    m_triPlant;            // per static triangle; ~0 = not in a plant
    std::vector<std::string> m_tagNames;            // NGGG
    std::vector<uint32_t>    m_plantTagRefs;        // NGGP's tag refs, into m_tagNames
    float                    m_lastCamPos[3] = {};
    AtmosphereMs             m_ms;
    bool                     m_msOn = true;
    UINT                     m_msSrvSlot = 0;
    bool                     m_msSrvReady = false;
    bool                     m_ambientSpecular = true;
    UINT                     m_envSrvSlot = 0;
    bool                     m_envSrvReady = false;

    ShadowMaps*              m_shadows = nullptr;
    bool                     m_shadowsOn = true;
    float                    m_shadowNormalBias = 2.5f;   // in shadow texels
    UINT                     m_shadowSrvSlot = 0;
    bool                     m_shadowSrvReady = false;
    bool                     m_cull = true;
    bool                     m_anchorToCamera = false;
    uint32_t                 m_recordCount = 0;

    // Deliberately NOT cleared by ResetScene: it is a view setting, not stage
    // state, and losing it on every track change would make it useless.
    int                      m_debugMode = 0;
    SkyParams                m_atmosphere;
    bool                     m_aerial = true;
    bool                     m_aerialMarch = false;
    int                      m_mipFallback = 2;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timer;          // 4 timestamps
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_timerReadback;
    UINT64                   m_timerHz = 0;
    bool                     m_timerPending = false;
    double                   m_passMs = 0.0;
    double                   m_aerialMs = 0.0;
    double                   m_envMs = 0.0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_shadowTimer;    // 2 timestamps around RenderShadows
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_shadowTimerReadback;
    bool                     m_shadowTimerPending = false;
    double                   m_shadowMapMs = 0.0;
    // Dynamic by default now that shadows are real. Mode 0 stays as the A/B
    // reference against RBR's own look, not as a fallback.
    int                      m_lightMode = 1;
    int                      m_legacyProfile = 2;    // B until compared (SetLegacyProfile)
    float                    m_wetness = 0.0f;       // SetWetness: 0 dry .. 1 wet
    bool                     m_legacyImport = false; // the loaded scene's kSceneFlagLegacyImport
    bool                     m_weldNormals = true;
    bool                     m_singleStageFix = true;
    bool                     m_foliageNormals = true;

    float                    m_jitterX = 0.0f, m_jitterY = 0.0f;
    float                    m_lastViewProj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float                    m_lastViewProjJittered[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    // Last frame's m_lastViewProj, for the motion target (see RecordPass).
    float                    m_prevViewProj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool                     m_havePrevViewProj = false;

    float                    m_bboxMin[3] = { 0.0f, 0.0f, 0.0f };
    float                    m_bboxMax[3] = { 0.0f, 0.0f, 0.0f };
    float                    m_focus[3] = { 0.0f, 0.0f, 0.0f };
    bool                     m_haveFocus = false;
};

} // namespace core
} // namespace ngg
