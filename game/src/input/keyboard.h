#pragma once
// The keyboard's helpers (wrally::input): which keys do what is input/bindings.h (Settings -> Controls); a Press is a
// key's going down, once.

#include <windows.h>

namespace wrally::input {

inline bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

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
