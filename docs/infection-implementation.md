# Infection implementation notes

## Status and build boundary

The Infection work lives on `feature/infection`, stacked on
`feature/multiplayer-bots`. It reuses that branch's manifest-based feature
selection and extension infrastructure but does not register an Infection
extension: its manifest sets `extension: false`, because the existing Slayer
engine hooks are the adapter seam used here.

`--infection` is opt-in and compiles `source/features/infection/`; the default
build excludes those C sources and their settings. Runtime activation also
requires `infection.local_enabled`, a Slayer variant and a local connection.
The adapter does not assign a new engine ID. Non-local games use ordinary
Slayer. Infection has no multiplayer protocol, packet, saved-variant layout,
Delta capability or network-version change; the network state read/write hooks
are suppressed while the local adapter is active rather than misrepresenting
Infection as Slayer state.

This is experimental local gameplay, not a publishable network mode. It has no
menu entry, and no online match or remote-player behavior is supported. Keep it
offline until there is a separately designed/versioned admission protocol and
host/client synchronization work.

## Components

- `source/features/infection/infection_rules.*`: allocation-free local rules
  state machine. Admission handles, round IDs and life IDs distinguish stale
  deaths and reused player slots. Deaths are batched before role conversion and
  victory checks. The header explicitly is not a wire format.
- `source/features/infection/game_engine_infection.*`: local Slayer adapter;
  loadouts, spawn scoring, roles/teams, action and pickup restrictions, damage
  policy, local HUD/status text, score presentation, lifecycle and local test
  fixtures.
- `source/features/infection/settings.inc`: feature-only settings fragment,
  included in the settings table by the feature build define.
- `tools/test_infection_rules.*`, `tools/infection_prototype.c`: asset-free
  rules tests and synthetic command-line harness.
- `tools/test_infection_local.py`, `tools/check_infection_local_log.py`:
  hidden-window Blood Gulch integration fixtures using owned Xbox-format maps.

The small core call sites in `game_engine.c`, `game.c`, `players.c`,
`weapons.c` and `main.c` are guarded by `HALO_FEATURE_INFECTION`. They do not
change networking structures. `game_engine_loadout_weapon_definition` is made
available to the adapter for Survivor starting weapons.

## Rules invariants

- Configuration is validated on initialization; selection is seeded and always
  leaves at least one Survivor. Automatic Alpha count is one for 2–7 players,
  two for 8+.
- Participant slots are bounded at 128. Admission handles increase through a
  session; rejoining into a reused slot cannot inherit score, role or a queued
  death. Death records also require the current round and spawned-life ID.
- One accepted death per participant/life per tick is queued. The complete
  batch is resolved before checking round victory, avoiding order-dependent
  same-tick outcomes. Environment and suicide deaths convert Survivors too.
- The host/local adapter owns all mutators. The data types do not define a
  packet encoding and must not be sent raw over the network.

## Integration details and limitations

The adapter retains Slayer's existing ID and variant representation. It chooses
its runtime `game_engine` callback table only for a local Slayer session with
the runtime setting enabled. If the connection changes after activation, the
adapter faults rather than silently dropping back to Slayer with custom roles.

The engine's ordinary unarmed melee path is used for Infected. Survivor weapons
are granted during spawn with a narrow allowance for the exact created weapon;
ammo pickup is separately blocked because it bypasses the ordinary weapon
pickup callback. Weapon stripping explicitly removes the held weapon, which
`unit_delete_all_weapons` otherwise preserves. The adapter also clears grenade
counts and player powerups on spawn/reset.

The ordinary Slayer scoreboard/team accumulators are not used for Infection
personal scoring. Team 0 is red (Infected) and team 1 is blue (Survivors), as
CE's scoreboard draws them. Round resets delete units instead of killing them,
so the reset cannot award deaths. The local mode's vehicle set is none.

The debug launcher creates only a local split-screen game; it does not create a
server. Local tests use actual engine damage/death/spawn paths but scripted
positions and, in some scenarios, wounds to make events deterministic. They are
not network tests or broad collision/tactical quality tests.

## Validation performed

On macOS/Apple silicon, on the stacked branch:

| Check | Result |
| --- | --- |
| `python3 tools/test_infection_rules.py --sanitize` | Passed the rules suite, including 480 seeded roster-selection trials, and the scripted prototype; ASan/UBSan enabled |
| `python3 -m tools.test_features` | Passed manifest selection, settings mapping and invalid-manifest checks |
| `ninja macos` with `--infection` | Passed; feature adapter and game linked |
| Local `melee` scenario | Passed; Infected weapon pickup/ranged input blocked and ordinary unarmed melee killed the wounded Survivor |
| Local `lifecycle` scenario | Passed; real deaths/spawns/conversions across three rounds, expected winners and seeded personal scores |
| Local `melee-full` scenario | Passed; one face-to-face unarmed strike killed the full-health target |
| Local `slayer-control` scenario | Passed with Infection disabled; ordinary Slayer score and spawn/respawn loadout unchanged |
| Live network session | Not run; this feature is local-only and has no network protocol |
| Android, Linux and Windows Infection builds | Not run in this macOS environment |

The local fixtures require legal `bloodgulch.map` and `ui.map` files; no map
assets, generated map data or run logs are committed. Use the commands in
[the Infection guide](infection.md) to rerun them.
