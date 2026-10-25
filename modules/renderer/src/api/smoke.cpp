// ---------------------------------------------------------------------------
// ngg-api-smoke - drives the flat C API the way Wallaby's editor does, without
// Wallaby. The API had no client but the editor, so a change to the frame it
// records (design 9b item 27) could only be checked by opening the editor.
//
//   ngg-api-smoke <file.ngg>
//
// Creates a viewport in a hidden window, loads the file, renders, then makes
// the edits the renderer's BVH policy distinguishes (design 5.0o) and checks
// the API's own log for the expected decision:
//   - hide / show / first move / restore -> one BLAS rebuild each
//   - drag (further moves)               -> TLAS only, no BLAS built
// then drives the weather API the way the editor's panel will (5.0q).
// Exit code 0 when every expectation holds.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

#include "ngg_api.h"

namespace {

std::vector<std::string> g_log;
void Sink(const char* msg) { g_log.push_back(msg); printf("  | %s\n", msg); }

size_t Count(const char* needle)
{
    size_t n = 0;
    for (const std::string& l : g_log) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

void LookAt(const float eye[3], const float dir[3], float* m)
{
    float f[3] = { dir[0], dir[1], dir[2] };
    float l = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]); for (float& c : f) c /= l;
    float r[3] = { f[2], 0.0f, -f[0] };                // up x f, up = +Y
    l = sqrtf(r[0]*r[0] + r[2]*r[2]); r[0] /= l; r[2] /= l;
    float u[3] = { f[1]*r[2] - f[2]*r[1], f[2]*r[0] - f[0]*r[2], f[0]*r[1] - f[1]*r[0] };
    const float* ax[3] = { r, u, f };
    for (int c = 0; c < 3; ++c)
    {
        m[0*4+c] = ax[c][0]; m[1*4+c] = ax[c][1]; m[2*4+c] = ax[c][2];
        m[3*4+c] = -(ax[c][0]*eye[0] + ax[c][1]*eye[1] + ax[c][2]*eye[2]);
    }
    m[3] = m[7] = m[11] = 0.0f; m[15] = 1.0f;
}

void Perspective(float fovY, float aspect, float zn, float zf, float* m)
{
    memset(m, 0, 16 * sizeof(float));
    const float ys = 1.0f / tanf(fovY * 0.5f), q = zf / (zf - zn);
    m[0] = ys / aspect; m[5] = ys; m[10] = q; m[11] = 1.0f; m[14] = -q * zn;
}

bool Expect(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: ngg-api-smoke <file.ngg>\n"); return 2; }

    FILE* f = nullptr;
    if (fopen_s(&f, argv[1], "rb") || !f) { printf("cannot open %s\n", argv[1]); return 2; }
    std::vector<char> bytes;
    fseek(f, 0, SEEK_END); bytes.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
    fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);

    ngg_set_log_sink(Sink);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ngg-api-smoke";
    RegisterClassW(&wc);
    HWND parent = CreateWindowExW(0, wc.lpszClassName, L"ngg-api-smoke", WS_OVERLAPPEDWINDOW,
                                  0, 0, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);

    ngg_scene* s = ngg_create(parent, 960, 540);
    if (!s) { printf("ngg_create failed\n"); return 1; }
    if (!ngg_load_memory(s, bytes.data(), bytes.size())) { printf("load failed: %s\n", ngg_last_error(s)); return 1; }

    float focus[3] = { 0, 0, 0 };
    ngg_focus_point(s, focus);
    const float eye[3] = { focus[0], focus[1] + 8.0f, focus[2] };
    const float dir[3] = { 0.0f, -0.15f, 1.0f };
    float view[16], proj[16];
    LookAt(eye, dir, view);
    Perspective(1.204f, 960.0f / 540.0f, 0.35f, 8000.0f, proj);
    ngg_set_camera(s, view, proj);

    auto frames = [&](int n) { for (int i = 0; i < n; ++i) ngg_render(s); };

    // Since 5.0o the BVH is per mesh: the world, each moved object, each added
    // mesh. "BLAS(es) built" is logged once per Rebuild (the mesh set changed);
    // a transform-only change is a TLAS update and logs nothing.
    const char* kRebuild = "BLAS(es) built";
    bool ok = true;
    frames(5);
    ok &= Expect(Count(kRebuild) == 1, "first frame builds the BVH once");

    // HIDE: the world's key changes (it leaves an object out) - one rebuild.
    size_t n = Count(kRebuild);
    ngg_set_visible(s, 0, 0);
    frames(3);
    ok &= Expect(Count(kRebuild) == n + 1, "hiding an object rebuilds once");
    n = Count(kRebuild);
    ngg_set_visible(s, 0, 1);
    frames(3);
    ok &= Expect(Count(kRebuild) == n + 1, "showing it again rebuilds once");

    // FIRST MOVE: the object leaves the world and gets its own BLAS - one
    // rebuild, and RT stays on (it used to fall back to the cascades).
    float xf[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    xf[13] = 5.0f;
    n = Count(kRebuild);
    ngg_set_transform(s, 0, xf);
    frames(3);
    ok &= Expect(Count(kRebuild) == n + 1, "first move: one rebuild (world + the object's own BLAS)");
    ok &= Expect(Count("paused") == 0 && Count("failed to build") == 0, "RT shadows stay active while moved");

    // DRAG: further moves are TLAS-only - no BLAS is built.
    n = Count(kRebuild);
    for (int k = 1; k <= 10; ++k)
    {
        xf[12] = 0.5f * k;
        ngg_set_transform(s, 0, xf);
        frames(1);
    }
    ok &= Expect(Count(kRebuild) == n, "dragging (10 moves) builds no BLAS - instances only");

    // BACK TO IDENTITY: the object rejoins the world.
    for (int k = 0; k < 16; ++k) xf[k] = (k % 5 == 0) ? 1.0f : 0.0f;
    n = Count(kRebuild);
    ngg_set_transform(s, 0, xf);
    frames(3);
    ok &= Expect(Count(kRebuild) == n + 1, "restoring the transform rebuilds once");

    // ---- weather (design 5.0q): what the editor's weather panel does
    const uint32_t presets = ngg_weather_preset_count(s);
    if (presets > 0)
    {
        char name[96] = {};
        ngg_weather_preset_name(s, 0, name, sizeof(name));
        ngg_weather_desc live = {}, p0 = {};
        ngg_get_weather(s, &live);
        ngg_get_weather_preset(s, 0, &p0);
        ok &= Expect(live.cloud_cover == p0.cloud_cover && live.fog_visibility == p0.fog_visibility &&
                     live.sun_direction[1] == p0.sun_direction[1],
                     "loading applies the file's default preset");

        // Select the foggiest preset the file has, and render under it.
        uint32_t foggiest = 0;
        float minVis = 1e30f;
        for (uint32_t i = 0; i < presets; ++i)
        {
            ngg_weather_desc d = {};
            ngg_get_weather_preset(s, i, &d);
            if (d.fog_visibility > 0.0f && d.fog_visibility < minVis) { minVis = d.fog_visibility; foggiest = i; }
        }
        ok &= Expect(ngg_select_weather_preset(s, foggiest) == 1, "selecting a preset");
        frames(3);
        ngg_get_weather(s, &live);
        ok &= Expect(minVis == 1e30f || live.fog_visibility == minVis, "the selected preset is live");

        // A live edit, as a slider makes it, reads back exactly.
        live.cloud_cover = 0.7f;
        live.fog_visibility = 900.0f;
        live.turbidity = 2.5f;
        ngg_set_weather(s, &live);
        frames(2);
        ngg_weather_desc back = {};
        ngg_get_weather(s, &back);
        ok &= Expect(back.cloud_cover == 0.7f && back.fog_visibility == 900.0f && back.turbidity == 2.5f,
                     "a live edit reads back");

        // JSON through the renderer's own schema, both ways.
        char json[2048];
        const uint32_t n = ngg_weather_to_json(&back, "edited", "smoke", json, sizeof(json));
        ngg_weather_desc rt = {};
        char rtName[64] = {};
        ok &= Expect(n > 0 && ngg_weather_from_json(json, &rt, rtName, sizeof(rtName)) == 1 &&
                     strcmp(rtName, "edited") == 0 && rt.cloud_cover == back.cloud_cover &&
                     rt.fog_visibility == back.fog_visibility && rt.turbidity == back.turbidity,
                     "JSON round-trips through the renderer's schema");
        printf("  (%u presets in the file, default [%s], foggiest %.0f m)\n", presets, name, minVis);
    }
    else
        printf("  (no NGGW in this file - weather checks skipped; rbrtm --import-weather adds it)\n");

    ok &= Expect(Count("D3D12 [ERROR]") == 0 && Count("D3D12 [CORRUPTION]") == 0,
                 "no D3D12 errors");

    ngg_destroy(s);
    DestroyWindow(parent);
    printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
