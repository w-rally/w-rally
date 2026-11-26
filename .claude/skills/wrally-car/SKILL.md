---
name: wrally-car
description: Load before working on a car in W-Rally - its loading (.sgc, <car>.ini, _shaders.ini, car_model/car_render/car_register), the car's physics folder or variant, wheels and loose parts, the car frame and axes, paint/clear coat/reflection probe, PBR maps and tools/car-pbr, lamps (car_lights, presets, the lamp lab, lit twins), the Garage showroom, or a visual bug on the car (wheels inside out, ghost behind the car, blinking brake lamps, glints). Gives the file map, the frames, the switches, the check commands and the known traps.
---
# W-Rally: cars

A car is RBR's (`<RBR>/Cars/<folder>/`, listed by `Cars/Cars.ini`) or W-Rally's (`cars/<id>/<version>/car.yaml`
in a content layer). The game reads the render model itself and hands the renderer a node tree. The physics DLL
reads the physics files through the VFS. Depth: `docs/car-pbr.md` (finishes, maps, probe), `docs/design.md` §6.4,
`docs/content-layers.md` §2-4 (car items, physics variants), `docs/ngg-design.md` §5.5 (lamps). Game-wide map:
skill `wrally-game-dev`. Capturing and comparing frames: skill `wrally-render-measure`.

## File map (game/src/assets unless noted)

| file | what |
|---|---|
| `car_model.*` | `ReadCarModel(sgc, surface, files)`: the .sgc (HedgeHog3D records), `<car>.ini` (FaceBlock material and shader class), `<car>_shaders.ini` (textures, OpacityChannel), PBR maps by name. Exterior `e_*` meshes, this surface's wheels, lamps' lit twins. Header comment = the .sgc format |
| `car_render.*` | `CarRender`: the renderer model on nodes. Finish rules per shader class, lamp twins lifted off the glass, `PoseMatrix`, `PartNode`, wheel/spin matrices |
| `car_lights.*` | lamp beams (`LampKind`, presets `kBeams`), layouts (`--lamps`), aim, the renderer's `LightDesc` per frame |
| `car_register.*` | names/class/power/physics in layers: Cars.ini, `kServerCars` (hardcoded server list), `car.yaml`. A car nobody knows is listed but not usable |
| `catalog.*` | `Car`: id, version, folder, source, sgc, physics variants, slot, engine. `PhysicsFor` |
| `showroom.*` + `app/game.cpp` `class Showroom` | the Garage turntable scene and the car loaded a few textures a frame |
| `lamp_lab.*` | the lamp lab scene (`W-Rally.exe --lab`) |
| `ui/quick_rally.cpp` | the Garage and Car setup pages |
| `tools/car-pbr/` | `uv_dump.cpp` (`car-uv-dump.exe`), `make_pbr.py`, `scan_transparent.py` |
| `tools/car-ghost/` | `check-car-ghost.sh` + `ghost.py`, two replays (`track41-i20.phr`, `track42-impreza.phr`) |
| `tools/lamp-wall/wall.py` | beam presets drawn on a wall at 10 m; a copy of `car_lights.cpp`'s, keep them in step |
| `tests/car_parts.cpp` | CTest `car-parts`: loose parts' nodes do not jump |

## Frames and axes (get these right first)

- **Physics/car frame** (`physics::Pose`, `math::Rotate`, lamps): **+x is the car's LEFT, -y forward, z up**.
  The pose's origin is the **centre of gravity**, not the model's origin.
- **Model/render frame** (the .sgc, inside the root node): y up, forward -z = the physics frame with y and z swapped.
  `math::ToRender(p) = {x, z, y}`. The swap mirrors handedness: anything with a "right" (a lamp lobe's yaw) takes its
  sign from where the car's right lands in render space.
- A body part sits at `v - cog` (node 0 = model frame to car frame, less the CoG).
- Nodes: 0 body; 1+k hub of wheel k (k = 0 lf, 1 rf, 2 lb, 3 rb; knuckle steer/camber/toe, for calipers);
  5+k spin under the hub (tyre, disc); 9+i loose part i (`physics::Part`: door, bonnet, bumper; meshes `e_<name>`,
  `e_int_<name>`). An undamaged car draws as one rigid body.
- **Wheels: the game places them where the physics has them.** A wheel mesh's own node matrix is a leftover: only
  its **scale** is used. Every wheel mesh has its rim towards +x; right wheels are turned half about y. The .sgc's
  `Point_e_wheelrep_*` helpers sit 5-15 cm inboard: do not use them. Without physics (a menu car) the hubs come from
  `Point_e_wheel_<lf|rf|lb|rb>` and the wheel nodes' places (`CarModel::hubs`, `wheelAt`).

## Physics folder and variants

- Lookup (content-layers.md §3-4): a loose `Cars/<folder>/physics/` (has `common.lsp`) is the car's `rbr` variant
  ("its own"). Otherwise its Cars.ini slot's folder in `physics.rbz` appears as `cars/<id>/physics/rbr/rbr/`
  ("slot N"). Shared tyres/global defs are the base `physics/rbr/rbr/`. Only the RBR mapping opens `physics.rbz`.
- `car.yaml`'s `physics:` names a variant id (or a folder). Quick rally's PHYSICS row keeps the choice as
  `physics_<car id>` in W-Rally.ini.
- On the command line: `--car <variant folder> --physics-base <base> --physics <module id>`, plus `--sgc <model>`
  and `--car-item <id>@<version>` (where the model's .ini files, textures and PBR maps come from).
- `car.yaml` keys (all optional): `name`, `maker`, `year`, `class`, `drive`, `power_hp`, `weight_kg`, `physics`.

## Finish, paint, maps (docs/car-pbr.md)

- No maps: the shader class decides. `CarPaint`/`CarPaintInt` clear coat; `CarTire` rubber 0.85; others 0.45.
  A transparent texture is glass only on `CarGlass`/`CarIntGlass`/`CarLight` or meshes `e_lightglass_*`,
  `e_window_*`. Elsewhere its alpha is holes (alpha test 0.25).
- Maps beside the texture: `<tex>_basecolor.dds` (sRGB), `<tex>_normal.dds`, `<tex>_orm.dds` (r AO, g rough,
  b metal, **a clear coat**). BC1 has no usable alpha: the coat would be 1 everywhere; use BC3/BC7.
- Draft maps:
  ```
  build/bin/car-uv-dump.exe <RBR>/Cars/IMPREZA05/impreza05.sgc > uv.txt
  python tools/car-pbr/make_pbr.py <RBR>/Cars/IMPREZA05 uv.txt --preview <dir>
  ```
  It writes a **new car version into the downloads layer** (`<downloads>/cars/<id>/<version>/textures/`, `car.yaml`
  with `base: rbr`); it refuses the RBR folder. `--downloads`, `--version`, `--force`, `--out`. A dev build reads
  `<bin>\Downloads`, an installed one `%LOCALAPPDATA%\W-Rally\Downloads`.
- Paint reflections come from the reflection probe (`modules/renderer/src/core/probe.h`, `NGG_PROBE_FACES`); the
  car's own parts block it by one ray against the car's TLAS (`NGG_NO_REFL_RAY=1` turns that off).

## Lamps

- RBR lamp meshes come in pairs: `e_light_lf` and its lit twin `e_light_lf_lit` (one shown at a time). Brake and
  reversing lamps have **only** a lit twin, modelled on the tail lamp's glass. Groups: `LampMeshGroup`
  (head, tail, brake, reverse, ramp, ramp housing).
- Lamp frame = the car frame (x LEFT, -y forward, z up, from the CoG); aim = yaw degrees right, pitch up.
- Layouts: `--lamps "ramp"|"2 pods"|"4 pods"|"6 pods"|"LED bars"|"no ramp"`, `--lamp-aim spread,pitch`; saved per car
  as `lamps_<folder> = layout|spread|pitch`.
- Keys: H headlights off/dipped/main, J ramp, Q/E indicators, G hazard.

| switch | effect |
|---|---|
| `PHYS_DRIVE_LAMPS=<0\|1\|2>[,noramp][,brake][,reverse][,left][,right][,hazard]` | lamps set from the start (brake/reverse held: for captures) |
| `PHYS_DRIVE_TEST_LIGHTS=ramp\|bars\|low\|lamp\|noshadow` | test lamp sets on the car |
| `PHYS_DRIVE_MATERIALS=1` | per material: maps found, maps the renderer refused |
| `PHYS_DRIVE_PARTS=1` | every mesh's node with the node tree above it |
| `--time 23:30` | night (sun from time and place) to judge lamps |
| `W-Rally.exe --lab [--camera 1] [--time HH:MM]` | lamps in a synthetic scene, no stage |
| `python tools/lamp-wall/wall.py <layout> <out.png> [m]` | the beam pattern on a wall, without the engine |

## Look at a car

- Showroom capture (no menus): `W-Rally.exe --showroom <car folder> [--turn 30] [--spin 12.6 --shots 20]`.
  Writes `showroom.png` / `showroom-NN.png` into the **cwd**: run it from a scratchpad folder.
  `PHYS_DRIVE_OFF=taa,ao,shadows,rt` works here too; `PHYS_DRIVE_FRAMEMS=1` prints the GPU frame times.
- On a stage: the run line from skill `wrally-game-dev` with `--camera`, `--skip-to`, `--exit-at`,
  `PHYS_DRIVE_STEP=0.0333 PHYS_DRIVE_SHOTS=<n>` (frames `shot-NNN.png` next to `--out`).
- Ghost check: `RBR=<RBR> bash tools/car-ghost/check-car-ghost.sh [W-Rally.exe] [out dir]` (needs numpy and
  Pillow; ~7 min on the iGPU; exit code non-zero = ghost back).

## Pitfalls

| symptom | cause | fix |
|---|---|---|
| Car drawn ~1.4 m off its shadow/collision | model drawn at its own origin | subtract the CoG (body node) |
| Left rims face inward (i20 R5 tarmac/snow) | wheel node's rotation used | take only the node's scale; turn right wheels half about y |
| Wheels "too far inside" | placed from `Point_e_wheelrep_*` | place from the physics' hub/knuckle |
| A lamp or lobe points the wrong side | assumed +x = right | car frame +x is LEFT; mind the y/z swap mirroring |
| Brake lamp blinks / speckles while braking | lit twin lies on the tail glass at one depth (z-fight) | lit twins pushed 1 mm, brake/reverse 2 mm along normals (`car_render.cpp`); keep it when touching lamp meshes |
| Brake lamps dark in a replay | lamps from live keys | they follow `Snapshot::brake` (recorded pedal) |
| Pale copy of the car behind it on the road | RT AO/sun denoiser history reprojected | renderer side (`rt_denoise.hlsl`); check with `tools/car-ghost` |
| Trails/smear behind a moving car or headlamps | TAA history of moving objects/glass | renderer TAA (moving glass marked in prepass); judge with `PHYS_DRIVE_OFF=taa` too |
| White lines on cage/spokes/arch lips at night | probe reflection seen through the car | the refl ray (`NGG_NO_REFL_RAY` to compare), specular AA |
| Vents/logos drawn as glass, or lenses as holes | transparency rule by class only | class + mesh-name rule; `scan_transparent.py <RBR>/Cars` |
| ORM coat 1 everywhere | ORM saved as BC1 | BC3 or BC7 |
| PBR maps not picked up | written beside RBR's textures, or the car item not given | maps live in a downloaded car version; a run needs `--car-item <id>@<version>`; check `PHYS_DRIVE_MATERIALS=1` |
| `make_pbr.py` / `ghost.py`: `No module named 'numpy'` | system Python has Pillow only | ask the user to install numpy, or write PIL-only analysis |
| `Image.open('showroom-03.bmp')` fails | captures are PNG now (old .bmp files left in `build/bin`) | read `showroom-NN.png` / `shot-NNN.png`; clear old shots first |
| Showroom run exits with `no renderer module "wrenderer" ... ErrVersion` | stale WRenderer.dll vs exe | rebuild `W-Rally` (it builds the modules), game closed |
| A car is listed but cannot be picked | no `kServerCars` entry and no `car.yaml` | add a `car.yaml` (name, physics) in a content layer |
