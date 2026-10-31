#include "render/renderer.h"

#include "wrally/modules/file_host.h"
#include "wrally/modules/module_host.h"

namespace wrally::render {

using iface::Status;

struct Renderer::Impl {
    modules::ModuleHost host;
    std::unique_ptr<modules::FileHost> files;
    iface::RendererOutput out{};
    std::mutex logLock;
    std::string log;
    std::string error;
};

Renderer::Renderer() = default;

Renderer::~Renderer()
{
    if (m_api && m_instance) m_api->destroy(m_instance);
}

std::string Renderer::Open(const std::filesystem::path& modulesDir, const std::string& moduleId, void* window,
                           uint32_t width, uint32_t height, const iface::RenderSettings& settings)
{
    m = std::make_unique<Impl>();
    m->host.Scan(modulesDir);
    Impl* impl = m.get();
    // The renderer reads nothing through the VFS yet (the game hands it bytes); its root is the game's folder.
    m->files = std::make_unique<modules::FileHost>(modulesDir.parent_path().parent_path(),
                                                   modulesDir.parent_path().parent_path() / "cache",
                                                   [impl](iface::LogLevel, const char* text) {
                                                       std::lock_guard<std::mutex> lock(impl->logLock);
                                                       impl->log += text;
                                                   });
    std::string tried;
    for (const modules::ModuleEntry* e : m->host.Available(iface::ModuleKind::Renderer)) {
        iface::RendererInput in{};
        iface::RendererOutput out{};
        iface::InitHeader(in, iface::ModuleKind::Renderer, iface::kRendererInterfaceVer);
        iface::InitHeader(out, iface::ModuleKind::Renderer, iface::kRendererInterfaceVer);
        in.host = m->files->Services(moduleId);
        in.window = window;
        in.width = width, in.height = height;
        in.settings = settings;
        std::string error;
        if (m->host.Init(*e, &in.header, &out.header, &error) != Status::Ok) {
            tried += "\n  " + e->path.filename().string() + ": " + error;
            continue;
        }
        if (moduleId == out.info.id) {
            m->out = out;
            break;
        }
        tried += "\n  " + e->path.filename().string() + ": id " + out.info.id;
        out.api->destroy(out.instance);
    }
    if (!m->out.instance) {
        for (const modules::ModuleEntry& e : m->host.Entries())
            if (!e.compatible && e.kind == iface::ModuleKind::Renderer)
                tried += "\n  " + e.path.filename().string() + ": interface " + std::to_string(e.interfaceVer) +
                         ", the game needs " + std::to_string(iface::kRendererInterfaceVer);
        return "no renderer module \"" + moduleId + "\" in " + modulesDir.string() + (tried.empty() ? "" : ":") + tried;
    }
    m_api = m->out.api;
    m_instance = m->out.instance;
    return "";
}

std::string Renderer::Error() const { return m ? m->error : "no renderer"; }

std::string Renderer::TakeLog()
{
    if (!m) return "";
    std::lock_guard<std::mutex> lock(m->logLock);
    std::string s;
    s.swap(m->log);
    return s;
}

std::string Renderer::ModuleName() const
{
    if (!m_instance) return "none";
    return std::string(m->out.info.name) + " (" + m->out.info.id + ")";
}

namespace {

template <class H> H Made(Status s, H h, const iface::RendererApi* api, iface::Renderer* r, std::string& error)
{
    if (s == Status::Ok) return error.clear(), h;
    error = api->lastError(r);
    return 0;
}

} // namespace

iface::SceneHandle Renderer::LoadScene(const void* ngg, uint64_t bytes)
{
    if (!m_instance) return 0;
    iface::SceneHandle h = 0;
    const Status s = m_api->loadScene(m_instance, ngg, bytes, &h);
    return Made(s, h, m_api, m_instance, m->error);
}

iface::TextureHandle Renderer::CreateTexture(const void* dds, uint64_t bytes)
{
    if (!m_instance) return 0;
    iface::TextureHandle h = 0;
    const Status s = m_api->createTexture(m_instance, dds, bytes, &h);
    return Made(s, h, m_api, m_instance, m->error);
}

iface::TextureHandle Renderer::CreateTextureRgba(const void* rgba, uint32_t width, uint32_t height)
{
    if (!m_instance) return 0;
    iface::TextureHandle h = 0;
    const Status s = m_api->createTextureRgba(m_instance, rgba, width, height, &h);
    return Made(s, h, m_api, m_instance, m->error);
}

void Renderer::ReleaseTexture(iface::TextureHandle t)
{
    if (m_instance && t) m_api->releaseTexture(m_instance, t);
}

bool Renderer::UpdateTextureRgba(iface::TextureHandle t, const void* rgba)
{
    return m_instance && Made(m_api->updateTextureRgba(m_instance, t, rgba), 1, m_api, m_instance, m->error);
}

iface::ModelHandle Renderer::CreateModel(const iface::ModelDesc& d)
{
    if (!m_instance) return 0;
    iface::ModelHandle h = 0;
    const Status s = m_api->createModel(m_instance, &d, &h);
    return Made(s, h, m_api, m_instance, m->error);
}

iface::InstanceHandle Renderer::CreateInstance(iface::ModelHandle model)
{
    if (!m_instance) return 0;
    iface::InstanceHandle h = 0;
    const Status s = m_api->createInstance(m_instance, model, &h);
    return Made(s, h, m_api, m_instance, m->error);
}

bool Renderer::ApplySettings(const iface::RenderSettings& s)
{
    return m_instance && Made(m_api->applySettings(m_instance, &s), 1, m_api, m_instance, m->error);
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
    if (m_instance) m_api->resize(m_instance, width, height);
}

bool Renderer::RenderFrame(const iface::FrameDesc& f)
{
    return m_instance && Made(m_api->renderFrame(m_instance, &f), 1, m_api, m_instance, m->error);
}

void Renderer::Present()
{
    if (m_instance) m_api->present(m_instance);
}

iface::RenderStats Renderer::Stats()
{
    iface::RenderStats s{};
    if (m_instance) m_api->stats(m_instance, &s);
    return s;
}

void Renderer::CaptureFrame(const std::string& path)
{
    if (m_instance) m_api->captureFrame(m_instance, path.c_str());
}

void Renderer::SetLegacyProfile(int32_t mode)
{
    if (m_instance) m_api->setLegacyProfile(m_instance, mode);
}

void Renderer::SetDebugView(int32_t mode)
{
    if (m_instance) m_api->setDebugView(m_instance, mode);
}

const char* Renderer::DebugViewName(int32_t mode) const
{
    const char* n = m_api ? m_api->debugViewName(mode) : nullptr;
    return n ? n : "?";
}

} // namespace wrally::render
