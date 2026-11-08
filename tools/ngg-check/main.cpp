// ngg-check: the .ngg writer and readers kept in step (docs/design.md decision 2).
//
//   ngg-check                      a made-up scene written, read back and compared; then loaded by the renderer
//   ngg-check <file.ngg>...        another producer's files (Wallaby's): read, written again, read back and compared;
//                                  the renderer loads the original and the rewrite
//   ngg-check --lbs <file.lbs> [--surface dry|damp|wet] [--out <file.ngg>]
//                                  W-Rally's legacy importer: the .lbs to an .ngg, read back, loaded by the renderer
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

#include "assets/legacy_stage.h"
#include "ngg_scene.h"
#include "render/renderer.h"

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
    return s;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The renderer module on a hidden window, for its own loader.
struct Gpu {
    HWND window = nullptr;
    wrally::render::Renderer renderer;
    std::string error;

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
        renderer.TakeLog();
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
};

std::string Arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], name)) return argv[i + 1];
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

    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
    if (files.empty()) {
        const ngg::SceneData s = MadeUp();
        std::vector<uint8_t> bytes;
        if (std::string e = RoundTrip(s, bytes); !e.empty()) return Fail("the made-up scene: " + e);
        if (std::string e = load(bytes, s); !e.empty()) return Fail("the made-up scene: " + e);
        std::printf("the made-up scene: written, read back the same (%zu bytes)%s\n", bytes.size(),
                    gpu.error.empty() ? ", loaded by the renderer" : "");
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
