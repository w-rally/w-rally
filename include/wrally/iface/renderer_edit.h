// ---------------------------------------------------------------------------
// wrally/iface/renderer_edit.h - the renderer's EDITOR table (WRenderer.dll).
//
// What a track editor needs from the renderer beyond what the game asks of it:
// per-object visibility and placement, a selection highlight, overlay line
// layers, world-space labels, live material editing and a pixel probe. It is
// ngg-api's editor surface (plugin-rbr ngg/api, removed from W-Rally in phase
// 4) over the same ngg-core calls, so an editor that used ngg-api.dll can draw
// through this one and stay on the renderer the game ships. Its client is
// Wallaby (NGG design 8.2); the game does not use it.
//
// Reached from RendererApi::editApi with kRendererEditVer, and versioned on its
// own: editor additions do not move kRendererInterfaceVer, and the game's
// interface bumps do not move this one. Same rules as renderer.h otherwise -
// one thread, POD, pack 4, nothing thrown across.
//
// OBJECTS are the scene's draw objects by INDEX: the .ngg's NGGO order first,
// then whatever createInstance added (instanceObjects says which). That is the
// unit ngg-api's instance handles named. An object can be moved by this table,
// by a FrameDesc's stageObjects (its key) or by its instance's InstanceUpdate;
// the last call wins, so an editor uses one of them per object.
//
// Everything here takes effect from the next renderFrame, and the readouts
// (overlayStats) describe the last one: renderFrame waits for the GPU, so they
// are ready when it returns. The highlight, the lines and the labels are drawn
// after the tonemap, depth-tested against the scene - not while the frame is
// upscaled (RenderSettings::upscaler), where the depth is not the output's size.
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/renderer.h"

namespace wrally::iface {

// 1: objects, selection, overlay lines, labels, materials, light mode, cull,
//    the pixel probe (ngg-api's editor calls, by object index).
constexpr uint32_t kRendererEditVer = 1;

// Overlay and label flags (ngg-core's kOverlay*).
constexpr uint32_t kEditNoDepth     = 1u << 0;   // lines and labels: drawn through the geometry (manipulators)
constexpr uint32_t kEditScreenSpace = 1u << 1;   // labels: pos[0..1] are pixels from the top-left, no depth
constexpr uint32_t kEditTextShadow  = 1u << 2;   // labels: a black copy one pixel under the text

constexpr uint32_t kEditOverlayLayers = 63;      // setOverlayLines layers 0..62
constexpr uint32_t kEditLabelLayers   = 64;      // setLabels layers 0..63

#pragma pack(push, 4)

struct EditLabel {
    float    pos[3];          // the world anchor (pixels with kEditScreenSpace)
    uint32_t colour;          // 0xAABBGGRR
    uint32_t textOffset;      // into the call's text blob, NUL-terminated
};
static_assert(sizeof(EditLabel) == 20, "EditLabel layout");

// The pixels the last frame's editor overlay wrote, by whether they passed the
// depth test: the only way to tell "drawn" from "drawn and hidden" from outside.
struct EditOverlayStats {
    uint32_t selectionVisible, selectionOccluded;
    uint32_t linesVisible, linesOccluded;
    uint32_t labelsVisible, labelsOccluded;
};
static_assert(sizeof(EditOverlayStats) == 24, "EditOverlayStats layout");

#pragma pack(pop)

// Valid while the Renderer it came from is. Every function tolerates a null
// renderer and an out-of-range index and returns ErrInvalidArg (or 0) for it.
struct RendererEditApi {
    // ---- objects: hide (deletion is hiding: undo costs nothing), move, bounds
    uint32_t (WRALLY_CALL* objectCount)(Renderer*);
    uint32_t (WRALLY_CALL* visibleObjectCount)(Renderer*);
    Status   (WRALLY_CALL* setObjectVisible)(Renderer*, uint32_t object, uint32_t visible);
    // A WORLD-SPACE delta from where the file put it (the .ngg bakes placements):
    // identity restores it. Turning about its own centre is the caller's to
    // compose, from objectBounds.
    Status   (WRALLY_CALL* setObjectTransform)(Renderer*, uint32_t object, const float delta[16]);
    Status   (WRALLY_CALL* objectBounds)(Renderer*, uint32_t object, float outMin[3], float outMax[3]);
    // The objects an instance's meshes became, in its model's mesh order. `count`
    // gets how many there are, whatever `capacity` is.
    Status   (WRALLY_CALL* instanceObjects)(Renderer*, InstanceHandle, uint32_t* out, uint32_t capacity,
                                            uint32_t* count);
    // A point ON the ground mid-stage, to aim a camera at (a bent stage's box
    // centre is in the air).
    Status   (WRALLY_CALL* focusPoint)(Renderer*, float out[3]);

    // ---- the selection highlight: a wire round each object, dimmed where it is
    //      behind something. Hidden objects draw none. Replaces the last set.
    Status   (WRALLY_CALL* setSelection)(Renderer*, const uint32_t* objects, uint32_t count);

    // ---- overlay lines: segments (vertex pairs), one layer replaced per call,
    //      kept until changed. count 0 empties the layer. An odd count is refused.
    Status   (WRALLY_CALL* setOverlayLines)(Renderer*, uint32_t layer, const LineVertex* vertices, uint32_t count,
                                            uint32_t flags);
    void     (WRALLY_CALL* clearOverlays)(Renderer*);   // every layer (FrameDesc::lines too)

    // ---- labels: a monospace atlas from the host (RGBA8, a cell per character
    //      from firstChar, row-major), then layers of text at world anchors,
    //      pixelHeight pixels tall at any distance.
    Status   (WRALLY_CALL* setFont)(Renderer*, const void* rgba, uint32_t width, uint32_t height, uint32_t columns,
                                    uint32_t rows, uint32_t firstChar);
    Status   (WRALLY_CALL* setLabels)(Renderer*, uint32_t layer, const EditLabel* labels, uint32_t count,
                                      const char* textBlob, uint32_t blobBytes, float pixelHeight, uint32_t flags);
    Status   (WRALLY_CALL* overlayStats)(Renderer*, EditOverlayStats* out);

    // ---- materials: the scene's table, handles 1-based. Read the whole
    //      descriptor, change it, write it back. A scalar shows on the next frame
    //      at no cost; a texture slot rebuilds the descriptor heap. flags carry
    //      kMaterialAlphaTest, kMaterialTwoSided and kMaterialBlend only; what the
    //      renderer derives from the slots is not exposed.
    uint32_t (WRALLY_CALL* materialCount)(Renderer*);
    Status   (WRALLY_CALL* getMaterial)(Renderer*, uint32_t material, MaterialDesc* out);
    Status   (WRALLY_CALL* setMaterial)(Renderer*, uint32_t material, const MaterialDesc* in);
    // Its base-colour texture's name, which is how RBR names a material.
    Status   (WRALLY_CALL* materialName)(Renderer*, uint32_t material, char* buf, uint32_t bytes);
    Status   (WRALLY_CALL* objectMaterial)(Renderer*, uint32_t object, uint32_t* outMaterial);

    // ---- shading switches a game does not offer
    Status      (WRALLY_CALL* setLightMode)(Renderer*, int32_t mode);   // 0 RBR's bake (the A/B reference), 1 dynamic
    const char* (WRALLY_CALL* lightModeName)(int32_t mode);             // null past the last
    Status      (WRALLY_CALL* setCull)(Renderer*, uint32_t on);         // frustum culling (on by default)

    // ---- the pixel probe: the next frame logs that pixel's lighting terms by
    //      name (through the host's log), and whether anything was there.
    Status   (WRALLY_CALL* probePixel)(Renderer*, int32_t x, int32_t y);
};

} // namespace wrally::iface
