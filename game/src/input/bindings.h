#pragma once
// The keys, bound to the game's actions (wrally::input): which key does what, set in Settings -> Controls and kept in
// wrally.ini (key_<action> = its scan code). A key is held by its PLACE (scan code), whatever the layout prints on it:
// the brake's default is the key left of X on QWERTY (Z), QWERTZ (Y) and AZERTY (W) alike. An extended key (the arrows,
// the right Ctrl) is its scan code with 0xE000. 0: unbound.
//
// Esc is not bindable: it is the menus'.

#include <windows.h>

#include <string>

namespace wrally::input {

enum class Action {
    Throttle, Brake, Handbrake, SteerLeft, SteerRight, GearUp, GearDown, BackOnRoad, AntiLagIdle,
    Camera, OrbitLeft, OrbitRight, ChaseIn, ChaseOut,
    Headlights, Ramp, IndicatorLeft, IndicatorRight, Hazard,
    Pause, Screenshot, Mute, Readout, Taa, Ao, Shadows, RayTracing, DebugView,
    Count
};
constexpr int kActions = static_cast<int>(Action::Count);

struct ActionInfo {
    const char* id;        // wrally.ini's key_<id>
    const char* label;     // for the menu
    const char* group;     // the menu's heading it goes under
    UINT defaultScan;
};
const ActionInfo& Info(Action a);

constexpr UINT kEscapeScan = 0x01;

struct Bindings {
    UINT scan[kActions];
    Bindings();            // the defaults
    UINT& operator[](Action a) { return scan[static_cast<int>(a)]; }
    UINT operator[](Action a) const { return scan[static_cast<int>(a)]; }
    // Whether its key is held now (unbound: never).
    bool Down(Action a) const;
    // `scan` to `a`; another action that had it is unbound and returned (Count: none).
    Action Bind(Action a, UINT scan);
};

// A key's name as the layout prints it ("Z", "Space", "Left"; "-" unbound).
std::string KeyName(UINT scan);
// The scan code of a WM_KEYDOWN's lParam (with 0xE000 for an extended key).
inline UINT ScanOf(LPARAM l) { return (static_cast<UINT>(l >> 16) & 0xff) | ((l & (1 << 24)) ? 0xe000u : 0u); }

} // namespace wrally::input
