#pragma once
// wrally::ui::ImGuiLayer - Dear ImGui for the game (docs/design.md decision 1): its context and Win32 input, its font
// atlas uploaded to the renderer (createTextureRgba), and each frame's draw data flattened into the renderer's
// UiDrawList (the UI pass after the frame).
//
// The fonts: ImGui's own for small text, and Consolas Bold for the readout (F1); Segoe UI for the menus, which the
// keyboard (arrows, Enter, Esc) works as well as the mouse; its italics and semibold for the main menu and the HUD, the
// black italic again large for the HUD's countdown, so its digits are drawn from big glyphs rather than blown-up ones.

#include <windows.h>

#include <string>
#include <vector>

#include "wrally/iface/renderer.h"

struct ImFont;

namespace wrally::render {
class Renderer;
}

namespace wrally::ui {

class ImGuiLayer {
public:
    ~ImGuiLayer();
    // Returns "" or what is wrong.
    std::string Init(HWND window, render::Renderer& renderer);
    // Forward the window's messages (true: ImGui took it).
    static bool WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

    void NewFrame();
    // The frame's draw data as the renderer takes it (valid until the next Render), the build's version and commit in
    // its bottom right corner.
    const iface::UiDrawList& Render();

    ImFont* HudSmall() const { return m_small; }
    // The HUD's countdown: the wordmark's face loaded large, its digits and capitals only. Never null.
    ImFont* HudDisplay() const { return m_display; }
    // The menus' face (Segoe UI; null: ImGui's own).
    ImFont* MenuFont() const { return m_menu; }
    // The main menu's: the wordmark (Segoe UI Black Italic), its items (Bold Italic), its labels and text (Semibold).
    // Never null (the menus' face, else ImGui's own).
    ImFont* Wordmark() const { return m_wordmark; }
    ImFont* MenuItem() const { return m_menuItem; }
    ImFont* MenuText() const { return m_menuText; }
    // The monitor's scale (its DPI / 96, at least 1): the menus' sizes are multiplied by it.
    float Scale() const { return m_scale; }

private:
    bool m_init = false;
    ImFont* m_small = nullptr;
    ImFont* m_display = nullptr;
    ImFont* m_menu = nullptr;
    ImFont* m_wordmark = nullptr;
    ImFont* m_menuItem = nullptr;
    ImFont* m_menuText = nullptr;
    float m_scale = 1.0f;
    std::vector<iface::UiVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<iface::UiCommand> m_commands;
    iface::UiDrawList m_list{};
};

} // namespace wrally::ui
