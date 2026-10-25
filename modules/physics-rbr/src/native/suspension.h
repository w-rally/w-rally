#pragma once
// A wheel's spring and damper, natively (M6 step 2; the classic CSpringDamper, core/suspension.cpp): the main spring with a
// helper spring in series (it takes over past its length), the bump rubbers at both ends of the travel (the inner one
// with its own damping), the damping by direction with a high-speed rate past a break speed, and the damper's wear under
// a big load.
#include <cstdint>

namespace phys::native {

// CSpringDamper::CDef (the .lsp's SpringDamper keys and the setup's): lengths are travels (m), stiffnesses N/m, dampings
// N s/m.
struct SpringDamperDef {
    float innerBumpRubberPos = 0, outerBumpRubberPos = 0;   // the travels the rubbers start at (bump, droop)
    float springLength = 0, springStiffness = 0;
    float helperSpringMinLength = 0, helperSpringLength = 0, helperSpringStiffness = 0;
    float bumpRubberStiffness = 0, droopStiffness = 0;
    float dampingBump = 0, dampingRebound = 0, dampingBumpRubber = 0;
    float highSpeedBreakBump = 0, highSpeedBreakRebound = 0;   // the speeds past which the high-speed rates apply
    float highSpeedDampingBump = 0, highSpeedDampingRebound = 0;
};

// The damping in use: the def's, worn (the wheel's suspension block).
struct SpringDamperState {
    float dampingBump = 0, dampingRebound = 0, highSpeedDampingBump = 0;
};

// A damper's damage (the car damage handler's record).
struct DamperDamage {
    float loadFactor = 0;   // how the loads wear it
    float wear = 0;         // 0 .. 1
};

class SpringDamper {
public:
    const SpringDamperDef* def = nullptr;
    SpringDamperState* state = nullptr;
    DamperDamage* damage = nullptr;
    uint32_t helperEngaged = 0;   // the last force was the springs in series
    uint32_t highSpeedHold = 0;   // 10 when a high-speed rate applied, then counting down
    float peakRate = 0;           // the largest |travel rate| seen
    float bumpRubberRate = 0;     // the rate into the inner rubber
    float droopRate = 0;          // the rate into the outer one

    // The damping in use from the def and the wear (DamperWear, 0x52dd70).
    void Wear();
    // The force at travel x (spring) / v (the rubbers) and rate u (SpringDamperForce, 0x52e090): o1 the springs' and the
    // rubbers' force, o2 the damping's base, o3 its rate (the force o2 - o3 u); the damper worn by a big load.
    void Force(float x, float v, float u, float* o1, float* o2, float* o3);
};

} // namespace phys::native
