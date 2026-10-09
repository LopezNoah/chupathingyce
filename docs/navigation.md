# BSP surface navigation: first working slice

This is a **host-local, walking-only geometry navigation foundation**, independent
of combat/tactical selection. It does not replace campaign actors or modify map
tags, original map files, player spawning, saved game layouts, or network messages.
Normal multiplayer bots still use their existing spawn-location graph. The new
resource is exercised by an opt-in Bot 1 walking probe, not silently enabled for
normal combat. Forge nav authoring is not implemented yet.

## Modules and contracts

- `source/engine_ai/surface_navigation.*`: dependency-light convex polygon resource,
  geometry validation, height-aware projection, budgeted deterministic BFS and
  center/portal routes. No Halo, ECS, timeline, renderer, heap or network types.
- `source/features/bots/navigation_world.*`: validated CE collision edge-ring adapter,
  actual biped tag dimensions/slope, double-buffer publication, live CE capsule
  checks, temporary region blocking and incremental diagnostic dumps.
- `source/features/bots/navigation_probe.*`: one consumer that steers a living Bot 1 through
  the ordinary `player_action` queue. It never sets positions, grants inventory,
  jumps, flies, or bypasses collision/physics. The normal solo-host Slayer gates
  still apply.

Resources are keyed by map-name identity, checksum, BSP index, agent dimensions,
format version and Forge object-edit revision. Queries/routes carry the key,
resource generation and temporary-obstacle generation; mismatches return stale.
BSP switching invalidates before unloading (also on failed-switch fallback), map
teardown invalidates before disposal, and game-state restoration invalidates
before loading. Resources contain copied geometry, never borrowed BSP pointers.

A failed staging build is not published. Previously published storage is retained
until replaced, but is **not usable** if its source key has changed. Forge object
creation/move/delete/undo/redo increments the derived-data revision. This does not
cook new walkable surfaces for placed objects: they are checked as live collision
obstacles. No authored seed points, jump hints or tactical markers are claimed.

### Geometry and movement

Use retail CE walkable/nonbreakable surface bits, validated convex collision
rings and reciprocal shared edges. Slopes use the spawned biped definition's
minimum normal K. Boundary projection and sufficiently wide portals account for
biped radius; stacked floors are projected separately by height. Only continuous
shared-edge walking is linked: no inferred step, jump, drop, climb or teleport
links. Radius-inset/centroid routing is intentionally conservative and can reject
valid passages that require a more sophisticated corridor/funnel representation.

The quantization tables in legacy `path_structure_bsp.c` are unused there; their
low-bit clearance interpretation was **not proven** for these multiplayer maps.
Using them as physical widths rejected every Blood Gulch polygon. The adapter
therefore reports clearance as unknown (`width=height=0`), and the core requires a
collision callback before it can return a route. Native callbacks use CE's pill:
lower sphere center above ground, center-to-center height = standing height minus
twice radius, with slope-aware ground offset. This corrected a second integration
error where ground-level sphere centers made all routes appear obstructed.

Queries check center-to-portal/portal-to-center segments against live BSP and object
collision. Following rechecks the next segment, waits for transient occupants,
then temporarily blocks a region and performs bounded replanning if stuck. This
is not crowd avoidance, door interaction or advanced dynamic-obstacle navigation.
Moving vehicles/bipeds are detected by these live checks, not by a comprehensive
object-change notification system.

### Explicit ceilings and costs

- 131,072 input surfaces; 16,384 accepted polygons; 8 vertices/edges per polygon.
- 512 polygons per output corridor, 1,025 output points. Longer results explicitly
  return capacity rather than an incomplete route.
- Build: at most 128 source/adjacency rows per game tick; no background thread.
- Probe search: at most 2 polygon expansions per tick, each with at most 8 edges.
- Dump: at most 64 polygons per tick, published by renaming a `.partial` file.
- Projection/query initialization is a bounded **linear** polygon scan, not tiled
  or spatially indexed. BFS minimizes polygon hops, not geometric distance.
- Resource 2,752,576 bytes; two resources plus 524,304-byte builder and the probe's
  147,552-byte query/16,456-byte route total about 6 MB static reserved storage.
  When disabled, geometry buffers are not built/touched. No per-query allocation.

This is suitable for validating the architecture, **not a claim of production
completion** or measured worst-case frame time on every CE map.

## Private playable probe

Build with `ninja macos`. Add these to the private solo-host Slayer command in
`docs/bots.md` (use isolated data/save roots and keep public networking disabled):

```sh
HALO_BOTS=1
HALO_NAV_PROBE=1
HALO_NAV_DUMP=bloodgulch.nav
HALO_NETWORK_TEST=host:bloodgulch:slayer
```

`debug.nav_probe` defaults false. `debug.nav_dump` defaults empty; filenames must
end in `.nav`, to avoid targeting map/Forge-overlay files. Paths go through the
port's game-file translation: use a **data-root-relative** filename, not a Unix
`/tmp` path. Dumping alone also works with the human's live biped (including
campaign), without overriding its controls. Allow time for both build and dump.
Dumps are diagnostic derived geometry, not an authored resource loader/cache.
Do not commit dumps derived from proprietary map assets.

The default probe chooses a nearby polygon center and tries at most eight goals.
Set `HALO_NAV_GOAL=x,y,z` / `debug.nav_goal` to request an explicit world-space goal;
its failures are reported without silently choosing a substitute. For example:

```sh
HALO_NAV_GOAL=99999,99999,99999  # explicit bad-goal test, no movement/teleport
```

Logs distinguish resource publication, query failures, route success, sampled
following, stuck replanning and goal arrival. Diagnostic `.nav` files include
version/agent/key headers, polygon/source IDs, normals, vertices and adjacency.
There is no in-game mesh renderer in this slice.

## Verification

```sh
python3 tools/test_surface_navigation.py --sanitize
python3 tools/test_engine_ai_navigation.py --sanitize
python3 tools/test_engine_ai_behavior.py --sanitize
python3 tools/test_engine_ai_traversal.py --sanitize
python3 tools/check_navigation_probe_log.py walk /path/to/probe.log
python3 tools/check_navigation_probe_log.py bad-goal /path/to/failure.log
```

Core tests compile with strict warnings as errors and ASan/UBSan. They cover flat
floors and ramps, stacked floors without cross-floor links, disconnected islands,
bad endpoints, narrow boundary clearance, insufficient height, unknown clearance
requiring a callback, dynamic blocked/unblocked callbacks, BSP/overlay/agent/
checksum/obstacle/resource invalidation, malformed vertices/references/counts,
source/polygon/corridor capacity, invalid budgets, failed staging isolation and
repeat-build determinism. They do not emulate every CE collision/asset-loader
failure or validate real campaign BSP transitions.

Native smoke evidence (legal local retail assets; `/tmp/nav-test`):

- Blood Gulch: 4,916 input surfaces, **2,807** walkable polygons; ~2.5 ms total
  build CPU distributed across ticks. 9-waypoint probe arrived with error **0.147**.
- Beaver Creek: 2,476 input surfaces, **639** polygons; ~0.8 ms build CPU.
  17-waypoint probe climbed from z=-1.356 to z=-0.217 and arrived with error **0.173**.
- Blood Gulch explicit out-of-bounds goal: **bad-goal**, with no fallback route.
- Both walking and bad-goal log checkers passed; no crash/assertion recorded.
- Native `ninja macos` and the four sanitized standalone runners passed.

Original-source clang probes can misinterpret CE layouts on LP64. Native generated
translation units and the Ninja build are authoritative for those layout checks;
do not remove assertions or alter tag layouts to make a standalone header probe
happy. Log checkers confirm sampled acceptance evidence, not absence of collision
errors on every frame or complete tactical correctness.

## Next increments (in order)

1. Instrument and test real moving/static obstacles, long paths, respawning,
   game-state reload and campaign BSP-switch/fallback lifecycle. Improve centroid
   corridor quality and projection indexing before enabling normal bot consumers.
2. Introduce explicit CE-grounded traversal capability/link policy and validate
   feasible directed jumps/drops with actual physics; do not assume clamber.
3. Add Forge seed/jump/tactical authoring, selection/undo/visualization and a
   separate versioned checksum-bound overlay. Keep Explore Neighbors explicit
   and Hide markers neighbor-free; validation must reject unusable authored links.
4. Evaluate spatial/tiled recooking and background scheduling for placed static
   geometry. Preserve stable publication/revision contracts and explicit capacity
   failures rather than enlarging the old small region graph.
