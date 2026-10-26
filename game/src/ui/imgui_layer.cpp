#include "ui/imgui_layer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "render/renderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace wrally::ui {

static_assert(sizeof(ImDrawVert) == sizeof(iface::UiVertex), "ImDrawVert is the UI pass's vertex");
static_assert(sizeof(ImDrawIdx) == 4, "32-bit indices (wrally_imconfig.h)");

ImGuiLayer::~ImGuiLayer()
{
    if (!m_init) return;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

std::string ImGuiLayer::Init(HWND window, render::Renderer& renderer)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // the game's cursor
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;      // the menus by the keyboard too
    ImGui_ImplWin32_Init(window);
    m_init = true;

    io.Fonts->AddFontDefault();
    char fonts[MAX_PATH] = {};
    GetWindowsDirectoryA(fonts, MAX_PATH);
    const std::string consolas = std::string(fonts) + "\\Fonts\\consolab.ttf";
    if (GetFileAttributesA(consolas.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_small = io.Fonts->AddFontFromFileTTF(consolas.c_str(), 48.0f);
        m_large = io.Fonts->AddFontFromFileTTF(consolas.c_str(), 200.0f);
    }
    if (!m_small) m_small = m_large = io.Fonts->Fonts[0];   // (ImGui's own, scaled up)
    const std::string segoe = std::string(fonts) + "\\Fonts\\segoeui.ttf";
    // The menus at the monitor's scale (the window is per-monitor DPI aware: its pixels are the screen's).
    m_scale = std::max(1.0f, static_cast<float>(GetDpiForWindow(window)) / 96.0f);
    if (GetFileAttributesA(segoe.c_str()) != INVALID_FILE_ATTRIBUTES)
        m_menu = io.Fonts->AddFontFromFileTTF(segoe.c_str(), 24.0f * m_scale);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.FrameRounding = 4.0f;
    style.WindowPadding = ImVec2(18, 14);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    style.ScaleAllSizes(m_scale);

    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    const iface::TextureHandle atlas = renderer.CreateTextureRgba(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    if (!atlas) return "the UI's font atlas: " + renderer.Error();
    io.Fonts->SetTexID(static_cast<ImTextureID>(atlas));
    io.Fonts->ClearTexData();
    return "";
}

bool ImGuiLayer::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, m, w, l);
}

void ImGuiLayer::NewFrame()
{
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

const iface::UiDrawList& ImGuiLayer::Render()
{
    ImGui::Render();
    const ImDrawData* d = ImGui::GetDrawData();
    m_vertices.clear();
    m_indices.clear();
    m_commands.clear();
    for (int n = 0; d && n < d->CmdListsCount; ++n) {
        const ImDrawList* l = d->CmdLists[n];
        const uint32_t vertexBase = static_cast<uint32_t>(m_vertices.size()), indexBase = static_cast<uint32_t>(m_indices.size());
        const size_t v0 = m_vertices.size(), i0 = m_indices.size();
        m_vertices.resize(v0 + size_t(l->VtxBuffer.Size));
        std::memcpy(m_vertices.data() + v0, l->VtxBuffer.Data, sizeof(ImDrawVert) * size_t(l->VtxBuffer.Size));
        m_indices.resize(i0 + size_t(l->IdxBuffer.Size));
        std::memcpy(m_indices.data() + i0, l->IdxBuffer.Data, sizeof(ImDrawIdx) * size_t(l->IdxBuffer.Size));
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback) continue;   // (none of ours)
            iface::UiCommand o{};
            o.texture = static_cast<iface::TextureHandle>(c.GetTexID());
            o.clip[0] = c.ClipRect.x - d->DisplayPos.x;
            o.clip[1] = c.ClipRect.y - d->DisplayPos.y;
            o.clip[2] = c.ClipRect.z - d->DisplayPos.x;
            o.clip[3] = c.ClipRect.w - d->DisplayPos.y;
            o.firstIndex = indexBase + c.IdxOffset;
            o.indexCount = c.ElemCount;
            o.vertexOffset = vertexBase + c.VtxOffset;
            m_commands.push_back(o);
        }
    }
    m_list = {m_vertices.data(), static_cast<uint32_t>(m_vertices.size()), m_indices.data(),
              static_cast<uint32_t>(m_indices.size()),  m_commands.data(), static_cast<uint32_t>(m_commands.size())};
    return m_list;
}

} // namespace wrally::ui
