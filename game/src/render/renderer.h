#pragma once
// wrally::render::Renderer - the game's side of a renderer module (docs/design.md 3): finds the module DLL, initialises
// it on the game's window and passes the game's calls to its function table (wrally/iface/renderer.h). The frame is the
// interface's FrameDesc, as it is.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "wrally/iface/renderer.h"

namespace wrally::render {

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // The module with id `moduleId` ("wrenderer") among the renderer DLLs in `modulesDir`, on `window` (an HWND).
    // Returns "" or what is wrong.
    std::string Open(const std::filesystem::path& modulesDir, const std::string& moduleId, void* window, uint32_t width,
                     uint32_t height, const iface::RenderSettings& settings);

    // The module's table and instance, for the calls this wrapper does not spell out. Null until Open succeeded.
    const iface::RendererApi* Api() const { return m_api; }
    iface::Renderer* Instance() const { return m_instance; }
    // The last call's error ("" none).
    std::string Error() const;
    // What the module logged since the last call.
    std::string TakeLog();
    std::string ModuleName() const;

    iface::SceneHandle LoadScene(const void* ngg, uint64_t bytes);   // 0 on failure (Error())
    iface::TextureHandle CreateTexture(const void* dds, uint64_t bytes);
    iface::TextureHandle CreateTextureRgba(const void* rgba, uint32_t width, uint32_t height);
    iface::ModelHandle CreateModel(const iface::ModelDesc& d);
    iface::InstanceHandle CreateInstance(iface::ModelHandle model);
    bool ApplySettings(const iface::RenderSettings& s);
    void Resize(uint32_t width, uint32_t height);
    bool RenderFrame(const iface::FrameDesc& f);
    void Present();
    iface::RenderStats Stats();
    void CaptureFrame(const std::string& path);
    void SetDebugView(int32_t mode);
    // An automatic .lbs import's card lighting (RendererApi::setLegacyProfile): 0 off, 1 A baked, 2 B occlusion.
    void SetLegacyProfile(int32_t mode);
    const char* DebugViewName(int32_t mode) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    const iface::RendererApi* m_api = nullptr;
    iface::Renderer* m_instance = nullptr;
};

} // namespace wrally::render
