#include "assets/car_lights.h"

#include <cmath>
#include <cstring>

namespace wrally::assets {

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

// The presets (LampKind order). Lobes: candela, half-width, half-height, yaw, pitch (degrees), sharpness.
// Halogen headlamps: main beams of a few tens of thousands of candela, a dipped beam's hot spot under the horizon;
// rally LED pods: rated at about 1 lux several hundred metres out, i.e. a few hundred thousand candela in the pencil;
// an LED bar: a wide, flat flood of lower peak. Round figures - N3c tunes them on the reference shots.
const LampBeam kBeams[] = {
    {"halogen-low", 3200.0f, 0.07f, 2,
     {{16000.0f, 22.0f, 3.5f, 0.0f, -2.0f, 4.0f}, {5000.0f, 45.0f, 8.0f, 0.0f, -5.0f, 2.0f}}},
    {"halogen-high", 3300.0f, 0.07f, 2,
     {{50000.0f, 7.0f, 3.5f, 0.0f, 0.0f, 2.0f}, {12000.0f, 22.0f, 7.0f, 0.0f, -1.0f, 2.0f}}},
    {"led-pencil", 5800.0f, 0.08f, 2,
     {{350000.0f, 3.5f, 2.5f, 0.0f, 0.0f, 2.0f}, {40000.0f, 10.0f, 5.0f, 0.0f, 0.0f, 2.0f}}},
    {"led-driving", 5800.0f, 0.08f, 2,
     {{150000.0f, 8.0f, 4.0f, 0.0f, 0.0f, 2.0f}, {30000.0f, 22.0f, 8.0f, 0.0f, -1.0f, 2.0f}}},
    {"led-bar", 5700.0f, 0.25f, 2,
     {{60000.0f, 30.0f, 4.5f, 0.0f, 0.0f, 4.0f}, {15000.0f, 60.0f, 9.0f, 0.0f, -2.0f, 2.0f}}},
    {"ditch", 5800.0f, 0.08f, 1,
     {{40000.0f, 20.0f, 6.0f, 0.0f, -2.0f, 2.0f}}},
};
static_assert(sizeof(kBeams) / sizeof(kBeams[0]) == static_cast<size_t>(LampKind::Count), "a beam per LampKind");

}  // namespace

const LampBeam& Beam(LampKind kind)
{
    const uint32_t i = static_cast<uint32_t>(kind);
    return kBeams[i < static_cast<uint32_t>(LampKind::Count) ? i : 0];
}

bool LampKindByName(const char* name, LampKind* kind)
{
    for (uint32_t i = 0; name && i < static_cast<uint32_t>(LampKind::Count); ++i)
        if (!std::strcmp(kBeams[i].name, name)) {
            *kind = static_cast<LampKind>(i);
            return true;
        }
    return false;
}

void KelvinToRgb(float kelvin, float rgb[3])
{
    const double t = std::fmin(std::fmax(kelvin, 1667.0), 25000.0);
    // The chromaticity of the Planckian locus (Kim et al. 2002).
    const double x = t <= 4000.0 ? -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
    const double y = t <= 2222.0 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                   : t <= 4000.0 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                                 : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    // XYZ at Y = 1, to linear sRGB (D65), the negative lobes clipped and the luminance made 1 again.
    const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    r = std::fmax(r, 0.0), g = std::fmax(g, 0.0), b = std::fmax(b, 0.0);
    const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    rgb[0] = static_cast<float>(r / lum);
    rgb[1] = static_cast<float>(g / lum);
    rgb[2] = static_cast<float>(b / lum);
}

iface::LightDesc LampLight(const Lamp& lamp, const physics::Pose& pose, bool shadows)
{
    const LampBeam& beam = Beam(lamp.kind);
    const float cy = std::cos(lamp.yawDeg * kDeg), sy = std::sin(lamp.yawDeg * kDeg);
    const float cp = std::cos(lamp.pitchDeg * kDeg), sp = std::sin(lamp.pitchDeg * kDeg);
    // Its axis and up in the car's frame (forward -y, LEFT +x, up +z), then the world's and the renderer's.
    const math::V3 axisCar{-sy * cp, -cy * cp, sp}, upCar{sy * sp, cy * sp, cp};
    const math::V3 world = math::V3{pose.position[0], pose.position[1], pose.position[2]} + math::Rotate(pose.rotation, lamp.at);
    const math::V3 p = math::ToRender(world);
    const math::V3 d = math::ToRender(math::Rotate(pose.rotation, axisCar));
    const math::V3 u = math::ToRender(math::Rotate(pose.rotation, upCar));
    const math::V3 right = math::ToRender(math::Rotate(pose.rotation, math::V3{-cy, sy, 0.0f}));

    iface::LightDesc l{};
    l.type = iface::kLightBeam;
    l.flags = shadows ? 0u : iface::kLightNoShadows;
    l.position[0] = p.x, l.position[1] = p.y, l.position[2] = p.z;
    l.direction[0] = d.x, l.direction[1] = d.y, l.direction[2] = d.z;
    l.up[0] = u.x, l.up[1] = u.y, l.up[2] = u.z;
    KelvinToRgb(beam.kelvin, l.colour);
    l.intensity = lamp.scale;
    l.radius = beam.radius;
    l.outerDeg = 90.0f;
    // A lobe's yaw is towards the renderer's up x axis; the axes' swap (math::ToRender) mirrors the car's right, so
    // the sign is taken from where that right points.
    const math::V3 rr = math::Cross(u, d);
    const float side = rr.x * right.x + rr.y * right.y + rr.z * right.z >= 0.0f ? 1.0f : -1.0f;
    l.lobeCount = beam.lobeCount;
    for (uint32_t k = 0; k < beam.lobeCount; ++k) {
        l.lobes[k] = beam.lobes[k];
        l.lobes[k].yawDeg *= side;
    }
    return l;
}

std::vector<Lamp> TestLayout(const char* name)
{
    // Round places on a rally hatchback: the headlamps at the nose's corners, a ramp across the bonnet's front edge.
    const math::V3 hl{-0.65f, -2.0f, 0.05f}, hr{0.65f, -2.0f, 0.05f};
    const float headPitch = -0.5f, rampPitch = -0.8f;
    if (!std::strcmp(name, "low"))
        return {{LampKind::HalogenLow, hl, 0.0f, 0.0f}, {LampKind::HalogenLow, hr, 0.0f, 0.0f}};
    std::vector<Lamp> lamps = {{LampKind::HalogenHigh, hl, 0.0f, headPitch}, {LampKind::HalogenHigh, hr, 0.0f, headPitch}};
    if (!std::strcmp(name, "ramp")) {
        // The driving beams outside, toed 2 degrees out (+x is the car's left).
        lamps.push_back({LampKind::LedDriving, {0.38f, -1.9f, 0.3f}, -2.0f, rampPitch});
        lamps.push_back({LampKind::LedPencil, {0.12f, -1.9f, 0.3f}, 0.0f, rampPitch});
        lamps.push_back({LampKind::LedPencil, {-0.12f, -1.9f, 0.3f}, 0.0f, rampPitch});
        lamps.push_back({LampKind::LedDriving, {-0.38f, -1.9f, 0.3f}, 2.0f, rampPitch});
    } else if (!std::strcmp(name, "bars")) {
        lamps.push_back({LampKind::LedBar, {-0.3f, -1.8f, 0.32f}, 0.0f, rampPitch});
        lamps.push_back({LampKind::LedBar, {0.3f, -1.8f, 0.32f}, 0.0f, rampPitch});
    } else {
        return {};
    }
    return lamps;
}

}  // namespace wrally::assets
