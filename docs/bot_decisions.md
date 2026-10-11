# Bot decisions: Halo Infinite's design in Halo CE

This is the plan for how OpenCE's multiplayer bots (`source/features/bots/`,
built with `configure.py --bots`) decide what to do. It follows Brie
Chin-Deyerle's GDC 2022 talk *Thinking Like Players: How Halo Infinite's Bots
Make Decisions* (slides in `Thinking_Like_Players_Markdown_and_Images/`), mapped
onto this code base. Halo Infinite keeps its game-mode rules and its utility
tuning in Lua; Halo CE's game modes are C (`source/game/game_engine_*.c`), so
everything here is C, with the tuning kept in tables so that it reads like data.

[docs/bots.md](bots.md) describes the milestone this builds on: what bots can
do, how to run them and how they were tested.

## The talk, and where each part lives here

| Talk | Halo Infinite | Here |
| --- | --- | --- |
| Verbs → behaviors (slides 10–15) | Acquire and fight, interact, pick up and deliver, hide and hunt, guard | Behavior-tree leaves in `bots.c`: `fight`, `retreat`, `scavenge` (acquire weapons, interact with vehicles), `vehicle`, `roam` (traverse); later `take`, `deliver`, `guard`, `hunt`, `hold` |
| Game rules in Lua, the authority (17) | Lua writes, BotManager stores | Each game engine's adapter in `bot_manager.c` reads the game engine's state and writes the blackboard. The game engine stays the authority: bots never change scores or objects except as a player does |
| BotManager blackboard (17) | Ambitions plus some bot state | `bot_manager.c`: ambitions, and each team's shared sightings of enemies |
| Ambitions (18–19) | An object or area that scores: type, object, teams allowed | `struct bot_ambition`: kind, object, position and radius, the teams allowed (a mask), the owning team |
| Utility system (21–23) | Every behavior scored, the highest runs | `source/engine_ai/utility.{h,c}`: the selector; `bots.c`: one scoring function per behavior |
| Tuning knobs (26–28) | Fixed and functional values, caps, weighted inputs | `engine_ai_utility_curve` (fixed, linear, power, logistic responses), `engine_ai_utility_weighted`, and caps: `struct bot_utility_tuning` in `bots.c`, one table, so a game mode can have its own |
| Confidence (27) | Bot health, target health, weapons, edge cases, in Lua | `bot_confidence()`: shields and health against the target's, weapon against weapon, enemies seen against teammates near |
| Reaction-time budget (31–32) | 200–250 ms per decision, 8 bots, 31.25 ms each | A decision every 8 ticks (267 ms at 30 ticks a second), staggered by slot so that 31 bots spread their scoring evenly. The behavior still runs every tick; only the choice waits. A behavior that can no longer run is replaced at once |
| Hysteresis (34–35) | Inflate the new behavior's utility for a while | The selector adds a bonus to the running behavior that fades over a second, and a newcomer must beat it by a margin |
| Always Be Shooting (36–37) | Combat is not a behavior the bot must choose | After any leaf that does not fight already, the bot aims and fires at an enemy it can see. Movement comes from the behavior, aim from combat: `bot_submit_action` takes a world-space move, so the two are independent |
| Shared awareness (40–43) | Bots of some difficulties share enemies they have seen | Spartan bots call out only enemies they actually see; each team's last-seen position is stored on the host-local blackboard and read by teammates without a target |
| Awareness overload (41–43) | Shared targets pulled bots off objectives | A shared contact contributes to fight utility through its age, distance and confidence, with a low cap so it cannot pull the whole team off its lane. Objective-mode adapters are not implemented yet, so there is no live objective-vs-contact comparison in the currently supported Slayer modes. |
| Behavioral simplicity (44–47) | Simple "stand in the zone" beat clever positioning | Objective behaviors walk to the ambition and stay in it, fighting from there (ABS); no positioning heuristics |
| Test and validate early (49) | | The selector's standalone test (`tools/test_engine_ai_utility.py`); the decision log (`debug.bot_decisions`); log checkers (`tools/check_bot_*`) |

Only Spartan difficulty currently shares contacts. A callout contains an enemy's
last observed position, not live tracking or hidden information. It is logged
once per new contact, meaningful movement, or four seconds; the blackboard's
freshness window expires it. There is no audible dialogue or HUD radio message.

## Decisions

Each bot keeps:

- the **selector** (`struct engine_ai_utility_selector`): the running behavior,
  when it was chosen, and when the next decision is due;
- the **scores** of its last decision, for the log.

Every tick, `bot_think` perceives (its own sight; then, with no target, its
team's sightings), then:

1. if a decision is due, or the running behavior's score has fallen to zero,
   it scores every behavior (`bot_score_behaviors`) and asks the selector;
2. if the choice changed, the behavior tree is cancelled, so the running leaf
   lets go (its route, its retreat);
3. the tree runs: its priority root's leaves fail at once unless they are the
   chosen one, so the tree descends straight to it. The tree keeps doing what it
   did before: it gives each leaf an intent emitter and rolls back a faulted
   branch's intents;
4. ABS: if the leaf does not engage by itself and an enemy is in sight, the bot
   aims and fires at it. A leaf that must face something (a seat's entrance, a
   weapon on the ground at arm's length) suppresses this;
5. the intents become the player's action, as before.

A behavior's score is 0 when it cannot run (fight with no target, vehicle on
foot), so the selector never picks it.

### Scores

Values are 0 to 1. The table in `bots.c` (`bot_utility_tuning`) holds them:

| Behavior | Score |
| --- | --- |
| `vehicle` | Fixed, while seated (a seated bot drives, guns or rides; leaving is the leaf's choice) |
| `retreat` | `(1 − confidence) × shields-down response`, for skills that retreat, out of cooldown; capped |
| `fight` | By how the target is known: seen, hurt by it, remembered, or a teammate's sighting; times a distance response; raised by confidence. A teammate's sighting is capped low |
| `scavenge` | The opportunity's value (an empty slot, or the weapon's gain over the held one; a seat), times a distance response; a fixed high score within 2.5 m, regardless of current enemy visibility (ABS continues fighting) |
| `roam` | A lane-patrol floor: after contact or scavenging stops mattering, the bot routes to its assigned map-relative lane and alternates near/far patrol depths; two bots share a lane as a squad |

Objective behaviors (Phase 2) take their scores from the ambitions: a
flag carrier's `deliver` is high and rises as the base nears; `guard` rises
when the team's flag is home and no teammate guards it; `hold` rises when the
hill is near and empty of teammates.

### Confidence

```text
confidence = weighted(
    own vitality (shields count double),        weight 3
    1 − target vitality,                         weight 2
    own weapon ÷ (own + target's weapon),        weight 1
    teammates near ÷ (teammates + enemies seen),  weight 1)
```

It is computed once per decision. Retreat rises as confidence falls; fight
rises with it. The target's vitality is what a player can judge from shield
flashes and how the target took its last hits.

## Phases

### Phase 1: the framework (done in this change)

- `engine_ai/utility`: responses, weighted inputs, the selector with cadence,
  hysteresis and a switching margin; standalone test.
- `bots.c`: the priority tree's choice replaced by utility scores; confidence;
  decisions every 8 ticks, staggered; ABS for every leaf; the decision log.
- `bot_manager.c`: the blackboard, ambitions (the type and API; Slayer
  publishes none) and shared awareness, written only by Spartan bots, read by
  teammates. In Slayer, a recent callout competes with lane patrol via fight
  utility; objective-mode comparison awaits the objective adapters.
- `bot_navigation.c`: collision-checked BSP routes for patrol, scavenging and
  pursuit; map-relative three-lane assignments keep squads spread. Moving
  targets replan after 12 metres rather than restarting on small motion.

### Phase 2: ambitions and objective behaviors

- Read-only accessors for each game engine's state where it is file-static
  (`game_engine_king.c`'s hill, `game_engine_oddball.c`'s balls,
  `game_engine_ctf.c`'s flags and stands, `game_engine_race.c`'s next flag),
  each a `port:` change.
- Adapters that publish ambitions every decision: CTF (enemy flag: take; own
  stand: deliver while carrying; own flag away: return, hunt its carrier),
  Oddball (ball: take; carrier: hunt; while carrying: survive), King (hill:
  hold), Race (next flag: reach).
- Behaviors `take`, `deliver`, `guard`, `hunt`, `hold`: walk to the ambition
  through `bot_navigation`'s BSP route, with an explicit fallback only when the
  map's surface resource is unavailable.
- Lift the Slayer-only gate in `bots_host_may_have_bots` mode by mode, as each
  adapter is tested.

### Phase 3: team play

- The first lane squads and host-local enemy callouts now exist. Next: roles
  from the blackboard, such as how many teammates already pursue an ambition
  (one guard, the rest attack), written by each bot when it chooses.
- Teammate-aware spawning is left to the game, as now.

### Phase 4: the network

Bots replicated to other machines (they are host-only now). Out of this plan's
scope; it needs `HALO_PORT_NETWORK_VERSION` and the netcode's rules
(`port/linux/NETCODE.md`).

## Testing

- `python3 tools/test_engine_ai_utility.py --sanitize`: the selector and the
  responses, without Halo.
- `debug.bot_decisions` (`HALO_BOT_DECISIONS=1`) logs every change of behavior
  with all its scores and the bot's confidence:
  `bots: bot 2 fight -> retreat (conf 0.21) vehicle 0.00 retreat 0.62 ...`.
- The Blood Gulch runs of [docs/bots.md](bots.md), and its log checkers.
