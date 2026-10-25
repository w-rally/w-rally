// ---------------------------------------------------------------------------
// ngg-api - the flat C surface Wallaby drives the renderer through (design 8.2).
//
// WHY FLAT C, AND NOT C++/CLI: SlimDX is a mixed-mode C++/CLI assembly, and that
// is exactly why it is discontinued, why it cannot be built against on one of
// this project's machines, and why the editor is still pinned to x86. Building
// the replacement the same way would reproduce all three. A flat C ABI called
// through P/Invoke has none of those properties: this is an ordinary native DLL
// with no CLR coupling, and the managed side stays pure IL.
//
// It is also what keeps the deployment choice open. The same surface can be
// linked into the editor's process or hosted behind a pipe in a separate one,
// so in-process versus out-of-process stays a decision that can be revisited
// with a working renderer rather than one that has to be right now.
//
// RULES FOR THIS HEADER
//   - C linkage, C types only. No STL, no exceptions, no ComPtr across the line.
//   - Handles are opaque integers, assigned by the renderer. The caller maps
//     them to its own object model; the renderer may move storage underneath.
//   - Zero is always the invalid handle, so a zeroed struct is "nothing".
//   - Every entry point is safe to call with a null scene; it returns a failure
//     rather than faulting. The editor is a long-running process and a crash in
//     here takes the user's unsaved work with it.
// ---------------------------------------------------------------------------
#ifndef NGG_API_H
#define NGG_API_H

#include <stdint.h>

#if defined(_WIN32)
#  include <windows.h>
#  ifdef NGG_API_EXPORTS
#    define NGG_API __declspec(dllexport)
#  else
#    define NGG_API __declspec(dllimport)
#  endif
#else
#  define NGG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ngg_scene_t ngg_scene;

typedef uint32_t ngg_mesh;
typedef uint32_t ngg_material;
typedef uint32_t ngg_texture;
typedef uint32_t ngg_instance;

// Mesh, texture and material handles are 1-BASED, so zero means "nothing" and a
// zeroed struct is empty.
#define NGG_INVALID 0u

// INSTANCES ARE DIFFERENT, deliberately. An instance handle is the object's
// INDEX -- zero is the first object in the file, not an error -- because that is
// how ngg_set_visible and ngg_set_transform already address the static scene and
// splitting the space in two would give the editor two kinds of object to track.
// So failure needs its own value.
#define NGG_INVALID_INSTANCE 0xFFFFFFFFu

// Matches ngg::Vertex in ngg_format.h. Kept in this header too so the editor
// does not need the C++ one -- 44 bytes, and the static_assert in the
// implementation fails the build if the two ever drift.
#pragma pack(push, 4)
typedef struct
{
    float    pos[3];
    float    normal[3];
    float    uv0[2];
    float    uv1[2];
    uint32_t colour;        // 0xAABBGGRR
} ngg_vertex;
#pragma pack(pop)

// Material flags, mirroring kMaterial* in ngg_format.h.
#define NGG_MATERIAL_ALPHA_TEST 0x1u
#define NGG_MATERIAL_TWO_SIDED  0x2u

// One material, as an editor sees it. Mirrors ngg::Material (design 3.3g,
// glTF metallic-roughness) with the texture indices turned into the API's
// 1-based handles, so 0 means "this slot is empty" here exactly as it does
// everywhere else in this header.
//
// EVERY FACTOR MULTIPLIES ITS MAP. An empty slot binds a white texture, so
// `map * factor == factor` and a material with no maps at all is still a
// complete material - which is what makes a slider meaningful on RBR's
// unauthored content.
#pragma pack(push, 4)
typedef struct {
    ngg_texture base_colour;      // sRGB artwork; alpha is coverage
    ngg_texture detail;           // second layer, blended by vertex alpha.
                                  // 0 here is what makes a material single-layer.
    ngg_texture normal;           // LINEAR, tangent-space, +Y up
    ngg_texture orm;              // LINEAR: R occlusion, G roughness, B metallic
    ngg_texture emissive;         // sRGB
    uint32_t    flags;            // NGG_MATERIAL_*
    float       base_colour_factor[4];
    float       roughness;        // 1 = fully rough
    float       metallic;         // 0 = dielectric, which is almost everything outdoors
    float       normal_scale;
    float       occlusion_strength;
    float       emissive_strength;
    float       alpha_cutoff;
} ngg_material_desc;

// A weather preset (design 5.0q), physical values only - the same quantities
// the .ngg's NGGW chunk stores as "ngg-weather" JSON. Blittable: 17 floats.
typedef struct ngg_weather_desc
{
    float sun_direction[3];   // the direction light TRAVELS (from the sun), unit length
    float sun_intensity;      // the model's units; 22 is a clear day
    float turbidity;          // aerosol: 1 clean air, ~3 hazy
    float ground_albedo;      // what the atmosphere sees below the horizon
    float ground_colour[3];   // bounce colour onto shaded surfaces
    float cloud_cover;        // 0 clear .. 1 overcast
    float fog_visibility;     // metres at which contrast falls to 2 %; 0 = no fog
    float fog_height;         // fog layer scale height, metres
    float rain;               // 0..1 - stored, not yet rendered
    float snow;               // 0..1 - stored, not yet rendered
    float exposure;           // exposure at reference brightness (auto exposure adapts around it)
} ngg_weather_desc;
#pragma pack(pop)

// ---------------------------------------------------------------------- life

// Creates a device, a swap chain and a child window inside `parent`.
//
// The child window is the renderer's, and it will paint over anything the UI
// toolkit puts on top of it -- the airspace problem. Overlays that need the
// depth buffer (selection wireframe, gizmos, grid) belong to the renderer via
// ngg_set_selection; 2D chrome belongs outside the viewport. See design 8.2.
NGG_API ngg_scene* ngg_create(HWND parent, int width, int height);
NGG_API void       ngg_destroy(ngg_scene*);
NGG_API void       ngg_resize(ngg_scene*, int width, int height);

// The renderer's child HWND, for forwarding mouse and keyboard from the host.
NGG_API HWND       ngg_hwnd(ngg_scene*);

// Last error, or "" - never null. Valid until the next call on this scene.
NGG_API const char* ngg_last_error(ngg_scene*);

// Everything the renderer logs, handed to the host.
//
// Not decoration: the renderer explains itself in the log -- which chunk a bad
// asset died on, how many objects a frame actually drew, whether its own weld
// fallback decided to stand down. Without a sink the editor is blind to all of
// it and every failure looks like "nothing happened". Global, not per scene, so
// install it before ngg_create to catch device-creation failures too.
typedef void (*ngg_log_fn)(const char* message);
NGG_API void ngg_set_log_sink(ngg_log_fn sink);

// --------------------------------------------------------------------- content

// Opens a whole project: the serialised .ngg the editor already builds, from
// memory. For OPENING only -- calling this per edit is the temp-file mistake
// with the cost moved from disk to PCIe. Edits go through the delta calls below.
NGG_API int ngg_load_memory(ngg_scene*, const void* ngg_bytes, size_t byte_count);

// Everything added after the load goes to a dynamic pool beside the static
// scene, so adding one prop does not re-upload the whole track.
//
// ngg_add_mesh validates the index array against the vertex count before it
// stores anything: an index past the end would read whatever the neighbouring
// mesh left there and draw a plausible wrong shape rather than failing.
//
// Indices are MESH-RELATIVE. The renderer draws with a base-vertex offset, so
// ngg_compact can move a mesh without rewriting a single index.
NGG_API ngg_mesh     ngg_add_mesh(ngg_scene*, const ngg_vertex* verts, uint32_t vertex_count,
                                  const uint32_t* indices, uint32_t index_count);
NGG_API ngg_texture  ngg_add_texture(ngg_scene*, const void* dds, size_t byte_count);
NGG_API ngg_material ngg_add_material(ngg_scene*, ngg_texture stage0, ngg_texture stage1,
                                      uint32_t flags);

// One mesh, many instances: placing fifty cones is one ngg_add_mesh and fifty
// ngg_add_instance. `xform` is a row-major 4x4 with translation in row 3,
// matching the .ngg convention; null means identity.
NGG_API ngg_instance ngg_add_instance(ngg_scene*, ngg_mesh, ngg_material,
                                      uint32_t layer, const float xform[16]);

// Moves one instance. For geometry that came from ngg_load_memory this is a
// WORLD-SPACE delta, because .ngg bakes placements into the vertices and there
// is no local space to place it in; identity puts it back where the file had it.
// Rotating about an object's own centre is composed by the caller --
// translate(-c) * R * translate(c) -- which is what ngg_object_bounds is for.
NGG_API int ngg_set_transform(ngg_scene*, ngg_instance, const float xform[16]);

// World bounds of one instance, after any transform: a pivot for a gizmo, and
// the frame for "zoom to selection".
NGG_API int ngg_object_bounds(ngg_scene*, ngg_instance, float out_min[3], float out_max[3]);

// The object's source key from the file (NGGK): RBR's instance id of the movable object it was built from - the
// physics' solid of it (the .trk collision instance has the same id). -1 for none, or a file without keys. Many
// objects can share a key (the converter splits objects); move them all.
NGG_API int32_t ngg_object_key(ngg_scene*, ngg_instance);
NGG_API int ngg_set_visible(ngg_scene*, ngg_instance, int visible);

// Removing an instance is a draw-list edit: no GPU work, and undo is therefore
// just as cheap, because the geometry never left the buffer. Storage returns on
// ngg_compact, which belongs on save rather than on the interactive path.
NGG_API int ngg_remove_instance(ngg_scene*, ngg_instance);
NGG_API int ngg_remove_mesh(ngg_scene*, ngg_mesh);      // refcounted by instances
NGG_API int ngg_compact(ngg_scene*);

// How many instances the scene holds, and how many of those are drawn. The
// editor uses the pair to show what a layer filter or a hide is actually doing.
NGG_API uint32_t ngg_instance_count(ngg_scene*);
NGG_API uint32_t ngg_visible_count(ngg_scene*);

// ------------------------------------------------------------- materials
//
// The editing surface for a material inspector. Read one, change what the
// human changed, write it back: `ngg_set_material` takes the whole descriptor
// rather than one field at a time, so there is no partial-update state to get
// wrong and no growing list of per-field setters.
//
// A SCALAR EDIT COSTS NOTHING AND SHOWS ON THE NEXT FRAME. The material reaches
// the shader as root constants read from the renderer's own table every frame,
// so dragging a roughness slider is a memcpy - no upload, no rebuild, no
// reload. Changing a TEXTURE slot is the one expensive case, and the renderer
// detects it rather than trusting the caller to say so.
//
// Handles are 1-based, so material 1 is the first. Iterate 1..ngg_material_count.
NGG_API uint32_t ngg_material_count(ngg_scene*);
NGG_API int      ngg_get_material(ngg_scene*, ngg_material, ngg_material_desc* out);
NGG_API int      ngg_set_material(ngg_scene*, ngg_material, const ngg_material_desc*);

// A name for the list, so an inspector shows "MB_ROCKS01.tga" rather than
// "material 82". It is the base-colour texture's name - which is how RBR
// identifies a material anyway. Writes at most `bytes` including the
// terminator; returns the length written, or 0 for an unknown handle.
NGG_API uint32_t ngg_material_name(ngg_scene*, ngg_material, char* buf, uint32_t bytes);

// Which material an instance draws with, so clicking a surface in the viewport
// can select the material that produced it. 0 if the handle is unknown.
NGG_API ngg_material ngg_object_material(ngg_scene*, ngg_instance);

// ----------------------------------------------------------------- view state

// Row-major, as everything else here.
NGG_API void ngg_set_camera(ngg_scene*, const float view[16], const float proj[16]);

// Direction the light travels (not the direction TO the sun), as core::SkyParams
// stores it.
NGG_API void ngg_set_sun(ngg_scene*, const float dir[3], float exposure);

NGG_API void ngg_set_debug_view(ngg_scene*, int mode);
NGG_API void ngg_set_light_mode(ngg_scene*, int mode);   // baked A/B vs dynamic
NGG_API void ngg_set_shadows(ngg_scene*, int on);
NGG_API void ngg_set_sky(ngg_scene*, int on, int aerial);
NGG_API void ngg_set_taa(ngg_scene*, int on);

// Auto exposure, on by default (design 5.0g). With it on, ngg_set_sun's
// exposure is the exposure at REFERENCE brightness; the renderer meters the
// image and adapts around it. Turn it OFF when judging a surface's brightness -
// in the material inspector, say - because a meter partly undoes exactly the
// change being judged: brighten a material that fills the view and the
// exposure comes down to meet it.
NGG_API void ngg_set_auto_exposure(ngg_scene*, int on);

// ------------------------------------------------------------------- weather
//
// For the WEATHER EDITOR (design 5.0q). The live weather is what renders: set
// it and the next frame shows it - sky, sun, fog, haze, environment and aerial
// tables all follow, because they share one atmosphere. Loading a file applies
// its default preset (the first in NGGW), as the game does.
//
// The FILE's presets are read-only here: they are what the converter wrote into
// the .ngg (and what competitions use). Editing is: pick one, change the live
// weather, save it to the project as JSON (ngg_weather_to_json), and let the
// converter write the project's presets into the next .ngg.
NGG_API int ngg_get_weather(ngg_scene*, ngg_weather_desc* out);
NGG_API int ngg_set_weather(ngg_scene*, const ngg_weather_desc*);

// The presets the loaded .ngg carries; index 0 is the track's default.
NGG_API uint32_t ngg_weather_preset_count(ngg_scene*);
NGG_API uint32_t ngg_weather_preset_name(ngg_scene*, uint32_t index, char* buf, uint32_t bytes);
NGG_API int      ngg_get_weather_preset(ngg_scene*, uint32_t index, ngg_weather_desc* out);
// Makes preset `index` the live weather (same as get + set).
NGG_API int      ngg_select_weather_preset(ngg_scene*, uint32_t index);

// The "ngg-weather" JSON schema, owned by the renderer so the editor never
// grows a second copy that could drift. to_json writes at most `bytes`
// including the terminator and returns the length (0 if it did not fit);
// from_json returns 1 on success and the preset's name in `name`.
NGG_API uint32_t ngg_weather_to_json(const ngg_weather_desc*, const char* name, const char* source,
                                     char* buf, uint32_t bytes);
NGG_API int      ngg_weather_from_json(const char* json, ngg_weather_desc* out,
                                       char* name, uint32_t nameBytes);

// Quality tier (design 1.8.4): 0 low, 1 medium (default), 2 high. Today the
// tiers differ in ray-traced AO; every tier keeps ray-traced sun shadows.
NGG_API void ngg_set_quality(ngg_scene*, int tier);

// Ray-traced sun shadows (and with them the RT AO) when the adapter can, or the
// cascaded shadow maps. On by default; off is the A/B against the cascades.
NGG_API void ngg_set_ray_tracing(ngg_scene*, int on);

// What the last frame did, for a client's own readout - the numbers the
// viewer's HUD shows. Times are GPU milliseconds of the last finished frame.
#pragma pack(push, 4)
typedef struct
{
    float    pass_ms, env_ms, aerial_ms;   // the main pass; its environment and aerial-table parts
    float    rt_ms, ao_ms, denoise_ms;     // the ray tracing: all of it, the AO's part, the denoise
    uint32_t rt_available;                 // the adapter can ray trace
    uint32_t rt_active;                    // this frame's shadows were ray traced
    uint32_t rt_pixels, rt_shadowed;       // pixels traced, of them in shadow
    uint32_t quality;                      // 0 low, 1 medium, 2 high (ngg_set_quality)
    uint32_t taa, shadows, ambient_occlusion, auto_exposure;   // 1 = on
    uint32_t debug_view;                   // ngg_set_debug_view's mode
    float    exposure;                     // applied last frame
    float    metered_ev, adapted_ev;       // auto exposure: log2 luminance, this frame / adapted
    char     rt_fallback[96];              // why RT is not active, "" when it is
} ngg_stats;
#pragma pack(pop)
NGG_API int ngg_get_stats(ngg_scene*, ngg_stats* out);

// The NEXT ngg_render's frame, as presented, written to `path` as a 24-bit
// .bmp - read back from the swap chain, so it does not depend on the window
// being visible the way a desktop capture does.
NGG_API void ngg_capture_frame(ngg_scene*, const char* path);

// The renderer's own load-time fallbacks, exposed so the editor can A/B them
// against what the converter did. Both stand down on a converted asset (they
// measure first), so these are diagnostics rather than settings.
NGG_API void ngg_set_weld_normals(ngg_scene*, int on);
NGG_API void ngg_set_foliage_normals(ngg_scene*, int on);
NGG_API void ngg_set_cull(ngg_scene*, int on);
NGG_API void ngg_set_tonemap(ngg_scene*, int curve);

// Names for the cycling modes, so the editor's UI does not hard-code a list
// that silently drifts from the renderer's. Null for an out-of-range index.
NGG_API const char* ngg_debug_view_name(int mode);
NGG_API const char* ngg_light_mode_name(int mode);

// --------------------------------------------------------------- editor extras

// Depth-correct selection highlighting, drawn by the renderer because the depth
// buffer is here. Pass count 0 to clear.
//
// Drawn as a wireframe AFTER the tonemap: the colour the editor sees is the
// colour written, and a one-pixel wire never enters TAA's history to ghost
// behind itself. The part that occludes correctly is the depth test, which is
// exactly the part a host-side overlay cannot do.
//
// HIDDEN INSTANCES DRAW NO HIGHLIGHT. Hiding is how deletion is expressed
// (ngg_set_visible), so a deleted object still trailing a wire would be as
// confusing as a handle that quietly draws nothing.
//
// An out-of-range handle clears the selection and sets ngg_last_error rather
// than highlighting the wrong object.
NGG_API void ngg_set_selection(ngg_scene*, const ngg_instance* instances, uint32_t count);

// ------------------------------------------------------------- overlay lines
//
// Everything a track editor draws that is not track geometry is a line: the
// drive line, trigger volumes, camera markers, bounding boxes, manipulator
// handles. Measured in Wallaby's own draw calls, its gizmos are 7 LineStrip, 4
// TriangleStrip and 3 TriangleList -- so lines with per-vertex colour is very
// nearly the whole requirement.
//
// THIS IS THE SHAPE THAT MAKES THE D3D9 REPLACEMENT POSSIBLE. The old editor
// path is immediate mode: every map object calls SetRenderState, SetTransform
// and DrawPrimitives for itself, and no retained-mode renderer can implement
// that interface. Here the caller says what a layer CONTAINS and the renderer
// owns how it is drawn -- described, not commanded.
//
// Layers are replaced wholesale by id (0..63), so the drive line can be rebuilt
// without resubmitting the trigger volumes. Pass count 0 to clear one.
//
// SEGMENTS, NOT STRIPS: consecutive PAIRS of vertices are one line each. One
// topology means no state to get wrong, and expanding a strip is the caller's
// one-liner. An odd count is rejected rather than silently dropping a vertex.
#pragma pack(push, 4)
typedef struct
{
    float    pos[3];
    uint32_t colour;        // 0xAABBGGRR, per vertex - no colour state to set
} ngg_overlay_vertex;
#pragma pack(pop)

// Drawn through geometry instead of behind it. For manipulator handles: a gizmo
// you cannot see because the thing you are dragging is in front of it is not a
// gizmo. Without this flag a layer is depth-tested, and the part behind
// geometry is drawn dimmed rather than dropped.
#define NGG_OVERLAY_NO_DEPTH 0x1u
// Labels only: pos[0..1] are PIXELS from the window's top-left (pos[2] is
// ignored) and the anchor is the text's top-left - a HUD line rather than a
// name on something in the world. Implies NGG_OVERLAY_NO_DEPTH.
#define NGG_LABEL_SCREEN     0x2u
// Labels only: a dark copy under the text, offset a pixel, so it reads over a
// sunlit road or a bright sky.
#define NGG_LABEL_SHADOW     0x4u

NGG_API int ngg_set_overlay_lines(ngg_scene*, uint32_t layer,
                                  const ngg_overlay_vertex* verts, uint32_t count,
                                  uint32_t flags);
NGG_API void ngg_clear_overlays(ngg_scene*);

// ------------------------------------------------------------- world labels
//
// Object names and ids at world positions -- the last thing the editor drew that
// the renderer could not. They cannot be painted by the host on top of the
// viewport: a native child window with a swap chain wins the airspace fight, so
// host-drawn text either flickers or never appears.
//
// THE FONT COMES FROM THE HOST. Embedding one would put glyph data in the
// renderer and freeze a size and character set into it, and the host already has
// a font rasteriser. Upload a MONOSPACE atlas once: `columns` x `rows` equal
// cells, RGBA8, character `first_char` in the top-left cell, reading left to
// right. Alpha is what gets sampled, so white glyphs on transparent black.
NGG_API int ngg_set_font(ngg_scene*, const void* rgba, uint32_t width, uint32_t height,
                         uint32_t columns, uint32_t rows, uint32_t first_char);

#pragma pack(push, 4)
typedef struct
{
    float    pos[3];         // world anchor
    uint32_t colour;         // 0xAABBGGRR
    uint32_t text_offset;    // byte offset into the blob; NUL-terminated there
} ngg_label;
#pragma pack(pop)

// Replaces one label layer. The strings live in one packed blob rather than an
// array of pointers: it marshals as a single buffer, and the renderer builds the
// glyph quads during this call, so nothing the caller passes has to outlive it.
//
// `pixel_height` is in PIXELS and stays that size at any distance -- a name you
// cannot read at 200 m is not a label. Labels are depth-tested at their ANCHOR,
// so one is occluded exactly when the thing it names is; behind geometry it
// fades rather than vanishing, as the overlay lines do.
// NGG_OVERLAY_NO_DEPTH applies here too.
NGG_API int ngg_set_labels(ngg_scene*, uint32_t layer,
                           const ngg_label* labels, uint32_t count,
                           const char* text_blob, uint32_t blob_bytes,
                           float pixel_height, uint32_t flags);

// Glyph pixels the last frame wrote, split by depth. Same purpose as the other
// two counters: "no labels" and "labels drawn and rejected" are one blank screen.
NGG_API int ngg_label_stats(ngg_scene*, uint32_t* visible_pixels,
                            uint32_t* occluded_pixels);

// Wire pixels the last frame wrote, split by whether they passed the depth
// test. "Nothing is highlighted" and "everything is highlighted and rejected by
// the depth test" are the same empty screen from out here and have nothing in
// common as causes; these two numbers separate them. Both zero with an empty
// selection.
NGG_API int ngg_selection_stats(ngg_scene*, uint32_t* visible_pixels,
                                uint32_t* occluded_pixels);

// The same, for the overlay layers. Counted apart from the selection, or "the
// drive line is drawing" and "the highlight is drawing" collapse into one number
// that answers neither question.
NGG_API int ngg_overlay_stats(ngg_scene*, uint32_t* visible_pixels,
                              uint32_t* occluded_pixels);

// Reads back what the shader produced at one pixel: albedo, normal, N.L, the
// lit HDR value and more, logged by name through the log sink. One shot.
//
// This is the renderer's own instrument, and it is the only way to check from
// outside that a change reached the SHADER rather than merely the draw call --
// counters cannot tell a correct transform from a garbage one.
NGG_API void ngg_probe_pixel(ngg_scene*, int x, int y);

// The scene's world bounds, and a sensible point to look at. Used to frame the
// camera on open instead of guessing.
NGG_API int ngg_bounds(ngg_scene*, float out_min[3], float out_max[3]);

// A point ON the ground, roughly mid-stage. Not the same as the centre of the
// bounding box, and the difference matters: a stage that bends puts its bbox
// centre in open air, so a camera aimed there looks at sky. Returns 0 if the
// scene has none.
NGG_API int ngg_focus_point(ngg_scene*, float out_point[3]);

NGG_API void ngg_render(ngg_scene*);

#ifdef __cplusplus
}
#endif

#endif // NGG_API_H
