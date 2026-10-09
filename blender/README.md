# Forge Grid — ChupathingyCE / Blender

First-pass static, semi-transparent blue holographic grid modeled from the screenshots.

## Files

- `forge_grid.blend` — editable Blender scene.
- `forge_grid.glb` — game-engine test asset (glTF 2.0 binary). One 32 × 32-unit XY plane, facing +Z, double-sided, 16 × 16 cells, major line every four cells.
- `forge_grid_color.png` — RGBA base-color/opacity texture.
- `forge_grid_emissive.png` — RGB emissive texture.
- `forge_grid_preview.jpg` — flat texture preview, *not* a render from the engine.
- `forge_gui.png` — transparent Forge cross/ring selector when nothing is selected.
- `forge_selected_gui.png` — transparent Forge bar/ring selector when selected or carrying.
- `build_forge_grid.py` — run inside Blender to create editable scene, save `.blend`, and re-export the GLB.

## Local asset policy

The seven binary assets listed above exist in the local working directory but are
intentionally **not committed**; `.gitignore` excludes Blender scenes, GLBs and
images in this directory. This README and the build script remain tracked so a
fresh clone has an inventory, not an assumption that the binaries are bundled.

Supply the assets separately (for example, from the original archive/local copy).
The script can rebuild the grid scene, GLB and grid textures, but the two GUI PNGs
must be supplied separately. The play launcher expects these filenames; missing
visuals use the engine's procedural board/text-reticle fallbacks. Tests that inspect
these assets require the local files.

**With `forge_grid.glb` supplied, Blender is not required to use it.**

## Install on your Mac

Unzip the supplied archive so the `blender` directory lands in:

`/Users/noahlopez/Development/Github/chupathingyce/blender`

For example, from a terminal in your Downloads folder:

```sh
ditto -x -k forge_grid_blender.zip /Users/noahlopez/Development/Github/chupathingyce
```

If the archive was downloaded elsewhere, replace the zip path.

## Open and edit in Blender

Option A: File > Import > glTF 2.0 (.glb/.gltf) and choose `forge_grid.glb`.

Option B: open Blender > Scripting > Open `build_forge_grid.py` > Run Script. The script creates the mesh and material, then writes `forge_grid.blend` and `forge_grid.glb` in this directory.

For Blender installed at the default macOS location, you can run:

```sh
/Applications/Blender.app/Contents/MacOS/Blender -b --python /Users/noahlopez/Development/Github/chupathingyce/blender/build_forge_grid.py
```

## ChupathingyCE notes

The image is an alpha-blended, emissive glTF material; the engine's external GLB renderer will need to support `alphaMode: BLEND`, `doubleSided`, `baseColorTexture`, and `emissiveTexture`. True bloom must be implemented in the renderer; emissive color alone won't produce a screen-space glow.

This asset is visual-only: it has no collision or gameplay logic. It's best for testing the proposed external-asset loader and later attaching to a Forge external placement.

The 32-unit size is a modeling default, not an assertion about the exact size of the Reach asset; resize it to fit the in-game Forge grid dimensions.
