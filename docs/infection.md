# Infection — offline rules prototype and local in-game adapter

**Status: Milestone 1 (rules prototype) complete. Milestone 2 (local in-game
adapter) is in progress: local split-screen Blood Gulch Infection runs through
the real game, but it is experimental, disabled by default, and not in any menu.
It is not networked.** Online hosts and clients always play ordinary Slayer. See
[the implementation report](infection-implementation.md) for work still required.

## Local in-game Infection (Milestone 2, experimental)

Local split-screen only, with no server. It needs your own Xbox-format CE maps
(`bloodgulch.map` and `ui.map`). Enable it with settings or environment
variables:

| Setting | Environment | Default |
| --- | --- | --- |
| `infection.local_enabled` | `HALO_INFECTION_LOCAL` | false |
| `infection.rounds` | `HALO_INFECTION_ROUNDS` | 3 |
| `infection.round_seconds` | `HALO_INFECTION_SECONDS` | 180 |
| `infection.alpha_count` | `HALO_INFECTION_ALPHAS` | 0 (automatic) |
| `infection.respawn_seconds` | `HALO_INFECTION_RESPAWN` | 3 |
| `infection.shotgun_rounds` | `HALO_INFECTION_SHOTGUN_ROUNDS` | 18 (loaded + reserve) |
| `infection.pistol_rounds` | `HALO_INFECTION_PISTOL_ROUNDS` | 36 (loaded + reserve) |
| `infection.seed` | `HALO_INFECTION_SEED` | 0 (automatic) |
| `debug.infection_test_map` | `HALO_INFECTION_TEST_MAP` | empty; e.g. `bloodgulch` launches a local game from the main menu |
| `debug.infection_test_players` | `HALO_INFECTION_TEST_PLAYERS` | 2 (2–4 split-screen players) |
| `debug.infection_test_scenario` | `HALO_INFECTION_TEST_SCENARIO` | empty (manual play); `lifecycle`, `melee`, `melee-full` and `slayer-control` are test fixtures only |

What the local adapter does now:

- Survivors spawn with a shotgun and pistol and no grenades. Infected spawn with
  no weapons and no grenades.
- Infected cannot fire, throw grenades, swap weapons, use the action button, or
  pick up weapons or ammunition. They attack with the game's existing unarmed
  melee, which one-hit kills a full-health Spartan face to face (measured).

  > **Note — energy sword:** Infected are meant to carry an energy sword. Halo CE
  > has no playable energy sword in multiplayer (the sword only exists as an
  > AI-only Elite weapon), so it can't be used here without new assets or map
  > edits. Infected use unarmed melee instead.
- When a Survivor becomes Infected, they join the Infected team immediately,
  on the same tick they die and before they respawn. Survivors are the blue
  team and Infected the red team, matching the scoreboard colours.
- Vehicles are turned off. Infected also cannot enter vehicles, because CE
  uses the action button to board and that button is blocked for them.
- Spawns are faction-aware: points within 8 world units (about 24 m) of a
  living enemy are almost never chosen, and points further away are preferred.
- Any Survivor death, including suicide or environmental death, converts that
  player to Beta Infected. Infected respawn after the configured delay. Survivors
  do not respawn.
- Personal scores use the rules defaults. Slayer team scores are not used.
- The game resets units between rounds without scoring the reset as deaths.
- Players see faction colours and a text status line with role, round, time,
  Survivor count, score, and phase ("Last Spartan Standing!" for the final
  Survivor). At round end each player sees "Your team won/lost" for their
  current faction. The postgame report shows faction names and Infection
  scores. All of this was checked in screenshots.
- Combat is disabled during countdown and results phases.

Not done yet: Infected movement or other traits, adjustable melee strength,
Last Spartan effects beyond the numeric bonus, menu selection, networking, and
bots. Split-screen players in the isolated test have no profile names, so the
score lists show blank names.

Automated real-game tests (they build, launch a hidden window, and check the log):

```sh
M="$HOME/Library/Application Support/ChupathingyCE/maps"
python3 tools/test_infection_local.py --maps "$M" --scenario melee
python3 tools/test_infection_local.py --maps "$M" --scenario lifecycle
python3 tools/test_infection_local.py --maps "$M" --scenario melee-full
python3 tools/test_infection_local.py --maps "$M" --scenario slayer-control
```

`melee-full` strikes a full-health, face-to-face Survivor (up to 12 times)
and reports how many strikes it took. `slayer-control` runs with Infection
**disabled**. It plays a real Slayer kill and checks that the adapter stays
off, Slayer scores the kill (1/0), and the map's Slayer loadout is the same at
spawn and respawn. Every scenario also checks spawn distances, team changes
and zero vehicles.

`melee` checks that the Infected pistol pickup and ranged inputs are blocked,
then kills a deliberately wounded Survivor with normal unarmed melee input.
`lifecycle` uses three players and three rounds. It checks Survivor-kills-Alpha,
Alpha respawn, infection, Beta respawn, environmental death, suicide, both
victory types, ammunition, stable player identities, and seeded final scores of
3/8/10. Fixtures use real damage and death paths, and use positions or wounds
only to make the tests deterministic. If the log contains "window closed", an
outside quit event interrupted the run; run it again.

## Run the prototype

From the repository root, with a C compiler:

```sh
clang -std=c11 -Wall -Wextra -Werror \
  source/game/infection_rules.c tools/infection_prototype.c \
  -o /tmp/infection-prototype
/tmp/infection-prototype players=2
```

No maps or copyrighted assets are needed. Participants are controlled test
identities, not simulated Spartans or AI bots. All decisions are local host
rules; there are no clients.

Commands:

| Command | Effect |
| --- | --- |
| `show` | Phase, roles, scores, remaining phase ticks, Survivors and ranking |
| `tick N` | Advance N ticks, automatically acknowledging eligible harness spawns |
| `death SLOT KILLER_SLOT` | Queue a death; same slot as killer means suicide |
| `death SLOT environment` | Queue an environmental death (killer may also be omitted) |
| `join SLOT` | Admit a new identity to an empty slot; active/countdown joins are Beta |
| `leave SLOT` | Disconnect that identity |
| `quit` | Exit |

Submit every death for a tick **before** `tick 1`. This avoids a fictitious Last
Spartan award between simultaneous deaths. The default simulation is 30 Hz.
`tick 94` reaches an active default round and applies its initial Last Spartan
Standing check. `show` tells you which slot is the Survivor; do not assume a
particular role when choosing a different seed.

For example, with the default seed 42 and two participants:

```text
tick 94
death 1 0
tick 1
```

Slot 1 becomes Beta Infected and the Infected win. `tick 150` finishes the
five-second results interval; `tick 94` prepares and starts the next round.
For a timer victory, leave at least one Survivor unconverted and advance the
round's remaining ticks. The default match has three rounds. The harness prints
personal scores in descending order; tied scores use ascending slot order.

## Configure the prototype

Pass space-separated `key=value` arguments:

```sh
/tmp/infection-prototype players=8 seconds=180 rounds=3 alphas=2 seed=123456
```

| Key | Default | Allowed |
| --- | --- | --- |
| `players` | 2 | 0–128; use `join` while waiting |
| `minimum` | 2 | 2–128 |
| `rounds` | 3 | 1–100 |
| `seconds` | 180 | 1–3600 |
| `alphas` | 0 (automatic) | 0–127; clamped to leave one Survivor |
| `respawn` | 3 seconds | 0–60 |
| `countdown` | 3 seconds | 0–60 |
| `results` | 5 seconds | 0–60 |
| `seed` | 42 | Nonzero 32-bit host RNG seed |

Automatic Alpha count is one for 2–7 participants, two for 8+. Selection is
without replacement, repeatable for a given seed and roster; a production host
must supply its own unpredictable seed rather than use the harness default.

The public `infection_rules_config` API additionally exposes tick rate (1–120),
Last Spartan bonus enablement, and nonnegative scoring values: infection +1,
kill Infected +1, survival +3, Last Spartan +2, faction win +1. Configuration is
validated at initialization and must stay unchanged during a match. Bonuses
are numeric only here: no Overshield, ammunition, markers or audio are applied.
All connected members of the winning faction at round end receive the win
point, including converted Beta participants.

Survivor deaths from any cause convert to Beta. There is no Survivor respawn:
the next eligible spawn is Infected after the configured delay. Scores survive
round resets for continuously connected identities. Reconnects are new
admissions with zero score and a new handle, not a means to regain Survivor
status. If all Infected disconnect, Survivors win; if the last Survivor
leaves, Infected win. An empty active round ends with no awards. A countdown
losing enough participants or an entire faction is abandoned without consuming
a match round; its round ID is never reused.

## Tests

```sh
python3 tools/test_infection_rules.py
python3 tools/test_infection_rules.py --sanitize
```

These compile the actual rules implementation with strict warnings, run the
rules scenarios and a scripted interactive-harness match. They do **not**
exercise Halo weapon restrictions, spawning geometry, HUD, or host/client
synchronization. No in-game `infection` variant is advertised yet, so an
unsupported client cannot accidentally enter this prototype as Slayer.
