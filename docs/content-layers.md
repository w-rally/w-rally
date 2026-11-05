# Content layers — how W-Rally finds a file (plan)

Status: plan, draft 4, 2026-11-05, not implemented. It replaces the layer list in [design.md §6.1](design.md) once
agreed.

## 1. The layers

| # | layer | folder | layout | written by the game |
|---|---|---|---|---|
| 1 | **Downloaded** | `download_path` (chosen at install, per user) | W-Rally's | yes — the only place the game writes content |
| 2 | **W-Rally** | `<install folder>\content` | W-Rally's | no (the installer) |
| 3 | **RBR** | `rbr_path` | RBR's, **never written** | no |

Layers 1 and 2 share **W-Rally's layout** (§2). RBR keeps its own layout; it is reached in two ways (§3): **mapped**
into W-Rally's layout (its cars, tracks and physics appear as W-Rally items, which downloads can patch), and **raw**
under `rbr/` for the RBR-only files the compatibility code reads (`rbr/Maps/Tracks.ini`,
`rbr/Maps/TrackSettings.ini`, `rbr/audio/...`). Any of the three may be missing: no RBR is standalone mode; layer 1
is created on first use.

Nothing writes to the RBR folder, ever: not the game, not the tools, not the installer.

## 2. W-Rally's layout (layers 1 and 2)

The **items** are tracks, cars, a car's physics variants and the shared physics bases. Each has an **id** and is kept
**per version**, so several versions sit side by side (§5): a replay needs the exact track and physics it was driven
with, and an update must not take them away.

An id is a lowercase slug of `a-z`, `0-9` and `_`, never a bare number:

- a **track**: `<country>_<stagename>` — the ISO 3166-1 alpha-2 code and the stage's name: `cz_ploskovice`,
  `fr_col_de_turini`. One track is one stage; its times of day and weathers are conditions of it, not other tracks.
- a **car**: its folder name, as RBR's are (`s_i2003`, `i20_r5`).
- a **physics variant**: a slug unique within its car (`rbr`, `wphysics_gravel`).
- a **physics base**: what every car of a format shares, tyres and global defs (`rbr`).

A version is `major.minor.patch`, set by the item's author; RBR's own items have the version `rbr`.

```
cars/<car-id>/
    <version>/                         the car at that version
        car.json                       id, version, base, name, maker, year, class, drive, power, weight, default physics
        <model>.sgc, textures, <texture>_orm.dds, lamps, engine sound ...
    physics/<physics-id>/<version>/    a physics variant at that version (§4), always loose files
        physics.json
        common.lsp, <surface>.lsp, ..., setups/*.lsp
tracks/<track-id>/<version>/
    track.json                         id, version, base, name, country, surface, length, conditions, rbr map number
    <track-id>.ngg, ...
physics/<base-id>/<version>/           a physics base: tyres, global defs
video/menu.mp4                         W-Rally's own files that are no item, as today
rbr/tracks.json                        the translation table: RBR map number <-> track id (§3)
```

Layer 2 has the same layout, with the one version of each item that the installed W-Rally ships.

**Patching:** a version may name a `base` in its manifest — an earlier version of the same item, or `rbr`. Its
files are its own folder's first, then its base's (and so on down the chain), file by file. So a download of a few
files (a texture's `_orm.dds`, a new `car.json`, a corrected `.lsp`) is a new version over RBR's item or over an
earlier one. Files read as a set (an `.lbs` with its textures) come from one version, the one that has the set's
main file. A version without `base` is complete.

`content/cars/s_i2003.ini` (a test, no meaning now) and the `content/cars/<car>.ini` lookup in `options.cpp` are
removed; `--overrides` stays for development.

## 3. RBR: mapped and raw

**Mapped** — the RBR layer answers W-Rally paths for RBR's items, all at version `rbr`:

- **Tracks:** `rbr/tracks.json` (shipped in layer 2, a newer one downloadable into layer 1) is written by hand and
  maps each RBR map number to a track id: `{ "41": "cz_ploskovice", ... }`. Then `tracks/<id>/rbr/track-41_M.trk` ↔
  `Maps/track-41_M.trk`, and so on for the map's files (`.lbs`, `.trk`, `.col`, `.dls`, `.ini`, textures), keeping
  RBR's file names inside the track's folder. A map the table does not know is offered as `rbr_<n>` until it is
  added. The map number lives on only in this table and in `track.json`'s `rbr` field, for compatibility (old
  replays, `TrackSettings.ini`'s sections, RBR's own track files).
- **Cars:** `cars/<id>/rbr/<file>` ↔ `Cars/<folder>/<file>`. `Cars.ini`'s name and the slot table fill an in-memory
  `car.json`.
- **`physics.rbz` is RBR's alone, kept for compatibility, and only the mapping opens it.** Nothing else in the game
  or in a module asks for `physics.rbz` or a member of it; car physics is always looked for **unpacked under the
  car's physics folder**:
  - a slot car's folder in it (`Physics/s_i2003/...`) appears as `cars/<id>/physics/rbr/rbr/...`, loose to the
    reader — unless the car has a loose `Cars/<folder>/physics/`, which then is its `rbr` variant;
  - its shared files (`Physics/<brand>/tyres.lsp` and the global defs) appear as the base `physics/rbr/rbr/...`;
  - the `physics.json` of these is made in memory (§4).
  The physics interface changes with it (v3): `CarDesc::physicsCar` is always a VFS folder, the `physics.rbz`
  folder-name form and member paths in `setup` go, and `StageDesc::rbz` becomes the base folder.
  `--extract-slot-physics` is removed: nothing needs copying.

**Raw** — `rbr/<RBR path>` reads the RBR folder as it is (read-only), for the compatibility code: the catalog's
reading of `Tracks.ini`/`Cars.ini`, the legacy importer, `TrackSettings.ini`, splash, saved setups, and the audio
module's RBR sounds (the game passes it the base `rbr/`). Step 1 checks every module for a path it builds itself.

## 4. Several physics per car

A car has any number of physics variants, `cars/<id>/physics/<physics-id>/<version>/`, from any layer (an RBR car's
original is `rbr`; a download can add others). A variant is versioned on its own, apart from the car's model. Each
has a `physics.json`:

```json
{ "id": "rbr", "version": "1.0.0", "base": "rbr", "name": "RBR original", "author": "...",
  "format": "rbr-lsp", "formatVersion": 1, "physicsBase": "rbr@rbr" }
```

- **The module says what it can drive, not the variant.** Each physics module reports the formats it reads, each
  with the range of format versions (`rbr-lsp` 1..1), and its own **simulation version**: a number it bumps only when
  the same inputs give a different result — not on every module release. Together with `buildHash` (the exact
  build, already in `ModuleInfo`) that separates "the physics changed" from "the module was rebuilt". New in the
  physics interface (v3): a `formats` list and `simVersion`.
- A variant is drivable by a module that reads its `format` at its `formatVersion`. The catalog lists `Car::physics`
  as a list of variants; Quick rally offers those the chosen physics module can drive, `car.json`'s `physics` the
  default, the last choice per car kept in `W-Rally.ini` (`physics_<car-id>`).
- A car with no variant any installed module can drive is listed but not usable, as today.

## 5. Versions

### Which version is used

- **Normally the newest** version of each item that is present in any layer (RBR's `rbr` counts as the oldest).
  The menus list one entry per item, not per version. A version can be pinned in `W-Rally.ini`
  (`version_<kind>_<id>`) for development.
- **A replay** uses the exact versions it recorded (below); **an online event** the versions it names.

### Fingerprints

A version says what an item is meant to be; it does not prove what is on a disk. So the game also computes a
**fingerprint** of each resolved version:

- the SHA-256 of the sorted list of `(path, SHA-256 of the file)` over the files the version resolves to, through its
  base chain;
- in two parts: the **drive fingerprint** over what changes a run, and the **full fingerprint** over everything
  (textures, `.ngg`, sounds). The drive files are **fixed in the game per item kind**: a track's `.trk`, `.col`,
  `.mat`, `.fnc`, `.dls` and `.lbs`; a car's collision model; a physics variant's and its base's files;
- hashes cached per file in the user folder's `cache\` by path, size and time, so a scan stays quick.

RBR's items have the version `rbr`, and the fingerprints identify them (stock RBR files are the same everywhere).

### What uses them

- **A replay** records, for the track, the car and the physics variant (and its base): id, version and drive
  fingerprint; and the physics module's id, `simVersion` and `buildHash`. Played back, it resolves exactly those
  versions. A missing version is **downloaded on demand** (with its base chain), checked against the recorded
  fingerprint; until there is a server, the replay says which item and version it needs. A replay whose content
  cannot be had is not played against other content (it would drive off the road); `--force` does it for
  development.
- **An online rally** names its track, cars and physics by id, version and drive fingerprint, and the physics
  module's id and `simVersion`. Before the start the game resolves them, downloads what is missing, and compares
  fingerprints; a mismatch shows which item and which file differs.
- **A submitted result** carries the same, plus the run's recording. The server **validates** it by checking the
  fingerprints against the event's and replaying the recording with the same module `simVersion` and content (the
  replay is deterministic for the same build and files: `golden-replay-41`).
- Visual differences (a texture, an `.ngg`) change only the full fingerprint, so they do not stop anyone from
  driving an event.

### Cleaning old versions

Old versions in layer 1 are removed automatically, since any of them can be downloaded again:

- **Kept:** the newest version of each item; every version used in the last `download_keep_days` days (default 30;
  driven, replayed, or named by an event the user entered and that has not ended); and every base those need.
- **Removed** at the game's start (in the background) and from Settings → Storage ("Clean now", with the space
  each item takes). `download_max_gb` (default none) removes the least recently used first beyond that size.
- The last use of each version is kept in the user folder's `cache\versions.json`. Layer 2 and RBR are never cleaned.
- Downloading an old version goes into layer 1 like any other: `cars/<id>/<version>/`.

## 6. Configuration and the installer

- **`download_path`**: the installer asks for it on a page after the RBR folder, pre-filled with
  `%LOCALAPPDATA%\W-Rally\Downloads` (changeable — stages are large, someone may want another drive). It is written
  to `install.ini` as the default for every Windows user, keeping `%LOCALAPPDATA%` unexpanded unless the user chose
  another folder; the game expands it. Each user can set their own in Settings, saved in their `W-Rally.ini`
  (read after `install.ini`). Kept in `HKLM\Software\W-Rally` (`DownloadPath`) for upgrades; a silent install takes
  `DOWNLOADDIR=`.
- The W-Rally layer needs no key: always `<exe>\content`.
- `ngg_folders` (default `<RBR>\Ngg` today) stays only for development; a track's `.ngg` belongs in its track folder.
- Without `install.ini` (a build run from its tree): layer 1 is `<exe>\Downloads`, layer 2 `<exe>\content` and then
  the source tree's `content/` (as `WRallyFile` does now), layer 3 the RBR folder from `W-Rally.ini` or the exe's.

## 7. One resolver

`wrally::vfs` (`libs/vfs`), used by the game and by `FileHost`:

```cpp
struct Layer { std::filesystem::path root; std::string name; bool writable; };    // "downloaded", "w-rally"
struct ItemRef { Kind kind; std::string id, version; };                           // version "" = the newest
class Vfs {
public:
    Vfs(std::vector<Layer> wrally, std::filesystem::path rbr, RbrMap map);
    std::vector<std::string> Versions(Kind, std::string_view id) const;     // present ones, newest first
    std::optional<Item> Open(const ItemRef&) const;                         // the version with its base chain
    std::optional<Resolved> Find(std::string_view path) const;              // an item path or rbr/ path
    std::string Source(std::string_view path) const;                        // "downloaded", "w-rally", "rbr"
    std::vector<std::string> List(std::string_view dir) const;              // union over the layers
    std::filesystem::path WritePath(std::string_view path) const;           // in layer 1, never elsewhere
};
```

- `Item` resolves a file of the item through its base chain and computes its fingerprints.
- `RbrMap` holds the car folders, the track table, the `physics.rbz` mapping and the in-memory manifests, built
  from `rbr/` at scan time.
- `FileHost` takes the `Vfs`; the paths it serves a module are versioned item paths
  (`cars/s_i2003/physics/rbr/rbr/common.lsp`), handed out by the game for the run's resolved versions. A mapped
  `physics.rbz` member is read like a loose file.
- The game's `rbrPath + "\\..."` concatenations and absolute paths (`car.sgc`, the `.ngg`, `--setup`) become VFS
  paths. The command line still takes absolute paths for development.
- Rebuilt when Settings changes `rbr_path` or `download_path`, or a download lands, where the catalog rescans today.

## 8. Writes — only into layer 1

- The future downloader unpacks a version into layer 1 (`<kind>/<id>/<version>/`); removing it removes the folder.
- `tools/car-pbr` writes its maps as a new version of the car in layer 1 with `base: rbr` (or `--out`), no longer
  beside RBR's textures. The maps already made in an RBR folder are moved out by hand once.
- The legacy importer's converted stages, the modules' caches, the fingerprint cache and `versions.json` stay in the
  user folder's `cache\` (derived data, not content).

## 9. Steps

1. Audit: every path the game builds from `rbrPath`, every path a module builds itself (incl. `physics.rbz` members);
   list each with its new VFS path.
2. `libs/vfs`: layers, items and versions, base chains, `rbr/` raw, `RbrMap` (cars, tracks table, `physics.rbz`
   mapping), union listing; unit tests.
3. `download_path` in Settings (+ expansion, UI field); `app::MakeVfs`.
4. `FileHost` over the `Vfs`; physics interface v3 (`physicsCar` a folder, base folder, `formats`, `simVersion`);
   WPhysicsRBR, audio and renderer hosts moved over.
5. Catalog and registers over the VFS: cars and tracks by id (newest version) with `source`; manifests from the
   layers; `rbr/tracks.json` written for the stages at hand with `<country>_<stagename>` ids.
6. Physics variants: `physics.json`, `Car::physics` as a list, Quick rally choice, `physics_<car-id>`;
   `--extract-slot-physics` removed.
7. Versions: fingerprints with their cache; replays record and resolve exact versions; `versions.json` last use.
8. Cleaning: the keep rules, Settings → Storage, `download_keep_days`, `download_max_gb`.
9. Writers to layer 1 (`car-pbr`); `s_i2003.ini` and its lookup removed.
10. Installer: `DOWNLOADDIR`, its page, `install.ini` `download_path`, registry value.
11. design.md §6.1 updated and this file folded in.

The server side (downloads on demand, events naming versions, validation) needs the server, so it is designed here
and built later; the replay records and the per-version layout are what it builds on.

Checks: `golden-replay-41` and the other CTests unchanged (RBR content resolves to the same bytes, mapped from
`physics.rbz` or not); a patch version overrides its base file by file; two versions of a track side by side each
replay their own recording; cleaning keeps the newest, the recently used and their bases; the game runs with the RBR
layer off; nothing in the RBR folder changes (its file list and times compared before and after a run); a changed
physics file changes the drive fingerprint, a changed texture only the full one.

## 10. Decisions

1. Downloads folder: chosen at install, `%LOCALAPPDATA%\W-Rally\Downloads` by default; each user may set their own.
2. Patching per file where a file stands alone.
3. Nothing is ever written to the RBR folder.
4. Each car (and track) in its own folder with all its files; `s_i2003.ini` dropped.
5. Tracks `<country>_<stagename>`, RBR map numbers only in a hand-written translation table.
6. `physics.rbz` only behind the RBR mapping; car physics always unpacked under the car's physics folder.
7. Physics modules report their formats and a simulation version of their own.
8. Every item versioned; versions side by side, old ones cleaned automatically and downloaded again on demand.
9. The drive fingerprint's files fixed in the game per item kind.
