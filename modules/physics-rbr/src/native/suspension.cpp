#include "native/suspension.h"

#include "num/f32.h"
#include "num/bits.h"
#include "num/x87.h"

namespace phys::native {

using num::F32;
using num::FromBits;
namespace jmp = num::jmp;

void SpringDamper::Wear()
{
    const F32 w = damage->wear;
    const F32 one = 1.0f;
    const SpringDamperDef& d = *def;
    SpringDamperState& st = *state;
    st.dampingBump = ((one - w * F32(FromBits(0x3f333333))) * F32(d.dampingBump)).f();                     // 0.7
    st.highSpeedDampingBump = ((one - w * F32(FromBits(0x3f4ccccd))) * F32(d.highSpeedDampingBump)).f();   // 0.8
    st.dampingRebound = ((one - w * F32(FromBits(0x3e99999a))) * F32(d.dampingRebound)).f();               // 0.3
    if (!jmp::BE(400.0f, st.dampingBump)) st.dampingBump = 400.0f;
}

void SpringDamper::Force(float x, float v, float u, float* o1, float* o2, float* o3)
{
    const SpringDamperDef& d = *def;
    const SpringDamperState& st = *state;
    const F32 zero = 0.0f;
    const F32 bump = st.dampingBump, rebound = st.dampingRebound, fastBump = st.highSpeedDampingBump;
    // The spring; past the helper's length the two in series.
    F32 spring = (F32(d.springLength) - (F32(x) - F32(d.helperSpringMinLength))) * F32(d.springStiffness);
    const F32 limit = (F32(d.helperSpringLength) - F32(d.helperSpringMinLength)) * F32(d.helperSpringStiffness);
    uint32_t pair = 0;
    if (!jmp::BE(limit.f(), spring.f())) {
        spring = ((F32(d.springStiffness) * F32(d.helperSpringStiffness)) / (F32(d.springStiffness) + F32(d.helperSpringStiffness))) *
                 ((F32(d.helperSpringLength) + F32(d.springLength)) - F32(x));
        pair = 1;
    }
    helperEngaged = pair;
    // The damping c by direction (the high-speed rate past its break, from the base f).
    F32 c, f = zero;
    bool fast = false;
    if (!jmp::BE(0.0f, u)) {   // bump
        const F32 lim = zero - F32(d.highSpeedBreakBump);
        c = bump;
        if (!jmp::BE(lim.f(), u) && !(d.highSpeedBreakBump == 0.0f)) {
            c = fastBump;
            f = (fastBump - bump) * lim;
            fast = true;
        }
    } else {   // rebound
        c = rebound;
        if (!jmp::BE(u, d.highSpeedBreakRebound) && !(d.highSpeedBreakRebound == 0.0f)) {
            c = F32(d.highSpeedDampingRebound);
            f = (F32(d.highSpeedDampingRebound) - rebound) * F32(d.highSpeedBreakRebound);
            fast = true;
        }
    }
    if (fast) highSpeedHold = 10;
    else --highSpeedHold;
    // The bump rubbers: r their force, k their damping.
    F32 r = zero, k = zero;
    if (!jmp::BE(d.innerBumpRubberPos, v)) {
        k = F32(d.dampingBumpRubber);
        r = (F32(d.innerBumpRubberPos) - F32(v)) * F32(d.bumpRubberStiffness);
        if (!jmp::BE(0.0f, u)) {
            k = F32(d.dampingBumpRubber) * F32(0.3f);
            bumpRubberRate = (zero - F32(u)).f();
        } else if (!jmp::A(r.f(), (F32(d.dampingBumpRubber) * F32(u)).f())) {
            k = r / F32(u);
        }
    } else if (!jmp::BE(v, d.outerBumpRubberPos)) {
        k = F32(d.dampingBumpRubber);
        r = (F32(d.outerBumpRubberPos) - F32(v)) * F32(d.droopStiffness);
        if (!jmp::BE(u, 0.0f)) {
            k = F32(d.dampingBumpRubber) * F32(0.3f);
            droopRate = u;
        } else if (!jmp::A((F32(d.dampingBumpRubber) * F32(u)).f(), r.f())) {
            k = r / F32(u);
        }
    }
    *o1 = (r + spring).f();
    *o3 = (k + c).f();
    *o2 = f.f();
    // The damper wears under a big load.
    const F32 rest = f - c * F32(u);
    const F32 stress = (!jmp::BE(0.0f, r.f()) ? zero : r) + rest;
    DamperDamage& w = *damage;
    if (!jmp::BE((F32(w.loadFactor) * stress).f(), 27000.0f)) {
        F32 e = (stress - F32(27000.0f)) * F32(FromBits(0x38690453));   // 1/18000
        e = e * e + F32(w.wear);
        if (!jmp::BE(0.0f, e.f())) e = zero;
        else if (!jmp::BE(e.f(), 1.0f)) e = 1.0f;
        w.wear = e.f();
        Wear();
    }
    const F32 au = !jmp::BE(u, 0.0f) ? F32(u) : zero - F32(u);
    const F32 pk = F32(peakRate);
    const F32 ap = !jmp::BE(pk.f(), 0.0f) ? pk : zero - pk;
    if (!jmp::BE(au.f(), ap.f())) peakRate = u;
}

} // namespace phys::native
