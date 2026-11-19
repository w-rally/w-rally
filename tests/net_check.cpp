// net-check - the net module's wrapper (game/src/net/net.cpp) as the game uses it, against a running server: the
// records it reports arrive on a signals::Bus, on this thread, from Pump.
//
//   net-check <folder with WNet.dll> <server url> <token>
//
// Logs in, reads the first page of the car catalogue, and matches the first entry it lists by id with made-up bytes
// (so: another version of a known car, or unknown). Prints "net-check: all good" when every step got its answer.
// Exit code: 0 all good, 1 a step failed, 2 usage, 3 no module.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "net/net.h"
#include "signals/bus.h"
#include "wrally/iface/net_records.h"

using namespace wrally;
namespace rec = iface::netrec;

namespace {

// Pumps (a frame each 16 ms) until `done` or 30 s.
template <class F> bool PumpUntil(net::Net& n, uint32_t& tick, F&& done)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        n.Pump(++tick);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: net-check <folder with WNet.dll> <server url> <token>\n");
        return 2;
    }
    net::Net n;
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "wrally-net-check";
    if (const std::string e = n.Open(argv[1], argv[2], cache); !e.empty()) {
        std::printf("net-check: %s\n", e.c_str());
        return 3;
    }
    std::printf("module %s\n", n.ModuleName().c_str());

    signals::Bus bus;
    uint64_t waiting = 0;
    bool answered = false, failed = false;
    const std::thread::id mainThread = std::this_thread::get_id();
    bool offThread = false;
    std::vector<rec::CatalogueEntry> entries;
    auto onThread = [&] { offThread |= std::this_thread::get_id() != mainThread; };
    bus.Connect<rec::LoginDone>([&](const rec::LoginDone& r) {
        onThread();
        std::printf("LoginDone account=%s expiresIn=%u\n", r.account, r.expiresIn);
        answered |= r.requestId == waiting;
    });
    bus.Connect<rec::RequestFailed>([&](const rec::RequestFailed& r) {
        onThread();
        std::printf("RequestFailed op=%u status=%d code=%u: %s\n", r.op, r.status, r.serverCode, r.message);
        if (r.requestId == waiting) answered = failed = true;
    });
    bus.Connect<rec::CatalogueEntry>([&](const rec::CatalogueEntry& r) {
        onThread();
        std::printf("CatalogueEntry %s v%u \"%s\" flags %u\n", r.id, r.version, r.name, r.flags);
        if (r.requestId == waiting) entries.push_back(r);
    });
    bus.Connect<rec::CatalogueDone>([&](const rec::CatalogueDone& r) {
        onThread();
        std::printf("CatalogueDone revision %llu, %u entries, more %u, from cache %u\n",
                    static_cast<unsigned long long>(r.revision), r.count, r.more, r.fromCache);
        answered |= r.requestId == waiting;
    });
    bus.Connect<rec::ContentMatch>([&](const rec::ContentMatch& r) {
        onThread();
        std::printf("ContentMatch %s: result %u, id %s, latest version %u\n", r.localId, r.result, r.id, r.latestVersion);
        answered |= r.requestId == waiting;
    });
    n.Connect(&bus);

    uint32_t tick = 0;
    auto step = [&](const char* what, uint64_t id, const std::string& error) {
        if (!id) {
            std::printf("net-check: %s refused: %s\n", what, error.c_str());
            return false;
        }
        waiting = id;
        answered = failed = false;
        if (!PumpUntil(n, tick, [&] { return answered; })) {
            std::printf("net-check: %s: no answer in 30 s\n", what);
            return false;
        }
        return !failed;
    };

    std::string error;
    bool ok = step("login", n.Login(argv[3], &error), error);
    ok = ok && n.SessionState() == iface::kNetReady;
    ok = ok && step("catalogue", n.QueryCatalogue(iface::ContentKind::Car, 10, "", "", &error), error);
    ok = ok && !entries.empty();
    if (ok) {
        const std::string id = entries.front().id;
        const std::vector<uint8_t> bytes = {'n', 'o', 't', ' ', 'a', ' ', 'c', 'a', 'r'};
        ok = step("match", n.MatchContent(iface::ContentKind::Car, id, bytes, &error), error);
    }
    if (offThread) {
        std::printf("net-check: a record arrived on another thread\n");
        ok = false;
    }
    std::printf("%s", n.TakeLog().c_str());
    std::printf(ok ? "net-check: all good\n" : "net-check: FAILED\n");
    return ok ? 0 : 1;
}
