# Forge (experimental local map editor)

Forge is an opt-in editor for supported 64-bit desktop builds. It edits the
scenario placements of objects already in a map and can place objects from that
map's palettes. It does not edit map files: a bounded JSON overlay is saved under
the local data root at `forge/<map>.forge.json`. The overlay is tied to the map
identity and cache checksum and is ignored when those do not match.

Forge is local-only. It does not synchronize edits, change network messages, or
edit multiplayer spawn points and game-type flags. Runtime objects made by game
scripts and players are generally not editable. Campaign Forge waits for the
opening cinematic to finish. Gamepad editing is not implemented.

## Build and open

```sh
python3 configure.py --forge
ninja macos                 # or: ninja linux64
```

Keep other configure options normally used. Forge is off by default. In a local
game, open/close it with **F7** or **B** (Fn+F7 may be needed on macOS). The
binding is configurable with `forge.toggle_key` (up to two keys/buttons, named
as in `[controls]`). The game pauses while Forge is open. The optional local
platform feature is not needed for ordinary object editing; it is needed for
editing and testing the external GLB platform object.

The `tools/play_platform_macos.sh` helper starts an offline Blood Gulch test game
with the optional platform/Forge features enabled. The platform's
`debug.local_test_launch` setting is test-only and disabled by default.

## Controls

- Mouse: look; **W/A/S/D**: fly; **Space/C**: up/down; **Shift/Alt**: faster/slower.
- Left click: select or drop; **right click/E**: grab or drop; wheel: adjust carry
distance, or choose a palette entry when not carrying.
- **X/Y/Z**: choose roll/pitch/yaw; **Q/R**: rotate that axis.
- Arrows and **Page Up/Page Down**: nudge; **G**: toggle snapping.
- **[/]**: palette item; **P**: place; **V**: duplicate; **Delete**: delete.
- **Ctrl+Z/Ctrl+Y**: undo/redo; **Ctrl+S**: save; **H**: help; **Esc**: put
back the carried object or deselect.

The player's character and attachments are hidden while the flying editor camera
owns the view. Closing Forge returns the character to the camera eye position
only when the full collision pill is clear; bounded depenetration and a clear
entry-position fallback are attempted otherwise. Forge stays open rather than
forcing a placement through a wall or floor. Exit vehicles before opening it.

## GLB platform objects

When built with `--platform`, Forge can select, move, rotate, duplicate, delete,
undo/redo, and save the platform feature's host-local geometry slots. Copies
share the configured `platform.asset` resource and have independent rigid poses.
This is not a multi-file asset browser or arbitrary-mesh collision editor. See
[`platform.md`](platform.md) and [`../blender/README.md`](../blender/README.md)
for the bounded GLB reader and local Blender asset workflow. Without platform,
Forge remains a tag-object editor.

The optional `forge.selector_asset` and `forge.selected_selector_asset` settings
load PNG reticles (absolute paths or paths relative to `paths.data`); empty or
invalid paths use the text cross/bar fallback. `forge.selector_size` is bounded
to 8–64 logical HUD pixels. These settings require a 64-bit desktop build but do
not require the platform feature.

## Tests

Feature selection tests do not use map data:

```sh
python3 -m pytest tools/test_features.py
```

Live tests require owned CE Xbox-format maps at a path supplied to the runner;
map files are not included in the repository. The Blender GLB and selector PNGs
used by the tests are tracked in `blender/`. With `--platform --forge` built, run:

```sh
python3 tools/test_forge_platform_local.py --maps /path/to/owned/maps
python3 tools/test_forge_platform_local.py --maps /path/to/owned/maps --visual
python3 tools/test_platform_transparency.py --maps /path/to/owned/maps
```

The GLB checks use `blender/forge_grid.glb`, and selector screenshot checks use
the tracked selector PNGs. The test launcher uses isolated save/data directories
and does not write into the installed maps.
