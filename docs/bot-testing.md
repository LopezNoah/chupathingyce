# Terminal bot tests and flags

Run all commands from the repository root. Python 3, clang, Ninja and the
platform build dependencies are required. Standalone runners do not need pytest
or game assets.

## Asset-free checks

```sh
python3 tools/test_engine_ai_utility.py --sanitize
python3 tools/test_engine_ai_aim_claims.py --sanitize
python3 tools/test_engine_ai_navigation.py --sanitize
python3 tools/test_surface_navigation.py --sanitize
python3 tools/test_engine_ai_behavior.py --sanitize
python3 tools/test_engine_ai_traversal.py --sanitize
python3 tools/test_bot_movement_log.py
python3 tools/test_bot_validation_runner.py
python3 tools/test_bot_awareness_log.py
python3 tools/test_bot_perception_steering.py --sanitize
python3 tools/test_human_callouts.py --sanitize
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/test_engine_ai_fire_control.c source/engine_ai/fire_control.c \
  -o /tmp/test-bot-fire
/tmp/test-bot-fire
sh -n tools/run_bot_match.sh
git diff --check
```

`--sanitize` enables AddressSanitizer and UndefinedBehaviorSanitizer;
`--cc clang` selects the compiler on the Python C runners. A successful primitive
test is not proof of live bot movement.

### Camouflage and final steering seams

`test_bot_perception_steering.py` compiles the real perception, recovery and
submission functions extracted from `bots.c` against a fake world (the `.c.in`
file supplies only fixtures/stubs). It checks:

- Active/super camouflage flags and residual fade amounts suppress **new visual
  acquisition and callouts**, even at two metres. There is no close-range visual
  exception yet. An earlier sighting remains a frozen last-known position until
  normal memory expiry; an old teammate callout is likewise a position, not
  live tracking. A recent attacker record still gives nonvisual awareness,
  without making that attacker visible or issuing a callout.
- All on-foot movement is filtered **after stuck recovery, before throttle**.
  Eight candidate headings and two clearance passes bound this to sixteen
  capsule sweeps per bot per tick, each with eight collision planes. The live
  capsule includes crouch height and follows its current support plane uphill.
  Ordinary movement first prefers 0.12 m of extra wall clearance; retreat
  initially accepts 0.02 m. Both can fall back to an exact capsule fit. If all
  probes fail, movement stops rather than submitting a blocked heading.
- Fake-wall, narrow-passage, boxed-in, uphill and seated/no-move cases, including
  submission of recovery-generated movement. Vehicle steering is unchanged.

There is no sneaking behavior. A future stealth behavior may choose a different
clearance preference, but must not bypass collision safety. These tests verify
policy and integration ordering, not the game's actual BSP collision routines.

## Implicit human callouts (Team Slayer)

With `configure.py --bots`, `bots.human_callouts = true` is the default.
Disable it with `HALO_BOT_HUMAN_CALLOUTS=false` or in the isolated config:

```toml
[bots]
count = 7
skill = "spartan"
human_callouts = true
```

Local humans (including split-screen teammates) automatically share an enemy's
last-seen position after **one continuous second** in a conservative cone inside
that player's actual camera view, within 60 m and with clear LOS. Zoom and pitch
matter. Looking away, obstruction, camouflage, death/respawn, a team change or a
missed simulation tick resets acquisition. Fully qualified contacts refresh
only while visible; bots consume them through the existing shared-awareness
policy, not as permission to shoot through walls. All difficulties can receive
human callouts; only Spartans automatically call out their own sightings.

When you take non-silent damage from an enemy, nearby teammate patrol bots
(within 32 m of you) turn toward the incoming-fire bearing for up to two seconds.
The cue is an approximate area 8 m from where you were hit, derived from the
**damage direction**, never the hidden attacker's location or distance. It has
no enemy identity, does not start firing or hijack combat/retreat/pickups, and
is allowed even if the attacker is cloaked. Near misses are not detected yet.
There is no dialogue, ping UI or remote-player callout protocol.

`test_human_callouts.py --sanitize` compiles the real module with fake cameras,
LOS and damage events. It checks the one-second threshold, loss/reset cases,
camouflage, zoom/pitch, team isolation, range, directional-only damage cues and
expiry. Live POV/zoom, split-screen and damage-to-bot-facing validation remain
manual checks: start the 4v4 fixture, hold an opponent in view, then break LOS;
look for `bots: human called out enemy ...` and teammates investigating the last
observed area. After taking a hit, nearby roaming teammates should turn without
shooting until they acquire an enemy themselves. Existing combat/memory can
legitimately take priority over a new human callout.

## Build and offline 4v4 Rat Race match (macOS)

One local host plus seven bots makes eight Team Slayer players. The script
balances them 4v4, disables online/public networking, runs the repository
executable, and defaults this preset to Rat Race:

```sh
python3 configure.py --bots
ninja macos
BOT_PRESET=4v4 BOT_SECONDS=120 sh tools/run_bot_match.sh \
  > build/macos/botrun/ratrace-4v4.log 2>&1
```

The previous Rat Race smoke run passed sampled movement for all seven bots
and logged both weapon and powerup pursuits. That run predates the latest claim
release logging, so it does not validate release/handoff behavior. The host is
the eighth player; there is no additional remote player in this offline fixture. Use
`BOT_HIDDEN_WINDOW=1` for a hidden-window/null-renderer run; it still uses the
platform's SDL video driver, so Linux windowed runs should use the documented
Xvfb setup. For the normal 24-bot Blood Gulch run, use the command below
without `BOT_PRESET`.

## Build and visible 24-bot match (macOS)

```sh
python3 configure.py --bots
ninja macos
# Prepare an isolated data root with maps/ from your own Xbox Halo installation.
# The existing build/macos/botrun/data root can be reused if already prepared.
sh tools/run_bot_match.sh > build/macos/botrun/match-$(date +%Y%m%d-%H%M%S).log 2>&1
```

The script runs `build/macos/halo` directly from the repository, not an app
bundle. It disables public networking/reporting, uses Team Slayer on Blood
Gulch, and equips Assault Rifle + Magnum. Keep existing run data and saves;
do not remove them just to rerun. For separately preserved evidence, use a new
data/save root for each run and copy or symlink your legal Xbox maps into it.

Script overrides:

```sh
BOT_DATA_ROOT=/absolute/path/to/test-data \
BOT_SAVE_ROOT=/absolute/path/to/test-saves \
BOT_COUNT=24 BOT_SKILL=spartan BOT_SECONDS=180 \
sh tools/run_bot_match.sh
```

| Script variable | Default / meaning |
| --- | --- |
| `BOT_BINARY` | Repository `build/macos/halo`; override for another built executable |
| `BOT_DATA_ROOT` | `build/macos/botrun/data`; contains Xbox `maps/` and game log |
| `BOT_SAVE_ROOT` | `build/macos/botrun/saves`; isolated settings/saves |
| `BOT_PRESET` | `24` (Blood Gulch, 24 bots) or `4v4` (Rat Race, 7 bots plus host) |
| `BOT_MAP` | Map file name; preset default is `bloodgulch` or `ratrace` |
| `BOT_GAME` | `team_slayer` (default) or `slayer` |
| `BOT_COUNT` | Preset default; supported bot range 1–31 |
| `BOT_SKILL` | `spartan`; also `recruit`, `marine`, `odst` |
| `BOT_LOADOUT` | `rifle_pistol`; Assault Rifle + Magnum |
| `BOT_SECONDS` | 180; requests an exit after this many seconds. Timed exit did not fire in the latest headless run, so stop headless runs externally |
| `BOT_HIDDEN_WINDOW` | `1` for hidden-window/null-renderer test runs |

The script's `BOT_COUNT` and `BOT_SKILL` override the saved configuration. For a
normal direct launch, persistent settings live in the save root's `config.toml`.
For an automatically started offline Rat Race match, merge these sections into
an **isolated test save root's** config (do not replace your normal config):

```toml
[bots]
count = 7
skill = "spartan"

[network]
online = false
public_lobby = false
host_public = false
list_hosted_games = false
report_joined_games = false
report_events = false
allow_upnp = false

[debug]
solo_game = true
network_test = "host:ratrace:team_slayer"
network_test_start = 5.0
network_test_loadout = "rifle_pistol"
bot_decisions = true
exit_after = 120.0
```

Launch that config with the built executable (Xbox maps must be in the data
root's `maps/`):

```sh
HALO_DATA_ROOT=/absolute/path/to/test-data \
HALO_SAVE_ROOT=/absolute/path/to/test-saves build/macos/halo
```

Environment overrides take precedence; clear stale `HALO_BOTS`, `HALO_BOT_SKILL`
and `HALO_NETWORK_TEST` overrides when using the config example. Remove the
`network_test`/`solo_game` test settings when returning to normal play.

The script enables decision logging automatically. The script sets these game flags:

| Flag | Meaning |
| --- | --- |
| `HALO_DATA_ROOT`, `HALO_SAVE_ROOT` | Isolate assets/logs and settings/saves |
| `HALO_SOLO_GAME=1` | Private local-host fixture |
| `HALO_NETWORK_TEST=host:<map>:<game>` | Start the selected private match automatically |
| `HALO_NETWORK_TEST_START=5` | Startup countdown |
| `HALO_NETWORK_TEST_LOADOUT=rifle_pistol` | Assault Rifle + Magnum |
| `HALO_BOTS`, `HALO_BOT_SKILL` | Bot count and difficulty |
| `HALO_BOT_DECISIONS=1` | Utility decisions and behavior diagnostics |
| `HALO_EXIT_AFTER` | Timed shutdown |
| `HALO_NET_ONLINE`, `HALO_NET_PUBLIC_LOBBY`, `HALO_NET_HOST_PUBLIC` | Disabled |
| `HALO_NET_LIST_GAMES`, `HALO_NET_REPORT_GAMES`, `HALO_NET_REPORT_EVENTS`, `HALO_NET_ALLOW_UPNP` | Disabled |

The script sets `HALO_HIDDEN_WINDOW` and `HALO_NULL_RENDERER` only when
`BOT_HIDDEN_WINDOW=1`; otherwise it clears them. It always unsets
`HALO_NAV_PROBE`, `HALO_BOT_SANDBOX` and the pickup fixtures for normal runs.
For manual controlled tests, see [bots.md](bots.md): `HALO_BOT_SANDBOX=1`
with three bots exercises gunner boarding; `HALO_NETWORK_TEST_PICKUP=5`
and `HALO_NETWORK_TEST_PICKUP_WEAPON=sniper` exercise pickup.
`HALO_NAV_PROBE=1` overrides Bot 1 for geometry diagnostics, not normal AI;
`HALO_NAV_DUMP=bloodgulch.nav` dumps derived geometry. See [navigation.md](navigation.md).

## Check the match afterward

```sh
# Use the fresh match-*.log captured above; debug.txt may contain older runs.
python3 tools/check_bot_navigation_log.py /path/to/match.log --minimum-bots 24 --movement-only
python3 tools/check_bot_navigation_log.py /path/to/match.log --minimum-bots 24
python3 tools/check_bot_awareness_log.py /path/to/ratrace-4v4.log --require-pursuit
```

The full lane checker is for the 24-bot fixture: it requires both teams' lane
assignments, spread-out goals and successful patrol routes in at least two lanes
per team. It counts distinct assigned bots, not repeated assignment lines. The
seven-bot 4v4 fixture cannot populate all three lanes per team; use movement-only
there rather than treating that structural mismatch as a navigation failure.
Both modes require sufficient status samples and reject crash/assertion logs.
An on-foot bot remaining within 0.5 metres **horizontally** for three consecutive
same-life status samples fails; vertical jumping cannot hide a wall stall.
Respawns reset the stationary window. Vehicle samples are exempt (a passenger
may legitimately stay seated). This is sampled evidence, not a guarantee that every tick moves; route
construction logs do not prove arrival. Navigation failures remain failures,
even when movement recovery prevents idle bots.

The awareness checker scopes each claim to its team: it rejects duplicate
same-team pursuit until a give-up/release event, but permits opposing teams to
pursue the same world object. New pursuit and release logs carry an explicit
team; older logs use the bot's team assignment when available. FFA uses one
shared scope.

Other fixture checkers (use their corresponding three-bot runs, not this 24-bot log):

```sh
python3 tools/check_bot_team_slayer_log.py /path/to/three-bot.log
python3 tools/check_bot_opportunity_log.py gunner /path/to/gunner.log
python3 tools/check_bot_opportunity_log.py weapon /path/to/pickup.log
```

## Fresh movement/navigation baseline (2026-10-11)

The PDF comparison and next feature order are in
[bot-pdf-gap-analysis.md](bot-pdf-gap-analysis.md).

An isolated POSIX/macOS evidence runner (Python 3.11+) creates fresh data/saves,
symlinks existing legal maps, clears inherited Halo/test overrides, disables
human callouts for a navigation baseline, and runs hidden/null-renderer:

```sh
python3 tools/run_bot_validation.py --map ratrace --bots 7 --seconds 75
python3 tools/run_bot_validation.py --map bloodgulch --bots 24 --seconds 75
# Run movement-only on either printed game.log; the full lane check is for 24 bots.
python3 tools/check_bot_navigation_log.py /printed/run/game.log --minimum-bots 7 --movement-only
```

The runner records `game.log`, isolated `data/debug.txt`, `run.json` (binary hash,
source commit/dirty flag, settings, exit status), and `trace.json` when available.
Its exit is not a checker verdict. The watchdog allows five seconds for SIGTERM,
then SIGKILL, and acts only on its own process group. Keep these local artifacts;
do not commit proprietary map-derived navigation dumps or copy maps into Git.

Both runs used the current dirty tree on base commit `76648ffd`, Spartan bots,
Team Slayer, rifle/pistol loadout and human callouts disabled. Both exceeded the
75-second run deadline, needed watchdog SIGKILL after five seconds of shutdown
grace, and lasted about 80 wall seconds. No assertion/crash signature was found
before that deliberate termination. Natural timed shutdown remains unresolved.

Evidence roots under `build/macos/botrun/validation/`:

| Run | Evidence directory | Revised movement check | Navigation result |
| --- | --- | --- | --- |
| Rat Race, 7 bots | `ratrace-7bots-20261011T044611-089820Z` | Pass: every bot has 5–7 samples; no detected same-life horizontal stall | 299 published polygons; full three-lane check not applicable to this fixture |
| Blood Gulch, 24 bots | `bloodgulch-24bots-20261011T044739-391345Z` | Pass: every bot has 4–8 samples; no detected same-life horizontal stall | 2,807 polygons; **fail**: team 0 has only two patrol routes, both in lane 2 |

Search failure counts in captured stdout (patrol and tactical combined):

| Run | no-path | bad-goal | bad-start | stale |
| --- | ---: | ---: | ---: | ---: |
| Rat Race | 2,347 | 1,296 | 361 | 1 |
| Blood Gulch | 10,481 | 2,608 | 100 | 0 |

These passes are sampled evidence, not goal-arrival proof or validation of every
corner/stair/door. Frequent failures remain real even when recovery keeps bots
moving. No fresh walking-probe/arrival evidence has been collected yet.

The trace files are **not adequate performance evidence**: the final reports
retain only 4 Rat Race / 3 Blood Gulch AI ticks and report millions of lost trace
events in the unpaced null-renderer runs. Do not infer full-run averages or a
worst-case capsule-probe budget from these snapshots.

Next diagnosis should separate tactical endpoint projection (aim positions versus
ground-foot goals), static connectivity/capsule-clearance rejection, and transient
occupants/obstacle revisions. Reproduce these in a one-bot geometry probe before
changing routing; also establish actual patrol-arrival logging and recheck the
24-bot fixture. No production navigation policy was changed in this validation
pass. Checker regressions now cover jumping-in-place, respawn windows and crashes;
runner tests cover isolation, natural failure and owned-process watchdog cleanup.

## Still unresolved

- Camouflage and final-steering regression tests pass under ASan/UBSan, and
  `ninja macos` builds. Fresh sampled movement evidence is recorded above, but
  controlled geometry acceptance still needs corners, narrow doors, ramps/stairs,
  crouching, moving occupants and recovery in Rat Race and Blood Gulch. The probe horizon is
  0.65 m; current velocity, ledges and step negotiation are not modeled by the
  steering selector, and stopping in a fully blocked pocket is intentional.
- Measure capsule-probe cost in a 24/31-bot match before extending the budget.
  Tune the close-range camouflage exception only after a deliberate design
  decision; it is currently disabled. Sneaking remains future work.
- Android validation was attempted, but this configured tree has no `android`
  Ninja target. Linux/Windows/Android builds remain to be checked; existing
  `configure_args = --bots` was preserved.
- The 85-second visible run at `build/macos/botrun/movement-terminal.log`
  passes sampled movement for all 24 bots, including previously stalled Bots 17
  and 18. This only checks positions at ten-second intervals; it does not prove
  physical progress on every tick. Movement recovery can issue input and change
  direction, but cannot guarantee progress against every collision or geometry.
- BSP search still reports `bad-start`, `bad-goal` and `no-path`. The lane checker
  fails: team 1 produced no successful patrol routes in that run. Movement
  recovery is not a BSP route repair; full lane coverage remains unverified.
- The previous Rat Race log predates claim-release logging. Re-run the
  awareness checker on a fresh match to validate handoffs after abandonment,
  death and bot exit.
- Aim-profile helper tests pass, but accuracy and difficulty differences have
  not been measured in a live match. Teleport reacquisition also needs a
  teleporter-map test.
- A vehicle entry was observed, but driver-to-teammate pickup and boarding has
  not been verified in the latest run. Vehicle obstacle avoidance and good
  driving remain unverified.
- Only Slayer and Team Slayer are supported. Objective modes, Android builds,
  and other platform builds have not been verified by this bot test procedure.
