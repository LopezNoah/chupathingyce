# Experimental desktop platform

The optional `platform` feature adds one host-local solid board to offline
Blood Gulch. It can use a static Blender GLB as a visual; without an asset, the
board uses the renderer's plain gray fallback. The feature creates no Halo tags,
changes no original map files, and adds no network messages or saved game-variant
state. It is disabled by default at build time and at runtime.

## Build and enable

Build for 64-bit macOS or Linux with the feature enabled (preserve any other
configure options you normally use):

```sh
python3 configure.py --platform
ninja macos                 # or: ninja linux64
```

In `config.toml`:

```toml
[platform]
enabled = true
width = 4.0
depth = 4.0
elevation = 0.6
thickness = 0.15
forward_offset = 3.0
```

All distances are Halo world units (about 3 metres each). The default board is
4 by 4 units, with its top 0.6 units above the ground. Width and depth follow
world X/Y. The board is placed once ahead of the first local player over
reasonably level ground and stays fixed for that map session. Start an offline
local Blood Gulch game; networked sessions are not supported. Reload the map
after changing placement settings. Environment equivalents include
`HALO_PLATFORM_ENABLED` and `HALO_PLATFORM_ELEVATION`.

Width/depth are bounded to 0.5–20, elevation to 0.2–10, thickness to 0.05–1
(and less than elevation), and forward offset to 0–20. Non-finite or out-of-range
values are rejected in favor of the minimum. Placement fails safely if no
suitable ground is found.

## Optional GLB visual

Set `platform.asset` to an absolute path or a path relative to `paths.data`:

```toml
[platform]
asset = "/path/to/blender/forge_grid.glb"
```

The renderer caches one asset for its lifetime; restart after changing it. A
failed load is logged and leaves the solid gray board usable. The custom bounded
reader supports the first mesh's first triangle primitive, float positions and
UV0, unsigned indices, embedded PNG/JPEG base-color and emissive images, material
factors, and OPAQUE/MASK/BLEND alpha. Rendering is unlit with additive emissive
color (not bloom or PBR). glTF Y-up is converted to Halo Z-up and the mesh's XZ
bounds are fitted to the board top.

This is not a general Blender scene importer. Node transforms, multiple
meshes/materials, animation, skins, texture transforms, external files and
required glTF extensions are unsupported. Apply object transforms before export.
Collision comes from the generated solid box, never the GLB. Transparent pixels
reveal the scene from either side; GLB copies use per-view back-to-front sorting,
but sorting against later Halo transparent passes is not integrated. GPU
resources are bounded to one asset until exit.

## Blender source assets

See [`blender/README.md`](../blender/README.md) for the Forge Grid asset inventory
and rebuild script. Binary scenes, GLBs and images are intentionally ignored by
git; supply or generate them locally. No proprietary maps or art are required by
the asset reader tests. Do not add local binary assets to a feature commit.

## Scope and limitations

- Collision uses Halo's normal movement solver with bounded polygon, edge, and
  vertex features; it does not clamp player positions or modify tags.
- Ray and swept-sphere tests preserve nearer native hits. External geometry has
  no BSP surface identity, so native BSP visibility, pathfinding, lighting,
  shadows, and low-level BSP queries are unchanged.
- Placement is local to the host process. Networking, persistence, arbitrary
  mesh collision, and AI pathfinding are not implemented.
- The feature is registered as an inert extension on unsupported targets.
  Procedural geometry hooks are limited to 64-bit desktop builds.

## Tests

Asset-free geometry tests and, when a local GLB is available, the bounded reader
suite can be run with sanitizers:

```sh
python3 tools/test_platform_shape.py --sanitize
python3 tools/test_glb_reader.py --sanitize
python3 -m pytest tools/test_features.py tools/test_platform_shape.py
```

`tools/test_glb_reader.py` uses `blender/forge_grid.glb`; generate or supply that
local fixture first. Live collision testing requires an offline/local game fixture with owned CE
Xbox-format maps. This foundation branch does not include a dedicated map-launch
test runner; map files and test assets are not included in the repository.
