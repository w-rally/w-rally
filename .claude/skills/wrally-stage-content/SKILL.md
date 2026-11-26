---
name: wrally-stage-content
description: Load when a task touches how a stage gets drawn in W-Rally - RBR's Maps files (.lbs/.trk/.col/.mat/.dls, TrackSettings.ini), which .ngg the game picks (Ngg folder, track id vs track-NN_X), the built-in .lbs import vs a Wallaby .ngg, .ngg chunks, LBS blocks, reconverting a stage with `wallaby-cli convert` (plants, grass, variants), checking it with ngg-check / WRendererCheck, or installing a new .ngg. Gives the lookup order, the full reconvert recipe, the install-with-backup steps and the traps.
---
# W-Rally stage content: from RBR's files to a drawn stage

The renderer draws only `.ngg`. Gameplay (physics, pacenotes) always reads RBR's own files. Depth:
`docs/ngg-design.md` §2.1-2.2 (producers, chunk table), §2.6 (plants, models, grass), §2.8 (RBR asset rules,
clip walls), §4.1a (clip walls drawn); `docs/design.md` §6.3 (stages); `docs/content-layers.md` §3 (RBR mapping,
`rbr-tracks.yaml`). Wallaby side: `<wallaby>/CLAUDE.md` "Headless tooling", "Plant models replace card plants";
`<wallaby>/docs/vegetation-plan.md` (every install so far, with numbers).

## RBR's files (`<RBR>/Maps/`)

| file | what | who reads it |
|---|---|---|
| `track-NN_X.lbs` | geometry blocks (below); X = time of day `M N E O` (+ `S`) | importer / Wallaby, physics (instances) |
| `track-NN_X_textures.rbz` | textures, `dry/damp/wet` folders | importer / Wallaby |
| `.trk` `.col` `.mat` `.fnc` | drive line + solids, collision, surfaces (9 conditions), fences | physics only |
| `.dls` | cameras, pacenotes, splits | game |
| `TrackSettings.ini` | weather sections `41M_...`, `42O_M_heavyrain_...` (map in 2+ digits) | catalog, importer (NGGW) |
| `Tracks.ini` | mod stages' names, `Surface=` | catalog |

Stage names: `track-41_M` is RBR's file stem (map 41, morning). W-Rally groups maps into TRACKS in
`<repo>/content/rbr-tracks.yaml` (`fr_cote_darbroz:` with stages `41: cote_darbroz` and `46: cote_darbroz_ii`).
A track is a place; its maps are stages (routes); RBR's `_M/_N/_E/_O` files are times of day of one stage.

### LBS blocks (`wallaby-cli info <track.lbs>` prints the table; names from `LbsFile.DescribeBlock`)

| block | name | note |
|---|---|---|
| (0,23) | texture name table | |
| (1,0) | ground meshes | render chunks with visibility; only 1 and 2 are drawn |
| (1,1) | general meshes "vegetation" | ALSO people, cars, houses, fences, rocks. Vertex type/sway/texture name tell nothing. Plants come from Wallaby's tags (NGGG/NGGP), never renderer guesses |
| (1,3) | scenery | far backdrop |
| (1,4) | "clipping planes" | really clip walls: triangles, drawn depth-only (NGGC) |
| (1,6) | movable meshes | instance id = `.trk` solid = NGGK key |
| (2,10) | ground bounding boxes | `(origin, size)`, not min/max; unused |
| (2,13)/(3,20) | drive line | (2,14)/(2,15) scripts / script objects (animated spectators, not drawn yet) |
| (3,22) | collision meshes | |

Protected stages (geometry block version 88/89) are never imported and never converted to a plain .ngg.

## Which file draws a stage (`game/src/assets/catalog.cpp`)

For each time of day, names tried in order: the time (`track-41_M`), the map (`track-41`), the track id
(`fr_cote_darbroz`) and the other maps of the track.
1. The track item's own `<name>.ngg` (content / downloads layer).
2. An `.ngg` in the `.ngg` folders: `ngg_folders` in `W-Rally.ini` (`;`-separated), default `<RBR>/Ngg`.
   Matched by lower-case file STEM, extension exactly `.ngg`.
3. Else the `.lbs` imported in memory (`game/src/assets/legacy_stage.cpp`, ~0.5 s, no cache): no de-atlas, no weld,
   no tags, no variants. The menu shows SD/HD for imported/own.

So name a Wallaby file after the track (`fr_joux_verte.ngg`) or the map (`track-42.ngg`). A merged file (41+46)
must be the track id. `W-Rally.ini` is in `<user folder>` (beside the exe for a build run from the tree).

## .ngg chunks (truth: `modules/renderer/src/format/ngg_format.h`)

`NGG0` header, version 3 only (older: "reconvert the asset"). Unknown chunks are skipped by size.
`NGGH` header, `NGGV` vertices 44 B, `NGGI` indices, `NGGM` materials v3 (flags incl. foliage, `kMaterialVertexTint`),
`NGGO` objects, `NGGT` embedded DDS, `NGGS` start (read by nothing), `NGGK` object keys, `NGGR`/`NGGQ`/`NGGA`
variants / masks / aliases, `NGGG`/`NGGP` tags and plant table, `NGGW` weather, `NGGD`/`NGGE` model library and
placements, `NGGF` grass, `NGGC` clip walls. Records grow by appending (`recordSize`). A refused NGGD/NGGE/NGGF
chunk is logged and the stage draws with its cards.

## Reconverting a stage (Wallaby)

Build the CLI first (see `wallaby-dev`), run from the Wallaby root (relative rules path):

```
CLI=Wallaby.Cli/bin/Release/net8.0/wallaby-cli.exe
$CLI convert <RBR>/Maps/track-41_M.lbs --dry-run --all-variants --with <RBR>/Maps/track-46_E.lbs   # the plan only
$CLI convert <RBR>/Maps/track-41_M.lbs --all-variants --with <RBR>/Maps/track-46_E.lbs \
     --plant-rules tools/vegetation/rules/cote-darbroz.json --plant-library <plant library> --grass \
     -o <scratch>/fr_cote_darbroz.ngg > <scratch>/convert.log 2>&1
```

- The input is the MAIN stage; `--with` adds the reversed or sibling maps (Joux Verte: `track-42_M.lbs --with
  track-45_N.lbs`). `--all-variants` collapses time-of-day siblings into one stage (NGGA aliases); `--keep <stage>`
  keeps one apart. Every merge verifies itself ("verified" in the log).
- `--plant-rules`: species key -> plant id; species without a rule keep their cards. `--grass` reads the rules'
  `"grass"` section and the `.col`/`.mat` beside the `.lbs` (road veto).
- `--plant-library <dir>`: folders `<id>/<version>/` with `plant.yaml` + glTF. Default `tools/vegetation/out`
  (git-ignored, empty in a fresh clone). Newest version wins unless a rule pins `version`.
- Other switches: `--surface dry|damp|wet`, `--no-textures`, `--no-mips` (fast tests), `--no-de-atlas`,
  `--no-weld-normals`, `--shading <o.json>`. `wallaby-cli plants <lbs> -o <dir>` lists species (species.json,
  sprites); `gltf` exports the editing project.
- Read the log: plant models / "placed" / "tree-card :" counts, "grass : built", clip walls, variants.

## Checking the new file

```
<repo>/build/bin/ngg-check.exe <scratch>/fr_cote_darbroz.ngg        # read, rewrite, compare, load in WRenderer.dll
<wallaby>/tests/WRendererCheck/bin/WRendererCheck.exe <repo>/build/bin/modules/renderer/WRenderer.dll \
     --models <scratch>/fr_cote_darbroz.ngg [--cards <old .ngg>]       # per variant: models, placements, dropped cards
```

Both end in `ngg-check: all good` / `ALL CHECKS PASSED`. `ngg-check --lbs <file.lbs>` runs W-Rally's own importer;
`--trees`/`--grass` add placeholders to a BARE stage (`<phys-data>/m5/track-41.ngg`, CTest `WRALLY_TEST_NGG`);
`WRALLY_TEST_STAGE_NGG` is stage 41 as shipped (CTest `stage-41`). `cmp -l old new | wc -l` shows how much changed.
Building ngg-check: `<cmake> --build build --config Release --target ngg-check` in `<repo>` (`wrally-build-test`).

## Installing (only when the user asks)

1. W-Rally must not be running: `tasklist | grep -i w-rally` (else stop and say so).
2. Keep the old file beside it, suffix AFTER `.ngg` so the catalog ignores it:
   `cd <RBR>/Ngg && mv fr_cote_darbroz.ngg fr_cote_darbroz.ngg.pre-<change> && cp <scratch>/fr_cote_darbroz.ngg .`
   then `cmp -s` the copy. Existing suffixes: `.pre-plants .pre-leaves .pre-hue .pre-bark .pre-grass
   .pre-clip-walls .pre-spectator-colours`.
3. Record it in `<wallaby>/docs/vegetation-plan.md` (size, plants replaced, checks, the kept file's name).
4. The user judges the look in the game. To see it yourself (only if asked):
   `PHYS_DRIVE_NGGLOG=1 W-Rally.exe <RBR> <RBR>/Ngg/fr_cote_darbroz.ngg --stage track-41_M --no-sound --overrides none --exit-at 600`.

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| Game shows the imported (SD) stage, no models/grass, from a W-Rally worktree | the `content` submodule is empty there: no `rbr-tracks.yaml`, so the track id is unknown | `git submodule update --init content` in the worktree, or copy main's `content/` files |
| A backup `x.pre-y.ngg` is drawn instead | catalog matches any `*.ngg` by stem | suffix after the extension: `x.ngg.pre-y` |
| Install refused / file in use | W-Rally (or a check) has the .ngg open | close it; never overwrite under a running game |
| Convert regenerates plants in Blender, slow, other versions | `--plant-library` missing or pointing at an empty `out/` | pass the library folder explicitly |
| WRendererCheck: interface version refused | Wallaby's bindings are older than W-Rally's `kRendererInterfaceVer` | `python tools/gen_wrenderer_interop.py`, rebuild (`wallaby-dev`) |
| "reconvert the asset" | .ngg older than format 3 | reconvert; formats are never upgraded |
| Plants or fences misclassified | trying to detect plants from (1,1), vertex type or texture names | tags come from Wallaby (`plants`, editor tag window), not heuristics |
| Physics on the wrong ground | `--stage` map differs from the .ngg's map (warning printed) | pass the right `--stage` |
| `<repo>/src` not found | W-Rally has no `src/` | `game/src`, `modules/renderer/src`, `libs/` |
| Joux Verte / 41 changes don't show | an old file with the same stem earlier in the order (track item's own .ngg wins over the folder) | check which file the log names |
