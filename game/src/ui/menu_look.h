#pragma once
// The full-window menus' look (the W-Rally design system, "Factory black"): its colours and slants, and the pieces every
// such screen draws with - its ground and livery, tracked labels, key chips, plates, markers, chevrons, the eased
// selection. Sizes are px at 1080p times the screen's `k` (min(height / 1080, width / 1280)).

#include <cmath>

#include "imgui.h"
#include "ui/menu_video.h"

namespace wrally::ui {

inline constexpr ImU32 kSurfaceBase = IM_COL32(10, 10, 11, 255), kSurfaceRaised = IM_COL32(20, 20, 22, 255),
                       kSurfaceOverlay = IM_COL32(30, 30, 33, 255), kLineStrong = IM_COL32(98, 98, 104, 255),
                       kInk = IM_COL32(255, 255, 255, 255), kInkSecondary = IM_COL32(196, 196, 200, 255),
                       kInkMuted = IM_COL32(140, 140, 144, 255), kSignal = IM_COL32(255, 75, 31, 255),
                       kLine = IM_COL32(44, 44, 48, 255);
// The selection's plate under the selected row: brighter than surface-raised, so it shows on a dim screen too; a list
// that has not the keys (the Stage page's other side) keeps its selection on kSelectIdle.
inline constexpr ImU32 kSelect = IM_COL32(42, 42, 47, 255), kSelectIdle = IM_COL32(28, 28, 31, 255);
inline const float kTanStripe = std::tan(22.0f * 3.14159265f / 180.0f);   // slant-stripe, the livery's
inline const float kTanPlate = std::tan(14.0f * 3.14159265f / 180.0f);    // slant-plate, the marker's
inline constexpr float kEase = 0.04f;   // the selection's ease (s): settled in ~120 ms, as the design's transitions

ImU32 Lerp(ImU32 a, ImU32 b, float t);
ImU32 Alpha(ImU32 c, float a);
// `text` at `at` with `spacing` (em) between its letters - the design's tracked labels. Returns its width (d null:
// measured only).
float Tracked(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 col, const char* text, float spacing);
// A key chip and what it does: "[ENTER] SELECT". Returns its width.
float KeyHint(ImDrawList* d, ImFont* f, float k, ImVec2 at, const char* key, const char* does);
// A livery stripe's corners (top left, top right, bottom right, bottom left): `left` and `w` px at 1080p, placed from the
// window's right edge, slanted from its top left, 40 px past the top and the bottom.
void Stripe(float W, float H, float k, float left, float w, float shift, ImVec2 q[4]);
// A stripe `q` (Stripe's corners) filled `top` at its top to `bottom` at its bottom.
void FadeQuad(ImDrawList* d, const ImVec2 q[4], ImU32 top, ImU32 bottom);
// A full screen's ground, its livery and the video's window, the stripes where every screen has them. `window`: whether
// the screen has the video's stripe (a screen whose content runs where it is has not: it fades out as the screen opens,
// in again when one with it does). `stripes`: whether it has the signal and white stripes at all (the Garage has not: its
// car is the picture) - they fade likewise.
void Backdrop(ImDrawList* d, float W, float H, float k, ImU32 ground, const VideoFrame& video, bool window, bool stripes = true);
// The video's stripe gone at once (a screen over a stage, where it never was).
void HideVideoStripe();
// A row's plate from `a`, `w` x `h`, its right edge cut in by `cut`.
void Plate(ImDrawList* d, ImVec2 a, float w, float h, float cut, ImU32 col);
// The selection's marker, `w` x `h` and slanted, at `x` centred on `mid`: `on` 0..1 slides it in from 12 px left.
void Marker(ImDrawList* d, float x, float mid, float w, float h, float on, float k);
// A chevron `size` px square centred on `c`, pointing right (or left).
void Chevron(ImDrawList* d, ImVec2 c, float size, bool right, ImU32 col);
// How far toward its target an eased value moves this frame (kEase).
float Ease();

} // namespace wrally::ui
