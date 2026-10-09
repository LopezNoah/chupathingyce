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
| `source/game/infection_rules.h` | Host rules/configuration API; participant, round and life identities |
| `source/game/infection_rules.c` | Allocation-free, bounded state machine and scoring |
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

## Remaining milestones, in order

1. **Milestone 2:** map opaque admission handles onto actual host player datums
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
