# Intel XeGTAO - screen-space ground-truth ambient occlusion

From <https://github.com/GameTechDev/XeGTAO> (Intel, MIT, archived), commit
`a5b1686c7ea37788eeb3576b5be47f7c03db532c` (2024-04-22), unchanged:

- `XeGTAO.hlsli`, `XeGTAO.h` - `Source/Rendering/Shaders/` (the technique and its constants)
- `vaGTAO.hlsl` - `Source/Rendering/Shaders/`, Intel's sample entry points; kept as a reference only, not compiled
- `LICENSE` - the repository's MIT licence

The renderer's own entry points (`modules/renderer/src/core/gtao.cpp`) include `XeGTAO.hlsli` and `XeGTAO.h`.
