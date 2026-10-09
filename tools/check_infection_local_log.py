"""Validate actual local Infection game logs, not rules-harness output."""
import argparse
import re
from pathlib import Path

SPAWN = re.compile(
    r"infection local: spawn slot=(\d+) datum=([0-9a-f]+) unit=([0-9a-f]+) "
    r"life=(\d+) role=(Survivor|Alpha Infected|Beta Infected) team=(\d+) "
    r"weapons=(\d+) grenades=(\d+)/(\d+) ammo=(\d+)/(\d+)"
)
# CE scoreboard colours: team 0 red, team 1 blue. Survivors are blue.
INFECTED_TEAM, SURVIVOR_TEAM = 0, 1
PHASE = re.compile(r"infection local: tick=(\d+) round=(\d+) phase=(.*?) remaining=(\d+) survivors=(\d+) winner=(\d+)")


def check_slayer_control(text):
    """Infection disabled: ordinary Slayer must behave as before."""
    assert "as an ordinary Slayer control" in text, "control launcher did not run"
    assert "window closed" not in text, "run interrupted by an external window close/quit event; rerun"
    assert "infection local: tick" not in text and "infection local: spawn" not in text, "Infection adapter active in Slayer"
    spawn = re.search(r"slayer control: spawn weapons=(\d+) grenades=(\d+)/(\d+) infection_active=(\d)", text)
    respawn = re.search(r"slayer control: respawn weapons=(\d+) grenades=(\d+)/(\d+) "
                        r"killer_score=(-?\d+) victim_score=(-?\d+) infection_active=(\d)", text)
    assert spawn and respawn, "Slayer kill/respawn not observed"
    # Blood Gulch's own Slayer starting profile (1 weapon, no grenades in the
    # observed run) is the map's, not ours: require it unchanged across respawn.
    assert spawn.group(4) == respawn.group(6) == "0", "Infection active in Slayer"
    assert int(spawn.group(1)) >= 1, "Slayer spawned unarmed"
    assert spawn.group(1, 2, 3) == respawn.group(1, 2, 3), "Slayer respawn loadout differs from spawn"
    assert (int(respawn.group(4)), int(respawn.group(5))) == (1, 0), "Slayer kill scoring changed"
    return 2


def check(text: str, match: bool = False, lifecycle: bool = False, melee: str = "") -> int:
    """melee: "" (none), "wounded" (single strike) or "full" (full-health)."""
    assert "local players (no server)" in text, "local launcher did not run"
    assert "window closed" not in text, "run interrupted by an external window close/quit event; rerun"
    assert "loadout failed" not in text and "aborting" not in text, "loadout/integration fault"
    assert "ASSERTION FAILED" not in text and "FATAL" not in text, "game assertion/fatal error"
    spawns = list(SPAWN.finditer(text))
    assert len(spawns) >= 2, "two real Spartan spawns required"
    identities = {}
    lives = {}
    roles = set()
    for spawn in spawns:
        slot, datum, unit, life, role, team, weapons, frag, plasma, shotgun, pistol = spawn.groups()
        assert int(unit, 16) != 0xffffffff, "missing unit"
        assert int(life) > lives.get(slot, 0), "life ID reused across respawn"
        lives[slot] = int(life)
        assert identities.setdefault(slot, datum) == datum, "player datum changed across respawn"
        assert (int(team), int(weapons)) == ((SURVIVOR_TEAM, 2) if role == "Survivor" else (INFECTED_TEAM, 0)), "wrong faction/loadout"
        assert int(frag) == int(plasma) == 0, "unexpected grenades"
        if lifecycle or melee:
            assert (int(shotgun), int(pistol)) == ((18, 36) if role == "Survivor" else (0, 0)), "wrong initial ammo"
        roles.add(role)
    assert {"Survivor", "Alpha Infected"} <= roles
    phases = [entry.groups() for entry in PHASE.finditer(text)]
    assert any(phase == "Active" for _, _, phase, _, _, _ in phases), "active round not reached"
    if match:
        assert any(phase == "Match results" for _, _, phase, _, _, _ in phases), "match not completed"
        assert sum(phase == "Round results" and int(remaining) == 150
                   for _, _, phase, remaining, _, _ in phases) >= 3, "three completed rounds required"
    if lifecycle or melee:
        phase = ""
        for line in text.splitlines():
            phase_entry = PHASE.search(line)
            if phase_entry:
                phase = phase_entry.group(3)
            state = re.search(r"role=(Survivor|Alpha Infected|Beta Infected).*?alive=1.*?ammo=(\d+/\d+)", line)
            if state and (phase == "Countdown" or state.group(1) != "Survivor"):
                assert state.group(2) == ("18/36" if state.group(1) == "Survivor" else "0/0"), "unexpected ammo pickup during countdown/Infected life"
    for slot, role, distance in re.findall(
            r"infection local: spawn distance slot=(\d+) role=(.*?) nearest_enemy=(-?[\d.]+)", text):
        assert float(distance) < 0 or float(distance) >= 8.0, \
            f"slot {slot} ({role}) spawned {distance} units from a living enemy"
    vehicles = re.findall(r"infection local: round=\d+ active vehicles=(\d+)", text)
    assert vehicles and all(count == "0" for count in vehicles), "vehicles present in Infection"
    # Every conversion moves the player to the Infected team immediately: the
    # team change follows the death line, before that slot's next spawn.
    lines = text.splitlines()
    for index, line in enumerate(lines):
        death = re.search(r"infection local: death round=\d+ slot=(\d+) .* role=Beta Infected", line)
        if not death:
            continue
        slot = death.group(1)
        for later in lines[index + 1:]:
            if re.search(rf"infection local: team slot={slot} \S+ Survivors -> Infected role=Beta Infected alive=0", later):
                break
            assert not re.search(rf"infection local: (spawn|player) slot={slot} ", later), \
                f"slot {slot} converted but did not join the Infected team immediately"
        else:
            raise AssertionError(f"slot {slot} never joined the Infected team")
    if lifecycle:
        assert "Beta respawn verified" in text
        assert "Survivor suicide through real damage" in text
        assert "environmental death of last Survivor" in text
        assert "role=Beta Infected" in text
        results = [(int(round_id), int(winner)) for _, round_id, phase, remaining, _, winner in phases
                   if phase == "Round results" and int(remaining) == 150]
        assert results == [(1, 2), (2, 2), (3, 1)], "wrong seeded fixture round winners"
        scores = {}
        for slot, score in re.findall(r"infection local: player slot=(\d+).*?score=(\d+)", text):
            scores[int(slot)] = int(score)
        assert scores == {0: 3, 1: 8, 2: 10}, "wrong seeded fixture final personal scores"
    if melee == "full":
        assert "positioned full-health Survivor" in text and "facing_dot=-1.000" in text, "not a face-to-face full-health test"
        strikes = re.findall(r"unarmed melee input queued slot=0 strike=(\d+)", text)
        assert strikes, "no strikes"
        print(f"Full-health face-to-face unarmed strikes to kill: {strikes[-1]}")
    if melee:
        assert "Infected pistol pickup blocked" in text
        assert "filtered ranged input flags=0 trigger=0.0 weapon=-1 grenade=-1 zoom=-1" in text
        assert "unarmed melee input queued" in text, "normal melee input not exercised"
        assert re.search(r"death round=1 slot=1 .*killer=0 role=Survivor", text), "unarmed impact did not kill Survivor"
        assert any(phase == "Round results" and winner == "2" for _, _, phase, _, _, winner in phases)
    return len(spawns)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--match", action="store_true")
    parser.add_argument("--lifecycle", action="store_true", help="three-player, seed 42 lifecycle fixture")
    parser.add_argument("--melee", action="store_true", help="two-player, seed 42 normal unarmed melee fixture")
    parser.add_argument("--melee-full", action="store_true", help="two-player full-health face-to-face melee")
    parser.add_argument("--slayer-control", action="store_true", help="Infection disabled: ordinary Slayer regression")
    args = parser.parse_args()
    text = args.log.read_text(errors="replace")
    if args.slayer_control:
        count = check_slayer_control(text)
    else:
        melee = "full" if args.melee_full else "wounded" if args.melee else ""
        count = check(text, args.match or args.lifecycle, args.lifecycle, melee)
    print(f"Local Infection integration checks passed ({count} real unit spawns)")
