#pragma once
// The replay cameras (M5): RBR's TV cameras from the stage's .dls (data/dls.h), switched by the car's distance along
// the stage's drive line (the .lbs (2,13)), as Wallaby's editor previews them (MapObjects/DLS/CameraItem.cs,
// ComputeCameraView):
// - the car's distance: the nearest point of the drive line, searched near the last one;
// - the camera: the one whose start trigger (a camera trigger of the drive events, end 0) is the latest at or before the
//   distance and whose end trigger is not yet passed;
// - its place: the record's position, or a point of its path (slot 1: id = its path, same animation set) at the
//   distance from its start trigger - or at a type 4 channel's value where it has one;
// - its channels (the drive events' camera channels: keys by distance, linear between, held past the ends) override
//   the position (types 1..3), the angles (5..7) and the zoom (12);
// - its aim (slot 3): 0 its angles (heading = angles[2], pitch = angles[0], degrees: the editor's camera angles
//   X = pi / 2 - heading, Y = -pitch), -1 (and the other negatives) the car, > 0 that camera's place;
// - its field of view: pi / 4 * zoom / 64 (the editor's).
// Not modelled: the roll (angles[1]), the tilt (slot 3), cameras of other animation sets.

#include <cstdint>
#include <vector>

#include "data/dls.h"
#include "data/lbs.h"

namespace wrally::camera {

struct ReplayView {
    float eye[3], at[3];   // the physics' axes
    float fovY;            // radians
    int32_t camera;        // the .dls camera's id
};

class ReplayCameras {
public:
    ReplayCameras(phys::data::Dls dls, std::vector<phys::data::LbsScripts::DrivePoint> line);
    bool Empty() const { return m_spans.empty() || m_line.size() < 2; }
    // The view for the car at `car` (the physics' axes); false where no camera covers its distance.
    bool View(const float car[3], ReplayView* out);
    float Distance() const { return m_distance; }

private:
    struct Span {
        float from, to;
        size_t camera;   // into m_dls.cameras
    };
    float Track(const float p[3]);
    void Place(size_t camera, float start, float eye[3], float angles[3], float* zoom) const;

    phys::data::Dls m_dls;
    std::vector<phys::data::LbsScripts::DrivePoint> m_line;
    std::vector<Span> m_spans;
    float m_distance = -1;
};

} // namespace wrally::camera
