# Build-time feature modules

The engine calls the permanent hooks in `source/extensions/extension_api.h`.
Features provide static descriptors; there is no plugin loader or binary ABI.

## Current modules

| Module | Interface | Build selection |
| --- | --- | --- |
| Infection | Ruleset | `--infection`, off by default; runtime admission remains local-only Slayer |
| Bots | Player controller, lifecycle hooks | `--bots`, off by default; host authority and game-report exclusions unchanged |
| Forge | Editor, object-placement hook | On by default; `--no-forge` disables it. Implementation still requires 64-bit desktop, not Android or a server |

Bots and Infection live under `source/features/<name>/`. Forge's manifest and
settings live there too; its desktop implementation remains in
`port/linux/game/forge.c` and `.h`. The SDL raw-input reader remains a platform
service. Platform input capture, interpolation, camera selection, rendering,
scenario placement and BSP events now call the generic dispatcher.

Navigation invalidation before saved-game restoration and tag unloading is a
shared lifecycle hook. Editors expose a world-edit revision, which navigation
uses without knowing about Forge. Forge identifies program-controlled players
through the controller interface, without depending on bots.

## Add a feature

Create `source/features/experiment/feature.json`:

```json
{
  "name": "experiment",
  "default": false,
  "order": 40,
  "help": "Experimental gameplay system",
  "extension": true,
  "settings": "settings.inc"
}
```

Add its C files and headers to that directory. Implement:

```c
#include "extensions/extension_api.h"

static void experiment_objects_placed(void)
{
    /* Your implementation. */
}

struct halo_extension const experiment_extension =
{
    .name = "experiment",
    .objects_placed = experiment_objects_placed,
};
```

`settings.inc`, if present, contains initializer rows using the existing
`config_setting` schema in `port_config.c`. Omit the manifest's `settings`
field if there are no settings. Names, defaults, environment aliases, comments
and platform masks all belong to the feature. Settings can share sections;
the default-config writer groups them, avoiding duplicate TOML tables.

Run:

```sh
python3 configure.py --experiment
ninja macos
```

Discovery supplies the CLI flags, `HALO_FEATURE_EXPERIMENT` define, extension
registry entry and settings includes. Disabled feature directories contribute
no C units, rather than compiling no-op integration functions. No global list
needs editing. A new engine event still needs a permanent hook at its call site.

Use `"extension": false` for a feature with no descriptor. `define` may override
its uppercase feature macro. `sources` documents existing externally located
units, such as Forge's port file; those remain in their existing source-discovery
paths and must gate their implementation themselves. See `tools/features.py`
for the manifest schema and validation.

## Contracts and boundaries

- Notifications run in ascending manifest `order`, then name. Forge remains
  before bots for map object placement, preserving the existing sequence.
- During engine initialization, ruleset selectors are tried in registry order
  against the same original engine and cleaned variant. A selector returns
  `TRUE` and supplies a non-NULL engine to claim the session, or `FALSE` without
  changing state to decline. Acceptance stops selection immediately; it may
  retain the original engine pointer. The selected ruleset owns callbacks until
  engine/map disposal or the next initialization: runtime predicates cannot
  switch ownership mid-session. Missing callbacks fall through to the engine.
  `local_only` blocks reading and writing network state for the owned session.
  Infection's selector still enforces local-only Slayer admission.
- Controllers update before player actions. Their ownership query identifies
  program-controlled players; it does not change network authority.
- Editors update at the original pre-director point. When idle, their update
  callbacks may poll activation in registry order; polling stops immediately
  when one opens. An inactive callback must not edit unless it activates itself.
  Once claimed, only the owner updates and renders; a later activation, even of
  a higher-priority editor, cannot steal ownership. Input capture, interpolation
  bypass and camera selection use that same owner. Closing ends that frame's
  update without polling another editor. The released editor's camera callback
  remains available to drain its per-player one-shot resets, but cannot claim
  the camera; a new owner's camera suppresses old reset requests. Lifecycle,
  object/BSP notifications and revision queries still reach every editor so
  persistent map edits and derived-state invalidation remain intact. Map
  initialization/disposal clears interactive ownership.
- Any extension may veto ordinary game reporting; bots retain their map-wide
  “had bots” check for the browser report and event log.
- Descriptors are constant and callbacks execute on the game thread. This is a
  source interface, not a promised stable binary ABI.

The current portable registry/settings implementation has eight compile-time
slots. Configuration rejects overflow; adding more slots requires changing the
dispatcher and settings include blocks. Unsupported Forge targets register an
empty descriptor when selected; its editor implementation is never activated.

## Validation

Local macOS gameplay smoke-test results and remaining gaps are recorded in
[extension-regression.md](extension-regression.md).

Validated on macOS: all four Infection/bots combinations with Forge enabled,
and both all-off and Infection+bots builds with Forge disabled. Each build
links only the selected descriptors. Non-macOS platform execution is not
validated by this refactor.

```sh
uv run --no-project --with pytest python -m pytest -q tools/test_features.py
python3 tools/test_infection_rules.py
```

The feature tests cover manifest/drop-in selection, disabled-source removal,
dispatcher precedence and fallthrough, network/reporting suppression, controller
ownership, session-stable ruleset selection, exclusive editor updates/rendering,
priority and handoff, per-player camera resets, and parsing defaults with shared
feature settings sections. They do not replace in-game lifecycle, bot or Forge
acceptance testing with map assets.
