#include "app/window.h"

#include <cstdio>
#include <vector>

#include "input/bindings.h"
#include "ui/imgui_layer.h"
#include "wrally/png/png.h"

namespace wrally::app {

namespace {

bool g_quit, g_escape;
UINT g_keyDown;          // the last key pressed (its scan code, input/bindings.h), 0 none
int g_width, g_height;   // the client area, as WM_SIZE last said

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (ui::ImGuiLayer::WndProc(h, m, w, l)) return true;
    if (m == WM_CLOSE) {
        g_quit = true;
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) {
        if (w == VK_ESCAPE) g_escape = true;
        g_keyDown = input::ScanOf(l);
    }
    if (m == WM_SIZE && LOWORD(l) && HIWORD(l)) {
        g_width = LOWORD(l);
        g_height = HIWORD(l);
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

std::string Window::Create(const wchar_t* title)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // the renderer's pixels are the screen's
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"W-Rally";
    RegisterClassW(&wc);
    // 3/4 of the screen's work area, 16:9
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = (work.right - work.left) * 3 / 4, height = width * 9 / 16;
    if (height > (work.bottom - work.top) * 3 / 4) height = (work.bottom - work.top) * 3 / 4, width = height * 16 / 9;
    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    m_hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) return "the window could not be created";
    GetClientRect(m_hwnd, &rc);
    g_width = rc.right - rc.left;
    g_height = rc.bottom - rc.top;
    return "";
}

bool Window::Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !g_quit;
}

void Window::SetFullscreen(bool on)
{
    if (on == m_fullscreen || !m_hwnd) return;
    if (on) {
        GetWindowPlacement(m_hwnd, &m_windowed);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(m_hwnd, &m_windowed);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    m_fullscreen = on;
}

void Window::Close() { g_quit = true; }
UINT Window::TakeKeyDown()
{
    const UINT k = g_keyDown;
    g_keyDown = 0;
    return k;
}
bool Window::TakeEscape()
{
    const bool e = g_escape;
    g_escape = false;
    return e;
}
int Window::Width() const { return g_width; }
int Window::Height() const { return g_height; }

bool Window::Screenshot(const std::string& path) const
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(m_hwnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool printed = PrintWindow(m_hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = w;
    bi.biHeight = -h;   // top-down, as a .png stores it
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
    GetDIBits(mdc, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(m_hwnd, wdc);
    if (!printed) return false;
    return png::WriteBgr(path, w, h, pixels.data(), static_cast<size_t>(stride));
}

} // namespace wrally::app
