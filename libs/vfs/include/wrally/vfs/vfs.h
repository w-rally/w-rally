#pragma once
// wrally::vfs - where W-Rally finds a file (docs/content-layers.md): the downloaded layer, the W-Rally layer and the
// RBR folder, in that order.
//
// Layers 1 and 2 have W-Rally's layout, every item kept per version:
//   tracks/<track-id>/<version>/                 track.yaml, the track's files
//   cars/<car-id>/<version>/                     car.yaml, the car's files
//   cars/<car-id>/physics/<physics-id>/<version>/ physics.yaml, a physics variant's files
//   physics/<base-id>/<version>/                 physics.yaml, a physics base's files (tyres, global defs)
// and plain files that are no item (video/menu.mp4, rbr-tracks.yaml), the first layer's that has one.
//
// A manifest is YAML (car.yaml; car.yml too) or, as before, JSON (car.json): the first of those a version has is its
// manifest, the others are left out (the log says so). Item::Find takes any of the names for the manifest.
//
// A version may name a `base` in its manifest (an earlier version of the same item, or "rbr"): its files are its own
// folder's first, then its base's, down the chain, file by file. The manifest itself is the version's own.
//
// The RBR folder is never written. It is reached two ways:
//   mapped  its items at version "rbr": the maps of Maps/Tracks.ini, and those with a track-<n>_<X>.trk in Maps it does
//           not list, as tracks (ids from rbr-tracks.yaml or .json, else rbr_<n>; the map's files Maps/track-<n>_* and
//           track-<n>.* inside the track), the folders of Cars with an
//           .sgc as cars (id: the folder's name as a slug), each car's physics as the variant "rbr" (its loose
//           Cars/<folder>/physics/, else its Cars.ini slot's folder in physics.rbz) and physics.rbz's Physics/ as the
//           base "rbr". Their manifests are made in memory. physics.rbz is opened here and nowhere else.
//   raw     rbr/<RBR path>: the RBR folder as it is, for the code that reads RBR's own lists.
//
// Ids are slugs of a-z, 0-9 and _; versions are numbers joined by dots (1, 1.2, 1.2.3), "rbr" the oldest of all.
// Paths use '/', are relative and case-insensitive. Everything is read when the Vfs is made; after that it is
// read-only and safe to use from any thread.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wrally/formats/json.h"

namespace wrally::vfs {

enum class Kind { Track, Car, Physics, PhysicsBase };

struct ItemRef {
    Kind kind = Kind::Track;
    std::string id;
    std::string car;            // Physics: the car whose variant it is
    std::string version;        // "" the newest present
};

struct Layer {
    std::filesystem::path root;
    std::string name;           // "downloaded", "w-rally"
};

// What a path resolved to: a loose file, or bytes the Vfs holds (a physics.rbz member, a manifest made in memory).
struct Resolved {
    std::filesystem::path file;
    const std::string* bytes = nullptr;
    std::string source;         // the layer's name, or "rbr"
};

// '\\' to '/', no leading, trailing or doubled '/', lowercase when `lower`; "" for a path that leaves its root ("..").
std::string NormalPath(std::string_view path, bool lower = false);
bool ValidId(std::string_view id);
bool ValidVersion(std::string_view version);            // "rbr" too
int CompareVersions(std::string_view a, std::string_view b);   // <0, 0, >0; "rbr" below every number
std::string ItemDir(const ItemRef& ref);                // "cars/s_i2003/1.0.0" (the version as given)
const char* ManifestName(Kind kind);                    // track.yaml, car.yaml, physics.yaml: what to write
const std::vector<std::string>& ManifestNames(Kind kind);   // the names read, in order: .yaml, .yml, .json
bool IsManifestName(Kind kind, std::string_view name);  // one of them, any case
std::string Slug(std::string_view name);                // lowercase, anything else than a-z 0-9 _ as _

class Vfs;

// One version of an item with its base chain. Valid while its Vfs lives.
class Item {
public:
    const ItemRef& Ref() const { return m_ref; }                // the version filled in
    const formats::Json& Manifest() const;
    std::string ManifestFile() const;                           // its manifest's name ("car.yaml", "car.json"), "" none
    std::vector<std::string> Chain() const;                     // this version, then its bases
    std::optional<Resolved> Find(std::string_view rel) const;   // the first version in the chain that has it
    std::string Source(std::string_view rel) const;             // its layer, "" none
    // A folder's entries over the whole chain, by name, folders ending in '/'.
    std::vector<std::string> List(std::string_view relDir = {}) const;
    std::vector<std::string> Files() const;                     // every file, recursively, sorted

    struct Version;             // (the Vfs's: one version's own files)

private:
    friend class Vfs;
    const Vfs* m_vfs = nullptr;
    ItemRef m_ref;
    std::vector<const Version*> m_chain;
};

class Vfs {
public:
    // `layers` in lookup order, the first the one written to; `rbr` "" for none.
    Vfs(std::vector<Layer> layers, std::filesystem::path rbr);
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // What was noticed while reading (an RBR map no table names, a bad manifest, an id two cars share...).
    const std::string& Log() const { return m_log; }
    const std::vector<Layer>& Layers() const { return m_layers; }
    const std::filesystem::path& RbrRoot() const { return m_rbr; }

    std::vector<std::string> Ids(Kind kind, std::string_view car = {}) const;     // sorted
    std::vector<std::string> Versions(Kind kind, std::string_view id, std::string_view car = {}) const;   // newest first
    std::optional<Item> Open(const ItemRef& ref, std::string* error = nullptr) const;

    // An item path ("tracks/<id>/<version>/<file>"), a raw RBR path ("rbr/Maps/Tracks.ini") or a plain file.
    std::optional<Resolved> Find(std::string_view path) const;
    bool Read(std::string_view path, std::vector<uint8_t>& out) const;
    std::vector<std::string> List(std::string_view dir) const;
    // Where a write of `path` goes: in the first layer, never in the RBR folder ("" then, or with no layer).
    std::filesystem::path WritePath(std::string_view path) const;
    // A file on disk as a VFS path: under the RBR folder rbr/<path>, under a layer its path there; anywhere else its
    // absolute path as it is (the command line's, for development - FileHost reads those directly). "" stays "".
    std::string VfsPath(const std::filesystem::path& file) const;

    // The RBR mapping: an RBR map's track id ("" none), and a track's map number (-1 none).
    std::string TrackOfMap(int map) const;
    int MapOfTrack(std::string_view id) const;
    // An RBR car's folder under Cars ("" not an RBR car).
    std::string RbrCarFolder(std::string_view id) const;

private:
    friend class Item;
    struct Index;
    void ReadLayers();
    void ReadRbr();
    std::optional<Resolved> FindPlain(std::string_view path) const;

    std::vector<Layer> m_layers;
    std::filesystem::path m_rbr;
    std::unique_ptr<Index> m_index;
    std::string m_log;
};

bool ReadResolved(const Resolved& r, std::vector<uint8_t>& out);

} // namespace wrally::vfs
