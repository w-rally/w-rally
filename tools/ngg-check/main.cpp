// ngg-check: the .ngg writer and readers kept in step (docs/design.md decision 2).
//
//   ngg-check                      a made-up scene written, read back and compared; then loaded by the renderer.
//                                  Its model has a ray-tracing-only LOD (kModelLodRayOnly); then each rule about
//                                  that LOD broken in turn - the format library and the renderer must refuse each
//                                  (the renderer draws the cards instead)
//   ngg-check <file.ngg>...        another producer's files (Wallaby's): read, written again, read back and compared;
//                                  the renderer loads the original and the rewrite
//   ngg-check --lbs <file.lbs> [--surface dry|damp|wet] [--out <file.ngg>]
//                                  W-Rally's legacy importer: the .lbs to an .ngg, read back, loaded by the renderer
//   ngg-check --trees <file.ngg> [--out <file.ngg>]
//                                  a placeholder tree model (placeholder_tree.h) in place of every tree-card plant:
//                                  written, read back the same, loaded by the renderer, which reports its models,
//                                  placements and the replaced plants (the CTest checks the counts)
//   ngg-check --trees <file.ngg> --rt-lod 0|1|ray [--out <file.ngg>]
//                                  the LOD the ray tracer takes (1 when not given); "ray": a ray-tracing-only shell
//                                  as a fourth LOD. With "ray" it also renders frames at distances through every LOD
//                                  band and requires the same draws (main pass, cascades) as the same trees with LOD1
//                                  traced and no shell; the ray tracer's BLAS to be the shell's triangles; and that
//                                  file written as a version-1 NGGD to load
//
// "the renderer" is WRenderer.dll (bin/modules/renderer) on a hidden window - its own loader, not this library's
// reader, so a writer and the renderer that disagree fail here. Prints "ngg-check: all good" at the end, or the first
// thing that differs and exits 1.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <filesystem>

#include <cmath>

#include "assets/legacy_stage.h"
#include "placeholder_tree.h"
#include "ngg_scene.h"
#include "render/renderer.h"
#include "wrally/iface/renderer_edit.h"

namespace {

int Fail(const std::string& why)
{
    std::printf("ngg-check: FAILED - %s\n", why.c_str());
    return 1;
}

template <class T> bool SameVec(const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(T)));
}

// "" or the first field that differs (the version aside: a rewrite is always the current one).
std::string Compare(const ngg::SceneData& a, const ngg::SceneData& b)
{
    if (a.flags != b.flags) return "the flags";
    if (a.bakedScale != b.bakedScale) return "the baked scale";
    if (std::memcmp(a.bboxMin, b.bboxMin, sizeof a.bboxMin) || std::memcmp(a.bboxMax, b.bboxMax, sizeof a.bboxMax)) return "the box";
    if (!SameVec(a.vertices, b.vertices)) return "the vertices";
    if (!SameVec(a.indices, b.indices)) return "the indices";
    if (!SameVec(a.materials, b.materials)) return "the materials";
    if (!SameVec(a.objects, b.objects)) return "the objects";
    if (a.textures.size() != b.textures.size()) return "the texture count";
    for (size_t i = 0; i < a.textures.size(); ++i)
        if (a.textures[i].name != b.textures[i].name || a.textures[i].dds != b.textures[i].dds) return "texture " + std::to_string(i);
    if (a.hasSpawn != b.hasSpawn || (a.hasSpawn && std::memcmp(&a.spawn, &b.spawn, sizeof a.spawn))) return "the spawn point";
    if (a.objectKeys != b.objectKeys) return "the object keys";
    if (!SameVec(a.weather, b.weather)) return "the weather presets";
    if (a.tagNames != b.tagNames || a.plants != b.plants) return "the plants";
    if (a.variants != b.variants || a.variantMasks != b.variantMasks || a.variantHasSpawn != b.variantHasSpawn)
        return "the stage variants";
    if (a.variantOffsets != b.variantOffsets) return "a stage variant's offset";
    if (a.variantAliases != b.variantAliases) return "a stage variant's aliases";
    if (!SameVec(a.models.models, b.models.models) || !SameVec(a.models.lods, b.models.lods) ||
        !SameVec(a.models.submeshes, b.models.submeshes) || !SameVec(a.models.materials, b.models.materials) ||
        !SameVec(a.models.vertices, b.models.vertices) || !SameVec(a.models.indices, b.models.indices))
        return "the model library";
    if (!SameVec(a.placements, b.placements)) return "the placements";
    for (size_t k = 0; k < a.variantSpawns.size() && k < b.variantSpawns.size(); ++k)
        if (std::memcmp(&a.variantSpawns[k], &b.variantSpawns[k], sizeof(ngg::SpawnPoint))) return "a stage variant's car start";
    return "";
}

// Written, read back, compared. `out`: the bytes written.
std::string RoundTrip(const ngg::SceneData& s, std::vector<uint8_t>& out)
{
    if (std::string e = ngg::Write(s, out); !e.empty()) return "write: " + e;
    ngg::SceneData back;
    if (std::string e = ngg::Read(out.data(), out.size(), back); !e.empty()) return "read back: " + e;
    if (back.version != ngg::kFormatCurrent) return "read back: version " + std::to_string(back.version);
    if (std::string e = Compare(s, back); !e.empty()) return "read back: " + e + " differ";
    return "";
}

ngg::SceneData MadeUp()
{
    ngg::SceneData s;
    s.bakedScale = 1.5f;
    for (int i = 0; i < 6; ++i) {
        ngg::Vertex v{};
        v.pos[0] = float(i % 3) * 10.0f, v.pos[1] = float(i / 3), v.pos[2] = float(i) * 2.0f;
        v.normal[1] = 1.0f;
        v.uv0[0] = v.uv1[1] = float(i) * 0.25f;
        v.colour = 0x80ff8040u + uint32_t(i);
        s.vertices.push_back(v);
    }
    s.indices = {0, 1, 3, 1, 4, 3, 1, 2, 4, 2, 5, 4};
    const float lo[3] = {0, 0, 0}, hi[3] = {20, 1, 10};   // (the writer computes the box: what it must come to)
    std::memcpy(s.bboxMin, lo, sizeof lo);
    std::memcpy(s.bboxMax, hi, sizeof hi);
    ngg::Material ground = ngg::DefaultMaterial(), leaf = ngg::DefaultMaterial(), glass = ngg::DefaultMaterial();
    ground.baseColour = 0, ground.detail = 1;
    leaf.baseColour = 1, leaf.flags = ngg::kMaterialAlphaTest | ngg::kMaterialTwoSided;
    glass.baseColour = 1, glass.flags = ngg::kMaterialBlend | ngg::kMaterialTwoSided, glass.roughness = 0.05f;
    s.materials = {ground, leaf, glass};
    s.objects = {{0, 0, 0, 6}, {3, 1, 6, 6}, {2, 2, 0, 3}};
    s.objectKeys = {-1, 42, -1};
    // A reversed stage in the file too (ngg_format.h: names map-qualified then): the ground in both, the leaf only
    // in 42_M, the glass only in 45_N.
    s.variants = {"42_M", "45_N"};
    s.variantMasks = {3, 1, 2};
    // Each its own start: the reversed stage starts where the other ends.
    s.variantHasSpawn = {1, 1};
    s.variantSpawns = {{{1, 2, 3}, {0, 0, 1}}, {{900, 5, -40}, {0, 0, -1}}};
    // and the reversed stage built at another origin (RBR's 55 is 51 moved)
    s.variantOffsets = {{0, 0, 0}, {-1193.0f, 664.25f, 1793.5f}};
    // and the RBR stage names each answers to (siblings collapsed into one stage)
    s.variantAliases = {{0, "track-42_M"}, {0, "track-42_O"}, {1, "track-45_N"}, {1, "track-45_O"}};
    // Two 4x4 DXT1 textures: a header and one block each.
    for (const char* name : {"ground.dds", "leaf.dds"}) {
        std::vector<uint8_t> dds(128 + 8, 0);
        std::memcpy(dds.data(), "DDS ", 4);
        const uint32_t header[] = {124, 0x1007, 4, 4, 8, 0, 1};
        std::memcpy(dds.data() + 4, header, sizeof header);
        const uint32_t pf[] = {32, 4};
        std::memcpy(dds.data() + 76, pf, sizeof pf);
        std::memcpy(dds.data() + 84, "DXT1", 4);
        dds[128] = 0xff, dds[129] = 0xff;
        s.textures.push_back({name, dds});
    }
    s.hasSpawn = true;
    s.spawn = {{1, 2, 3}, {0, 0, 1}};
    ngg::WeatherRecord w{};
    std::snprintf(w.name, sizeof w.name, "41M_crisp_clear");
    w.sunDirection[1] = -1, w.sunIntensity = 22, w.turbidity = 2, w.exposure = 1;
    s.weather = {w};
    // An instanced model (NGGD): a two-LOD marker, a triangle near and a billboard far, placed twice (NGGE) - once in
    // each stage variant.
    ngg::ModelLibrary& lib = s.models;
    lib.materials = {leaf, ground};
    for (int k = 0; k < 4; ++k) {
        ngg::Vertex v{};
        v.pos[0] = float(k & 1), v.pos[1] = float(k >> 1) * 2.0f;
        v.normal[2] = 1.0f;
        v.uv0[0] = float(k & 1), v.uv0[1] = float(k >> 1);
        v.colour = 0xFFFFFFFFu;
        lib.vertices.push_back(v);
    }
    lib.indices = {0, 1, 2, 0, 2, 1};
    lib.submeshes = {{1, 0, 3, 0, 3}, {0, 3, 3, 1, 3}};
    // ... and the triangle again as its ray-tracing-only LOD (never drawn; the ray tracer's).
    lib.lods = {{0, 1, 40.0f, 0}, {1, 1, 400.0f, ngg::kModelLodBillboard}, {0, 1, 0.0f, ngg::kModelLodRayOnly}};
    ngg::ModelRecord m{};
    std::snprintf(m.name, sizeof m.name, "marker");
    m.lodCount = 3, m.rtLod = 2, m.bboxMax[0] = 1, m.bboxMax[1] = 2, m.fadeWidth = 4;
    lib.models = {m};
    ngg::Placement p{};
    p.model = 0, p.plant = ngg::kNoPlant, p.position[0] = 5, p.yaw = 0.5f, p.scaleXZ = 1, p.scaleY = 1.5f, p.tint = 0xFFFFFFFFu;
    p.variants = 1;
    s.placements = {p};
    p.position[2] = 8, p.variants = 2, p.tint = 0xFF80C0FFu;
    s.placements.push_back(p);
    return s;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// An NGGD payload as the writer lays it out, with any version in its header (a malformed one, or version 1).
std::vector<uint8_t> ModelPayload(const ngg::ModelLibrary& m, uint32_t version)
{
    std::vector<uint8_t> p;
    auto put = [&](const void* d, size_t n) { p.insert(p.end(), (const uint8_t*)d, (const uint8_t*)d + n); };
    const ngg::ModelChunkHeader h{version, uint32_t(m.models.size()), uint32_t(m.lods.size()), uint32_t(m.submeshes.size()),
                                  uint32_t(m.materials.size()), uint32_t(m.vertices.size()), uint32_t(m.indices.size()), 0};
    put(&h, sizeof h);
    put(m.models.data(), m.models.size() * sizeof(ngg::ModelRecord));
    put(m.lods.data(), m.lods.size() * sizeof(ngg::ModelLod));
    put(m.submeshes.data(), m.submeshes.size() * sizeof(ngg::ModelSubmesh));
    put(m.materials.data(), m.materials.size() * sizeof(ngg::Material));
    put(m.vertices.data(), m.vertices.size() * sizeof(ngg::Vertex));
    put(m.indices.data(), m.indices.size() * 4);
    return p;
}

// The file with its NGGD chunk's payload replaced (what no writer would write: the writer checks). Empty: no NGGD.
std::vector<uint8_t> WithModelChunk(const std::vector<uint8_t>& file, const std::vector<uint8_t>& payload)
{
    ngg::FileHeader fh{};
    if (file.size() < sizeof fh) return {};
    std::memcpy(&fh, file.data(), sizeof fh);
    size_t at = sizeof fh;
    for (uint32_t i = 0; i < fh.chunkCount && at + sizeof(ngg::ChunkHeader) <= file.size(); ++i) {
        ngg::ChunkHeader ch{};
        std::memcpy(&ch, file.data() + at, sizeof ch);
        const size_t end = at + sizeof ch + ch.byteSize + (4 - ch.byteSize % 4) % 4;
        if (ch.fourcc == ngg::ChunkModels()) {
            std::vector<uint8_t> out(file.begin(), file.begin() + ptrdiff_t(at));
            const ngg::ChunkHeader nh{ch.fourcc, uint32_t(payload.size())};
            out.insert(out.end(), (const uint8_t*)&nh, (const uint8_t*)&nh + sizeof nh);
            out.insert(out.end(), payload.begin(), payload.end());
            out.resize(out.size() + (4 - payload.size() % 4) % 4, 0);
            out.insert(out.end(), file.begin() + ptrdiff_t(std::min(end, file.size())), file.end());
            return out;
        }
        at = end;
    }
    return {};
}

// The made-up scene's model library broken one rule of the ray-only LOD at a time (ngg_format.h kModelLodRayOnly):
// the format library and the renderer must each refuse it, the renderer then drawing the cards. `lib`: the valid one.
struct BadLibrary {
    const char* rule;
    ngg::ModelLibrary lib;
    uint32_t version;
    const char* formatSays;     // in ngg::Read's error
    const char* rendererSays;   // in the renderer's log
};
std::vector<BadLibrary> RayOnlyRulesBroken(const ngg::ModelLibrary& valid)
{
    const ngg::ModelLod lod0 = valid.lods[0], board = valid.lods[1], ray = valid.lods[2];
    auto with = [&](std::vector<ngg::ModelLod> lods, uint32_t rtLod) {
        ngg::ModelLibrary l = valid;
        l.lods = lods;
        l.models[0].lodCount = uint32_t(lods.size());
        l.models[0].rtLod = rtLod;
        return l;
    };
    ngg::ModelLod rayBoard = ray, rayFar = ray;
    rayBoard.flags |= ngg::kModelLodBillboard;
    rayFar.maxDistance = 500.0f;
    return {
        {"a ray-only LOD that is not the last", with({lod0, ray, board}, 1), 2, "is not its last", "is not its last"},
        {"two ray-only LODs", with({lod0, ray, ray}, 2), 2, "two ray-only LODs", "two ray-only LODs"},
        {"a ray-only LOD that rtLod does not name", with({lod0, board, ray}, 0), 2, "rtLod does not name",
         "rtLod does not name"},
        {"a ray-only LOD that is also a billboard", with({lod0, board, rayBoard}, 2), 2, "also a billboard",
         "also a billboard"},
        {"a ray-only LOD with a distance", with({lod0, board, rayFar}, 2), 2, "has a distance", "has a distance"},
        {"a ray-only LOD with no drawn LOD before it", with({ray}, 0), 2, "no drawn LOD before",
         "no drawn LOD before"},
        {"a ray-only LOD in a version-1 chunk", valid, 1, "version 1 with a ray-only LOD",
         "version-1 LOD is ray-tracing only"},
    };
}

// The camera (row vectors, left-handed, as the renderer's iface takes it).
void LookAtLH(float m[16], const float eye[3], const float at[3])
{
    float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& c : z) c /= zl;
    float x[3] = {z[2], 0.0f, -z[0]};   // up (0,1,0) x z
    const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    for (float& c : x) c /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    auto dot = [&](const float* a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
    const float r[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x), -dot(y), -dot(z), 1};
    std::memcpy(m, r, sizeof r);
}
void PerspectiveFovLH(float m[16], float fovY, float aspect, float zn, float zf)
{
    const float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect;
    const float r[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    std::memcpy(m, r, sizeof r);
}

// The renderer module on a hidden window, for its own loader.
struct Gpu {
    HWND window = nullptr;
    wrally::render::Renderer renderer;
    std::string error;
    std::string log;   // the renderer's log of the last Load

    Gpu()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ngg-check";
        RegisterClassW(&wc);
        window = CreateWindowExW(0, wc.lpszClassName, L"ngg-check", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
                                 wc.hInstance, nullptr);
        wrally::iface::RenderSettings rs{};
        rs.vsync = 0, rs.quality = 1, rs.taa = 1, rs.shadows = 1, rs.autoExposure = 1, rs.renderScale = 1;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        error = renderer.Open(std::filesystem::path(exe).parent_path() / "modules" / "renderer", "wrenderer", window, 320, 240, rs);
    }
    // "" or why the renderer would not take them.
    std::string Load(const std::vector<uint8_t>& bytes, const ngg::SceneData& s)
    {
        if (!error.empty()) return "the renderer: " + error;
        const wrally::iface::SceneHandle scene = renderer.LoadScene(bytes.data(), bytes.size());
        log = renderer.TakeLog();
        if (!scene) return "the renderer refused it: " + renderer.Error();
        uint32_t keyed = 0, keys = 0;
        renderer.Api()->sceneKeyStats(renderer.Instance(), scene, &keyed, &keys);
        const uint32_t presets = renderer.Api()->weatherPresetCount(renderer.Instance(), scene);
        renderer.Api()->unloadScene(renderer.Instance(), scene);
        size_t wantKeyed = 0;
        for (int32_t k : s.objectKeys) wantKeyed += k >= 0;
        if (keyed != wantKeyed) return "the renderer keyed " + std::to_string(keyed) + " objects, the file " + std::to_string(wantKeyed);
        if (presets != s.weather.size()) return "the renderer has " + std::to_string(presets) + " weather presets, the file " + std::to_string(s.weather.size());
        return "";
    }

    // Loaded, and a frame drawn looking at `at` from each distance in `dist` (horizontally), ray tracing off so that
    // the sun's cascades are drawn: what the renderer's model statistics (editor table) say each frame drew. Then
    // ray tracing on for a frame from the last distance (`rs` its statistics). `log`: the renderer's log over it all.
    std::string Sweep(const std::vector<uint8_t>& bytes, const float at[3], const std::vector<float>& dist,
                      std::vector<wrally::iface::EditModelStats>& stats, wrally::iface::RenderStats& rs, std::string& sweepLog)
    {
        using namespace wrally::iface;
        stats.clear();
        if (!error.empty()) return "the renderer: " + error;
        RenderSettings set{};
        set.vsync = 0, set.quality = 1, set.taa = 1, set.shadows = 1, set.autoExposure = 1, set.renderScale = 1, set.rayTracing = 0;
        renderer.ApplySettings(set);
        const RendererEditApi* edit = nullptr;
        if (renderer.Api()->editApi(renderer.Instance(), kRendererEditVer, &edit) != Status::Ok || !edit)
            return "the renderer has no editor table";
        const SceneHandle scene = renderer.LoadScene(bytes.data(), bytes.size());
        sweepLog = renderer.TakeLog();
        if (!scene) return "the renderer refused it: " + renderer.Error();
        FrameDesc f{};
        f.scene = scene;
        f.weatherPreset = 0;
        for (float d : dist) {
            const float eye[3] = {at[0] + d, at[1], at[2]};
            LookAtLH(f.camera.view, eye, at);
            PerspectiveFovLH(f.camera.proj, 1.0f, 320.0f / 240.0f, 0.5f, 30000.0f);
            f.camera.nearZ = 0.5f, f.camera.farZ = 30000.0f, f.camera.fovY = 1.0f, f.camera.cut = 1;
            renderer.RenderFrame(f);
            renderer.RenderFrame(f);   // (the statistics describe the last frame drawn)
            EditModelStats st{};
            edit->modelStats(renderer.Instance(), &st);
            stats.push_back(st);
        }
        set.rayTracing = 1;
        renderer.ApplySettings(set);
        renderer.RenderFrame(f);
        renderer.RenderFrame(f);
        rs = renderer.Stats();
        set.rayTracing = 0;
        renderer.ApplySettings(set);
        sweepLog += renderer.TakeLog();
        renderer.Api()->unloadScene(renderer.Instance(), scene);
        return "";
    }
};

std::string Arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
    return "";
}

// --trees --rt-lod ray: `bytes` the stage as it was, `s` / `out` with the placeholder trees and their ray-only shell.
std::string RayOnlyTrees(Gpu& gpu, const std::vector<uint8_t>& bytes, const ngg::SceneData& s, const std::vector<uint8_t>& out,
                         const PlaceholderReport& rep)
{
    // The same trees with LOD1 ray traced and no shell: what drawing must equal.
    ngg::SceneData ref;
    if (std::string e = ngg::Read(bytes.data(), bytes.size(), ref); !e.empty()) return e;
    PlaceholderReport refRep;
    if (std::string e = AddPlaceholderTrees(ref, refRep, 1); !e.empty()) return e;
    std::vector<uint8_t> refBytes;
    if (std::string e = ngg::Write(ref, refBytes); !e.empty()) return "without the shell: " + e;

    // That file as a version-1 NGGD (the layout is the same): read, the same, and loaded.
    const std::vector<uint8_t> v1 = WithModelChunk(refBytes, ModelPayload(ref.models, 1));
    ngg::SceneData v1Back;
    if (std::string e = ngg::Read(v1.data(), v1.size(), v1Back); !e.empty()) return "version 1: " + e;
    if (std::string e = Compare(ref, v1Back); !e.empty()) return "version 1: " + e + " differ";
    if (gpu.error.empty()) {
        if (std::string e = gpu.Load(v1, ref); !e.empty()) return "version 1: " + e;
        if (gpu.log.find("scene: models - 1 model(s): 3 LODs (0 ray tracing only)") == std::string::npos ||
            gpu.log.find("scene: models replace") == std::string::npos)
            return "version 1: the renderer did not load its models:\n" + gpu.log;
    }
    std::printf("a version-1 NGGD (the trees, LOD1 ray traced): read the same, loaded by the renderer\n");
    if (!gpu.error.empty()) return "";

    // Frames looking at a tree from inside each LOD, each crossfade band and past the end (the model's distances at
    // scale 1: 50 LOD0, 150 LOD1, 900 the billboard, a 6 m band before each), scaled by its height: the tree nearest
    // the model's own size.
    size_t pick = 0;
    for (size_t k = 1; k < s.placements.size(); ++k)
        if (std::fabs(s.placements[k].scaleY - 1.0f) < std::fabs(s.placements[pick].scaleY - 1.0f)) pick = k;
    const ngg::Placement& p0 = s.placements[pick];
    const float sy = p0.scaleY;
    const float at[3] = {p0.position[0], p0.position[1] + 5.0f * sy, p0.position[2]};
    std::vector<float> dist;
    for (float d : {5.0f, 25.0f, 46.0f, 49.0f, 52.0f, 100.0f, 146.0f, 149.0f, 155.0f, 400.0f, 895.0f, 899.0f, 905.0f, 1500.0f})
        dist.push_back(d * sy);
    std::vector<wrally::iface::EditModelStats> withShell, without;
    wrally::iface::RenderStats rsShell{}, rsRef{};
    std::string logShell, logRef;
    if (std::string e = gpu.Sweep(out, at, dist, withShell, rsShell, logShell); !e.empty()) return e;
    if (std::string e = gpu.Sweep(refBytes, at, dist, without, rsRef, logRef); !e.empty()) return "without the shell: " + e;
    uint32_t visible = 0, shadow = 0, fading = 0, boards = 0;
    for (size_t k = 0; k < dist.size(); ++k) {
        const wrally::iface::EditModelStats &a = withShell[k], &b = without[k];
        char row[256];
        std::snprintf(row, sizeof row, "%7.1f m: %u visible (LOD0 %u, LOD1 %u, LOD2 %u, billboard %u, %u fading), %u shadow-map, %u draws",
                      dist[k], a.visible, a.lod0, a.lod1, a.lod2, a.billboard, a.fading, a.shadowInstances, a.draws);
        std::printf("  %s\n", row);
        // (the shell is LOD index 3, not a billboard: drawn, it would count in lod2 - the placeholder has no LOD2)
        if (a.lod2) return std::string("the ray-only LOD was drawn at ") + row;
        if (a.placements != b.placements || a.visible != b.visible || a.lod0 != b.lod0 || a.lod1 != b.lod1 || a.lod2 != b.lod2 ||
            a.billboard != b.billboard || a.fading != b.fading || a.shadowInstances != b.shadowInstances || a.draws != b.draws)
            return std::string("drawn otherwise than without the shell at ") + row;
        visible += a.visible, shadow += a.shadowInstances, fading += a.fading, boards += a.billboard;
    }
    if (!visible || !shadow || !fading || !boards) return "the sweep drew no trees, no shadows, no crossfade or no billboard";
    std::printf("drawn the same as without the shell at %zu distances, in the main pass and the cascades; never the shell\n",
                dist.size());

    // The ray tracer: its BLAS from the shell (and from LOD1 without it).
    if (!rsShell.rtActive) return std::string("ray tracing is not active (") + rsShell.rtFallback + "): the BLAS is not checked";
    const uint32_t shellTris = rep.trianglesPerLod.back(), lod1Tris = rep.trianglesPerLod[1];
    const std::string wantShell = "scene: models ray traced - 2 BLAS mesh(es), " + std::to_string(shellTris) +
                                  " triangle(s) (1 model(s) from a ray-only LOD)";
    const std::string wantRef = "scene: models ray traced - 2 BLAS mesh(es), " + std::to_string(lod1Tris) +
                                " triangle(s) (0 model(s) from a ray-only LOD)";
    if (logShell.find(wantShell) == std::string::npos) return "the ray tracer did not build the shell (want \"" + wantShell + "\")";
    if (logRef.find(wantRef) == std::string::npos) return "without the shell the ray tracer did not build LOD1";
    std::printf("the ray tracer's BLAS: the shell's %u triangles (LOD1's %u without it)\n", shellTris, lod1Tris);
    return "";
}

} // namespace

int main(int argc, char** argv)
{
    Gpu gpu;
    if (!gpu.error.empty()) std::printf("ngg-check: no renderer (%s) - the renderer's half is skipped\n", gpu.error.c_str());
    auto load = [&](const std::vector<uint8_t>& bytes, const ngg::SceneData& s) {
        return gpu.error.empty() ? gpu.Load(bytes, s) : std::string();
    };

    if (const std::string lbs = Arg(argc, argv, "--lbs"); !lbs.empty()) {
        const std::string surface = Arg(argc, argv, "--surface");
        wrally::assets::LegacyImport imp;
        const DWORD t0 = GetTickCount();
        if (std::string e = wrally::assets::ImportLegacyStage(lbs, surface == "wet" ? 2 : surface == "damp" ? 1 : 0, imp); !e.empty())
            return Fail(e);
        std::printf("%s(%.1f s)\n", imp.log.c_str(), (GetTickCount() - t0) / 1000.0);
        ngg::SceneData s;
        if (std::string e = ngg::Read(imp.ngg.data(), imp.ngg.size(), s); !e.empty()) return Fail("the import read back: " + e);
        if (std::string e = load(imp.ngg, s); !e.empty()) return Fail(lbs + ": " + e);
        if (const std::string out = Arg(argc, argv, "--out"); !out.empty()) {
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(imp.ngg.data()), std::streamsize(imp.ngg.size()));
            std::printf("written %s (%zu bytes)\n", out.c_str(), imp.ngg.size());
        }
        std::printf("ngg-check: all good\n");
        return 0;
    }

    if (const std::string in = Arg(argc, argv, "--trees"); !in.empty()) {
        const std::vector<uint8_t> bytes = ReadFile(in);
        ngg::SceneData s;
        if (std::string e = ngg::Read(bytes.data(), bytes.size(), s); !e.empty()) return Fail(in + ": " + e);
        PlaceholderReport rep;
        const std::string rtLodArg = Arg(argc, argv, "--rt-lod");
        const bool rayShell = rtLodArg == "ray";
        const uint32_t rtLod = rayShell ? kPlaceholderRtShell : rtLodArg.empty() ? 1u : uint32_t(std::atoi(rtLodArg.c_str()));
        if (std::string e = AddPlaceholderTrees(s, rep, rtLod); !e.empty())
            return Fail(in + ": " + e);
        std::printf("placeholder trees: %u placement(s) replacing tree-card plants (%u too small, skipped), %.1f-%.1f m tall; "
                    "LOD triangles", rep.placements, rep.skipped, rep.minHeight, rep.maxHeight);
        for (uint32_t t : rep.trianglesPerLod) std::printf(" %u", t);
        std::printf("\n");
        std::vector<uint8_t> out;
        if (std::string e = RoundTrip(s, out); !e.empty()) return Fail("with trees: " + e);
        std::printf("with trees: written, read back the same (%zu bytes)\n", out.size());
        if (gpu.error.empty()) {
            if (std::string e = gpu.Load(out, s); !e.empty()) return Fail("with trees: " + e);
            // What the renderer says it loaded: the models' and the replaced plants' lines.
            size_t at = 0;
            while (at < gpu.log.size()) {
                const size_t end = gpu.log.find('\n', at);
                const std::string line = gpu.log.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (line.find("scene: models") != std::string::npos) std::printf("renderer: %s\n", line.c_str());
                at = end == std::string::npos ? gpu.log.size() : end + 1;
            }
        }
        if (rayShell) {
            if (std::string e = RayOnlyTrees(gpu, bytes, s, out, rep); !e.empty()) return Fail("with a ray-only LOD: " + e);
        }
        if (const std::string o = Arg(argc, argv, "--out"); !o.empty()) {
            std::ofstream(o, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
            std::printf("written %s\n", o.c_str());
        }
        std::printf("ngg-check: all good\n");
        return 0;
    }

    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
    if (files.empty()) {
        const ngg::SceneData s = MadeUp();
        std::vector<uint8_t> bytes;
        if (std::string e = RoundTrip(s, bytes); !e.empty()) return Fail("the made-up scene: " + e);
        if (std::string e = load(bytes, s); !e.empty()) return Fail("the made-up scene: " + e);
        std::printf("the made-up scene: written, read back the same (%zu bytes)%s\n", bytes.size(),
                    gpu.error.empty() ? ", loaded by the renderer" : "");
        if (gpu.error.empty() && gpu.log.find("scene: models - 1 model(s): 3 LODs (1 ray tracing only)") == std::string::npos)
            return Fail("the made-up scene: the renderer did not count its ray-only LOD");
        // Each rule of the ray-only LOD broken: refused by the format library (CheckModels, then Read) and by the
        // renderer, which loads the scene and draws the cards (no models).
        for (const BadLibrary& b : RayOnlyRulesBroken(s.models)) {
            if (b.version == ngg::kModelVersion) {
                const std::string e = ngg::CheckModels(b.lib, s.textures.size());
                if (e.find(b.formatSays) == std::string::npos) return Fail(std::string(b.rule) + ": CheckModels said \"" + e + "\"");
            }
            const std::vector<uint8_t> bad = WithModelChunk(bytes, ModelPayload(b.lib, b.version));
            if (bad.empty()) return Fail("the made-up scene has no NGGD");
            ngg::SceneData back;
            const std::string e = ngg::Read(bad.data(), bad.size(), back);
            if (e.find(b.formatSays) == std::string::npos) return Fail(std::string(b.rule) + ": Read said \"" + e + "\"");
            if (gpu.error.empty()) {
                if (std::string le = gpu.Load(bad, s); !le.empty()) return Fail(std::string(b.rule) + ": " + le);
                if (gpu.log.find(b.rendererSays) == std::string::npos || gpu.log.find("no models, the cards drawn") == std::string::npos ||
                    gpu.log.find("scene: models -") != std::string::npos)
                    return Fail(std::string(b.rule) + ": the renderer did not refuse it:\n" + gpu.log);
            }
            std::printf("refused: %s (%s)\n", b.rule, e.c_str());
        }
    }
    for (const std::string& f : files) {
        const std::vector<uint8_t> bytes = ReadFile(f);
        ngg::SceneData s;
        if (std::string e = ngg::Read(bytes.data(), bytes.size(), s); !e.empty()) return Fail(f + ": " + e);
        std::vector<uint8_t> again;
        if (std::string e = RoundTrip(s, again); !e.empty()) return Fail(f + ": " + e);
        if (std::string e = load(bytes, s); !e.empty()) return Fail(f + ": " + e);
        if (std::string e = load(again, s); !e.empty()) return Fail(f + " rewritten: " + e);
        std::printf("%s: v%u, %zu objects, %zu triangles, %zu textures, %zu weather presets: read, rewritten the same%s\n",
                    f.c_str(), s.version, s.objects.size(), s.indices.size() / 3, s.textures.size(), s.weather.size(),
                    gpu.error.empty() ? ", both loaded by the renderer" : "");
    }
    std::printf("ngg-check: all good\n");
    return 0;
}
