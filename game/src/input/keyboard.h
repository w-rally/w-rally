#pragma once
// The keyboard, as phys-drive read it: keys by their PLACE (scan code), whatever the layout prints on them - the brake
// is the key left of X on QWERTY (Z), QWERTZ (Y) and AZERTY (W) alike; the layout is asked each time, so switching it
// while driving works. A Press is a key's going down, once.
//
// Keys (US names): A throttle, Z brake, Space handbrake, Left / Right steering, S / X gear up / down, R back on the road,
// C the camera, , / . the beside camera round the car, [ / ] the chase camera in / out, T / O / L / K the renderer's
// TAA / AO / shadows / ray tracing, V its debug views, F1 its readout, F2 a screenshot, P pause, M mute, N the anti-lag at
// standstill, Esc quit.

#include <windows.h>

namespace wrally::input {

enum Place : UINT {
    kThrottle = 0x1e, kGearUp = 0x1f, kBrake = 0x2c, kGearDown = 0x2d, kCamera = 0x2e, kReset = 0x13, kTaa = 0x14,
    kPause = 0x19, kAo = 0x18, kShadows = 0x26, kView = 0x2f, kOrbitLeft = 0x33, kOrbitRight = 0x34, kChaseIn = 0x1a,
    kChaseOut = 0x1b, kRt = 0x25, kMute = 0x32, kAntiLagIdle = 0x31
};   // US: A S Z X C R T P O L V , . [ ] K M N

inline bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
inline bool DownAt(Place p) { return Down(static_cast<int>(MapVirtualKeyExW(p, MAPVK_VSC_TO_VK, GetKeyboardLayout(0)))); }

// A key's press, once: true on the frame it goes down.
struct Press {
    bool was = false;
    bool operator()(bool down)
    {
        const bool p = down && !was;
        was = down;
        return p;
    }
};

} // namespace wrally::input
