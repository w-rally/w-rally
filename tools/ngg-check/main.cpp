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
#include <wincodec.h>
#include <wrl/client.h>

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
    w.windDirectionDeg = 90.0f, w.windSpeed = 6.0f, w.gustiness = 0.4f;   // (NGGW's appended wind)
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
        v.uv1[0] = float(k) * 0.25f, v.uv1[1] = 0.375f + float(k * 25);   // the wind's weights (kModelWind)
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
    m.flags = ngg::kModelWind, m.windBend = 0.4f;   // it sways (NGGD version 3)
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

// The file with a chunk's payload replaced (what no writer would write: the writer checks). Empty: no such chunk.
std::vector<uint8_t> WithChunk(const std::vector<uint8_t>& file, uint32_t fourcc, const std::vector<uint8_t>& payload)
{
    ngg::FileHeader fh{};
    if (file.size() < sizeof fh) return {};
    std::memcpy(&fh, file.data(), sizeof fh);
    size_t at = sizeof fh;
    for (uint32_t i = 0; i < fh.chunkCount && at + sizeof(ngg::ChunkHeader) <= file.size(); ++i) {
        ngg::ChunkHeader ch{};
        std::memcpy(&ch, file.data() + at, sizeof ch);
        const size_t end = at + sizeof ch + ch.byteSize + (4 - ch.byteSize % 4) % 4;
        if (ch.fourcc == fourcc) {
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

std::vector<uint8_t> WithModelChunk(const std::vector<uint8_t>& file, const std::vector<uint8_t>& payload)
{
    return WithChunk(file, ngg::ChunkModels(), payload);
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
        {"a ray-only LOD that is not the last", with({lod0, ray, board}, 1), ngg::kModelVersion, "is not its last", "is not its last"},
        {"two ray-only LODs", with({lod0, ray, ray}, 2), ngg::kModelVersion, "two ray-only LODs", "two ray-only LODs"},
        {"a ray-only LOD that rtLod does not name", with({lod0, board, ray}, 0), ngg::kModelVersion, "rtLod does not name",
         "rtLod does not name"},
        {"a ray-only LOD that is also a billboard", with({lod0, board, rayBoard}, 2), ngg::kModelVersion, "also a billboard",
         "also a billboard"},
        {"a ray-only LOD with a distance", with({lod0, board, rayFar}, 2), ngg::kModelVersion, "has a distance", "has a distance"},
        {"a ray-only LOD with no drawn LOD before it", with({ray}, 0), ngg::kModelVersion, "no drawn LOD before",
         "no drawn LOD before"},
        {"a ray-only LOD in a version-1 chunk", valid, 1, "version 1 with a ray-only LOD",
         "version-1 LOD is ray-tracing only"},
    };
}

// The same for the wind (ngg_format.h kModelWind, NGGD version 3): `valid` has it.
std::vector<BadLibrary> WindRulesBroken(const ngg::ModelLibrary& valid)
{
    auto bend = [&](float b) { ngg::ModelLibrary l = valid; l.models[0].windBend = b; return l; };
    ngg::ModelLibrary noFlag = valid, unknown = valid, weight = valid, phase = valid;
    noFlag.models[0].flags = 0;
    unknown.models[0].flags |= 1u << 5;
    weight.vertices[0].uv1[0] = 1.5f;       // (vertex 0 is LOD0's only: the billboard's are not read)
    phase.vertices[0].uv1[1] = 101.0f;
    return {
        {"a wind model with a windBend of 0", bend(0.0f), ngg::kModelVersion, "windBend not in (0, 10]", "windBend is not in (0, 10]"},
        {"a wind model with a negative windBend", bend(-0.3f), ngg::kModelVersion, "windBend not in (0, 10]", "windBend is not in (0, 10]"},
        {"a wind model with a windBend over 10 m", bend(12.0f), ngg::kModelVersion, "windBend not in (0, 10]", "windBend is not in (0, 10]"},
        {"a wind model with a NaN windBend", bend(std::nanf("")), ngg::kModelVersion, "windBend not in (0, 10]", "windBend is not in (0, 10]"},
        {"a windBend without the wind flag", noFlag, ngg::kModelVersion, "a windBend without wind", "a windBend without wind"},
        {"a model with an unknown flag", unknown, ngg::kModelVersion, "unknown flags", "unknown flags"},
        {"a wind model with a branch weight over 1", weight, ngg::kModelVersion, "wind weights (uv1) out of range",
         "vertex weights (uv1) are out of range"},
        {"a wind model with a flutter over 100", phase, ngg::kModelVersion, "wind weights (uv1) out of range",
         "vertex weights (uv1) are out of range"},
        {"wind in a version-2 chunk", valid, 2, "NGGD version 2 with wind", "a version-1/2 model with wind"},
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
    wrally::iface::WeatherDesc preset0{};   // the last Load's weather preset 0, as the renderer hands it out

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
        preset0 = {};
        char name[64];
        if (presets) renderer.Api()->weatherPreset(renderer.Instance(), scene, 0, &preset0, name, sizeof name);
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
                         const PlaceholderReport& rep, bool wind)
{
    // The same trees with LOD1 ray traced and no shell: what drawing must equal (with the wind if they have it: a wind
    // model's culling box is wider).
    ngg::SceneData ref;
    if (std::string e = ngg::Read(bytes.data(), bytes.size(), ref); !e.empty()) return e;
    PlaceholderReport refRep;
    if (std::string e = AddPlaceholderTrees(ref, refRep, 1, wind); !e.empty()) return e;
    std::vector<uint8_t> refBytes;
    if (std::string e = ngg::Write(ref, refBytes); !e.empty()) return "without the shell: " + e;

    // That file as a version-1 NGGD (the layout is the same; without the wind, which version 1 cannot hold): read, the
    // same, and loaded.
    ngg::SceneData refV1 = ref;
    refV1.models.models[0].flags = 0, refV1.models.models[0].windBend = 0.0f;
    std::vector<uint8_t> refV1Bytes;
    if (std::string e = ngg::Write(refV1, refV1Bytes); !e.empty()) return "version 1: " + e;
    const std::vector<uint8_t> v1 = WithModelChunk(refV1Bytes, ModelPayload(refV1.models, 1));
    ngg::SceneData v1Back;
    if (std::string e = ngg::Read(v1.data(), v1.size(), v1Back); !e.empty()) return "version 1: " + e;
    if (std::string e = Compare(refV1, v1Back); !e.empty()) return "version 1: " + e + " differ";
    if (gpu.error.empty()) {
        if (std::string e = gpu.Load(v1, refV1); !e.empty()) return "version 1: " + e;
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

// A captured frame read back (WIC): 24-bit BGR rows, top first.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgr;
    const uint8_t* at(int x, int y) const { return bgr.data() + (size_t(y) * w + x) * 3; }
    bool same(const Image& o, int x, int y) const { return !std::memcmp(at(x, y), o.at(x, y), 3); }
};

bool ReadPng(const std::string& path, Image& img)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
    const std::wstring w = std::filesystem::path(path).wstring();
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT cw = 0, ch = 0;
    conv->GetSize(&cw, &ch);
    img.w = int(cw), img.h = int(ch);
    img.bgr.resize(size_t(cw) * ch * 3);
    return SUCCEEDED(conv->CopyPixels(nullptr, cw * 3, UINT(img.bgr.size()), img.bgr.data()));
}

// Pixels that differ between two frames (in a rectangle of rows, all columns).
size_t Differ(const Image& a, const Image& b, int y0 = 0, int y1 = 1 << 30)
{
    size_t n = 0;
    for (int y = (std::max)(0, y0); y < (std::min)(a.h, y1); ++y)
        for (int x = 0; x < a.w; ++x) n += a.same(b, x, y) ? 0 : 1;
    return n;
}

// The renderer's view of a scene for the wind's checks: TAA, auto exposure and ray tracing off (the frames exact, the
// sun's cascades drawn), the scene's weather preset 0 handed in as a WeatherDesc with the wind set, frames captured.
struct WindSession {
    Gpu& gpu;
    wrally::iface::SceneHandle scene = 0;
    const wrally::iface::RendererEditApi* edit = nullptr;
    wrally::iface::WeatherDesc weather{};
    std::string dir;   // where the captures go
    int shot = 0;
    float windDeg = 90.0f;   // where the wind blows: towards +x (the near views look along -x)

    WindSession(Gpu& g, const std::string& folder) : gpu(g), dir(folder) {}
    ~WindSession() { Close(); }
    void Close()
    {
        if (scene) gpu.renderer.Api()->unloadScene(gpu.renderer.Instance(), scene);
        scene = 0;
    }

    std::string Open(const std::vector<uint8_t>& bytes)
    {
        using namespace wrally::iface;
        RenderSettings set{};
        set.vsync = 0, set.quality = 1, set.taa = 0, set.shadows = 1, set.autoExposure = 0, set.renderScale = 1, set.rayTracing = 0;
        gpu.renderer.ApplySettings(set);
        if (gpu.renderer.Api()->editApi(gpu.renderer.Instance(), kRendererEditVer, &edit) != Status::Ok || !edit)
            return "the renderer has no editor table";
        scene = gpu.renderer.LoadScene(bytes.data(), bytes.size());
        gpu.log = gpu.renderer.TakeLog();
        if (!scene) return "the renderer refused it: " + gpu.renderer.Error();
        char name[64];
        if (gpu.renderer.Api()->weatherPreset(gpu.renderer.Instance(), scene, 0, &weather, name, sizeof name) != Status::Ok) {
            // No NGGW: the format's default record (the renderer's default sky, a morning sun).
            const ngg::WeatherRecord d = ngg::DefaultWeatherRecord();
            for (int k = 0; k < 3; ++k) weather.sunDirection[k] = d.sunDirection[k], weather.groundColour[k] = d.groundColour[k];
            weather.sunIntensity = d.sunIntensity, weather.turbidity = d.turbidity, weather.groundAlbedo = d.groundAlbedo;
            weather.cloudCover = d.cloudCover, weather.fogVisibility = d.fogVisibility, weather.fogHeight = d.fogHeight;
            weather.exposure = d.exposure;
        }
        return "";
    }
    // Only the placement nearest `p` left (the editor's removePlacements).
    void KeepOnly(const float p[3])
    {
        const uint32_t n = edit->placementCount(gpu.renderer.Instance());
        std::vector<wrally::iface::EditPlacement> all(n);
        edit->getPlacements(gpu.renderer.Instance(), 0, n, all.data());
        uint32_t keep = 0;
        float best = 1e30f;
        for (uint32_t k = 0; k < n; ++k) {
            const float d = std::fabs(all[k].position[0] - p[0]) + std::fabs(all[k].position[2] - p[2]);
            if (d < best) best = d, keep = k;
        }
        std::vector<uint32_t> gone;
        for (uint32_t k = 0; k < n; ++k)
            if (k != keep) gone.push_back(k);
        edit->removePlacements(gpu.renderer.Instance(), gone.data(), uint32_t(gone.size()));
    }
    void RemoveAll()
    {
        const uint32_t n = edit->placementCount(gpu.renderer.Instance());
        std::vector<uint32_t> all(n);
        for (uint32_t k = 0; k < n; ++k) all[k] = k;
        edit->removePlacements(gpu.renderer.Instance(), all.data(), n);
    }
    // A frame at `time` in a wind of `speed` m/s (towards +x, gustiness 1) through debug view `debug`; captured into
    // `out` when given. The frame's camera: from `eye` towards `at`.
    std::string Frame(const float eye[3], const float at[3], float fovY, float time, float speed, int debug, Image* out,
                      wrally::iface::EditModelStats* stats = nullptr)
    {
        using namespace wrally::iface;
        FrameDesc f{};
        f.scene = scene;
        f.weatherPreset = -1;
        f.weather = weather;
        f.weather.windDirectionDeg = windDeg, f.weather.windSpeed = speed, f.weather.gustiness = 1.0f;
        f.time = time;
        LookAtLH(f.camera.view, eye, at);
        PerspectiveFovLH(f.camera.proj, fovY, 320.0f / 240.0f, 0.5f, 30000.0f);
        f.camera.nearZ = 0.5f, f.camera.farZ = 30000.0f, f.camera.fovY = fovY, f.camera.cut = 1;
        gpu.renderer.SetDebugView(debug);
        std::string path;
        if (out) {
            path = dir + "/wind-" + std::to_string(shot++) + ".png";
            gpu.renderer.CaptureFrame(path);
        }
        if (!gpu.renderer.RenderFrame(f)) return "a frame failed: " + gpu.renderer.Error();
        if (stats) edit->modelStats(gpu.renderer.Instance(), stats);
        if (out && !ReadPng(path, *out)) return "the capture " + path + " could not be read";
        return "";
    }
    // Two frames `dt` apart (the first at `t`), the second captured - so its motion is over dt.
    std::string Pair(const float eye[3], const float at[3], float fovY, float t, float dt, float speed, int debug, Image& a, Image& b)
    {
        if (std::string e = Frame(eye, at, fovY, t - 1.0f, speed, debug, nullptr); !e.empty()) return e;   // (a jump: no motion)
        if (std::string e = Frame(eye, at, fovY, t - dt, speed, debug, nullptr); !e.empty()) return e;
        if (std::string e = Frame(eye, at, fovY, t, speed, debug, &a); !e.empty()) return e;
        return Frame(eye, at, fovY, t + dt, speed, debug, &b);
    }
};

// Where a world point lands on the 320x240 frame (row; -1 behind).
float ScreenY(const float eye[3], const float at[3], float fovY, const float p[3])
{
    float view[16], proj[16];
    LookAtLH(view, eye, at);
    PerspectiveFovLH(proj, fovY, 320.0f / 240.0f, 0.5f, 30000.0f);
    float v[4] = {}, c[4] = {};
    for (int k = 0; k < 4; ++k) v[k] = p[0] * view[0 * 4 + k] + p[1] * view[1 * 4 + k] + p[2] * view[2 * 4 + k] + view[3 * 4 + k];
    for (int k = 0; k < 4; ++k) c[k] = v[0] * proj[0 * 4 + k] + v[1] * proj[1 * 4 + k] + v[2] * proj[2 * 4 + k] + v[3] * proj[3 * 4 + k];
    if (c[3] <= 0.0f) return -1.0f;
    return (1.0f - c[1] / c[3]) * 0.5f * 240.0f;
}

// --trees --wind: `out` the trees with the wind (kModelWind), `calmBytes` the same trees built without it.
std::string WindTrees(Gpu& gpu, const std::vector<uint8_t>& out, const std::vector<uint8_t>& calmBytes, const ngg::SceneData& s,
                      const std::string& dir)
{
    using wrally::iface::EditModelStats;
    if (!gpu.error.empty()) return "";
    size_t pick = 0;
    for (size_t k = 1; k < s.placements.size(); ++k)
        if (std::fabs(s.placements[k].scaleY - 1.0f) < std::fabs(s.placements[pick].scaleY - 1.0f)) pick = k;
    const ngg::Placement& p0 = s.placements[pick];
    const float sy = p0.scaleY;
    const float* b = p0.position;
    const float at[3] = {b[0], b[1] + 5.0f * sy, b[2]};
    const float eyeNear[3] = {b[0] + 20.0f * sy, b[1] + 5.0f * sy, b[2]};          // LOD0
    const float eyeMid[3] = {b[0] + 100.0f * sy, b[1] + 8.0f * sy, b[2]};          // LOD1
    const float eyeDown[3] = {b[0] + 8.0f * sy, b[1] + 14.0f * sy, b[2] + 3.0f * sy};   // down at the ground round it
    const float atGround[3] = {b[0], b[1] + 1.0f, b[2]};
    const float t = 1234.5f;

    // 1. Calm: the frames the trees without wind give, exactly - normal shading, near, mid and down; the statistics too
    //    (but `visible`: a wind model's culling box is wider, by what it can lean at 20 m/s).
    {
        Image wa, wb, wc, ca, cb, cc;
        EditModelStats ws[3], cs[3];
        WindSession w(gpu, dir), c(gpu, dir);
        if (std::string e = w.Open(out); !e.empty()) return e;
        if (gpu.log.find("; 1 with wind") == std::string::npos) return "the renderer did not load the trees with wind:\n" + gpu.log;
        for (int k = 0; k < 2; ++k) w.Frame(eyeNear, at, 1.0f, t, 0.0f, 0, nullptr);
        if (std::string e = w.Frame(eyeNear, at, 1.0f, t, 0.0f, 0, &wa, &ws[0]); !e.empty()) return e;
        w.Frame(eyeMid, at, 1.0f, t, 0.0f, 0, nullptr);
        if (std::string e = w.Frame(eyeMid, at, 1.0f, t, 0.0f, 0, &wb, &ws[1]); !e.empty()) return e;
        w.Frame(eyeDown, atGround, 1.2f, t, 0.0f, 0, nullptr);
        if (std::string e = w.Frame(eyeDown, atGround, 1.2f, t, 0.0f, 0, &wc, &ws[2]); !e.empty()) return e;
        w.Close();
        if (std::string e = c.Open(calmBytes); !e.empty()) return "without wind: " + e;
        for (int k = 0; k < 2; ++k) c.Frame(eyeNear, at, 1.0f, t, 0.0f, 0, nullptr);
        if (std::string e = c.Frame(eyeNear, at, 1.0f, t, 0.0f, 0, &ca, &cs[0]); !e.empty()) return e;
        c.Frame(eyeMid, at, 1.0f, t, 0.0f, 0, nullptr);
        if (std::string e = c.Frame(eyeMid, at, 1.0f, t, 0.0f, 0, &cb, &cs[1]); !e.empty()) return e;
        c.Frame(eyeDown, atGround, 1.2f, t, 0.0f, 0, nullptr);
        if (std::string e = c.Frame(eyeDown, atGround, 1.2f, t, 0.0f, 0, &cc, &cs[2]); !e.empty()) return e;
        const size_t d = Differ(wa, ca) + Differ(wb, cb) + Differ(wc, cc);
        std::printf("calm: the trees with wind drawn as without it - %zu pixel(s) differ in 3 frames (near %u, mid %u, down %u "
                    "visible; without wind %u, %u, %u)\n", d, ws[0].visible, ws[1].visible, ws[2].visible, cs[0].visible,
                    cs[1].visible, cs[2].visible);
        if (d) return "in calm air the trees with wind are not drawn as the trees without it";
        for (int k = 0; k < 3; ++k)   // (the wider boxes can only let more placements through the frustum)
            if (ws[k].visible < cs[k].visible || ws[k].lod0 < cs[k].lod0 || ws[k].lod1 < cs[k].lod1 || ws[k].billboard < cs[k].billboard)
                return "in calm air the trees with wind are counted fewer than without";
        if (!ws[0].lod0 || !ws[1].lod1) return "the calm frames drew no LOD0 near or no LOD1 at 100 m";
    }

    WindSession w(gpu, dir);
    if (std::string e = w.Open(out); !e.empty()) return e;
    const float wind = 15.0f;
    // 2. Two frames 0.1 s apart (the uv0 view: no lighting, so only geometry moves it): the trees move, and nothing else
    //    does - the same frames with every placement removed are the same.
    Image a, bb, ra, rb, ma, mb, sa, sb, calmA, calmB;
    if (std::string e = w.Pair(eyeNear, at, 1.0f, t, 0.1f, wind, 5, a, bb); !e.empty()) return e;
    if (std::string e = w.Pair(eyeNear, at, 1.0f, t, 0.1f, 0.0f, 5, calmA, calmB); !e.empty()) return e;
    const size_t moved = Differ(a, bb), movedCalm = Differ(calmA, calmB);
    // 3. The motion target (debug view 16): its own motion (blue) on the swaying leaves, none in calm air.
    Image mc, mcalm;
    if (std::string e = w.Pair(eyeNear, at, 1.0f, t, 0.1f, wind, 16, ma, mb); !e.empty()) return e;
    if (std::string e = w.Pair(eyeNear, at, 1.0f, t, 0.1f, 0.0f, 16, mc, mcalm); !e.empty()) return e;
    // 4. The sun's cascades (ray tracing off; debug view 7, sun visibility) looking down at the ground round the tree:
    //    the shadows move on the ground - pixels the trees cover in neither frame (the uv0 view against no trees).
    Image ga, gb, gna, gnb;
    if (std::string e = w.Pair(eyeDown, atGround, 1.2f, t, 0.1f, wind, 7, sa, sb); !e.empty()) return e;
    if (std::string e = w.Pair(eyeDown, atGround, 1.2f, t, 0.1f, wind, 5, ga, gb); !e.empty()) return e;
    Image sCalmA, sCalmB;
    if (std::string e = w.Pair(eyeDown, atGround, 1.2f, t, 0.1f, 0.0f, 7, sCalmA, sCalmB); !e.empty()) return e;
    w.RemoveAll();
    if (std::string e = w.Pair(eyeNear, at, 1.0f, t, 0.1f, wind, 5, ra, rb); !e.empty()) return e;
    if (std::string e = w.Pair(eyeDown, atGround, 1.2f, t, 0.1f, wind, 5, gna, gnb); !e.empty()) return e;
    const size_t terrainMoved = Differ(ra, rb);
    // (3.) The leaves: the pixels a tree covers in the uv0 view (it differs there from the same view without trees).
    size_t withMotion = 0, motionCalm = 0, leafPixels = 0;
    for (int y = 0; y < mb.h; ++y)
        for (int x = 0; x < mb.w; ++x) {
            const uint8_t* p = mb.at(x, y);
            const uint8_t* q = mcalm.at(x, y);
            if (!bb.same(rb, x, y)) {
                ++leafPixels;
                withMotion += (p[0] > 128 && (p[1] > 8 || p[2] > 8)) ? 1 : 0;   // blue: its own motion; red/green: some
            }
            if (!calmB.same(rb, x, y)) motionCalm += (q[0] > 8 || q[1] > 8 || q[2] > 8) ? 1 : 0;
        }
    size_t groundShadow = 0, groundPixels = 0;
    for (int y = 0; y < sa.h; ++y)
        for (int x = 0; x < sa.w; ++x) {
            if (!ga.same(gna, x, y) || !gb.same(gnb, x, y)) continue;   // a tree there in either frame
            ++groundPixels;
            groundShadow += sa.same(sb, x, y) ? 0 : 1;
        }
    const size_t shadowCalm = Differ(sCalmA, sCalmB);
    std::printf("wind %.0f m/s: two frames 0.1 s apart differ in %zu pixel(s) (calm: %zu); without the trees in %zu\n", wind, moved,
                movedCalm, terrainMoved);
    std::printf("the motion target: %zu of %zu tree pixel(s) with motion of their own (calm: %zu not black)\n", withMotion,
                leafPixels, motionCalm);
    std::printf("the cascades: the sun's visibility changed on %zu of %zu ground pixel(s) the trees cover in neither frame "
                "(calm: %zu pixel(s) of the whole frame)\n", groundShadow, groundPixels, shadowCalm);
    if (moved < 20 || movedCalm || terrainMoved) return "the trees did not sway, or something else moved";
    if (withMotion < 10 || motionCalm) return "the motion target has no motion on the swaying leaves (or some in calm air)";
    if (groundShadow < 5 || shadowCalm) return "the trees' shadows on the ground did not move with them (or moved in calm air)";

    // 5. The billboard, alone (the other placements removed), through a long lens from 400 m: its top leans, its base
    //    stays - frames a second apart in a 20 m/s wind, measured as how far the tree's pixels move across in a band of
    //    rows (the top 30%, the bottom 5%).
    {
        WindSession bw(gpu, dir);
        if (std::string e = bw.Open(out); !e.empty()) return e;
        bw.KeepOnly(b);
        bw.windDeg = 0.0f;   // across the view (towards +z), so the lean shows
        const float eyeFar[3] = {b[0] + 400.0f * sy, b[1] + 120.0f * sy, b[2]};
        const float atTree[3] = {b[0], b[1] + 5.0f * sy, b[2]};
        const float fov = 2.0f * std::atan(7.0f / std::sqrt(400.0f * 400.0f + 115.0f * 115.0f));   // ~14 m of height
        Image fa, fb;
        EditModelStats st{};
        bw.Frame(eyeFar, atTree, fov, t, 20.0f, 5, nullptr);
        if (std::string e = bw.Frame(eyeFar, atTree, fov, t, 20.0f, 5, nullptr, &st); !e.empty()) return e;   // (the last frame's)
        if (st.billboard != 1 || st.visible != 1)
            return "the lone tree is not drawn as a billboard from 400 m (" + std::to_string(st.visible) + " visible, " +
                   std::to_string(st.billboard) + " billboard(s), " + std::to_string(st.placements) + " placement(s))";
        if (std::string e = bw.Pair(eyeFar, atTree, fov, t, 1.0f, 20.0f, 5, fa, fb); !e.empty()) return e;
        const float top[3] = {b[0], b[1] + 10.0f * sy, b[2]};
        const float yBase = ScreenY(eyeFar, atTree, fov, b), yTop = ScreenY(eyeFar, atTree, fov, top);
        const float hpx = yBase - yTop;
        // The tree's pixels in a band of rows: where it differs from the same view without it, in either frame.
        Image none;
        bw.RemoveAll();
        bw.Frame(eyeFar, atTree, fov, t + 1.0f, 20.0f, 5, nullptr);
        if (std::string e = bw.Frame(eyeFar, atTree, fov, t + 1.0f, 20.0f, 5, &none); !e.empty()) return e;
        // In a band of rows: the tree's pixels in each frame (where it differs from the view without it), and how far
        // their middle moved across (pixels).
        auto band = [&](float f0, float f1, size_t& tree, double& shift) {
            double sa = 0, sb = 0;
            size_t na = 0, nb = 0;
            for (int y = (std::max)(0, int(yBase - f1 * hpx)); y < (std::min)(fa.h, int(yBase - f0 * hpx) + 1); ++y)
                for (int x = 0; x < fa.w; ++x) {
                    if (!fa.same(none, x, y)) sa += x, ++na;
                    if (!fb.same(none, x, y)) sb += x, ++nb;
                }
            tree = (std::min)(na, nb);
            shift = (na && nb) ? std::fabs(sb / double(nb) - sa / double(na)) : 0.0;
        };
        size_t topTree = 0, baseTree = 0;
        double topShift = 0, baseShift = 0;
        band(0.7f, 1.0f, topTree, topShift);
        band(0.0f, 0.05f, baseTree, baseShift);
        std::printf("the billboard from 400 m (%.0f px tall), a second apart: its top 30%% (%zu px) moved %.2f px across, its "
                    "bottom 5%% (%zu px, where it leans under 6 mm = 0.1 px) %.2f px\n", hpx, topTree, topShift, baseTree,
                    baseShift);
        if (hpx < 50.0f || baseTree < 10 || topTree < 50) return "the billboard is too small on screen (or hidden) to judge";
        // (The base's geometry moves hf^2 = 1/400 of the top's, ~0.01 px; what the bottom band shows is the hashed alpha
        // edge of a 2-3 px trunk flipping - 0.46 px on this machine. So: the top at least 2 px, the base under an eighth.)
        if (topShift < 2.0 || baseShift * 8.0 > topShift) return "the billboard's top did not lean, or its base moved";
    }
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
        bool wind = false;
        for (int i = 1; i < argc; ++i) wind = wind || !std::strcmp(argv[i], "--wind");
        if (std::string e = AddPlaceholderTrees(s, rep, rtLod, wind); !e.empty())
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
            if (std::string e = RayOnlyTrees(gpu, bytes, s, out, rep, wind); !e.empty()) return Fail("with a ray-only LOD: " + e);
        }
        if (wind) {
            // The same trees without the wind: what calm air must draw.
            ngg::SceneData calm;
            if (std::string e = ngg::Read(bytes.data(), bytes.size(), calm); !e.empty()) return Fail(e);
            PlaceholderReport calmRep;
            if (std::string e = AddPlaceholderTrees(calm, calmRep, rtLod, false); !e.empty()) return Fail(e);
            std::vector<uint8_t> calmBytes;
            if (std::string e = ngg::Write(calm, calmBytes); !e.empty()) return Fail("without wind: " + e);
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ngg-check-wind";
            std::filesystem::create_directories(dir);
            if (std::string e = WindTrees(gpu, out, calmBytes, s, dir.string()); !e.empty()) return Fail("with wind: " + e);
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
        if (gpu.error.empty() && gpu.log.find("; 1 with wind") == std::string::npos)
            return Fail("the made-up scene: the renderer did not count its wind model");
        if (gpu.error.empty() && (gpu.preset0.windDirectionDeg != 90.0f || gpu.preset0.windSpeed != 6.0f || gpu.preset0.gustiness != 0.4f))
            return Fail("the made-up scene: the renderer's weather preset lost its wind");
        std::printf("the made-up scene's wind: its model's (NGGD version %u) and its weather's (%.0f deg, %.0f m/s, gustiness "
                    "%.1f) round-trip and reach the renderer\n", ngg::kModelVersion, gpu.preset0.windDirectionDeg,
                    gpu.preset0.windSpeed, gpu.preset0.gustiness);

        // A version-2 NGGD (before the wind): the same model without it reads the same and loads, with no wind.
        {
            ngg::SceneData calm = s;
            calm.models.models[0].flags = 0;
            calm.models.models[0].windBend = 0.0f;
            std::vector<uint8_t> calmBytes;
            if (std::string e = ngg::Write(calm, calmBytes); !e.empty()) return Fail("without wind: " + e);
            const std::vector<uint8_t> v2 = WithModelChunk(calmBytes, ModelPayload(calm.models, 2));
            ngg::SceneData back;
            if (std::string e = ngg::Read(v2.data(), v2.size(), back); !e.empty()) return Fail("version 2: " + e);
            if (std::string e = Compare(calm, back); !e.empty()) return Fail("version 2: " + e + " differ");
            if (gpu.error.empty()) {
                if (std::string e = gpu.Load(v2, calm); !e.empty()) return Fail("version 2: " + e);
                if (gpu.log.find("scene: models - 1 model(s): 3 LODs (1 ray tracing only)") == std::string::npos ||
                    gpu.log.find("; 0 with wind") == std::string::npos)
                    return Fail("version 2: the renderer did not load it without wind:\n" + gpu.log);
            }
            std::printf("a version-2 NGGD (no wind): read the same, loaded by the renderer with no wind model\n");
        }
        // A 160-byte NGGW record (before the wind was appended): reads as calm, the rest as it was.
        {
            std::vector<uint8_t> p(sizeof(ngg::WeatherChunkHeader) + 160);
            const ngg::WeatherChunkHeader wh{ngg::kWeatherVersion, 1, 160, 0};
            std::memcpy(p.data(), &wh, sizeof wh);
            std::memcpy(p.data() + sizeof wh, &s.weather[0], 160);
            const std::vector<uint8_t> shortW = WithChunk(bytes, ngg::ChunkWeather(), p);
            ngg::SceneData back;
            if (std::string e = ngg::Read(shortW.data(), shortW.size(), back); !e.empty()) return Fail("a 160-byte NGGW: " + e);
            ngg::WeatherRecord want = s.weather[0];
            want.windDirectionDeg = want.windSpeed = want.gustiness = 0.0f;
            if (back.weather.size() != 1 || std::memcmp(&back.weather[0], &want, sizeof want))
                return Fail("a 160-byte NGGW record does not read as the same weather, calm");
            if (gpu.error.empty()) {
                if (std::string e = gpu.Load(shortW, back); !e.empty()) return Fail("a 160-byte NGGW: " + e);
                if (gpu.preset0.windSpeed != 0.0f || gpu.preset0.gustiness != 0.0f || gpu.preset0.sunIntensity != 22.0f)
                    return Fail("a 160-byte NGGW: the renderer's preset is not calm");
            }
            std::printf("a 160-byte NGGW record (before the wind): reads as calm, by both readers\n");
        }
        // Each rule of the ray-only LOD broken: refused by the format library (CheckModels, then Read) and by the
        // renderer, which loads the scene and draws the cards (no models).
        std::vector<BadLibrary> broken = RayOnlyRulesBroken(s.models);
        for (BadLibrary& b : WindRulesBroken(s.models)) broken.push_back(std::move(b));
        for (const BadLibrary& b : broken) {
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
