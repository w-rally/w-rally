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
#include <string>
#include <vector>

#include "assets/car_model.h"
#include "core/math.h"
#include "physics/physics.h"
#include "wrally/iface/renderer.h"

namespace wrally::assets {

enum class LampKind : uint32_t {
    HalogenLow,    // a headlamp's dipped beam: wide, with the cut-off just under the horizon
    HalogenHigh,   // its main beam
    HalogenPencil,   // a pod's long, narrow spot - the reach down the road
    HalogenDriving,  // a pod's wider driving beam
    LedBar,        // an LED bar's flat flood - the verges and the trees beside them
    Ditch,         // a cornering lamp, aimed off to the side
    Tail,          // the small lamps: a tail lamp (red),
    Brake,         // a brake lamp (red),
    Reverse,       // a reversing lamp (white),
    Indicator,     // an indicator (amber)
    Count
};

struct LampBeam {
    const char*     name;            // as car.yaml names it
    float           kelvin;          // colour temperature; 0: `rgb` (a coloured lamp)
    float           radius;          // the emitter's size, metres (its shadows' and its highlight's softness)
    uint32_t        lobeCount;
    iface::LightLobe lobes[iface::kMaxLightLobes];
    float           rgb[3] = {};     // a coloured lamp's linear tint (its luminance made 1)
};

const LampBeam& Beam(LampKind kind);
// The kind car.yaml names ("halogen-low", "halogen-pencil", ...); false if none is.
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

// The renderer's light for a lamp on the car at its pose; `delta`, where the lamp sits on a part that has moved (a
// bonnet hanging on its hinge), the part's move in the car's frame (render axes, row vectors: rest to now).
iface::LightDesc LampLight(const Lamp& lamp, const physics::Pose& pose, bool shadows = true, const float* delta = nullptr);

// A test layout (PHYS_DRIVE_TEST_LIGHTS) until the cars carry their own: "ramp" two main beams and a 4-pod ramp
// (pencils inside, driving beams outside), "bars" two main beams and two LED bars, "low" the dipped beams alone.
// Empty for a name it does not know.
std::vector<Lamp> TestLayout(const char* name);

// ---- a car's lamps (N3b) --------------------------------------------------------------------------------------------
// What switches a lamp: the headlamps (dipped or main beam by the switch), the ramp (with the main beam, and its own
// switch), and the small lamps - tail (with the headlamps), brake (the pedal), reverse (the gear), the indicators.
enum class LampRole : uint32_t { Head, Ramp, Tail, Brake, Reverse, IndicatorLeft, IndicatorRight };

struct CarLamp {
    Lamp     lamp;
    LampRole role;
};

// A ramp the setup can choose: its name and its lamps; how far out the headlamps' main beams turn with it (each to its
// side: with pods, to the corners the pods do not cover, as tuned on the wall; with LED bars, straight on).
struct LampLayout {
    std::string       name;
    std::vector<Lamp> lamps;
    float             headSpread = 0.0f;
};

struct CarLamps {
    std::vector<CarLamp>    fixed;     // every layout's: the headlamps, the tail, brake and reverse lamps, the indicators
    std::vector<LampLayout> layouts;   // the ramps: "ramp" (the car's own pods), "2/4/6 pods", "LED bars", "no ramp"
    size_t defaultLayout = 0;          // the one it starts with: its own ramp, else four pods
    bool fromCar = false;              // the places are the car's own (its .ini's Point_light_*), not defaults
};

// A car's lamps from RBR's car .ini beside its .sgc (Point_light_lf/rf/lb/rb/brake_*/reverse_*/bon<n>: their places,
// in the model's frame, made the car's: y and z swapped, less the centre of gravity `cog`); what it lacks from the
// headlamps' places, or a hatchback's. The aims are W-Rally's, as tuned on the wall (tools/lamp-wall/wall.py): RBR's
// lamp normals are no aims (the 207's pods point sideways).
CarLamps LoadCarLamps(const std::string& sgcPath, const float cog[3]);

// The switches, the driver's and the car's.
struct LampSwitches {
    int   beam = 0;              // 0 off, 1 dipped, 2 main
    bool  ramp = true;           // the ramp's own switch (it shines with the main beam)
    bool  brake = false;
    bool  reverse = false;
    int   indicator = 0;         // -1 left, 1 right
    bool  hazard = false;
    double seconds = 0.0;        // a clock for the indicators' blink
    bool  smallLamps = true;     // the small lamps' light given (false by day: it shows only on their glass)
};

// Where the lamps sit (CarRender::Mounts): per LampMeshGroup and side (0 left, 1 right, 2 the middle / one for both),
// whether that lamp's part is gone - off the car: its cable torn, so it is dark - and, where the part has moved but
// hangs on, its move (LampLight's delta).
struct LampMounts {
    bool  off[kLampGroups][3] = {};
    bool  moved[kLampGroups][3] = {};
    float delta[kLampGroups][3][16] = {};
};

// The lights of a car's lamps at its pose, by the switches: `layout` the ramp (an index into lamps.layouts), the setup's
// aim added to its lamps - `spreadDeg` turning each further out to its side, `pitchDeg` up; `mounts` where they sit
// (null: all in place).
void CarLights(const CarLamps& lamps, size_t layout, float spreadDeg, float pitchDeg, const LampSwitches& s,
               const physics::Pose& pose, std::vector<iface::LightDesc>& out, const LampMounts* mounts = nullptr);

}  // namespace wrally::assets
