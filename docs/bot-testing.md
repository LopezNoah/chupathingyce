# Terminal bot tests and flags

Run all commands from the repository root. Python 3, clang, Ninja and the
platform build dependencies are required. Standalone runners do not need pytest
or game assets.

## Asset-free checks

```sh
python3 tools/test_engine_ai_utility.py --sanitize
python3 tools/test_engine_ai_navigation.py --sanitize
python3 tools/test_surface_navigation.py --sanitize
python3 tools/test_engine_ai_behavior.py --sanitize
python3 tools/test_engine_ai_traversal.py --sanitize
python3 tools/test_bot_movement_log.py
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
| `BOT_COUNT` | 24; supported bot range 1–31 |
| `BOT_SKILL` | `spartan`; also `recruit`, `marine`, `odst` |
| `BOT_SECONDS` | 180; exit timer starts at window creation, not match start |

The script sets these game flags:

| Flag | Meaning |
| --- | --- |
| `HALO_DATA_ROOT`, `HALO_SAVE_ROOT` | Isolate assets/logs and settings/saves |
| `HALO_SOLO_GAME=1` | Private local-host fixture |
| `HALO_NETWORK_TEST=host:bloodgulch:team_slayer` | Start Team Slayer automatically |
| `HALO_NETWORK_TEST_START=5` | Startup countdown |
| `HALO_NETWORK_TEST_LOADOUT=rifle_pistol` | Assault Rifle + Magnum |
| `HALO_BOTS`, `HALO_BOT_SKILL` | Bot count and difficulty |
| `HALO_BOT_DECISIONS=1` | Utility decisions and behavior diagnostics |
| `HALO_EXIT_AFTER` | Timed shutdown |
| `HALO_NET_ONLINE`, `HALO_NET_PUBLIC_LOBBY`, `HALO_NET_HOST_PUBLIC` | Disabled |
| `HALO_NET_LIST_GAMES`, `HALO_NET_REPORT_GAMES`, `HALO_NET_REPORT_EVENTS`, `HALO_NET_ALLOW_UPNP` | Disabled |

`HALO_HIDDEN_WINDOW`, `HALO_NULL_RENDERER`, `HALO_NAV_PROBE` and
`HALO_BOT_SANDBOX` are unset by the script for normal visible gameplay.
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
```

The checker requires both teams' lane assignments, spread-out lane goals and
successful patrol routes in at least two lanes per team. It also rejects an
on-foot bot remaining within 0.5 metres for three consecutive ten-second status
samples. Vehicle samples are exempt (a gunner/passenger may legitimately stay
seated). This is sampled evidence, not a guarantee that every tick moves; route
construction logs do not prove arrival. Navigation failures remain failures,
even when movement recovery prevents idle bots.

Other fixture checkers (use their corresponding three-bot runs, not this 24-bot log):

```sh
python3 tools/check_bot_team_slayer_log.py /path/to/three-bot.log
python3 tools/check_bot_opportunity_log.py gunner /path/to/gunner.log
python3 tools/check_bot_opportunity_log.py weapon /path/to/pickup.log
```

## Still unresolved

- The 85-second visible run at `build/macos/botrun/movement-terminal.log`
  passes sampled movement for all 24 bots, including previously stalled Bots 17
  and 18. This only checks positions at ten-second intervals; it does not prove
  physical progress on every tick. Movement recovery can issue input and change
  direction, but cannot guarantee progress against every collision or geometry.
- BSP search still reports `bad-start`, `bad-goal` and `no-path`. The lane checker
  fails: team 1 produced no successful patrol routes in that run. Movement
  recovery is not a BSP route repair; full lane coverage remains unverified.
- A vehicle entry was observed, but driver-to-teammate pickup and boarding has
  not been verified in the latest run. Vehicle obstacle avoidance and good
  driving remain unverified.
- Only Slayer and Team Slayer are supported. Objective modes, Android builds,
  and other platform builds have not been verified by this bot test procedure.
