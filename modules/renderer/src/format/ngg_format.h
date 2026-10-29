// ---------------------------------------------------------------------------
// .ngg asset format - v0 (minimal).
//
// This is the real container design from docs/ngg-design.md 3.5,
// implemented so far only as much as the renderer needs: header, vertices,
// indices. The chunk framing is the part worth getting right now, because it is
// what lets later versions add materials, clusters, lights and skeletons
// without invalidating anything already written - an unknown chunk is skipped,
// not an error.
//
// WRally's writer of this layout is ngg_scene.h beside this file (the legacy
// importer writes through it); Wallaby's NggWriter.cs is another producer.
// tools/ngg-check keeps them in step: the writer against a reader and the
// renderer's own loader, and Wallaby's files read and rewritten the same.
//
// All little-endian. All chunk payloads 4-byte aligned.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

namespace ngg {

inline uint32_t FourCC(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

const uint32_t kMagic       = 0x3047474EU;  // 'NGG0'
// v1 added a second UV set and a second texture per material: RBR's ground is
// a two-stage blend (tex0 by uv0, tex1 by uv1) and rendering only the first
// stage loses the detail layer that hides chunk seams.
const uint32_t kFormatV0    = 0;
const uint32_t kFormatV1    = 1;
const uint32_t kFormatV2    = 2;   // SceneHeader.bakedScale
// v3 replaces the material struct outright: metallic-roughness PBR, five
// texture slots, per-material alpha cutoff. See Material below for why this is
// a break rather than an extension.
//
// The reader still accepts v2 materials, and that is a TRANSITION, not a
// compatibility promise: it exists so a converter and a renderer in two
// different repositories do not have to land the same change in the same hour.
// Delete it once Wallaby writes v3.
const uint32_t kFormatV3    = 3;
const uint32_t kFormatCurrent = kFormatV3;

#pragma pack(push, 4)

struct FileHeader
{
    uint32_t magic;        // kMagic
    uint32_t version;      // kFormatV0
    uint32_t chunkCount;
    uint32_t reserved;
};

struct ChunkHeader
{
    uint32_t fourcc;
    uint32_t byteSize;     // payload bytes that follow, excluding this header
};

// SceneHeader.flags
// Set only by the synthetic test scene. Converted track geometry is already in
// the game's per-stage world space, so offsetting it would be exactly wrong -
// this is the switch that keeps the debug behaviour out of the real path.
const uint32_t kSceneFlagAnchorToCamera = 1u << 0;
// Set by WRally's automatic .lbs importer (game/src/assets/legacy_stage.h): converted with no human input and no
// tags, the vertex colour RBR's bake. The renderer then lights alpha-tested geometry by its legacy profile
// (Scene::SetLegacyProfile). Wallaby's files never carry it.
const uint32_t kSceneFlagLegacyImport = 1u << 1;

// 'NGGH' - scene header
struct SceneHeader
{
    float    bboxMin[3];
    float    bboxMax[3];
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t flags;

    // What to multiply the baked vertex colour by to reach full range.
    //
    // MEASURED PER TRACK, because it is not a property of the format: Joux
    // Verte's ground bakes cap at 128 (half scale), track-108's at 255 (full
    // scale, and mostly saturated). A constant tuned on one track blows the
    // other out - which is exactly what a hard-coded 2.0 did.
    //
    // Physically this stands in for the game's own per-track lighting constant
    // (design 3.2b: sun colour x exposure, from Maps\TrackSettings.ini). Using
    // the bake's own headroom is a self-calibrating approximation of it, and
    // disappears entirely once lighting is dynamic.
    float    bakedScale;
};

// 'NGGV' - vertices. 44 bytes each.
struct Vertex
{
    float    pos[3];
    float    normal[3];
    float    uv0[2];       // stage 0
    float    uv1[2];       // stage 1 - the detail/blend layer
    uint32_t colour;       // 0xAABBGGRR, matches DXGI_FORMAT_R8G8B8A8_UNORM
};

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 16,  "FileHeader layout");
static_assert(sizeof(ChunkHeader) == 8,  "ChunkHeader layout");
static_assert(sizeof(SceneHeader) == 40, "SceneHeader layout");
static_assert(sizeof(Vertex) == 44,     "Vertex layout - keep the Python writers in step");

// 'NGGI' - indices, uint32 each.

// "no texture in this slot". A real value, not a sentinel bolted on: whether a
// slot is filled is the material's most load-bearing property, and asking the
// index is more reliable than carrying a parallel flag that can disagree with it.
const uint32_t kNoTexture = 0xFFFFFFFFu;

// Material flags. Mirrored in Wallaby's NggWriter.cs (NggMaterialFlags) - keep
// the two in step.
const uint32_t kMaterialAlphaTest = 1u << 0;   // clip() against alphaCutoff
const uint32_t kMaterialTwoSided  = 1u << 1;   // no back-face culling
// 1u << 2 was kMaterialSingleStage in v2. It is gone, and its absence is the
// point: "this material has no second layer" is now said by `detail ==
// kNoTexture`, which cannot disagree with itself. See the note below.

// 'NGGM' - materials. METALLIC-ROUGHNESS, v3.
//
// v2 was RBR's model wearing our names: two texture stages, a blend weight in
// vertex alpha, and lighting baked into vertex colour. That was the right shape
// while the job was reproducing 2004 output faithfully enough to A/B against
// it. It is the wrong shape for the renderer we now have - there is nowhere to
// put a normal map, nowhere to say how rough a surface is, and no way to make
// something emit light, which is most of what separates a 2004 image from a
// modern one.
//
// v3 is glTF metallic-roughness, near enough 1:1. That is deliberate and it is
// the same argument as design 7.2: glTF is the interchange format, every DCC
// tool speaks it, and a runtime material model that maps onto it means an
// authored material survives the trip without a translation layer inventing
// values on the way.
//
// TWO THINGS ARE KEPT FROM RBR, and both earned their place by measurement
// rather than by inheritance:
//
//   - `detail` + the vertex-alpha blend. Design 3.3c measured this live on both
//     test tracks, and it is what hides terrain chunk seams. Detail texturing is
//     a standard technique in its own right; this is not a legacy stage, it is
//     a second layer that happens to be how RBR spelled it too.
//   - `baseColour.a` as coverage on alpha-tested geometry.
//
// And one thing is deliberately NOT here: the baked vertex colour. It stays in
// the vertex data (design 3.2) as the A/B reference against vanilla, not as a
// material property, because it is a record of one particular sun on one
// particular day.
struct Material
{
    // Texture slots, all indices into NGGT, all kNoTexture when unused.
    uint32_t baseColour;     // sRGB. Albedo, plus coverage in alpha.
    uint32_t detail;         // sRGB. Second layer, blended by vertex alpha.
                             // kNoTexture is what makes a material single-layer:
                             // the shader then never reads uv1, which matters
                             // because RBR's uv1 on object meshes is garbage
                             // (design 3.3f) - and cannot be fixed by a flag
                             // that a writer might forget to set.
    uint32_t normal;         // LINEAR. Tangent-space, +Y up (glTF convention).
    uint32_t orm;            // LINEAR. R = occlusion, G = roughness, B = metallic.
                             // glTF's packing, so an exported material needs no
                             // channel shuffle.
    uint32_t emissive;       // sRGB. Design 5: night lighting needs surfaces that
                             // are bright without being lit.
    uint32_t flags;          // kMaterial*

    // Factors. These MULTIPLY their map where one exists and stand alone where
    // it does not, so an untextured material is a complete material rather than
    // a special case - which is what lets a converter emit sensible PBR for
    // RBR's 6,515 textures before a single map has been authored.
    float baseColourFactor[4];
    float roughness;         // 1 = fully rough. Dielectric default is ~0.6-0.8.
    float metallic;          // 0 = dielectric. Almost everything outdoors.
    float normalScale;       // 1 = as authored; 0 flattens the map out.
    float occlusionStrength; // how much of orm.R to apply.
    float emissiveStrength;  // multiplies the emissive texture, or emits flat.
    float alphaCutoff;       // was hard-coded 170/255, which is RBR's AlphaRef.
                             // Per material now, because it is a property of the
                             // artwork and RBR's number only suited RBR's.
};

// 64 bytes, four cache-line-friendly float4s. No `reserved` tail: growing this
// struct is what a format version is for, and a spare field is where a writer
// and a reader quietly start disagreeing about what it means.
static_assert(sizeof(Material) == 64, "Material v3 layout - keep NggWriter.cs in step");

// Sensible values for a material a converter has not thought about yet, so
// "no PBR data" produces a plausible outdoor surface rather than a black or
// mirror-finish one. Dielectric, fairly rough, fully opaque.
inline Material DefaultMaterial()
{
    Material m = {};
    m.baseColour = m.detail = m.normal = m.orm = m.emissive = kNoTexture;
    m.flags = 0;
    m.baseColourFactor[0] = m.baseColourFactor[1] =
    m.baseColourFactor[2] = m.baseColourFactor[3] = 1.0f;
    m.roughness         = 0.75f;
    m.metallic          = 0.0f;
    m.normalScale       = 1.0f;
    m.occlusionStrength = 1.0f;
    m.emissiveStrength  = 0.0f;
    m.alphaCutoff       = 170.0f / 255.0f;   // RBR's AlphaRef, as the default only
    return m;
}

// The v2 material, READ ONLY, so a v2 asset still loads while the converter
// catches up. This is a transition, not a compatibility promise - delete it and
// the version branch in Scene::Load once Wallaby writes v3.
struct MaterialV2
{
    uint32_t texture0;
    uint32_t texture1;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(MaterialV2) == 16, "MaterialV2 layout");

// 'NGGO' - objects. The unit of addressing for layers and variants (design
// 3.4b) and, more immediately, the unit of drawing: one object = one index
// range with one material. RBR's own render chunks map onto these directly.
struct Object
{
    uint32_t layerId;
    uint32_t materialId;
    uint32_t firstIndex;
    uint32_t indexCount;
};

// 'NGGT' - embedded textures, laid out as:
//     uint32 count
//     per texture: uint32 nameLen, char name[nameLen],
//                  uint32 dataSize, uint8 dds[dataSize], pad to 4
// Embedded rather than referenced so a stage is one file and therefore one CRC
// in the fairness chain (design 3.6).

// 'NGGS' - where the car starts: float position[3] then float facing[3].
//
// OPTIONAL and written last, so a reader that does not know it skips it by size. Without
// it the viewer falls back to a geometry centroid, which on an 8 km stage is nowhere in
// particular. NOTE: nothing reads this yet -- Wallaby writes it, the renderer still has
// to pick it up.
//
// Position comes from RBR's block (2,11), read in FILE order: that block is already in the
// geometry's space, unlike RBR's drive-line blocks, which store (x, z, y). Facing is NOT
// from that block -- its second vector measures 90 degrees off the route with inconsistent
// sign -- so it is derived from the drive line. Measured over 82 corpus tracks, the start
// then sits a median 3.7 m from the nearest drive-line node, against 786 m if the drive
// line is read in file order. Wallaby's GameplayBlocks.CarInfoBlock records the details.
struct SpawnPoint
{
    float position[3];
    float facing[3];
};

inline uint32_t ChunkSceneHeader() { return FourCC('N','G','G','H'); }
inline uint32_t ChunkVertices()    { return FourCC('N','G','G','V'); }
inline uint32_t ChunkIndices()     { return FourCC('N','G','G','I'); }
inline uint32_t ChunkMaterials()   { return FourCC('N','G','G','M'); }
inline uint32_t ChunkObjects()     { return FourCC('N','G','G','O'); }
inline uint32_t ChunkTextures()    { return FourCC('N','G','G','T'); }
inline uint32_t ChunkSpawn()       { return FourCC('N','G','G','S'); }

// 'NGGK' - each object's source key, parallel to NGGO: uint32 count, then int32 key[count], -1 for none. The key is
// RBR's instance id of the movable object the object was built from (an .lbs block (1,6) placement), which is also
// the .trk collision instance's id - the physics' solid of that object. So a renderer driven by the physics finds the
// objects a moving solid draws as (every piece: the converter splits objects, and each piece keeps the key).
// OPTIONAL: without it nothing is linked.
inline uint32_t ChunkObjectKeys()  { return FourCC('N','G','G','K'); }

// PLANTS - the trees, bushes and grass of the alpha-tested geometry, and what each is.
// Written by Wallaby (Plants.cs). OPTIONAL: without them the renderer finds plants
// itself (Scene::FindPlants), which a human cannot correct.
//
// An object is a render chunk, and on stage 41 an alpha-tested one holds a whole stand
// of trees. A plant is a list of PARTS, each a contiguous run of one object's indices -
// a plant cut by a chunk edge has a part in each. The converter reorders an object's
// triangles by plant, so the draws are unchanged.
//
// 'NGGG' - tag names: uint32 count, then per tag uint32 byteLength, UTF-8 bytes, pad to 4.
//   "tree-card", "bush-card", "grass-card": old crossed-card vegetation, which the
//   renderer shades as one volume per plant and which is to be replaced by real models
//   later. Others are the tagger's classes ("bush", "fence", "sign", "backdrop", ...).
//   Unknown tags are carried, not rejected.
// 'NGGP' - PlantTableHeader, then plantCount PlantRecord, partCount PlantPart, then
//   uint32 tagRef[tagRefCount] (indices into NGGG); a plant's tags are
//   tagRef[firstTagRef .. firstTagRef + tagCount).
struct PlantTableHeader
{
    uint32_t plantCount;
    uint32_t partCount;
    uint32_t tagRefCount;
    uint32_t reserved;
};
struct PlantRecord
{
    float    bboxMin[3];      // the whole plant, every part, in the file's space
    float    bboxMax[3];
    uint32_t firstTagRef;
    uint32_t tagCount;
};
struct PlantPart
{
    uint32_t plant;
    uint32_t object;          // into NGGO
    uint32_t firstIndex;      // into NGGI, inside the object's range
    uint32_t indexCount;
};
static_assert(sizeof(PlantTableHeader) == 16, "PlantTableHeader layout - keep Plants.cs in step");
static_assert(sizeof(PlantRecord) == 32,      "PlantRecord layout - keep Plants.cs in step");
static_assert(sizeof(PlantPart) == 16,        "PlantPart layout - keep Plants.cs in step");

inline uint32_t ChunkTagNames()    { return FourCC('N','G','G','G'); }
inline uint32_t ChunkPlants()      { return FourCC('N','G','G','P'); }

// WEATHER PRESETS (design 5.0q, 5.0t). Inside the file, not beside it: the
// preset is part of an event's definition and must be covered by the same CRC
// (and encryption envelope) as the geometry, or a competitor could thin the fog
// with a text editor (design 3.6).
//
// Payload: WeatherChunkHeader, then `count` records of `recordSize` bytes. The
// FIRST record is the track's default.
//
// Binary like every other chunk: the runtime loads a track without a text
// parser. The "ngg-weather" JSON schema (core/weather.h) is the PROJECT and
// editor interchange form - it is what Wallaby's sidecar holds and what
// `--export-weather` writes - and the converter packs it into these records.
//
// Growing it: append fields to WeatherRecord and keep `version` at 1. A reader
// takes min(recordSize, sizeof(WeatherRecord)) bytes of each record and keeps
// defaults for the fields a shorter (older) record lacks; a longer (newer)
// record's tail is skipped. `version` changes only if an existing field's
// meaning does.
//
// `name` is the preset's identity, and for a preset the game should ever select
// it must be RBR's TrackSettings section name for that condition
// ("108M_crisp_clear", "04O_M_heavyfog_clear") - that is what RBRTM passes
// (design 5.0s). Other names are legal and the game never picks them.
#pragma pack(push, 4)
struct WeatherChunkHeader
{
    uint32_t version;         // kWeatherVersion
    uint32_t count;
    uint32_t recordSize;      // bytes per record, >= kWeatherRecordMinSize
    uint32_t reserved;
};

struct WeatherRecord
{
    char  name[64];           // UTF-8, NUL-terminated, unique within the file
    float sunDirection[3];    // the direction light TRAVELS (from the sun), unit length
    float sunIntensity;       // the model's units; 22 = clear day
    float turbidity;          // aerosol: 1 clean .. ~3 hazy
    float groundAlbedo;       // what the atmosphere sees below the horizon
    float groundColour[3];    // bounce tint onto shaded surfaces
    float cloudCover;         // 0 clear .. 1 overcast
    float fogVisibility;      // metres to 2 % contrast (Koschmieder); 0 = no fog
    float fogHeight;          // fog layer scale height, metres
    float rain;               // 0..1
    float snow;               // 0..1
    float exposure;           // exposure at reference brightness
};
#pragma pack(pop)

const uint32_t kWeatherVersion = 1;
// Everything through `exposure`: a record shorter than this is not v1.
const uint32_t kWeatherRecordMinSize = (uint32_t)sizeof(WeatherRecord);

static_assert(sizeof(WeatherChunkHeader) == 16, "WeatherChunkHeader layout");
static_assert(sizeof(WeatherRecord) == 124,     "WeatherRecord v1 layout - keep NggWriter.cs in step");

inline uint32_t ChunkWeather()     { return FourCC('N','G','G','W'); }

static_assert(sizeof(Object) == 16,   "Object layout");

} // namespace ngg
