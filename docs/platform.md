# Elevated Blood Gulch platform (experimental)

A solid rectangular collision board, optionally covered by a textured Blender
GLB visual. Without an asset it renders a plain gray board. No new Halo tags are
needed, and original map files are not modified.

## Enable

Build the optional feature (64-bit desktop only):

```sh
python3 configure.py --platform --bots
ninja macos                 # or: ninja linux64
```

Keep any other configure options you normally use. In your `config.toml`:

```toml
[platform]
enabled = true
width = 4.0
depth = 4.0
elevation = 0.6
thickness = 0.15
forward_offset = 3.0
```

All distances are Halo world units, approximately 3 metres per unit. The default
board is about 12 × 12 metres, with its top 1.8 metres above the ground. Width
and depth follow world X/Y, not the player's heading.

Start **offline/local Blood Gulch**, not a System Link/network host or client.
Once the first local player spawns, the board is placed ahead of that player's
initial position, over the ground there. It stays fixed unless edited in Forge.
Saved Forge poses are reapplied after the default board is initialized. Turning
the platform feature off and reloading the map restores normal geometry.

The environment equivalent is `HALO_PLATFORM_ENABLED=true`. Each dimension also
has an environment variable, e.g. `HALO_PLATFORM_ELEVATION=1.0`. Settings are
sampled at placement time; reload the map after changing them.

Width/depth: 0.5–20; elevation: 0.2–10; thickness: 0.05–1 and less than elevation;
forward offset: 0–20. Invalid/nonfinite settings are logged and replaced with
the minimum. Placement fails safely if no reasonably level ground is found.

## macOS shortcut: skip the menus

Build with `--platform --infection` (preserving other options), then run:

```sh
sh tools/play_platform_macos.sh
```

This opens the full app directly into one-player offline Blood Gulch with the
Blender grid enabled. It uses ordinary Slayer, with Infection and automated
gameplay fixtures disabled. It does not require navigating profile or multiplayer
menus, and does not teleport you, reset the map or quit on a timer.

## Use the Blender grid

Export **glTF Binary (.glb)** from Blender with UVs and embedded textures. To use
our supplied grid, add this to `[platform]`:

```toml
asset = "/absolute/path/to/chupathingyce/blender/forge_grid.glb"
```

Or launch with `HALO_PLATFORM_ENABLED=true` and
`HALO_PLATFORM_ASSET=/absolute/path/to/blender/forge_grid.glb`. Relative asset paths
are resolved against `paths.data` (the game data root), not the working directory.
Restart the process after changing the asset; the renderer caches one asset for
its lifetime, including across map resets. A failed load is logged once and the
solid procedural board remains usable.

The custom bounded reader supports the **first mesh's first triangle primitive**,
float positions and UV0, unsigned indices, embedded PNG/JPEG base-color and
emissive images, material factors and OPAQUE/MASK/BLEND alpha. Rendering is unlit
with additive emissive color (not bloom or PBR). It is currently double-sided.
glTF Y-up is converted to Halo Z-up, and the mesh's XZ bounds are fitted to the
board's top. Apply object transforms in Blender before export: node transforms,
multiple meshes/materials, animation, skins, texture transforms, external files
and required extensions are not supported. This is a grid-visual importer, not a
general Blender scene or Halo-tag converter.

When the GLB loads, only its visual is drawn: the collision box is invisible,
so transparent pixels reveal the scene from either side instead of an opaque
backing. The gray solid shell is drawn only for missing/failed assets. Scene depth
testing and split-screen viewports are respected; GLB copies are drawn back-to-front
per view. Sorting against Halo's later transparent passes is not integrated.
**Collision still comes from the board**, never the GLB; authoring arbitrary mesh
collision is not implemented. GPU resources are bounded to one asset until exit.

## Edit the GLB in Forge

Press **B** or **F7** (Fn+F7 on some Macs). Aim at the grid and click: the HUD
shows `Selected: forge_grid.glb`, with the same spherical highlight as native
boulders (grey hover, yellow selection, cyan carry). Picking remains against the
actual box, not the larger highlight sphere. Controls:

- **E/right-click** grab, fly to move, **click** drop; wheel adjusts carry distance.
- **WASD + Space/C** moves the grabbed object freely in 3D with the flying camera.
- **X/Y/Z** chooses roll/pitch/yaw rotation; **Q/R** turns that axis. The HUD shows it.
- Arrows nudge horizontally; **PgUp/PgDn** nudges vertically; **G** toggles snapping.
- **V** duplicate; **Delete** delete; **Ctrl+Z/Y** undo/redo.
- Select the GLB palette entry with **[ / ]** or the wheel, then **P** places a copy.
- **Ctrl+S** saves native and GLB edits together in `forge/<map>.forge.json`.

All copies share the configured `platform.asset` texture/mesh resource, but have
independent XYZ positions and yaw/pitch/roll rotations. Rendering, picking, full
solid-box collision and saved poses use the same Halo-convention rigid transform.
There are 64 slots per map, including deleted copies retained for undo. There is
no multi-file asset browser or resizing. The saved extension name and asset path
must match on load;
unknown providers or changed assets are not silently applied to another model.

### Transparent PNG selector

The macOS play shortcut uses `blender/forge_gui.png` (cross/ring when nothing is
selected) and `blender/forge_selected_gui.png` (bar/ring when selected or carried).
Original PNGs are not modified. The image is centered in each player's viewport,
scaled with the HUD, with aspect ratio and transparency preserved. Premultiplied
mipmaps prevent dark edges at this small size; it does not write scene depth or
Halo's scratch alpha channel. The PNG is drawn after Forge's help text.

Configure `forge.selector_asset`, `forge.selected_selector_asset` and
`forge.selector_size` (default **32 logical HUD pixels** for the whole image,
roughly assault-rifle crosshair size for these assets; bounded to 8–64). Paths may
be absolute or relative to `paths.data`. Images are limited to 4096×4096 and 64 MiB
files. Two resources are cached until exit; restart to change them. Empty or
invalid PNGs fall back to text cross/bar. PNG support does not require the platform
feature. `tools/test_platform_transparency.py` checks both selector states and
above/below/grazing GLB visuals using actual GL screenshots and pixel checks.

### Returning to your character

The Spartan and attached objects are hidden while Forge flies noclip and the game
is paused. On exit, the Spartan's **eye position** matches the Forge camera when
the full character collision pill fits there; facing is also transferred and
velocity cleared. This is not a feet-at-camera teleport. Native map geometry,
objects and GLB boards are checked, not just a point at the camera.

If obstructed, the engine tries a small local depenetration, then the still-clear
Forge entry position. If neither is safe, Forge stays open and asks you to fly
into clear space. No wall/floor placement is forced. Exit a vehicle before opening
Forge; existing visibility is restored on close or map disposal, and the prior
pause state is restored on normal close. The character remains dormant while flying; there is no physical
Monitor/ball object or character animation during the edit session.

## Scope

- Loaded assets use an unlit textured shader with scene depth testing and alpha
  blending (no depth writes for BLEND). The fallback shell uses Halo's existing
  colored-geometry shader. Both draw before later transparency and HUD/UI.
- Movement uses Halo's existing pill collision solver, with generated polygon,
  edge and vertex features. Characters aren't held up by teleporting or clamping
  their positions during normal play.
- Ray and swept-sphere queries include the board and preserve nearer native hits.
- External surfaces deliberately have no BSP surface/tag identity. Original
  BSP visibility, pathfinding, lighting, shadows and low-level BSP queries remain
  unchanged. AI does **not** automatically know how to reach the board.
- Forge selection and pose persistence are supported through the optional world
  geometry editor adapter. Networking and arbitrary mesh collision authoring
  remain unimplemented.
- The live standing fixture currently tests a Spartan. Bots and Elites share the
  biped movement path, but dedicated bot/Elite standing fixtures are not yet tested.
- Collision feature lists retain their fixed engine capacity. In a saturated
  query the board's features are not appended; this is a prototype limit.

The feature is disabled at build time by default and also requires runtime
`platform.enabled = true`. Unsupported targets register an inert extension.

## Tests

Pure geometry and dispatcher/settings tests (no map data):

```sh
python3 tools/test_platform_shape.py --sanitize
python3 tools/test_glb_reader.py --sanitize
python3 -m pytest tools/test_features.py tools/test_platform_shape.py
```

For the live test, build with `--platform --infection` (the existing test launcher
starts ordinary offline Slayer; Infection rules remain disabled):

```sh
python3 tools/test_platform_local.py --maps /path/to/owned/maps
python3 tools/test_platform_local.py --maps /path/to/owned/maps --disabled
python3 tools/test_platform_local.py --maps /path/to/owned/maps --asset blender/forge_grid.glb
python3 tools/test_forge_platform_local.py --maps /path/to/owned/maps
python3 tools/test_forge_platform_local.py --maps /path/to/owned/maps --visual
python3 tools/test_platform_transparency.py --maps /path/to/owned/maps
```

The enabled fixture drops a Spartan onto the board, checks that it is grounded
at the top surface after three seconds, resets the map, and repeats. The disabled
control verifies that neither the board nor the fixture runs. Each test uses an
isolated data/save root and retains its log outside the repository.

`debug.platform_test` is strictly an automated fixture: it teleports the player
and restarts the map once. Leave it **false** for normal play.

Validated on macOS: native build, ASan/UBSan geometry and GLB reader tests,
dispatcher/settings tests, live GLB upload and standing before/after map reset,
split-screen grid screenshots and the disabled control; real Forge ray selection,
carry/drop/rotate/copy/palette/delete/undo/redo/save, fresh-process pose reload,
hidden avatar and clear/obstructed camera handoff (native wall/floor and GLB board),
and Forge spherical selection/rotated-copy screenshots; above/below/grazing GLB
render screenshots and pixel checks (including transparent underside). Linux,
Android, 32-bit neutrality and bot/Elite fixtures have not been run.
