# Infection

Infection is an opt-in, local-only rules mode layered over the existing Slayer
engine ID. It is not a network game type: no variant layout, packet, save format,
Delta capability or network version is changed. LAN/online games continue to use
ordinary Slayer, and this adapter refuses to activate unless the connection is
local. It is disabled by default and has no menu entry.

The current feature branch is intentionally stacked on
[`feature/multiplayer-bots`](bots.md)'s feature/build framework. Infection's
manifest is `source/features/infection/feature.json`; select it when configuring:

```sh
python3 configure.py --infection
ninja macos
```

Omit `--infection` (or pass `--no-infection`) for a build with the feature
sources excluded. `--infection` only opts into building the feature; gameplay is
still disabled unless `infection.local_enabled` is set.

## Local rules and current behavior

Settings are registered only when the feature is built:

| Setting | Environment | Default |
| --- | --- | --- |
| `infection.local_enabled` | `HALO_INFECTION_LOCAL` | `false` |
| `infection.rounds` | `HALO_INFECTION_ROUNDS` | `3` |
| `infection.round_seconds` | `HALO_INFECTION_SECONDS` | `180` |
| `infection.alpha_count` | `HALO_INFECTION_ALPHAS` | `0` (automatic) |
| `infection.respawn_seconds` | `HALO_INFECTION_RESPAWN` | `3` |
| `infection.shotgun_rounds` | `HALO_INFECTION_SHOTGUN_ROUNDS` | `18` total rounds |
| `infection.pistol_rounds` | `HALO_INFECTION_PISTOL_ROUNDS` | `36` total rounds |
| `infection.seed` | `HALO_INFECTION_SEED` | `0` (automatic) |

The bounded rules core (`source/features/infection/infection_rules.*`) owns
role changes, round phases, death batching, respawns and personal scoring.
Survivor deaths—including suicide and environmental deaths—convert to Beta
Infected. Alpha/Beta Infected respawn; Survivors do not. Deaths for a simulation
tick are committed together so same-tick results do not depend on callback
arrival order. Survivor/Alpha selection is seeded, without replacement, and
always leaves at least one Survivor.

The local game adapter currently provides:

- Survivors spawn with a shotgun and pistol and no grenades. Infected have no
  weapons or grenades and use Halo CE's ordinary unarmed melee.
- Infected ranged fire, grenades, weapon swaps, action-button use, weapon
  pickups and ammo collection are blocked. Same-faction damage is suppressed.
- Survivors are blue and Infected are red; conversions change team immediately.
  Faction-aware spawn ratings prefer separation from living enemies, and
  vehicles are disabled.
- Countdown/results phases suppress combat. The status message, faction names,
  personal Infection score and faction win/loss are supplied by the adapter.

The energy sword is not a playable multiplayer weapon in stock Halo CE, so this
implementation does not invent assets or edit maps. The experimental mode has
no network synchronization, public/online menu option, bots integration, role
traits, authored-map support or full player-facing menus. Split-screen test
players can have blank profile names. Do not expose this as an online game type.

## Validation

The rules-only test needs no maps and uses strict C compilation plus optional
ASan/UBSan:

```sh
python3 tools/test_infection_rules.py
python3 tools/test_infection_rules.py --sanitize
python3 -m tools.test_features
```

For the real local fixture, provide legally obtained Xbox-format
`bloodgulch.map` and `ui.map` files. The runner uses a fresh data/save root,
hidden window and disables public/online networking. It only starts and stops
its own game process:

```sh
maps="$HOME/Library/Application Support/ChupathingyCE/maps"
python3 tools/test_infection_local.py --maps "$maps" --scenario melee
python3 tools/test_infection_local.py --maps "$maps" --scenario lifecycle
python3 tools/test_infection_local.py --maps "$maps" --scenario melee-full
python3 tools/test_infection_local.py --maps "$maps" --scenario slayer-control
```

`slayer-control` disables Infection and checks that ordinary Slayer's kill
scoring and loadout remain unchanged. These local fixtures are not network
sessions. The feature was tested on macOS; other platform builds are not yet
validated for Infection.

## Standalone rules harness

`tools/infection_prototype.c` is a synthetic participant harness—not a game
client, bot or network endpoint. It can be built without proprietary assets:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  source/features/infection/infection_rules.c tools/infection_prototype.c \
  -o /tmp/infection-prototype
/tmp/infection-prototype players=2
```

Use `show`, `tick N`, `death SLOT KILLER_SLOT`, `death SLOT environment`,
`join SLOT`, `leave SLOT` and `quit`. Queue all deaths from a simulation tick
before advancing it. The prototype exposes configuration and exact rules-level
limits; it does not model Halo movement, weapons, collision, HUD or networking.
See [the implementation notes](infection-implementation.md) for design
boundaries and test evidence.
