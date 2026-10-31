#include "assets/car_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace wrally::assets {

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

// The presets (LampKind order). Lobes: candela, half-width, half-height, yaw, pitch (degrees), sharpness.
// Halogen headlamps: main beams of a few tens of thousands of candela, a dipped beam's hot spot under the horizon;
// rally LED pods: rated at about 1 lux several hundred metres out, i.e. a few hundred thousand candela in the pencil;
// an LED bar: a wide, flat flood of lower peak. Round figures, tuned on a wall (tools/lamp-wall/wall.py, which keeps
// a copy of this table and the test layouts - change both).
const LampBeam kBeams[] = {
    {"halogen-low", 3200.0f, 0.07f, 2,
     {{16000.0f, 22.0f, 3.5f, 0.0f, -2.0f, 4.0f}, {5000.0f, 45.0f, 8.0f, 0.0f, -5.0f, 2.0f}}},
    {"halogen-high", 3300.0f, 0.07f, 2,
     {{80000.0f, 7.0f, 3.5f, 0.0f, 0.0f, 2.0f}, {18000.0f, 22.0f, 7.0f, 0.0f, -1.0f, 2.0f}}},
    // The pods' beams narrow, with a harder edge: each lamp lights its own spot (the user's tuning on the wall,
    // tools/lamp-wall/wall.py, 2026-10-31).
    // And a wide, dim spill around each hot spot (~7% of its peak): every patch of road lit by several lamps, so a
    // thing casts several lighter shadows, not one lamp's dark one.
    // Halogen pods (warm as the headlamps; the white LED look is the LED bars').
    {"halogen-pencil", 3300.0f, 0.08f, 3,
     {{350000.0f, 2.5f, 1.8f, 0.0f, 0.0f, 3.0f}, {15000.0f, 6.0f, 3.5f, 0.0f, 0.0f, 2.0f},
      {25000.0f, 25.0f, 10.0f, 0.0f, -1.0f, 2.0f}}},
    {"halogen-driving", 3300.0f, 0.08f, 3,
     {{150000.0f, 5.0f, 3.0f, 0.0f, 0.0f, 3.0f}, {15000.0f, 12.0f, 5.0f, 0.0f, -1.0f, 2.0f},
      {12000.0f, 30.0f, 12.0f, 0.0f, -1.0f, 2.0f}}},
    // A combo LED bar: a spot in the middle, a wide flood round it, smooth - no cut-off (the halogens' and the pods'
    // tuning not its).
    {"led-bar", 5700.0f, 0.25f, 3,
     {{150000.0f, 6.0f, 3.5f, 0.0f, 0.0f, 2.0f}, {30000.0f, 18.0f, 6.0f, 0.0f, -0.5f, 2.0f},
      {10000.0f, 40.0f, 10.0f, 0.0f, -1.0f, 2.0f}}},
    {"ditch", 5800.0f, 0.08f, 1,
     {{40000.0f, 20.0f, 6.0f, 0.0f, -2.0f, 2.0f}}},
    // The small lamps, as their regulations have them in round figures: a tail lamp some 5-10 cd, a brake lamp ~100,
    // a reversing lamp a few hundred, an indicator ~150 - wide, they are seen from everywhere behind or beside.
    {"tail", 0.0f, 0.05f, 1, {{8.0f, 45.0f, 20.0f, 0.0f, -5.0f, 2.0f}}, {1.0f, 0.03f, 0.02f}},
    {"brake", 0.0f, 0.05f, 1, {{100.0f, 45.0f, 20.0f, 0.0f, -5.0f, 2.0f}}, {1.0f, 0.03f, 0.02f}},
    {"reverse", 4000.0f, 0.05f, 1, {{300.0f, 45.0f, 20.0f, 0.0f, -10.0f, 2.0f}}},
    {"indicator", 0.0f, 0.05f, 1, {{150.0f, 50.0f, 20.0f, 0.0f, -5.0f, 2.0f}}, {1.0f, 0.42f, 0.0f}},
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

iface::LightDesc LampLight(const Lamp& lamp, const physics::Pose& pose, bool shadows, const float* delta)
{
    const LampBeam& beam = Beam(lamp.kind);
    const float cy = std::cos(lamp.yawDeg * kDeg), sy = std::sin(lamp.yawDeg * kDeg);
    const float cp = std::cos(lamp.pitchDeg * kDeg), sp = std::sin(lamp.pitchDeg * kDeg);
    // Its place, axis, up and right in the car's frame (forward -y, LEFT +x, up +z) - moved with its part, where the part
    // has moved (in render axes: the axes' swap is its own inverse) -, then the world's and the renderer's.
    math::V3 at = lamp.at, axisCar{-sy * cp, -cy * cp, sp}, upCar{sy * sp, cy * sp, cp}, rightCar{-cy, sy, 0.0f};
    if (delta) {
        auto xf = [&](math::V3 v, float w) {
            const math::V3 r = math::ToRender(v);
            return math::ToRender(math::V3{r.x * delta[0] + r.y * delta[4] + r.z * delta[8] + w * delta[12],
                                           r.x * delta[1] + r.y * delta[5] + r.z * delta[9] + w * delta[13],
                                           r.x * delta[2] + r.y * delta[6] + r.z * delta[10] + w * delta[14]});
        };
        at = xf(at, 1.0f), axisCar = xf(axisCar, 0.0f), upCar = xf(upCar, 0.0f), rightCar = xf(rightCar, 0.0f);
    }
    const math::V3 world = math::V3{pose.position[0], pose.position[1], pose.position[2]} + math::Rotate(pose.rotation, at);
    const math::V3 p = math::ToRender(world);
    const math::V3 d = math::ToRender(math::Rotate(pose.rotation, axisCar));
    const math::V3 u = math::ToRender(math::Rotate(pose.rotation, upCar));
    const math::V3 right = math::ToRender(math::Rotate(pose.rotation, rightCar));

    iface::LightDesc l{};
    l.type = iface::kLightBeam;
    l.flags = shadows ? 0u : iface::kLightNoShadows;
    l.position[0] = p.x, l.position[1] = p.y, l.position[2] = p.z;
    l.direction[0] = d.x, l.direction[1] = d.y, l.direction[2] = d.z;
    l.up[0] = u.x, l.up[1] = u.y, l.up[2] = u.z;
    if (beam.kelvin > 0.0f) {
        KelvinToRgb(beam.kelvin, l.colour);
    } else {   // a coloured lamp: its tint, of luminance 1
        const float lum = 0.2126f * beam.rgb[0] + 0.7152f * beam.rgb[1] + 0.0722f * beam.rgb[2];
        for (int k = 0; k < 3; ++k) l.colour[k] = beam.rgb[k] / std::max(lum, 1e-6f);
    }
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
    // +x is the car's left, and a yaw is to the right: the left side's lamps turn out with a negative one.
    const math::V3 hl{0.65f, -2.0f, 0.05f}, hr{-0.65f, -2.0f, 0.05f};
    const float headPitch = -0.5f, rampPitch = -0.8f;
    if (!std::strcmp(name, "low"))
        return {{LampKind::HalogenLow, hl, 0.0f, 0.0f}, {LampKind::HalogenLow, hr, 0.0f, 0.0f}};
    std::vector<Lamp> lamps = {{LampKind::HalogenHigh, hl, 0.0f, headPitch}, {LampKind::HalogenHigh, hr, 0.0f, headPitch}};
    if (!std::strcmp(name, "ramp")) {
        // Each lamp to its own spot (the user's tuning on the wall): the main beams 10 degrees out, the outer pods 20
        // degrees out to the corners, the inner ones 5 degrees out and 4 up.
        lamps[0].yawDeg = -10.0f;
        lamps[1].yawDeg = 10.0f;
        lamps.push_back({LampKind::HalogenDriving, {0.38f, -1.9f, 0.3f}, -20.0f, rampPitch});
        lamps.push_back({LampKind::HalogenPencil, {0.12f, -1.9f, 0.3f}, -5.0f, 4.0f});
        lamps.push_back({LampKind::HalogenPencil, {-0.12f, -1.9f, 0.3f}, 5.0f, 4.0f});
        lamps.push_back({LampKind::HalogenDriving, {-0.38f, -1.9f, 0.3f}, 20.0f, rampPitch});
    } else if (!std::strcmp(name, "bars")) {
        lamps.push_back({LampKind::LedBar, {-0.3f, -1.8f, 0.32f}, 0.0f, rampPitch});
        lamps.push_back({LampKind::LedBar, {0.3f, -1.8f, 0.32f}, 0.0f, rampPitch});
    } else {
        return {};
    }
    return lamps;
}

namespace {

// RBR's car .ini's Point_light_<name> places (Pos = x y z), in its model's frame (y up, forward -z).
std::map<std::string, math::V3> ReadLampPoints(const std::string& iniPath)
{
    std::map<std::string, math::V3> points;
    std::ifstream f(iniPath);
    std::string line, section;
    while (std::getline(f, line)) {
        const size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, line.find_last_not_of(" \t\r") - a + 1);
        if (line.front() == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (section.rfind("Point_light_", 0) != 0 || eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key != "Pos") continue;
        std::istringstream v(line.substr(eq + 1));
        math::V3 p{};
        if (v >> p.x >> p.y >> p.z) points[section.substr(12)] = p;
    }
    return points;
}

// The pods of a ramp, sorted left to right (+x first), aimed as tuned on the wall: the outer pair 20 degrees out and a
// little down (driving beams to the corners), the inner pair 5 out and 4 up (pencils down the road), a middle pair 10
// out, a lone centre one straight on.
std::vector<Lamp> AimRamp(std::vector<math::V3> places)
{
    std::sort(places.begin(), places.end(), [](const math::V3& a, const math::V3& b) { return a.x > b.x; });
    const size_t n = places.size();
    std::vector<Lamp> lamps;
    for (size_t i = 0; i < n; ++i) {
        const size_t fromOutside = std::min(i, n - 1 - i);         // 0 the outermost pair
        const size_t pairs = n / 2;
        const float side = places[i].x > 0.0f ? -1.0f : 1.0f;      // a left lamp turns out with a negative yaw
        Lamp l;
        l.at = places[i];
        if (n % 2 && i == n / 2) {
            l.kind = LampKind::HalogenPencil, l.yawDeg = 0.0f, l.pitchDeg = 0.0f;
        } else if (pairs == 1) {   // a single pair: driving beams 10 degrees out, below the main beams' corners
            l.kind = LampKind::HalogenDriving, l.yawDeg = 10.0f * side, l.pitchDeg = -0.8f;
        } else if (fromOutside + 1 == pairs) {   // the innermost pair
            l.kind = LampKind::HalogenPencil, l.yawDeg = 5.0f * side, l.pitchDeg = 4.0f;
        } else if (fromOutside == 0) {
            l.kind = LampKind::HalogenDriving, l.yawDeg = 20.0f * side, l.pitchDeg = -0.8f;
        } else {   // between: 10 degrees out, 2 up (their own spot above the outer ones', as tuned on the wall)
            l.kind = LampKind::HalogenDriving, l.yawDeg = 10.0f * side, l.pitchDeg = 2.0f;
        }
        lamps.push_back(l);
    }
    return lamps;
}

}  // namespace

CarLamps LoadCarLamps(const std::string& sgcPath, const float cog[3])
{
    CarLamps out;
    std::string ini = sgcPath;
    if (const size_t dot = ini.find_last_of('.'); dot != std::string::npos) ini.resize(dot);
    const std::map<std::string, math::V3> points = ReadLampPoints(ini + ".ini");
    // The model's frame to the car's: y and z swapped, less the centre of gravity.
    auto car = [&](const math::V3& p) { return math::V3{p.x - cog[0], p.z - cog[1], p.y - cog[2]}; };
    auto get = [&](const char* name, math::V3 fallback, bool* found = nullptr) {
        const auto it = points.find(name);
        if (found) *found = it != points.end();
        return it != points.end() ? car(it->second) : fallback;
    };
    bool haveHead = false;
    const math::V3 lf = get("lf", {0.65f, -2.0f, 0.05f}, &haveHead), rf = get("rf", {-0.65f, -2.0f, 0.05f});
    const math::V3 lb = get("lb", {0.65f, 1.9f, 0.2f}), rb = get("rb", {-0.65f, 1.9f, 0.2f});
    out.fromCar = haveHead;
    // The headlamps (the switch makes them dipped or main), the tail lamps, the brake and reversing lamps, behind.
    out.fixed.push_back({{LampKind::HalogenHigh, lf}, LampRole::Head});
    out.fixed.push_back({{LampKind::HalogenHigh, rf}, LampRole::Head});
    out.fixed.push_back({{LampKind::Tail, lb, 180.0f}, LampRole::Tail});
    out.fixed.push_back({{LampKind::Tail, rb, 180.0f}, LampRole::Tail});
    out.fixed.push_back({{LampKind::Brake, get("brake_lb", lb), 180.0f}, LampRole::Brake});
    out.fixed.push_back({{LampKind::Brake, get("brake_rb", rb), 180.0f}, LampRole::Brake});
    bool haveCentre = false;
    const math::V3 mb = get("brake_mb", {}, &haveCentre);   // the high centre one, where the car has it
    if (haveCentre) out.fixed.push_back({{LampKind::Brake, mb, 180.0f}, LampRole::Brake});
    out.fixed.push_back({{LampKind::Reverse, get("reverse_lb", lb), 180.0f}, LampRole::Reverse});
    out.fixed.push_back({{LampKind::Reverse, get("reverse_rb", rb), 180.0f}, LampRole::Reverse});
    // The indicators - no RBR car has them: at the corners beside the head and tail lamps, turned out 30 degrees.
    auto outward = [](math::V3 p, float by) { return math::V3{p.x + (p.x > 0.0f ? by : -by), p.y, p.z}; };
    out.fixed.push_back({{LampKind::Indicator, outward(lf, 0.08f), -30.0f}, LampRole::IndicatorLeft});
    out.fixed.push_back({{LampKind::Indicator, outward(lb, 0.05f), 210.0f}, LampRole::IndicatorLeft});
    out.fixed.push_back({{LampKind::Indicator, outward(rf, 0.08f), 30.0f}, LampRole::IndicatorRight});
    out.fixed.push_back({{LampKind::Indicator, outward(rb, 0.05f), 150.0f}, LampRole::IndicatorRight});

    // The ramps: the car's own pods (Point_light_bon<n>, its ramp's meshes shown with them), then two, four or six
    // halogen pods across the bonnet's front edge, a hand behind and above the headlamps (a count the car's own has
    // left out), two LED bars, none. The main beams turn 10 degrees out with pods; the car's own ramp is the default,
    // else four pods.
    std::vector<math::V3> pods;
    for (int k = 1; k <= 8; ++k)
        if (const auto it = points.find("bon" + std::to_string(k)); it != points.end()) pods.push_back(car(it->second));
    const math::V3 mid{0.0f, 0.5f * (lf.y + rf.y) + 0.15f, 0.5f * (lf.z + rf.z) + 0.25f};
    if (!pods.empty()) out.layouts.push_back({"ramp", AimRamp(pods), 10.0f});
    static const std::vector<float> kAcross[3] = {{0.25f, -0.25f}, {0.38f, 0.12f, -0.12f, -0.38f},
                                                  {0.47f, 0.3f, 0.12f, -0.12f, -0.3f, -0.47f}};
    for (const std::vector<float>& xs : kAcross) {
        if (xs.size() == pods.size()) continue;
        std::vector<math::V3> places;
        for (float x : xs) places.push_back({x, mid.y, mid.z});
        if (xs.size() == 4 && pods.empty()) out.defaultLayout = out.layouts.size();
        out.layouts.push_back({std::to_string(xs.size()) + " pods", AimRamp(places), 10.0f});
    }
    out.layouts.push_back({"LED bars", {{LampKind::LedBar, {0.3f, mid.y, mid.z + 0.02f}, 0.0f, -0.8f},
                                        {LampKind::LedBar, {-0.3f, mid.y, mid.z + 0.02f}, 0.0f, -0.8f}}, 0.0f});
    out.layouts.push_back({"no ramp", {}, 0.0f});
    return out;
}

void CarLights(const CarLamps& lamps, size_t layout, float spreadDeg, float pitchDeg, const LampSwitches& s,
               const physics::Pose& pose, std::vector<iface::LightDesc>& out, const LampMounts* mounts)
{
    // A lamp's mount: its group's on its side, else the group's middle one; null where no mesh says (in place).
    auto side = [](const Lamp& l) { return l.at.x > 0.05f ? 0 : l.at.x < -0.05f ? 1 : 2; };
    auto place = [&](int group, const Lamp& l, bool* off) -> const float* {
        *off = false;
        if (!mounts) return nullptr;
        const int sd = side(l);
        for (int k : {sd, 2})
            if (mounts->off[group][k] || mounts->moved[group][k]) {
                *off = mounts->off[group][k];
                return mounts->moved[group][k] ? mounts->delta[group][k] : nullptr;
            }
        return nullptr;
    };
    const float headSpread = layout < lamps.layouts.size() ? lamps.layouts[layout].headSpread : 0.0f;
    // The indicators blink at 1.5 Hz, half the time on.
    const bool blinkOn = std::fmod(s.seconds * 1.5, 1.0) < 0.5;
    for (const CarLamp& c : lamps.fixed) {
        Lamp l = c.lamp;
        bool on = false, shadows = false;
        int group = kLampHead;
        switch (c.role) {
        case LampRole::Head:
            on = s.beam > 0, shadows = true;
            if (s.beam == 1) l.kind = LampKind::HalogenLow;   // dipped: straight on, its own cut-off
            else l.yawDeg = l.at.x > 0.0f ? -headSpread : headSpread, l.pitchDeg = -0.5f;   // main: the layout's spread
            break;
        case LampRole::Tail: on = s.smallLamps && s.beam > 0, group = kLampTail; break;
        case LampRole::Brake: on = s.smallLamps && s.brake, group = kLampBrake; break;
        case LampRole::Reverse: on = s.smallLamps && s.reverse, group = kLampReverse; break;
        case LampRole::IndicatorLeft:   // (beside the headlamps or the tail lamps: on their parts)
            on = s.smallLamps && blinkOn && (s.hazard || s.indicator < 0), group = l.at.y < 0.0f ? kLampHead : kLampTail;
            break;
        case LampRole::IndicatorRight:
            on = s.smallLamps && blinkOn && (s.hazard || s.indicator > 0), group = l.at.y < 0.0f ? kLampHead : kLampTail;
            break;
        default: break;
        }
        bool gone = false;
        const float* delta = place(group, l, &gone);
        if (on && !gone) out.push_back(LampLight(l, pose, shadows, delta));
    }
    // The ramp: on its pods' part, else its housing's (a bonnet's) - a ramp that came off with it is dark.
    if (s.beam == 2 && s.ramp && layout < lamps.layouts.size())
        for (Lamp l : lamps.layouts[layout].lamps) {
            l.yawDeg += (l.at.x > 0.0f ? -spreadDeg : spreadDeg);
            l.pitchDeg += pitchDeg;
            bool gone = false;
            const float* delta = place(kLampRamp, l, &gone);
            if (!gone && !delta) delta = place(kLampRampHousing, l, &gone);
            if (!gone) out.push_back(LampLight(l, pose, true, delta));
        }
}

}  // namespace wrally::assets
