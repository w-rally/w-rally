// ---------------------------------------------------------------------------
// wrally/iface/net.h - the Net module interface (WNet.dll).
//
// WNet talks to the W-Rally server for the game: it logs the player in and
// reads the server's car and track catalogue - the canonical list of content
// the server holds, which ranked runs are checked against - and tells the game
// whether a local car or track matches a catalogue entry. Online races
// (registering local content, signed input streaming, leaderboards) come in
// later versions of this interface.
//
// This header says WHAT WNet does, never HOW: its protocol, crypto and
// anti-tamper machinery are its own business and do not appear here.
//
// REQUESTS ARE ASYNCHRONOUS. Every request call returns at once with a
// requestId (never 0); its result arrives later as records (net_records.h)
// through NetInput::sink, each carrying that requestId. The game re-emits them
// on its own signals bus unchanged, like the physics' records.
//
// THREADING: every call from one thread, the game's main thread. WNet does
// its network work on threads of its own but NEVER calls the sink from them:
// finished results wait inside WNet until the game calls pump, which delivers
// them through the sink on the calling thread. pump never blocks on the
// network; call it once per frame.
//
// STRINGS passed in are copied before the call returns.
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/common.h"

namespace wrally::iface {

// 1: login, the content catalogue, content matching.
constexpr uint32_t kNetInterfaceVer = 1;

struct NetModule;   // the module's instance

// NetCaps (ModuleInfo::caps)
constexpr uint64_t kNetCapCatalogue = 1ull << 0;   // queryCatalogue / matchContent

#pragma pack(push, 4)

enum class ContentKind : uint32_t { Car = 1, Track = 2 };

// Where WNet delivers its result records: kind is a net_records.h kKind,
// `data` that record, valid only during the call. Called only from inside
// NetApi::pump. Unknown kinds are ignored by the game - which is how a newer
// module adds records without a version bump.
struct NetRecordSink {
    void* ctx;
    void (WRALLY_CALL* onRecord)(void* ctx, uint32_t kind, const void* data, uint32_t bytes);
};

// Session states (NetApi::sessionState).
constexpr uint32_t kNetLoggedOut = 0;
constexpr uint32_t kNetLoggingIn = 1;
constexpr uint32_t kNetReady     = 2;   // logged in; catalogue requests go out
constexpr uint32_t kNetExpired   = 3;   // the server ended the session; WNet logs in again by itself if it can

// Login. The token proves who the player is; how it was obtained (password,
// e-mail link, a platform account) is the server's business, not WNet's.
struct LoginDesc {
    const char* token;        // UTF-8, never "" - the account's login token
};

// One page of the catalogue. An empty cursor starts at the top; the
// CatalogueDone record carries the cursor of the next page.
struct CatalogueQuery {
    ContentKind kind;
    uint32_t    limit;        // entries per page, 0: the server's default; the server may send fewer
    const char* filter;       // "" all; else a substring of the entry's id or name
    const char* cursor;       // "" the first page
};

// A local car or track to look up. `canonical` is the content's canonical
// serialization - everything the simulation reads, in the game's canonical
// form, not raw file bytes - so packaging never changes it. WNet hashes it
// (the scheme is WNet's) and asks the server which entry it is.
struct ContentDesc {
    ContentKind kind;
    uint32_t    reserved;     // 0
    const char* localId;      // the game's id for it ("s_i2003", "track-41"), so the server can tell "another version"
    const void* canonical;
    uint64_t    bytes;
};

#pragma pack(pop)

// ------------------------------------------------------------- the table
struct NetApi {
    void        (WRALLY_CALL* destroy)(NetModule*);
    const char* (WRALLY_CALL* lastError)(NetModule*);        // never null: why the last call returned an error

    // Delivers every finished result through the sink, in the order the
    // results finished. `tick` goes into each record's Stamp. Returns how many
    // records it delivered.
    uint32_t (WRALLY_CALL* pump)(NetModule*, uint32_t tick);

    // kNet* session state, as of the last pump.
    uint32_t (WRALLY_CALL* sessionState)(NetModule*);

    // Result: LoginDone or RequestFailed.
    Status (WRALLY_CALL* login)(NetModule*, const LoginDesc*, uint64_t* requestId);
    // Ends the session; pending requests fail with ErrInvalidArg.
    void   (WRALLY_CALL* logout)(NetModule*);

    // Needs a session (login first; requests made while logging in wait for
    // it). Result: one CatalogueEntry per entry, then CatalogueDone - or
    // RequestFailed.
    Status (WRALLY_CALL* queryCatalogue)(NetModule*, const CatalogueQuery*, uint64_t* requestId);
    // Needs a session. Result: ContentMatch or RequestFailed.
    Status (WRALLY_CALL* matchContent)(NetModule*, const ContentDesc*, uint64_t* requestId);

    // The request's result is dropped (no record is delivered for it). Unknown
    // or finished ids are ignored.
    void   (WRALLY_CALL* cancel)(NetModule*, uint64_t requestId);
};

struct NetInput {
    StructHeader  header;     // kind Net, kNetInterfaceVer
    HostServices  host;
    const char*   serverUrl;  // "https://..." - plain http only for a server on localhost (tests)
    NetRecordSink sink;       // onRecord non-null
};

struct NetOutput {
    StructHeader  header;
    ModuleInfo    info;
    const NetApi* api;
    NetModule*    instance;
    // The protocol revision WNet speaks to the server. Online results record it.
    uint32_t      protocolVersion;
    uint32_t      reserved;
};

} // namespace wrally::iface
