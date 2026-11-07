#pragma once
// The game's window (wrally::app): 3/4 of the screen's work area at 16:9 or the whole screen, its messages pumped once a frame (ImGui's
// Win32 backend sees them first), closed by its close box. Esc is the game's (its menus): TakeEscape.

#include <windows.h>

#include <string>

namespace wrally::app {

class Window {
public:
    // Returns "" or what failed.
    std::string Create(const wchar_t* title);
    HWND Handle() const { return m_hwnd; }
    // The pending messages handled. False once the window was closed.
    bool Pump();
    // Whether Esc was pressed since the last call (its auto-repeat not counted).
    bool TakeEscape();
    // The scan code of the last key pressed since the last call (input/bindings.h), 0 none - for binding a key.
    UINT TakeKeyDown();
    void Close();
    // The client area as it is now (a resize shows here at the next Pump).
    int Width() const;
    int Height() const;
    bool Focused() const { return GetForegroundWindow() == m_hwnd; }
    void SetTitle(const wchar_t* title) { SetWindowTextW(m_hwnd, title); }
    // The whole of its monitor, borderless (the window's place kept), or back to that window. The new size shows at the
    // next Pump, as a resize does.
    void SetFullscreen(bool on);
    // The client area to a .png, as shown (PrintWindow with the full content, which reads a flip-model swap
    // chain too).
    bool Screenshot(const std::string& path) const;

private:
    HWND m_hwnd = nullptr;
    bool m_fullscreen = false;
    WINDOWPLACEMENT m_windowed{sizeof(WINDOWPLACEMENT)};   // where the window was before the full screen
};

} // namespace wrally::app
