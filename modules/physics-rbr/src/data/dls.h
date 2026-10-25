#pragma once
// RBR stage .dls reader (M5): the replay cameras, as Wallaby reads them (Wallaby.Formats/DlsFile.cs, DlsCameras.cs,
// DlsDriveLine.cs; the editor's MapObjects/DLS readers for the rest). Positions are in the physics' axes (z up).
//
// Header: "ANIM" "DATA", u32 version, u32 reserved, i32 offsets[10] (absolute; -1 none; not in file order).
// Slot 0, drive events: u32 n, n x {u32 offset, u32 -}; at each offset u32 L[17] - counts L[1] event ids, L[2] camera
//   triggers, L[3] animation triggers, L[5] camera channels; L[9], L[10], L[13] their (absolute) offsets. Event ids
//   {i32 object id, i32 flags}; a camera trigger {i32 event (index into the ids), f32 distance, u32 end (0 the start)};
//   a channel {u32 camera, u32 type, u32 n, u32 -, n x {u32 flags, f32 distance, f32 value, f32 x3, u32 -}}. L[6]
//   pacenotes at L[14] (they end at L[16]): {i32 type, i32 flags, f32 distance}; types 21 the start, 22 the finish, 23
//   a checkpoint are the stage clock's marks (the exe's Regulation::CTimeKeeper reads them, 0x4a01f0).
// Slot 1, camera paths: u32 n, n x {u32 id, u32 animation set, u32 n, n x {u32 -, f32 position[3], f32 v[3],
//   f32 v2[3], f32 distance}}.
// Slot 2, cameras: u32 n, n x {i32 id, i32 id2, i32 path, i32 animation set, f32 position[3], f32 angles[3] (degrees:
//   pitch, roll, heading), f32 scale[3], i32 -}.
// Slot 3, camera properties: u32 n, n x {i32 camera, i32 aim (0 its own angles, -1 the car, > 0 another camera),
//   f32 zoom (the editor's field of view: pi / 4 * zoom / 64), f32 tilt, f32 -, f32 -}.

#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

struct DlsCamera {
    int32_t id = 0, path = 0, animationSet = 0;
    float position[3] = {}, angles[3] = {};
    // slot 3 (defaults when the camera has no entry: the editor's)
    int32_t aim = 0;
    float zoom = 64.0f, tilt = 0.0f;
};

struct DlsCameraTrigger {
    int32_t camera = 0;   // the camera's id
    float distance = 0;   // along the stage's drive line
    bool end = false;
};

struct DlsCameraChannel {
    int32_t camera = 0;
    uint32_t type = 0;   // 1..3 the position's x, y, z; 5..7 the angles; 12 the zoom
    struct Key {
        float distance, value;
    };
    std::vector<Key> keys;
};

struct DlsPath {
    uint32_t id = 0, animationSet = 0;
    struct Point {
        float position[3], distance;
    };
    std::vector<Point> points;
};

struct DlsPacenote {
    int32_t type = 0, flags = 0;   // 21 the start, 22 the finish, 23 a checkpoint (kDlsStart ..), the rest calls
    float distance = 0;            // along the stage's drive line
};
constexpr int32_t kDlsStart = 21, kDlsFinish = 22, kDlsCheckpoint = 23;

struct Dls {
    std::vector<DlsCamera> cameras;
    std::vector<DlsCameraTrigger> triggers;
    std::vector<DlsCameraChannel> channels;
    std::vector<DlsPath> paths;
    std::vector<DlsPacenote> pacenotes;
};

// Throws std::runtime_error on a bad file.
Dls ReadDls(const std::string& path);

} // namespace phys::data
