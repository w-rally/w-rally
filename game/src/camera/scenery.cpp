#include "camera/scenery.h"

#include <fstream>
#include <iterator>
#include <vector>

#include "native/stage_collision.h"

namespace wrally::camera {

struct Scenery::Impl {
    phys::native::StageCollision collision;
    phys::native::MaterialMaps maps;   // none loaded: a hit's materials are not asked for
    bool loaded = false;
};

Scenery::Scenery() : m(std::make_unique<Impl>()) {}
Scenery::~Scenery() = default;

std::string Scenery::Load(const std::string& colPath)
{
    m->loaded = false;
    std::ifstream f(colPath, std::ios::binary);
    if (!f) return "cannot open " + colPath;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string e = m->collision.Load(std::move(file));
    m->loaded = e.empty() && m->collision.Root();
    return e;
}

float Scenery::Clear(math::V3 from, math::V3 to) const
{
    if (!m->loaded) return 1.0f;
    phys::native::GroundHit hit{};
    if (!m->collision.SegmentQuery(m->maps, {from.x, from.y, from.z}, {to.x, to.y, to.z}, hit)) return 1.0f;
    return hit.t < 0.0f ? 0.0f : hit.t > 1.0f ? 1.0f : hit.t;
}

} // namespace wrally::camera
