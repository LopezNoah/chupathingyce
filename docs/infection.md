# Infection — offline rules prototype

**Status: Milestone 1 complete in a controlled participant harness. Infection is
not yet selectable or playable inside Halo CE.** The game build compiles the
rules module, but does not invoke it. This is not a networked Infection release.
See [the implementation report](infection-implementation.md) for integration
work still required.

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
