# Car materials — RBR finishes and W-Rally's PBR maps

Status: 2026-11-03. Code: [`game/src/assets/car_model.*`](../game/src/assets/car_model.h),
[`game/src/assets/car_render.cpp`](../game/src/assets/car_render.cpp), the renderer's clear coat and reflection probe
([`modules/renderer/src/core/scene.cpp`](../modules/renderer/src/core/scene.cpp),
[`probe.h`](../modules/renderer/src/core/probe.h)), the draft tool [`tools/car-pbr/`](../tools/car-pbr/).

RBR's car textures are colour only (DXT3, their alpha opaque or a transparency mask): nothing in them says which
texels are lacquered paint, rubber, plastic, metal or glass. W-Rally decides a car's finish in two layers:

1. **PBR maps** with the car's textures (beside them, or in a version of the car over them), where a car has them
   (§2) — the finish per texel.
2. **Rules from RBR's own data** otherwise (§1) — the finish per mesh group.

A car with no maps looks exactly as the rules make it; a map, once there, overrides the rules for its texture only.

## 1. The rules (no maps)

Every mesh group in `<car>.ini` names its material (`FaceBlock<g>_Material`) and its RBR shader class
(`FaceBlock<g>_Shader`). The class sets the finish:

| class | finish |
|---|---|
| `CarPaint`, `CarPaintInt` | clear coat (lacquer, roughness 0.02 over a base at 0.2) |
| `CarTire` | rubber, roughness 0.85 |
| any other (`CarPlastic`, `CarTexture*`, `CarColor*`, `CarRim`, …) | roughness 0.45, no coat |

A **transparent** texture (`OpacityChannel = true` in `<car>_shaders.ini`, or "transparent" in its name) is glass —
blended, smooth, reflective — only on glass: class `CarGlass`, `CarIntGlass` or `CarLight`, or a mesh named for it
(`e_lightglass_*`, `e_window_*`; most cars draw their lamp lenses as `CarPlastic`). Anywhere else its alpha is
**holes** (grilles, vents, logos, a wing's cut-outs): alpha-tested at 0.25, matte, lacquered on `CarPaint`.
Checked over the 42 cars of the dev install with
[`tools/car-pbr/scan_transparent.py`](../tools/car-pbr/scan_transparent.py) `C:/Brb/Cars`: it moves ~90 vents, logos and
odd parts off the glass and no lens or pane. (Its glass test is by class alone; the name rule above keeps the lenses.)

## 2. PBR maps

### Where and how they are found

Beside the RBR texture in `Cars/<car>/textures/`, named after it. For `impreza05_external.dds`:

| file | content | colour space |
|---|---|---|
| `impreza05_external_basecolor.dds` | the colour, used instead of the RBR texture | sRGB |
| `impreza05_external_normal.dds` | tangent-space normal; x and y are enough (z is rebuilt) | linear |
| `impreza05_external_orm.dds` | **r** occlusion, **g** roughness, **b** metallic, **a** clear coat (0 matte, 1 lacquer) | linear |

Each map is optional and found by its name alone (case-insensitive); the others keep falling back. With an ORM map
the map is the finish: roughness, metallic and coat come from it (factors 1), and the class rules no longer touch
them — the class still decides glass or holes (§1). `PHYS_DRIVE_MATERIALS=1` prints, per material, the maps found and
any the renderer refused.

Meaning as glTF's metallic-roughness, plus the coat in ORM's alpha. One RBR atlas is usually shared by paint, tyres,
plastic and glass parts, so the maps must be right for every part that samples them.

### Formats

DDS, with mip chains (without them the maps alias at a distance):

- BC1 / BC2 / BC3 (DXT1/3/5), the classic header;
- BC4, BC5, BC7 behind a DX10 header (UNORM, UNORM_SRGB or TYPELESS).

Recommended — Microsoft's [texconv](https://github.com/microsoft/DirectXTex/wiki/Texconv) (DirectXTex), from PNG/TGA
sources:

```
texconv -f BC7_UNORM      -m 0 -y -o <textures> impreza05_external_orm.png        (needs the alpha: the coat)
texconv -f BC5_UNORM      -m 0 -y -o <textures> impreza05_external_normal.png
texconv -f BC7_UNORM_SRGB -m 0 -y -o <textures> impreza05_external_basecolor.png
```

(`-m 0`: the full mip chain.) Normal maps differ in their green channel's direction (DirectX vs OpenGL); if a
baked map's bumps look inverted, flip its green channel. BC3 (DXT5) is fine for ORM too; BC1 (DXT1) has no usable alpha, so the coat would be
1 everywhere.

## 3. Drafting the maps automatically — `tools/car-pbr`

Hand-painting a mask over a 2048² atlas is the best result; the tool gives a starting point in seconds.

```
build/bin/car-uv-dump.exe C:/Brb/Cars/IMPREZA05/impreza05.sgc > uv.txt
python tools/car-pbr/make_pbr.py C:/Brb/Cars/IMPREZA05 uv.txt --preview <dir>
```

- **`car-uv-dump`** (C++, built with the game: `cmake --build build --config Release --target car-uv-dump`) reads the
  car as W-Rally does and prints every exterior mesh group's UV triangles with its texture file and shader class.
- **`make_pbr.py`** (Python 3, numpy, Pillow) paints those triangles into each atlas class by class — paint lacquered
  (0.2), rims metallic (0.8, roughness 0.35), tyres 0.85, plastic 0.55, other textured parts 0.6, glass 0.05; the paint
  wins a texel two kinds of part share, and texels no part samples take their neighbours' finish so the mips do not
  bleed. Inside the paint, what is **dark and grey** becomes matte (grilles, black trim, seals drawn on the body),
  judged on the texture **blurred** (4 texels), so a decal's black lettering keeps its lacquer.
  It writes `<texture>_orm.dds` (DXT5, mips) into a **new version of the car in the downloaded layer**
  ([content-layers.md](content-layers.md): the RBR folder is never written) — `<downloads>/cars/<car id>/<version>/
  textures/`, with a `car.yaml` saying it is a patch over RBR's car (`base: rbr`), so the game finds the maps over
  RBR's textures (the model's files are read through the car's item). `--downloads <dir>`: the game's downloads
  folder (default `%LOCALAPPDATA%\W-Rally\Downloads`, an installed game's; a build run from its tree reads
  `<bin>\Downloads`); `--version` the version made (default 0.1.0). An ORM map already there is kept unless `--force`
  (hand-made maps are safe). `--out <dir>` writes the maps alone to a folder of your own; `--preview <dir>` writes the
  class map and the ORM's roughness / metallic / coat as PNGs — the place to start correcting.

What it cannot know, and where to correct by hand:

- a **black-painted car** loses its lacquer to the dark-and-grey rule (raise the coat in the preview's alpha);
- the class is per mesh group, so a group that mixes materials in one texture region gets one finish;
- no normal or base colour maps: from the colour alone they would emboss every decal.

Tried on the Impreza 2005 (`C:/Brb/Cars/IMPREZA05`): three maps, 70% of the body atlas lacquered, rims metallic,
tyres and trim matte.

## 4. The renderer side, briefly

- **Clear coat** (`MaterialDesc::clearCoat`, times the ORM alpha): a second GGX lobe, f0 0.04, roughness 0.02, over the
  base layer — sun, local lights (deferred: G-buffer kind 3) and the environment; the base's light scaled by
  1 − coat × Fresnel.
- **Reflection probe** (`FrameDesc::reflectionFocus`, `core/probe.h`): the world around the car drawn into a cube
  (6 × 256², two faces a frame, `NGG_PROBE_FACES`), resolved to an octahedral chain the car's paint and glass read
  instead of the sky. The car itself is left out but kept as a box for its own shadow on the ground it reflects.
  About 1 ms a frame on the dev iGPU at 1080p ("scene: reflection probe … ms" with `PHYS_DRIVE_NGGLOG=1`).
- `createTextureData` (renderer interface 17) loads a normal or ORM map as data, without the sRGB decode.
