# Solo custom-game bots: playable milestone

> **Feature flag:** bots are compiled in only by `python3 configure.py --bots`,
> and are off by default. Without the flag, the sources under
> `source/features/bots/` are excluded: no bot can join, and the `bots.*`,
> `debug.nav_*` and `debug.bot_sandbox` settings are not offered. Existing
> `config.toml` entries are kept and logged as unknown. The startup log reports
> `features: ... bots on|off`. The reusable `source/engine_ai/` library and its
> standalone tests are unaffected.

## Supported scope

Up to 31 computer-controlled players in **FFA Slayer or Team Slayer**, hosted
by yourself on one machine. Bots are disabled by default (`bots.count = 0`).
They are real Halo player datums with normal spawning, scoring, weapons, damage
and respawning; their inputs enter the same player-action pipeline as a human.
Campaign AI is unchanged.

Only a local game or network host with no remote distributed machines may run
bots. Bots leave when that gate stops permitting them, including postgame,
unsupported modes or a remote machine joining. They are **not replicated bots
for LAN or online matches**. Games bots have participated in are excluded from
game-list/Delta Stats reporting. This is not a substitute for disabling public
hosting/listing when running a private test.

A utility selector scores VEHICLE, RETREAT, SCAVENGE, FIGHT and lane PATROL from
the bot's confidence, target and opportunities. It uses hysteresis to avoid
thrashing and decides every eight ticks, staggered across bots. The combat and
movement outputs are independent: seeing a distant opponent makes the bot aim,
choose whether to pursue or retreat, and close distance; it does not fire past
its skill's fire range or stand still to fire. Only Spartan difficulty currently
shares enemy callouts: the host-local blackboard keeps the last observed
position, and the contact's freshness, distance and bot confidence determine
whether to investigate it. This is not live tracking or hidden information;
callouts are logged, not spoken or shown as HUD text.

Aim is deliberately imperfect and difficulty-scaled. Error grows with range and
while moving, then settles as the bot tracks; it never falls to zero. Recruits
turn slowly, turn worse while moving and drift clumsily instead of stopping to
shoot. Higher skills strafe more capably; Spartans sometimes crouch-strafe.
Sight range is separate from fire range, so a target can attract pursuit without
turning into perfect long-range fire. BSP navigation builds walkable surfaces
and uses collision-checked routes for lane patrol, scavenging and distant
pursuit. The spawn graph remains a fallback if BSP navigation is unavailable
for ten seconds. Three map-relative lanes, paired squads and near/far patrol
phases keep teammates spread while allowing them to rally on a strong contact.

Weapon input supports automatic fire, slower semi-auto taps, charging/releasing
against visible shielded targets at medium range, reloads, switching away from
empty ammunition weapons, and close-range melee. Charging releases on lost
sight or changed targets. Bots do not deliberately target teammates. There is
**no teammate-in-the-line-of-fire or grenade blast safety policy**, so enemy-only
targeting does not imply friendly-fire immunity.

Bots scan up to 256 nearby weapon, equipment and vehicle objects once per
second. Weapons and powerups must be within the bot's sight/field of view (or
arm's reach) and pass a bounded line-of-sight budget; a last-seen item location
is remembered for up to 30 seconds, not live-tracked. They seek usable weapons
for empty slots or clear upgrades, and prefer overshield, camouflage, speed and
vision; health is useful when hurt. One host-local claim per team and item
prevents teammates from choosing the same pickup; opposing teams may pursue it
independently. Claims never reserve the item from a human, who can still race a
bot. A bot abandons an item it cannot route
to for 15 seconds before reconsidering it. A nearby upgrade or powerup can take
priority during combat; distant weapon scavenging does not. Utility is a coarse
damage/rate estimate, not a complete range-aware loadout strategy.

If a target is visibly observed more than six metres from its last observed
position within two game ticks, the bot logs a teleport observation and uses
that ordinary new sighting for chase/retreat utility. It never reads an invisible
target's live destination. The AI does not yet plan a route through teleporter
pairs.

Bots seek empty seats in parked ground vehicles and fixed turrets, never evict
occupants, and prefer a gunner seat when a friendly driver is present. They can
drive supported ground vehicles, man turrets, and use an available passenger
seat. When a driver has no contact to pursue and an extra seat is open, it steers
toward a nearby teammate and stops so the teammate can board normally. There is
no supported horn input, so the invitation is behavioral/logged rather than an
audible signal. Aircraft are excluded; there is no vehicle navmesh, advanced
driving, obstacle avoidance or crew role assignment.

No objective play or cross-machine replication is claimed. Objective variants
are explicitly gated out, so callout-vs-objective ambition scoring is future
work; in the supported Slayer modes, callouts compete with lane patrol and
combat. The map-wide navigation, vehicle and combat policies still need broader
testing. Normal spawn selection remains Halo's own; teammate-proximate spawning
is not forced.

## Configuration and visible private demo

For the repository executable, a runnable script, all test flags and terminal
checks, see [Terminal bot tests](bot-testing.md) and `tools/run_bot_match.sh`.
For a 4v4 Rat Race setup (one host plus seven bots), run
`BOT_PRESET=4v4 sh tools/run_bot_match.sh`; it is an offline local-host fixture.

In `config.toml`:

```toml
[bots]
count = 3
skill = "marine"
```

Skills: `recruit`, `marine`, `odst`, `spartan`. Environment overrides:
`HALO_BOTS=3`, `HALO_BOT_SKILL=spartan`. Set `debug.bot_decisions = true`
(or `HALO_BOT_DECISIONS=1`) to log utility scores and behavior changes.

For the native macOS executable, run from a directory with the proper data/map
roots (prefer isolated save/data roots for testing):

```sh
env HALO_NET_ONLINE=false HALO_NET_PUBLIC_LOBBY=false \
  HALO_NET_HOST_PUBLIC=false HALO_NET_LIST_GAMES=false \
  HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
  HALO_NET_ALLOW_UPNP=false HALO_SOLO_GAME=1 \
  HALO_NETWORK_TEST=host:bloodgulch:team_slayer HALO_NETWORK_TEST_START=5 \
  HALO_NETWORK_TEST_LOADOUT=rifle_pistol \
  HALO_BOTS=3 HALO_BOT_SKILL=spartan \
  /path/to/ChupathingyCE.app/Contents/MacOS/halo
```

Use `host:bloodgulch:slayer` for FFA. Leave `HALO_HIDDEN_WINDOW` and
`HALO_NULL_RENDERER` unset for visible play. `HALO_EXIT_AFTER=95` optionally adds
a timed exit; its clock starts when the window opens, not when gameplay begins.

### Optional controlled gunner test (not normal spawning)

Add `HALO_BOT_SANDBOX=1` to the Team Slayer command with three bots. Once per
map, this **debug-only fixture** puts the host in a parked Warthog's driver seat
and positions teammate Bot 2 near the gunner entrance. Bot 2 must still approach,
select the gunner seat and enter through normal interaction. Keep the Warthog
stopped while it boards. The teleport is intentionally visible and is not a
same-side/team spawn preference. Leave the variable unset for normal games;
`debug.bot_sandbox` defaults to false.

For a controlled weapon pickup test, instead add
`HALO_NETWORK_TEST_PICKUP=5 HALO_NETWORK_TEST_PICKUP_WEAPON=sniper` without the
sandbox variable. This existing network-test fixture positions the last player
on a ground sniper rifle; the normal bot/pickup pipeline must acquire it.

`tools/system_link_bots.py` is a different protocol/load-test utility: it joins
remote stand-in machines that mostly stand still. It is not this gameplay AI.

## Verification for this milestone

- Native `ninja macos` build passes, including the bot and reusable AI objects.
- Utility, aim/claims, navigation, behavior-definition, traversal and
  fire-control standalone runners pass with sanitizers.
- Visible Blood Gulch FFA testing demonstrated movement, combat, kills, deaths
  and respawning. The latest FFA sample ran approximately 46 game seconds.
- Visible Blood Gulch Team Slayer sample ran 1,350 ticks (45 game seconds):
  human and Bot 2 on team 0, Bots 1 and 3 on team 1; nine logged target samples
  were opponent-only. Bot 1 scored a kill and Bot 2 died. No friendly-fire kills
  or crash/assertion were recorded. This is a smoke test, not full-team gameplay
  coverage; Team Slayer respawning and team score/endgame need longer testing.

To validate the sampled Team Slayer evidence from a network-test log:

```sh
python3 tools/check_bot_team_slayer_log.py /path/to/match.log
```

The checker requires this one-human/three-bot fixture, balanced teams, at least
30 game seconds, enemy-only target samples and a bot kill without friendly-fire
kills. It cannot verify every tick, charged projectiles or correct team totals.

For 24-bot lane/patrol coverage:

```sh
python3 tools/check_bot_navigation_log.py /path/to/match.log
```

It requires actual patrol goals and completed patrol routes in all three lanes
for both teams, with goals at least 10 metres apart; tactical routes alone do
not count.

Additional visible smoke tests for weapon/vehicle support:

- Controlled gunner fixture: Bot 2 sought the host's Warthog and entered seat 2
  through the normal interaction path; later status showed vehicle behavior.
- Pickup fixture: Bot 3 acquired the ground sniper rifle into inventory slot 1;
  Bot 1 also gained a second weapon. Bots 1 and 2 independently entered ground
  vehicles as drivers. This proves acquisition/entry, not good driving or turret
  accuracy. The initial sandbox orientation assertion was fixed by preserving
  biped orientation during position-only test teleports.

Validate the corresponding logs with:

```sh
python3 tools/check_bot_opportunity_log.py gunner /path/to/gunner.log
python3 tools/check_bot_opportunity_log.py weapon /path/to/pickup.log
```

Known follow-ups:

- BSP `bad-start`, `bad-goal` and `no-path` searches still prevent full lane
  coverage; movement recovery is not a route fix.
- The previous Rat Race smoke log predates claim-release logging, so live claim
  handoffs after abandonment, death and bot exit remain unverified. Test pickup
  races across maps and with a human collecting the same item.
- Aim helper tests pass, but live accuracy and difficulty differences need
  tuning. Validate visible teleport reacquisition on a teleporter map;
  teleporter route planning is not implemented.
- Instrument actual charge/release/projectile events; helper tests do not prove
  that every weapon's runtime charging behavior is correct.
- Investigate a reported brief movement/jump interruption under two-bot fire.
  One FFA log sample shows Halo's stunned-movement state while the human was
  badly hurt, but that is not a confirmed reproduction or a fix.
- Re-run the 24-bot lane checker after the current patrol/callout changes;
  validate turret combat and the stop-near-teammate boarding behavior; there is
  no horn control in the current player-input API.
- Longer Team Slayer lifecycle/endgame tests, other maps, grenade safety,
  vehicle obstacle avoidance, and supported non-macOS builds.

See [reusable primitive documentation](../source/engine_ai/README.md) for test
commands and ownership/bounds contracts.
