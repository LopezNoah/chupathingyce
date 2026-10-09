# Solo custom-game bots: playable milestone

## Supported scope

Up to three computer-controlled players in **FFA Slayer or Team Slayer**, hosted
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

Behavior is RETREAT > FIGHT > ROAM. Bots use limited sight, target memory,
reaction time, skill-dependent aim error, strafing and occasional grenades.
Navigation searches a walkability graph built from player starting locations;
stuck traversal links are disabled and routes replanned. It is not a navmesh.

Weapon input supports automatic fire, slower semi-auto taps, charging/releasing
against visible shielded targets at medium range, reloads, switching away from
empty ammunition weapons, and close-range melee. Charging releases on lost
sight or changed targets. Bots do not deliberately target teammates. There is
**no teammate-in-the-line-of-fire or grenade blast safety policy**, so enemy-only
targeting does not imply friendly-fire immunity.

No objective play, vehicles, weapon pickups, coordinated squad tactics, or
cross-machine replication is claimed. Objective variants are explicitly gated
out. The map-wide navigation and combat policies still need broader testing.

## Configuration and visible private demo

In `config.toml`:

```toml
[bots]
count = 3
skill = "marine"
```

Skills: `recruit`, `marine`, `odst`, `spartan`. Environment overrides:
`HALO_BOTS=3`, `HALO_BOT_SKILL=spartan`.

For the native macOS executable, run from a directory with the proper data/map
roots (prefer isolated save/data roots for testing):

```sh
env HALO_NET_ONLINE=false HALO_NET_PUBLIC_LOBBY=false \
  HALO_NET_HOST_PUBLIC=false HALO_NET_LIST_GAMES=false \
  HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
  HALO_NET_ALLOW_UPNP=false HALO_SOLO_GAME=1 \
  HALO_NETWORK_TEST=host:bloodgulch:team_slayer HALO_NETWORK_TEST_START=5 \
  HALO_BOTS=3 HALO_BOT_SKILL=spartan \
  /path/to/ChupathingyCE.app/Contents/MacOS/halo
```

Use `host:bloodgulch:slayer` for FFA. Leave `HALO_HIDDEN_WINDOW` and
`HALO_NULL_RENDERER` unset for visible play. `HALO_EXIT_AFTER=95` optionally adds
a timed exit; its clock starts when the window opens, not when gameplay begins.

`tools/system_link_bots.py` is a different protocol/load-test utility: it joins
remote stand-in machines that mostly stand still. It is not this gameplay AI.

## Verification for this milestone

- Native `ninja macos` build passes, including the bot and reusable AI objects.
- Navigation, behavior-definition and traversal standalone runners pass with
  sanitizers. Fire-control tests pass with ASan/UBSan: tap spacing, automatic
  fire, charged hold/release, interrupted charge and return to tapping.
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

Known follow-ups:

- Instrument actual charge/release/projectile events; helper tests do not prove
  that every weapon's runtime charging behavior is correct.
- Investigate a reported brief movement/jump interruption under two-bot fire.
  One FFA log sample shows Halo's stunned-movement state while the human was
  badly hurt, but that is not a confirmed reproduction or a fix.
- Longer Team Slayer lifecycle/endgame tests, other maps, grenade safety and
  supported non-macOS native builds.

See [reusable primitive documentation](../source/engine_ai/README.md) for test
commands and ownership/bounds contracts.
