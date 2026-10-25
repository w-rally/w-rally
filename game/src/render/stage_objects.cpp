#include "render/stage_objects.h"

#include <cstring>

#include "core/math.h"

namespace wrally::render {

using math::V3;

const std::vector<iface::StageObjectUpdate>& StageObjects::Update(const std::vector<physics::Body>& bodies)
{
    m_updates.clear();
    if (m_moving.size() != bodies.size()) m_moving.assign(bodies.size(), 0);
    for (size_t i = 0; i < bodies.size(); ++i) {
        const physics::Body& b = bodies[i];
        if (!m_moving[i]) {
            if (!std::memcmp(b.position, b.startPosition, 12) && !std::memcmp(b.rotation, b.startRotation, 16)) continue;
            m_moving[i] = 1;
        }
        // The move since the load: q = now * start^-1, then x' = q (x - start) + now; as the renderer's row-major
        // matrix (rows: the images of its axes; translation in row 3).
        const float* a = b.rotation;
        const float bs[4] = {-b.startRotation[0], -b.startRotation[1], -b.startRotation[2], b.startRotation[3]};
        const float q[4] = {a[3] * bs[0] + a[0] * bs[3] + a[1] * bs[2] - a[2] * bs[1],
                            a[3] * bs[1] - a[0] * bs[2] + a[1] * bs[3] + a[2] * bs[0],
                            a[3] * bs[2] + a[0] * bs[1] - a[1] * bs[0] + a[2] * bs[3],
                            a[3] * bs[3] - a[0] * bs[0] - a[1] * bs[1] - a[2] * bs[2]};
        iface::StageObjectUpdate u{};
        u.key = b.id;
        u.visible = 1;
        float* m = u.delta;
        const V3 rows[3] = {math::ToRender(math::Rotate(q, {1, 0, 0})), math::ToRender(math::Rotate(q, {0, 0, 1})),
                            math::ToRender(math::Rotate(q, {0, 1, 0}))};
        for (int r = 0; r < 3; ++r) {
            m[r * 4 + 0] = rows[r].x;
            m[r * 4 + 1] = rows[r].y;
            m[r * 4 + 2] = rows[r].z;
            m[r * 4 + 3] = 0;
        }
        const V3 st = math::ToRender({b.startPosition[0], b.startPosition[1], b.startPosition[2]});
        const V3 now = math::ToRender({b.position[0], b.position[1], b.position[2]});
        const V3 turned = rows[0] * st.x + rows[1] * st.y + rows[2] * st.z;   // the start point turned
        m[12] = now.x - turned.x;
        m[13] = now.y - turned.y;
        m[14] = now.z - turned.z;
        m[15] = 1;
        m_updates.push_back(u);
    }
    return m_updates;
}

} // namespace wrally::render
