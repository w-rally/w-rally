#include "net/net.h"

#include <cstring>
#include <mutex>

#include "signals/bus.h"
#include "wrally/iface/net_records.h"
#include "wrally/modules/file_host.h"
#include "wrally/modules/module_host.h"
#include "wrally/vfs/vfs.h"

namespace wrally::net {

using iface::Status;
namespace rec = iface::netrec;

struct Net::Impl {
    modules::ModuleHost host;
    std::unique_ptr<modules::FileHost> files;
    iface::NetOutput out{};
    signals::Bus* bus = nullptr;
    std::mutex logLock;   // the module may log from its own threads
    std::string log;

    ~Impl()
    {
        if (out.instance) out.api->destroy(out.instance);
    }

    template <class R> void Emit(const void* data, uint32_t bytes)
    {
        if (bytes != sizeof(R) || !bus) return;
        R r;
        std::memcpy(&r, data, sizeof r);   // (the module's buffer has no alignment promise)
        bus->Emit(r);
    }

    static void WRALLY_CALL OnRecord(void* ctx, uint32_t kind, const void* data, uint32_t bytes)
    {
        Impl& m = *static_cast<Impl*>(ctx);
        switch (kind) {
        case rec::RequestFailed::kKind: return m.Emit<rec::RequestFailed>(data, bytes);
        case rec::LoginDone::kKind: return m.Emit<rec::LoginDone>(data, bytes);
        case rec::SessionExpired::kKind: return m.Emit<rec::SessionExpired>(data, bytes);
        case rec::CatalogueEntry::kKind: return m.Emit<rec::CatalogueEntry>(data, bytes);
        case rec::CatalogueDone::kKind: return m.Emit<rec::CatalogueDone>(data, bytes);
        case rec::ContentMatch::kKind: return m.Emit<rec::ContentMatch>(data, bytes);
        default: return;   // a newer module's record: ignored (net_records.h)
        }
    }

    uint64_t Refused(Status s, std::string* error) const
    {
        if (error) *error = std::string(modules::StatusName(s)) + ": " + out.api->lastError(out.instance);
        return 0;
    }
};

Net::Net() = default;
Net::~Net() = default;

std::string Net::Open(const std::filesystem::path& modulesDir, const std::string& serverUrl,
                      const std::filesystem::path& cacheRoot)
{
    m = std::make_unique<Impl>();
    m->host.Scan(modulesDir);
    Impl* impl = m.get();
    // WNet reads no game files: the services are for its log and its cache folder.
    m->files = std::make_unique<modules::FileHost>(
        std::make_shared<vfs::Vfs>(std::vector<vfs::Layer>{}, std::filesystem::path()), cacheRoot,
        [impl](iface::LogLevel level, const char* text) {
            if (level < iface::LogLevel::Info) return;
            std::lock_guard<std::mutex> lock(impl->logLock);
            impl->log += text;
            impl->log += '\n';
        });

    std::string tried;
    for (const modules::ModuleEntry* e : m->host.Available(iface::ModuleKind::Net)) {
        iface::NetInput in{};
        iface::NetOutput out{};
        iface::InitHeader(in, iface::ModuleKind::Net, iface::kNetInterfaceVer);
        iface::InitHeader(out, iface::ModuleKind::Net, iface::kNetInterfaceVer);
        in.host = m->files->Services("wnet");
        in.serverUrl = serverUrl.c_str();
        in.sink = {m.get(), &Impl::OnRecord};
        std::string error;
        if (m->host.Init(*e, &in.header, &out.header, &error) != Status::Ok) {
            tried += "\n  " + e->path.filename().string() + ": " + error;
            continue;
        }
        m->out = out;
        break;
    }
    if (!m->out.instance) {
        for (const modules::ModuleEntry& e : m->host.Entries())
            if (!e.compatible && e.kind == iface::ModuleKind::Net)
                tried += "\n  " + e.path.filename().string() + ": interface " + std::to_string(e.interfaceVer) +
                         ", the game needs " + std::to_string(iface::kNetInterfaceVer);
        std::string why = "no net module in " + modulesDir.string() + (tried.empty() ? "" : ":") + tried;
        m.reset();
        return why;
    }
    return "";
}

bool Net::IsOpen() const
{
    return m && m->out.instance;
}

void Net::Connect(signals::Bus* bus)
{
    if (m) m->bus = bus;
}

uint32_t Net::Pump(uint32_t tick)
{
    return IsOpen() ? m->out.api->pump(m->out.instance, tick) : 0;
}

uint64_t Net::Login(const std::string& token, std::string* error)
{
    if (!IsOpen()) return error ? (*error = "no net module", 0) : 0;
    iface::LoginDesc d{token.c_str()};
    uint64_t id = 0;
    const Status s = m->out.api->login(m->out.instance, &d, &id);
    return s == Status::Ok ? id : m->Refused(s, error);
}

void Net::Logout()
{
    if (IsOpen()) m->out.api->logout(m->out.instance);
}

uint64_t Net::QueryCatalogue(iface::ContentKind kind, uint32_t limit, const std::string& filter,
                             const std::string& cursor, std::string* error)
{
    if (!IsOpen()) return error ? (*error = "no net module", 0) : 0;
    iface::CatalogueQuery q{kind, limit, filter.c_str(), cursor.c_str()};
    uint64_t id = 0;
    const Status s = m->out.api->queryCatalogue(m->out.instance, &q, &id);
    return s == Status::Ok ? id : m->Refused(s, error);
}

uint64_t Net::MatchContent(iface::ContentKind kind, const std::string& localId, const std::vector<uint8_t>& canonical,
                           std::string* error)
{
    if (!IsOpen()) return error ? (*error = "no net module", 0) : 0;
    iface::ContentDesc c{kind, 0, localId.c_str(), canonical.data(), canonical.size()};
    uint64_t id = 0;
    const Status s = m->out.api->matchContent(m->out.instance, &c, &id);
    return s == Status::Ok ? id : m->Refused(s, error);
}

void Net::Cancel(uint64_t request)
{
    if (IsOpen()) m->out.api->cancel(m->out.instance, request);
}

uint32_t Net::SessionState() const
{
    return IsOpen() ? m->out.api->sessionState(m->out.instance) : iface::kNetLoggedOut;
}

std::string Net::ModuleName() const
{
    return IsOpen() ? std::string(m->out.info.name) : std::string();
}

std::string Net::TakeLog()
{
    if (!m) return {};
    std::lock_guard<std::mutex> lock(m->logLock);
    std::string s;
    s.swap(m->log);
    return s;
}

const char* RecordName(uint32_t kind)
{
    switch (kind) {
    case rec::RequestFailed::kKind: return "RequestFailed";
    case rec::LoginDone::kKind: return "LoginDone";
    case rec::SessionExpired::kKind: return "SessionExpired";
    case rec::CatalogueEntry::kKind: return "CatalogueEntry";
    case rec::CatalogueDone::kKind: return "CatalogueDone";
    case rec::ContentMatch::kKind: return "ContentMatch";
    default: return "";
    }
}

} // namespace wrally::net
