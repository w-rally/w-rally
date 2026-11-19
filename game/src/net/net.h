#pragma once
// wrally::net::Net - the game's side of the Net module (WNet.dll, iface/net.h): finds the module DLL in modules/net,
// initialises it against a server, and re-emits what it reports (iface/net_records.h: logged in, catalogue entries,
// content matches, failures) on a signals::Bus, unchanged - as physics::Physics does with the physics' records.
//
// Everything on the game's main thread: the requests, and Pump once a frame, which is when the records arrive on the
// bus. WNet does its network work on threads of its own and never calls back from them.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wrally/iface/net.h"

namespace signals {
class Bus;
}

namespace wrally::net {

class Net {
public:
    Net();
    ~Net();
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    // The net module among the DLLs in `modulesDir`, talking to `serverUrl` ("https://..."; plain http only for
    // localhost). `cacheRoot`: where it gets its cache folder (cacheRoot/wnet). Returns "" or what is wrong.
    std::string Open(const std::filesystem::path& modulesDir, const std::string& serverUrl,
                     const std::filesystem::path& cacheRoot);
    bool IsOpen() const;
    // The module's records on `bus` from the next Pump on (nullptr: none). The bus must outlive this Net, or be
    // disconnected first.
    void Connect(signals::Bus* bus);
    // Once a frame: the records that are ready, on the bus. `tick` goes into their Stamp. Returns how many.
    uint32_t Pump(uint32_t tick);

    // The requests. Each returns its request id (0: refused at once - `error` says why); the result arrives as records
    // carrying that id (LoginDone / CatalogueEntry..., CatalogueDone / ContentMatch, or RequestFailed).
    uint64_t Login(const std::string& token, std::string* error = nullptr);
    void Logout();
    uint64_t QueryCatalogue(iface::ContentKind kind, uint32_t limit, const std::string& filter,
                            const std::string& cursor, std::string* error = nullptr);
    uint64_t MatchContent(iface::ContentKind kind, const std::string& localId, const std::vector<uint8_t>& canonical,
                          std::string* error = nullptr);
    void Cancel(uint64_t request);

    uint32_t SessionState() const;   // iface::kNet*
    std::string ModuleName() const;
    // What the module logged since the last call.
    std::string TakeLog();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// The records' names, for logs: "LoginDone", "CatalogueEntry", ... ("" an unknown kind).
const char* RecordName(uint32_t kind);

} // namespace wrally::net
