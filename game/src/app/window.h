#pragma once
// The game's window (wrally::app): 3/4 of the screen's work area at 16:9, its messages pumped once a frame (ImGui's
// Win32 backend sees them first), closed by its close box or Esc.

#include <windows.h>

#include <string>

namespace wrally::app {

class Window {
public:
    // Returns "" or what failed.
    std::string Create(const wchar_t* title);
    HWND Handle() const { return m_hwnd; }
    // The pending messages handled. False once the window was closed (or Esc pressed).
    bool Pump();
    void Close();
    // The client area as it is now (a resize shows here at the next Pump).
    int Width() const;
    int Height() const;
    bool Focused() const { return GetForegroundWindow() == m_hwnd; }
    void SetTitle(const wchar_t* title) { SetWindowTextW(m_hwnd, title); }
    // The client area to a 24-bit .bmp, as shown (PrintWindow with the full content, which reads a flip-model swap
    // chain too).
    bool Screenshot(const std::string& path) const;

private:
    HWND m_hwnd = nullptr;
};

} // namespace wrally::app
