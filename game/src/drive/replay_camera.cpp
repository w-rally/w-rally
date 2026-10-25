#include "drive/replay_camera.h"

#include <cmath>
#include <limits>
#include <utility>

namespace phys::drive {

namespace {

constexpr float kWindow = 60.0f;   // how far (m along the line) the car's distance may move between two views
constexpr float kDegree = 3.14159265f / 180.0f;

// A channel's value at distance d: linear between its keys, held past the ends.
bool ChannelAt(const data::DlsCameraChannel& c, float d, float* out)
{
    const data::DlsCameraChannel::Key *prev = nullptr, *next = nullptr;
    for (const data::DlsCameraChannel::Key& k : c.keys) {
        if (k.distance <= d && (!prev || k.distance > prev->distance)) prev = &k;
        if (k.distance > d && (!next || k.distance < next->distance)) next = &k;
    }
    if (prev && next) {
        const float t = (d - prev->distance) / (next->distance - prev->distance);
        *out = prev->value + (next->value - prev->value) * t;
    } else if (prev || next) {
        *out = (prev ? prev : next)->value;
    } else {
        return false;
    }
    return true;
}

} // namespace

ReplayCameras::ReplayCameras(data::Dls dls, std::vector<data::LbsScripts::DrivePoint> line)
    : m_dls(std::move(dls)), m_line(std::move(line))
{
    // A camera's spans: each start trigger to the first end trigger of the same camera after it.
    for (const data::DlsCameraTrigger& t : m_dls.triggers) {
        if (t.end) continue;
        size_t camera = m_dls.cameras.size();
        for (size_t i = 0; i < m_dls.cameras.size(); ++i)
            if (m_dls.cameras[i].id == t.camera) camera = i;
        if (camera == m_dls.cameras.size()) continue;   // an animation object's trigger
        float to = std::numeric_limits<float>::infinity();
        for (const data::DlsCameraTrigger& e : m_dls.triggers)
            if (e.end && e.camera == t.camera && e.distance > t.distance && e.distance < to) to = e.distance;
        m_spans.push_back({t.distance, to, camera});
    }
}

float ReplayCameras::Track(const float p[3])
{
    float best = std::numeric_limits<float>::infinity(), at = m_distance;
    for (size_t i = 0; i + 1 < m_line.size(); ++i) {
        const auto &a = m_line[i], &b = m_line[i + 1];
        if (m_distance >= 0 && (b.distance < m_distance - kWindow || a.distance > m_distance + kWindow)) continue;
        float ab[3], ap[3], l2 = 0, dot = 0;
        for (int k = 0; k < 3; ++k) {
            ab[k] = b.position[k] - a.position[k];
            ap[k] = p[k] - a.position[k];
            l2 += ab[k] * ab[k];
            dot += ab[k] * ap[k];
        }
        const float t = l2 > 0 ? std::fmin(1.0f, std::fmax(0.0f, dot / l2)) : 0.0f;
        float d2 = 0;
        for (int k = 0; k < 3; ++k) {
            const float e = ap[k] - ab[k] * t;
            d2 += e * e;
        }
        if (d2 < best) {
            best = d2;
            at = a.distance + (b.distance - a.distance) * t;
        }
    }
    return at;
}

void ReplayCameras::Place(size_t camera, float start, float eye[3], float angles[3], float* zoom) const
{
    const data::DlsCamera& c = m_dls.cameras[camera];
    for (int k = 0; k < 3; ++k) {
        eye[k] = c.position[k];
        angles[k] = c.angles[k];
    }
    *zoom = c.zoom;
    float along = m_distance - start, v;
    for (const data::DlsCameraChannel& ch : m_dls.channels) {
        if (ch.camera != c.id || !ChannelAt(ch, m_distance, &v)) continue;
        if (ch.type >= 1 && ch.type <= 3) eye[ch.type - 1] = v;
        else if (ch.type == 4) along = v;
        else if (ch.type >= 5 && ch.type <= 7) angles[ch.type - 5] = v;
        else if (ch.type == 12) *zoom = v;
    }
    if (c.path <= 0) return;
    for (const data::DlsPath& p : m_dls.paths) {
        if (static_cast<int32_t>(p.id) != c.path || static_cast<int32_t>(p.animationSet) != c.animationSet || p.points.empty())
            continue;
        const data::DlsPath::Point* prev = &p.points.front();
        for (const data::DlsPath::Point& q : p.points) {
            if (q.distance <= along) {
                prev = &q;
                continue;
            }
            const float t = q.distance > prev->distance ? (along - prev->distance) / (q.distance - prev->distance) : 0.0f;
            for (int k = 0; k < 3; ++k) eye[k] = prev->position[k] + (q.position[k] - prev->position[k]) * std::fmax(0.0f, t);
            return;
        }
        for (int k = 0; k < 3; ++k) eye[k] = prev->position[k];   // past the path's end
        return;
    }
}

bool ReplayCameras::View(const float car[3], ReplayView* out)
{
    if (Empty()) return false;
    m_distance = Track(car);
    const Span* span = nullptr;
    for (const Span& s : m_spans)
        if (s.from <= m_distance && m_distance < s.to && (!span || s.from > span->from)) span = &s;
    if (!span) return false;
    const data::DlsCamera& c = m_dls.cameras[span->camera];
    float angles[3], zoom;
    Place(span->camera, span->from, out->eye, angles, &zoom);
    out->camera = c.id;
    out->fovY = 3.14159265f / 4.0f * zoom / 64.0f;
    if (c.aim == 0) {   // its own angles
        const float yaw = 3.14159265f / 2.0f - angles[2] * kDegree, pitch = -angles[0] * kDegree;
        const float dir[3] = {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
        for (int k = 0; k < 3; ++k) out->at[k] = out->eye[k] + dir[k] * 10.0f;
        return true;
    }
    for (int k = 0; k < 3; ++k) out->at[k] = car[k];   // the car
    if (c.aim > 0)
        for (const data::DlsCamera& o : m_dls.cameras)
            if (o.id == c.aim)
                for (int k = 0; k < 3; ++k) out->at[k] = o.position[k];
    return true;
}

} // namespace phys::drive
