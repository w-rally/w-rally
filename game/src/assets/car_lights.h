#pragma once
// wrally::assets - the car's lamps (NGG design 5.4 N3): what a lamp's beam is like, where it sits on the car and where
// it points, made the renderer's light (iface::LightDesc, kLightBeam) for the car's pose each frame.
//
// A BEAM is a lamp's character, a preset (LampKind): its colour temperature, its emitter's size, and the lobes of its
// light - a pod's narrow pencil and its driving spread, an LED bar's one wide flood, a headlamp's low beam with its
// cut-off - each an elliptical peak in candela in the lamp's own frame (iface::LightLobe). The numbers are the
// photometry of the lamp classes as published, in round figures, until they are tuned on the reference shots (N3c).
//
// A LAMP is a beam at a place on the car - its frame: x LEFT, -y forward, z up, from the centre of gravity (the
// pose's origin) - aimed yaw degrees right and pitch degrees up of the car's forward. A car's layouts (2 headlamps and
// a 2-lamp ramp, a 4-pod ramp, LED bars) and the setup's aim come from the car's data (N3b).

#include <cstdint>
#include <vector>

#include "core/math.h"
#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::assets {

enum class LampKind : uint32_t {
    HalogenLow,    // a headlamp's dipped beam: wide, with the cut-off just under the horizon
    HalogenHigh,   // its main beam
    LedPencil,     // a pod's long, narrow spot - the reach down the road
    LedDriving,    // a pod's wider driving beam
    LedBar,        // an LED bar's flat flood - the verges and the trees beside them
    Ditch,         // a cornering lamp, aimed off to the side
    Count
};

struct LampBeam {
    const char*     name;            // as car.json names it
    float           kelvin;          // colour temperature
    float           radius;          // the emitter's size, metres (its shadows' and its highlight's softness)
    uint32_t        lobeCount;
    iface::LightLobe lobes[iface::kMaxLightLobes];
};

const LampBeam& Beam(LampKind kind);
// The kind car.json names ("halogen-low", "led-pencil", ...); false if none is.
bool LampKindByName(const char* name, LampKind* kind);

struct Lamp {
    LampKind  kind = LampKind::HalogenHigh;
    math::V3  at{};                  // in the car's frame
    float     yawDeg = 0.0f;         // its aim, right of the car's forward
    float     pitchDeg = 0.0f;       // and up
    float     scale = 1.0f;          // its output (1: the beam's own)
};

// A colour temperature as a linear sRGB tint of luminance 1 (the CIE daylight / Planckian locus, Kim et al.'s fit,
// 1667-25000 K), so that candela stay candela whatever the colour.
void KelvinToRgb(float kelvin, float rgb[3]);

// The renderer's light for a lamp on the car at its pose.
iface::LightDesc LampLight(const Lamp& lamp, const physics::Pose& pose, bool shadows = true);

// A test layout (PHYS_DRIVE_TEST_LIGHTS) until the cars carry their own: "ramp" two main beams and a 4-pod ramp
// (pencils inside, driving beams outside), "bars" two main beams and two LED bars, "low" the dipped beams alone.
// Empty for a name it does not know.
std::vector<Lamp> TestLayout(const char* name);

}  // namespace wrally::assets
