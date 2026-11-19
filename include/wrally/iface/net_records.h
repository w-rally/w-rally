// ---------------------------------------------------------------------------
// wrally/iface/net_records.h - what the Net module tells the game, through
// NetInput::sink (iface/net.h), from inside NetApi::pump.
//
// Same rules as records.h: plain structs, fixed layout, sizes static_assert'ed,
// no pointers. Text is UTF-8 in fixed arrays, always NUL-terminated
// (truncated if longer). Every record starts with a Stamp: car 0, tick the one
// passed to pump. Every result of a request carries its requestId.
//
// ADDING A RECORD is not an interface change: the game ignores kinds it does
// not know. CHANGING one is: bump kNetInterfaceVer.
// ---------------------------------------------------------------------------
#pragma once

#include "wrally/iface/records.h"

#include <cstdint>

namespace wrally::iface::netrec {

using records::Kind;
using records::Stamp;

// Which request a RequestFailed is about.
constexpr uint32_t kOpLogin     = 1;
constexpr uint32_t kOpCatalogue = 2;
constexpr uint32_t kOpMatch     = 3;

// A content hash, as the catalogue and ContentMatch carry it. scheme 1: the
// first 32 bytes of BLAKE2b-512 over "wnet-content-v1/car" or ".../track", a
// 0 byte, and the canonical serialization.
constexpr uint32_t kHashBytes = 32;

// ContentMatch::result
constexpr uint32_t kMatchUnknown      = 0;   // neither the hash nor the local id is in the catalogue
constexpr uint32_t kMatchExact        = 1;   // the hash is a catalogue entry's: id and version below
constexpr uint32_t kMatchOtherVersion = 2;   // the local id is an entry's, but the hash is none of its versions

// CatalogueEntry::flags
constexpr uint32_t kEntryRanked = 1u << 0;   // ranked runs allowed on this version
constexpr uint32_t kEntryLatest = 1u << 1;   // this is the entry's newest version

#pragma pack(push, 4)

// A request did not succeed. status: ErrInvalidArg (refused by the server, or
// cancelled by logout), ErrIo (the network: no connection, a timeout, a
// server that does not answer), ErrVersion (the server no longer accepts this
// WNet build), ErrInternal (anything else).
struct RequestFailed {
    static constexpr uint32_t kKind = Kind('N', 'E', 'r', 'r');
    Stamp    stamp;
    uint64_t requestId;
    uint32_t op;              // kOp*
    int32_t  status;          // a Status
    uint32_t serverCode;      // the server's error code, 0 none (a network failure)
    uint32_t httpStatus;      // 0 none
    char     message[256];
};
static_assert(sizeof(RequestFailed) == 288, "RequestFailed layout");

struct LoginDone {
    static constexpr uint32_t kKind = Kind('N', 'L', 'o', 'g');
    Stamp    stamp;
    uint64_t requestId;
    uint32_t expiresIn;       // seconds until the session ends unless renewed
    uint32_t reserved;
    char     account[64];     // the account's display name
};
static_assert(sizeof(LoginDone) == 88, "LoginDone layout");

// The session ended (expired or revoked). relogin 1: WNet is logging in again
// with the token it has; requests wait for it. 0: login again.
struct SessionExpired {
    static constexpr uint32_t kKind = Kind('N', 'E', 'x', 'p');
    Stamp    stamp;
    uint32_t relogin;
    uint32_t reserved;
};
static_assert(sizeof(SessionExpired) == 16, "SessionExpired layout");

// One catalogue entry (one version of a car or track).
struct CatalogueEntry {
    static constexpr uint32_t kKind = Kind('N', 'C', 'a', 't');
    Stamp    stamp;
    uint64_t requestId;
    uint32_t kind;            // ContentKind
    uint32_t version;
    uint32_t hashScheme;
    uint32_t flags;           // kEntry*
    char     id[64];          // the server's id
    char     name[96];        // for the UI
    uint8_t  hash[kHashBytes];
};
static_assert(sizeof(CatalogueEntry) == 224, "CatalogueEntry layout");

// The end of a catalogue page: after its CatalogueEntry records.
struct CatalogueDone {
    static constexpr uint32_t kKind = Kind('N', 'C', 'a', 'D');
    Stamp    stamp;
    uint64_t requestId;
    uint64_t revision;        // the catalogue's revision; it changes whenever the catalogue does
    uint32_t kind;            // ContentKind
    uint32_t count;           // CatalogueEntry records delivered for this page
    uint32_t more;            // 1: nextCursor has the next page
    uint32_t fromCache;       // 1: the server said the page had not changed; served from WNet's cache
    char     nextCursor[64];
};
static_assert(sizeof(CatalogueDone) == 104, "CatalogueDone layout");

struct ContentMatch {
    static constexpr uint32_t kKind = Kind('N', 'M', 'a', 't');
    Stamp    stamp;
    uint64_t requestId;
    uint32_t kind;            // ContentKind
    uint32_t result;          // kMatch*
    uint32_t version;         // kMatchExact: the matched version
    uint32_t latestVersion;   // kMatchExact / kMatchOtherVersion: the entry's newest version
    uint32_t hashScheme;
    uint32_t flags;           // kMatchExact: the matched version's kEntry* flags
    char     localId[64];     // as passed to matchContent
    char     id[64];          // kMatchExact / kMatchOtherVersion: the server's id
    uint8_t  hash[kHashBytes];   // the local content's hash, as WNet computed it
};
static_assert(sizeof(ContentMatch) == 200, "ContentMatch layout");

#pragma pack(pop)

} // namespace wrally::iface::netrec
