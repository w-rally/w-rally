#include "input/bindings.h"

namespace wrally::input {

namespace {

// The defaults: phys-drive's keys (US names in the comments).
const ActionInfo kInfo[kActions] = {
    {"throttle", "Throttle", "Driving", 0x1e},              // A
    {"brake", "Brake", "Driving", 0x2c},                    // Z
    {"handbrake", "Handbrake", "Driving", 0x39},            // Space
    {"steer_left", "Steer left", "Driving", 0xe04b},        // Left
    {"steer_right", "Steer right", "Driving", 0xe04d},      // Right
    {"gear_up", "Gear up", "Driving", 0x1f},                // S
    {"gear_down", "Gear down", "Driving", 0x2d},            // X
    {"back_on_road", "Back on the road", "Driving", 0x13},  // R
    {"anti_lag_idle", "Anti-lag at standstill", "Driving", 0x31},   // N
    {"camera", "Next camera", "Camera", 0x2e},              // C
    {"orbit_left", "Beside camera left", "Camera", 0x33},   // ,
    {"orbit_right", "Beside camera right", "Camera", 0x34}, // .
    {"chase_in", "Chase camera in", "Camera", 0x1a},        // [
    {"chase_out", "Chase camera out", "Camera", 0x1b},      // ]
    {"pause", "Pause", "Other", 0x19},                      // P
    {"screenshot", "Screenshot", "Other", 0x3c},            // F2
    {"mute", "Mute", "Other", 0x32},                        // M
    {"readout", "Readout", "Debug", 0x3b},                  // F1
    {"taa", "TAA", "Debug", 0x14},                          // T
    {"ao", "Ambient occlusion", "Debug", 0x18},             // O
    {"shadows", "Shadows", "Debug", 0x26},                  // L
    {"ray_tracing", "Ray tracing", "Debug", 0x25},          // K
    {"debug_view", "Debug view", "Debug", 0x2f},            // V
};

} // namespace

const ActionInfo& Info(Action a) { return kInfo[static_cast<int>(a)]; }

Bindings::Bindings()
{
    for (int i = 0; i < kActions; ++i) scan[i] = kInfo[i].defaultScan;
}

bool Bindings::Down(Action a) const
{
    const UINT s = (*this)[a];
    if (!s) return false;
    // The layout asked each time, so switching it while driving works. MAPVK_VSC_TO_VK_EX takes the 0xE0 prefix.
    const UINT vk = MapVirtualKeyExW(s, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
    return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

Action Bindings::Bind(Action a, UINT s)
{
    Action taken = Action::Count;
    for (int i = 0; i < kActions; ++i)
        if (i != static_cast<int>(a) && s && scan[i] == s) scan[i] = 0, taken = static_cast<Action>(i);
    (*this)[a] = s;
    return taken;
}

std::string KeyName(UINT s)
{
    if (!s) return "-";
    const LONG l = static_cast<LONG>((s & 0xff) << 16 | ((s & 0xe000) ? 1u << 24 : 0u));
    char name[64] = {};
    if (GetKeyNameTextA(l, name, sizeof name) > 0) return name;
    char hex[16];
    wsprintfA(hex, "key %#x", s);
    return hex;
}

} // namespace wrally::input
