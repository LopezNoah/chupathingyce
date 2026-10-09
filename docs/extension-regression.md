# Local extension gameplay regression results

Tested locally on macOS against `95c9a61` (ownership fixes), with
`--infection --bots --lto off --pgo off`; Forge enabled. No CI runner changes.

Maps were supplied from the user's existing Xbox-format map directory.
Runs used isolated data/save roots, hidden windows, and disabled public
networking and auto-updates. Original maps were not modified; Forge overlays
and the navigation dump remain outside the repository.

## Network isolation

Public internet play, public lobby/listing, UPnP and clipboard joining were
disabled throughout. The initial host smoke tests did not pin LAN binding or
broadcast destinations; public-listing flags alone do **not** prevent LAN
announcements. Subsequent runs explicitly set both:

```sh
HALO_NET_ADDRESS=127.0.0.1
HALO_NET_BROADCAST=127.0.0.1
```

A live socket snapshot during the gunner fixture confirmed that all game TCP/UDP
listeners and connections used `127.0.0.1`, not wildcard/LAN addresses.
`tools/test_infection_local.py` now also supplies these loopback overrides.
Reporting was disabled except for explicit suppression checks, where the browser
URL pointed exclusively to an ephemeral HTTP mock bound to `127.0.0.1`.
No public reporting service was configured for those checks.

## Results

| Scenario | Result | Evidence |
| --- | --- | --- |
| Infection lifecycle | PASS | Existing integration checker; 12 real unit spawns |
| Infection melee | PASS | Existing integration checker; 2 real unit spawns |
| Infection melee-full | PASS | Existing integration checker; 2 real unit spawns |
| Ordinary Slayer control | PASS | Existing integration checker; Infection compiled in but inactive |
| Bot Team Slayer | Smoke evidence PASS | Existing checker: 2,131 ticks, balanced teams, three enemy-only targeting samples, a bot kill, no sampled friendly-fire kills |
| Navigation walking probe | PASS | Clean exit; existing checker; resource with 2,807 polygons |
| Forge edit/save | PASS | Clean exit; camera movement, scenery movement/deletion, vehicle placement, save, undo/redo, close with gameplay unpaused |
| Forge saved-overlay reload | PASS | Separate clean process; verify reports 3 of 3 edits on the map |
| Slayer → lobby → CTF | PASS (sampled) | Same process: two starts, second-map gameplay, no bot join/player-1 samples in CTF |
| Bot gunner fixture | PASS | Loopback-only, clean exit; existing opportunity checker |
| Bot weapon-pickup fixture | PASS | Loopback-only, clean exit; existing opportunity checker |
| Event-recording suppression | PASS (limited) | Loopback-only reporting enabled; no-bot control recorded events; bot admission discarded recording and sent no POSTs |
| Infection → menu → Slayer → menu → Infection | PASS (sampled) | Loopback-only, same process; custom/none/custom session owners and blocked/ordinary/blocked network state |
| Open Forge → menu | PASS | Loopback-only, same process; editor active/input capture changed from 1/1 to 0/0 |
| Dispatcher contract tests | PASS | Five feature tests, including competing owners and per-player camera resets |
| Sanitized navigation/AI checks | PASS | Surface navigation, AI navigation, behavior definitions and traversal |

The bot run was interrupted before timed shutdown. Its retained gameplay log
passes `tools/check_bot_team_slayer_log.py`, but this is not evidence of clean
shutdown, complete per-tick AI correctness, or endgame/reporting behavior.
Navigation ran for a 30-second timed window; Forge edit/save for 25 seconds and
reload verification for 15 seconds. Infection used the existing four launcher
scenarios and their configured time limits.

The same-process ownership checks used temporary fourth-feature modules
registered through `main_frame_update`, plus the existing local map/menu APIs.
The ruleset-transition module sampled each session after at least 90 ticks. The modules were
removed and the original build flags restored afterward; no shared engine code
was changed for these checks. The Forge-menu fixture intentionally left Forge
open rather than completing its edit script, to exercise cleanup of a live owner.

## Retained local logs

Infection launcher transcripts are at
`/tmp/ownership-infection-{lifecycle,melee,melee-full,slayer-control}.log`.
Each records the isolated directory containing its full `run.log`.

The other logs are under the local temporary directory
`extension-gameplay-_7dc8ia2`:

- `bots-team.log`
- `navigation.log`
- `forge-edit.log`
- `forge-verify.log`

Additional temporary roots contain:

| Root suffix | Log |
| --- | --- |
| `extension-verify-g885hiwr` | `slayer-to-ctf.log` |
| `extension-verify-c_lcvbft` | `bot-gunner-loopback.log` |
| `extension-verify-ov2i_dxr` | `bot-weapon-loopback.log` |
| `extension-verify-eyhwdz8w` | `loopback-report-control.log` |
| `extension-verify-kb1ok242` | `loopback-report-endgame.log` (did not reach endgame) |
| `extension-verify-y9i271iy` | `infection-menu-slayer-menu-infection.log` |
| `extension-verify-33a2uo7t` | `forge-menu-owner-release.log` |

These are temporary local evidence, not checked-in assets. Do not commit
proprietary maps or derived geometry dumps.

## Still outstanding

- Full bot endgame/carnage-report suppression. The reporting-enabled run
  confirmed event recording was dropped, but did not reach match end within
  its 32-second window; no POSTs alone does not prove the endgame path.
- Bot admission rejection when another machine joins. The stand-in utility's
  default `127.0.0.2` address was unavailable on this Mac; no network-interface
  changes or LAN/public clients were used to work around it.
- Forge reopening and continuing edits after another map loads in the same
  process. The completed check proves release at the menu, not reactivation.
- Human keyboard/mouse interaction and full camera/input coverage. The Forge
  fixture exercises scripted controls, not an interactive manual session.
- Linux/Windows gameplay validation and cross-platform feature builds.

Generated registration and external-source inclusion are separate build-workflow
follow-ups. The earlier temporary fourth-feature build proved ordinary in-tree
source/settings discovery, not external `sources` inclusion or removal of the
eight-slot limit.
