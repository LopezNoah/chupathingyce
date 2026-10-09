# Infection implementation report

## Scope and status

**Completed: Milestone 1, offline rules prototype, tested with controlled local
participants. Not completed: a playable Halo multiplayer round (Milestone 2),
online synchronization (Milestone 3), polish (Milestone 4), or bots (Milestone 5).**
The final Blood Gulch launch/menu goal remains outstanding. The module is
compiled into the native game build, but no engine callbacks activate it yet.
There are no fake menu entries, placeholder network handlers or unsupported
assets masquerading as implemented features.

Starting state: local `main`, commit `d0568e2b` (Start bot navigation work),
clean worktree. Local sources were used, not GitHub. No reset, unrelated source
replacement, commit, push or force-push was performed.

## Files added

| File | Purpose |
| --- | --- |
| `source/features/infection/infection_rules.h` | Host rules/configuration API; participant, round and life identities |
| `source/features/infection/infection_rules.c` | Allocation-free, bounded state machine and scoring |
| `tools/test_infection_rules.c` | Automated scenarios, 480 seeded roster-selection trials |
| `tools/test_infection_rules.py` | Strict compilation, optional ASan/UBSan, scripted prototype test |
| `tools/infection_prototype.c` | Interactive controlled-participant harness |
| `docs/infection.md` | Prototype guide, configuration, exact limitations |
| `docs/infection-implementation.md` | This report |

No existing tracked files changed. Generated ignored build products were
regenerated using `python3 configure.py` with the existing default arguments.
The native build discovers `source/**/*.c`, so no project-file or source-list
change was necessary. Tests build the core independently of the Xbox datum ABI;
the macOS game build also compiles it through the existing LP64 pipeline.

## Implemented rules

- Explicit waiting, preparation, Alpha selection, countdown, active, round
  results and match-results phases; at most one phase transition per tick.
- Validated configuration, default two-player minimum, three 180-second rounds,
  three-second Infected respawn, and automatic/overridden Alpha counts.
- Host-seeded selection without replacement, always leaving a Survivor.
- Separate Survivor/Alpha/Beta roles and immutable session admission handles.
  Handles must increase on every admission; slot reuse/reconnect never inherits
  another player's queued death, role or score. No Halo player identity is changed.
- Death validation by admission handle, round ID and spawned-life ID. Duplicate,
  stale-round, old-life and invalid-slot deaths are rejected. Events are not
  received from clients: callers must be host-authoritative.
- Bounded death batching, role conversion after every Survivor death including
  suicide/environment, Infected respawn delay and per-life duplicate protection.
  Kill awards use pre-batch roles, including reciprocal kills in one tick.
- Victory after draining the whole tick's death batch. Zero Survivors wins for
  Infected before a same-tick timer expiry. Otherwise timeout awards Survivors.
- Numeric Last Spartan bonus, once per round, only for a living final Survivor.
  No transient award while processing simultaneous deaths. Round-end latch reset.
- Personal scores across rounds, configurable awards, saturating arithmetic,
  deterministic final connected-participant rankings and bounded top-N output.
- Active/countdown late joins are Beta; the next selection restores eligibility.
  All Infected leaving awards Survivors; last Survivor leaving awards Infected.
  An empty round gives no awards. Broken countdowns reselect in a new round ID
  without counting as completed rounds.
- Humans and future bots can use identical admission/death/spawn APIs.

The core only acknowledges spawn eligibility. The interactive harness marks
those participants alive automatically; it does not create units or implement
weapon, collision, damage, HUD or AI simulation.

## Architecture investigation and safe integration decision

Local sources inspected included `game_engine.h/.c`, `game_engine_list.c`,
`game_engine_slayer.c`, `players.h/.c`, `player_control.c`,
`port/linux/game/network_distributed.c`, `port/linux/game/menu_functions.c`,
the multiplayer postspawn/death callbacks, the biped/unarmed damage path, native
build generators and the Delta/netcode documentation.

**Preferred next integration: Option B, a dedicated Infection rules layer on
Slayer's existing variant ID, enabled only by a versioned ChupathingyCE mode
identifier. That activation/negotiation is NOT implemented by this milestone.**
The implemented rules layer is independent of Slayer, ready for an adapter;
no existing Slayer callback calls it or contributes Slayer scoring to it.

Reasons not to add an engine enum now:

- Existing IDs and `game_engines` dispatch are fixed. Variant cleanup clamps to
  1–5; menus also clamp/use fixed type-name tables.
- `game_variant` is 0x68 bytes and extended options are 0x1C. Neither changed.
- `game_engine_write_network_state` dispatches fixed per-engine layouts. Slayer
  serializes its existing scores/targets/speeds by copying its state. Appending
  Infection there would break old clients or silently play it as Slayer.
- Delta Peer already provides a capability-negotiated side channel, but its
  current handshake deliberately falls back to OpenCE and happens **after**
  legacy admission. Advertising a custom mode merely in a Slayer name is not a
  safe admission gate. Required-mode sessions need explicit lobby/start gating,
  unsupported-peer rejection, failure on missing Delta support, and an
  Infection capability before participants can enter gameplay. Silent legacy
  fallback remains appropriate only when Infection is disabled.

**Multiplayer/serialization changes in this patch: none.** No capability bits,
protocol versions, packet IDs, legacy packet layouts or saved variant formats
were altered. The header's C structs are local state, expressly not a wire
format. No snapshot codec or client receiver has been added or tested.

The adapter must keep a session epoch as well as round/snapshot sequence IDs on
the eventual wire; `infection_rules_initialize` starts a new local session, so
round/life IDs alone cannot distinguish messages from a previous match.

## CE-compatible melee finding

This local fork already implements unarmed player melee in `bipeds.c`: on-foot
unarmed players receive a bounded melee animation/hit timing, with the normal
melee damage call at the impact tick. `unit_cause_player_melee_damage` in
`units.c` uses the biped's melee damage or `unit_unarmed_melee_damage`, borrowing
an existing weapon's melee damage/response when necessary. It does not equip a
projectile weapon merely to melee.

This is the candidate for the Infected loadout: empty inventory, no grenades,
pickup/fire/vehicle-weapon restrictions on the host. **Source inspection is not
an in-game verification** of unarmed damage on Blood Gulch or Custom Edition
maps. That test and the restrictions must precede enabling real Infection.
No sword, Infinite movement ability, proprietary new asset or compiled map edit
was added.

## Build and test evidence

Executed locally on macOS/Apple silicon:

| Check | Result |
| --- | --- |
| `python3 tools/test_infection_rules.py` | Passed rules harness |
| `python3 tools/test_infection_rules.py --sanitize` | Passed expanded rules plus scripted prototype; ASan/UBSan, strict warnings as errors |
| Strict standalone compilation and interactive scripted prototype | Passed; roles, death, respawn, score output observed |
| `python3 configure.py`, `ninja macos` | Passed; native executable and application bundle linked |
| `python3 tools/test_network_messages.py --build macos` | Passed: 35 legacy messages byte-identical to reference, no out-of-bounds access |
| `ninja linux linux64`, separately `ninja -j2 linux64` | Blocked by current cross-build configuration: `-march=native` resolves to `apple-m1`, invalid for x86 Linux targets |
| Windows/Android/dedicated-server builds | Not run; those targets were not generated in this local configuration |
| Active LSP diagnostics on changed C/header/Python files | Checked; tokenizer warnings fixed rather than suppressed |
| Live Infection host/client session | **Not run; integration/protocol does not exist yet** |
| In-game ordinary Slayer and other modes | **Not run**; their tracked sources/packets were unchanged, native game builds and legacy packet regression pass |

The Linux failure is not a claim about Linux Infection correctness. Cross-build
portability/sysroot setup must be supplied before those builds can be verified.

### Required scenario coverage

Rules-level automated coverage: two players/one Alpha; automatic selection for
2–16 participants with varying seeds, explicit override and 128-slot bounds;
Survivor killed by Infected; environmental and suicide conversion; Survivor
killing an Infected; last Survivor death; timer victory; multiple deaths in one
tick in both arrival orders; reciprocal kills; duplicate events; stale round
and old-life events; active late join; Alpha leaving; final Survivor leaving;
empty round; slot reuse; abandoned countdown reselection; correct numeric
respawn delay; scores retained through three rounds; LSS latch/reset; final
rankings; configuration rejection and score overflow safety.

Not covered: network duplicate/out-of-order **messages**, actual host/client
state agreement, real weapon pickup/fire restrictions, real role-specific unit
spawns, trait cleanup on units, HUD/postgame integration, live Slayer gameplay,
or unsupported-client admission to a custom session. These require later
milestones; passing a local rules event test is not a networking test.

## Milestone 2 progress: local in-game adapter (in progress)

Milestone 1 was committed as `1b7c7edc`. Milestone 2 work is uncommitted.

Added files:

- `source/features/infection/game_engine_infection.{c,h}`: local-only adapter
- `tools/test_infection_local.py`: real-game launcher
- `tools/check_infection_local_log.py`: real-game log checker

Changed tracked files:

- `game_engine.c/.h`: adapter selection, death, spawn, end, colour, message, and
  damage hooks; exposes the loadout weapon resolver; refuses Infection network
  state
- `game.c`: commits the rules tick after objects and players update
- `players.c`: filters player actions
- `main.c`: calls the local launcher
- `port_config.c`: settings
- `items/weapons.c`: ammo-scavenging gate

Activation requires a local connection, Slayer, and `infection.local_enabled`.
No engine ID, variant layout, packet, or Delta capability changed. Non-local
connections keep ordinary Slayer. If the connection changes during a game, the
adapter faults and stops rather than continuing as Infection.

Defects found by real-game tests and fixed:

1. **Survivor loadout failed at spawn.** `unit_add_weapon_to_inventory` applies
   the engine pickup callback even to starting weapons. Spawns happen during
   Countdown, when the Infection pickup policy refuses everything. The fix
   allows only the specific weapon object currently being granted.
2. **Survivors gained ammo during Countdown.** Ammo merging
   (`weapon_handle_potential_inventory_item`) bypasses the pickup callback, so a
   separate gate now applies in Infection.
3. **Ammo was double-counted.** In CE, `rounds_total` includes the loaded
   rounds. The loadout now uses `weapon_set_total_rounds`.
4. **Infected could keep their held weapon.** `unit_delete_all_weapons` keeps the
   currently held weapon. A full strip now removes it too.

Real-game evidence from macOS/Apple silicon, Blood Gulch, hidden window, no
server, `--no-build` reruns after the final build:

| Check | Result |
| --- | --- |
| `tools/test_infection_local.py --scenario melee` | Passed: Infected pistol grant refused; ranged/grenade/zoom/swap input cleared; normal melee input killed the wounded Survivor and Infected won |
| `tools/test_infection_local.py --scenario lifecycle` | Passed: 12 real spawns, stable player datums, increasing life IDs, expected roles/teams/ammo, round winners Infected/Infected/Survivors, final scores 3/8/10 |
| One earlier lifecycle run | Interrupted in round 3 by an SDL window-close/quit event; the identical rerun passed. The checker now reports this case explicitly |
| Rules tests and legacy network-message regression | Passed again |

Team changes are logged. The checker requires every converted player to switch
`Survivors -> Infected` (while still dead) before that player's next spawn. It
also requires zero vehicles at the start of each active round; the variant's
vehicle set is "none". Players with no role yet stay on the Survivors team until
the Alpha is chosen.

Energy sword: the design calls for Infected to use an energy sword. CE has no
playable multiplayer energy sword (only an AI-only Elite weapon), so Infected
use unarmed melee. A sword would need new assets or map edits, which this
project avoids.

The unarmed-melee test uses a 5%-health target. It proves the impact and kill
path, but not the number of hits a full-health Spartan takes.

### Milestone 2 completion work

- **Visual check (screenshots via `debug.screenshot_directory`).** The status
  line renders for both roles, including "Last Spartan Standing!". The Alpha has
  no weapon or ammo HUD, and the Survivor holds a shotgun. The postgame report
  shows Survivors/Infected and the Infection score column.
- **Bug: "Your team won/lost" was wrong.** `did_player_win` returned the top
  *personal* score, so the winning Alpha saw "lost". It now uses the last
  round's winning faction and the player's current faction. Confirmed in
  screenshots.
- **Bug: scoreboard colours didn't match.** Survivors were team 0, which CE
  draws red, while their bodies are blue. Survivors are now team 1 (blue) and
  Infected team 0 (red), set through named constants.
- **Faction-aware spawns** use the existing `starting_location_rating`
  callback. Before the change, a respawned Alpha appeared 5.5 units from a
  Survivor. After it, every spawn with a living enemy was 93–116 units
  away, and the checker requires at least 8.
- **Full-health melee measured.** Unarmed melee uses the biped's own
  `characters\cyborg\melee` damage. It killed a full-health, face-to-face
  Spartan (`facing_dot=-1.000`) in **one** strike. The first measurement hit
  the target's back, which is CE's instant kill, so the fixture now turns the
  target to face the attacker. One-hit kills match the intended sword design;
  adjustable strength is left for Milestone 4 traits.
- **Vehicles.** The vehicle set is "none" (0 vehicles every round on Blood
  Gulch), and Infected can't use the action button that boards vehicles.
- **Ordinary Slayer regression.** `slayer-control` runs with Infection disabled
  through the same launcher. Result: adapter inactive, a real kill scored 1/0,
  and Blood Gulch's Slayer loadout (1 weapon, no grenades) was the same at spawn
  and respawn. Every Infection hook returns immediately when disabled. Other
  modes (CTF, Oddball, King, Race) were not played in-game.

All five real-game scenarios pass: `lifecycle`, `melee`, `melee-full`,
`slayer-control`, plus the earlier wounded-target run. The rules tests and the
legacy network-message regression also pass.

Known limits that don't block local play:

- the isolated test has no profiles, so names are blank
- other modes were not played in-game
- a human hasn't played it with controllers yet; all play so far was scripted

## Build-time feature flags (Infection and bots)

Milestone 2 is committed as `b37bcb35`. After that, Infection and the
multiplayer bots were put behind compile-time flags, following the existing
`--game-browser` pattern.

- `configure.py --infection` defines `HALO_FEATURE_INFECTION`, and `--bots`
  defines `HALO_FEATURE_BOTS`. Both are off by default.
- Each build generator (`linux_build.py` for Linux 32-bit and the server,
  `lp64_build.py` for macOS and Linux 64-bit, `windows_build.py`,
  `android_build.py`) passes `feature_defines(sln)` beside the game-browser
  define. That covers both the game and the port sources.
- With a flag off, the integration units compile their real code out and
  provide only the functions other files call:
  - Infection: inactive, original engine, no launcher.
  - Bots: no joins, `bots_game_had_bots()` returns false, navigation resets
    do nothing.
  - Call sites are unchanged. The pure libraries (`infection_rules.c`,
    `source/engine_ai/`) stay compiled with their standalone tests.
- The settings are `#ifdef`ed out. An old `config.toml` keeps its entries; they
  are logged as unknown and ignored, and apply again once rebuilt with the flag.
- The startup log prints `features: infection on|off, bots on|off`.

Verification on macOS:

| Check | Result |
| --- | --- |
| Default build (both off) | Builds and links. The binary contains no Infection or bot strings or settings. With `bots.count = 2`, `infection.local_enabled = true` and `HALO_BOTS`/`HALO_INFECTION_*` set, the game warns about unknown settings, no bot joins, and no Infection launches |
| `--infection --bots` build | Builds. All four Infection real-game scenarios pass |
| Documented bot Team Slayer smoke test, three runs | One pass (2,610 ticks, bot kill, no friendly fire) and two checker failures. `bots.c` only gains lines, so the enabled code is unchanged; this is existing smoke-test variance (one failed run's status lines show bot kills) |
| Checker on an Infection-off log | Fails with a rebuild hint |

## Remaining milestones, in order

1. **Milestone 2 (remaining):** map opaque admission handles onto actual host player datums
   (and bot participants); drive real death callbacks and normal respawn; update
   player/team/lobby representations consistently; bypass Slayer awards, team
   balancing, suicide/betrayal/lives penalties only for Infection; freeze
   countdown/results combat; round-reset units safely; apply empty Infected
   inventory/no grenades and pistol/shotgun limited-ammo Survivor loadouts after
   every spawn; enforce host pickup/ranged-attack restrictions; verify unarmed
   hits; add role colors and minimal HUD using existing rendering. First test a
   real local two-Spartan round before exposing online hosting.
2. **Milestone 3:** define the versioned mode/rules identity and admission gate,
   reserve a negotiated Delta capability, explicitly encode/validate snapshots
   and roster identities, reject stale epochs/rounds/sequences atomically,
   synchronize scores/roles/round results/team changes, test late join and
   reconnect over actual host/client sessions. Preserve plain OpenCE traffic
   and fallback whenever Infection is disabled. Only then expose safe `infection`
   selection in network menus/variant builders and dedicated playlists.
3. **Milestone 4:** supported configurable role traits, Last Spartan Overshield/
   ammo/navpoint/audio, option menus, safer faction spawn choices, optional Forge
   placements, in-game final rankings and regression playtests of every old mode.
   No unused trait settings are presented as functional by this prototype.
4. **Milestone 5:** attach shared bot controllers/navmesh to these same rules;
   no human-only assumptions were put into the rules API. Do not block human
   Infection on this work.

No advanced gameplay effects or options were enabled before the initial real
multiplayer round. See [prototype usage](infection.md) for what can run today.
