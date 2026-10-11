#!/usr/bin/env python3
"""Check a bot-enabled Blood Gulch run patrols distinct BSP lanes."""

import argparse
import math
import re
from pathlib import Path

LANE_RE = re.compile(
    r"bots: bot (\d+) assigned team (\d+) lane (\d+) squad (\d+) side (-?\d+) phase (\d+)"
)
LANE_GOAL_RE = re.compile(
    r"bots: bot (\d+) lane target team (\d+) lane (\d+) phase (\d+).*?"
    r"goal \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)"
)
ROUTE_RE = re.compile(
    r"bots: bot (\d+) team (\d+) lane (\d+) squad (\d+) surface (patrol|tactical) "
    r"route goal \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\) waypoints (\d+)"
)
STATUS_RE = re.compile(
    r"bots: bot (\d+) at \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\) "
    r"yaw -?[\d.]+ (\w+) \(conf"
)
STATIONARY_DISTANCE = 0.5
STATIONARY_SAMPLES = 3


def parse_number(value: str, kind: type) -> int | float:
    try:
        return kind(value)
    except ValueError as error:
        raise SystemExit(f"invalid number in bot navigation log: {value!r}") from error


def check_movement(text: str) -> None:
    stationary: dict[int, tuple[tuple[float, float, float], int]] = {}
    for line in text.splitlines():
        match = STATUS_RE.search(line)
        if not match:
            continue
        bot = int(match.group(1))
        position: tuple[float, float, float] = (
            float(match.group(2)), float(match.group(3)), float(match.group(4))
        )
        behavior = match.group(5)
        if behavior == "vehicle":
            stationary.pop(bot, None)
            continue
        previous = stationary.get(bot)
        if previous and math.dist(previous[0], position) < STATIONARY_DISTANCE:
            stationary[bot] = (previous[0], previous[1] + 1)
            if stationary[bot][1] >= STATIONARY_SAMPLES:
                raise SystemExit(
                    f"bot {bot} stayed within {STATIONARY_DISTANCE:.1f} m for "
                    f"{STATIONARY_SAMPLES} consecutive status samples"
                )
        else:
            stationary[bot] = (position, 1)


def check_log(path: Path, minimum_bots: int) -> None:
    text = path.read_text(errors="replace")
    check_movement(text)
    assignments = [
        tuple(parse_number(value, int) for value in match)
        for match in LANE_RE.findall(text)
    ]
    lane_goals = [
        tuple(parse_number(value, int if index < 4 else float)
              for index, value in enumerate(match))
        for match in LANE_GOAL_RE.findall(text)
    ]
    routes = [
        tuple(
            parse_number(value, int if index in (0, 1, 2, 3, 8) else float)
            if index != 4 else value
            for index, value in enumerate(match)
        )
        for match in ROUTE_RE.findall(text)
    ]

    if len(assignments) < minimum_bots:
        raise SystemExit(
            f"expected at least {minimum_bots} lane assignments, found {len(assignments)}"
        )
    teams = {team for _, team, *_ in assignments}
    if len(teams) < 2:
        raise SystemExit(f"expected both teams to receive lanes, found teams {sorted(teams)}")
    for team in teams:
        lanes = {lane for _, assigned_team, lane, *_ in assignments if assigned_team == team}
        if lanes != {0, 1, 2}:
            raise SystemExit(f"team {team} uses lanes {sorted(lanes)}, expected 0, 1, 2")

        team_goals = [goal for goal in lane_goals if goal[1] == team]
        goal_lanes = {goal[2] for goal in team_goals}
        if goal_lanes != {0, 1, 2}:
            raise SystemExit(
                f"team {team} has patrol goals in lanes {sorted(goal_lanes)}, expected 0, 1, 2"
            )
        points = [(goal[4], goal[5], goal[6]) for goal in team_goals]
        spread = max(
            math.dist(a, b)
            for index, a in enumerate(points)
            for b in points[index + 1 :]
        ) if len(points) > 1 else 0.0
        if spread < 10.0:
            raise SystemExit(f"team {team} patrol goals are not spread out ({spread:.1f} m)")

        patrol_routes = [route for route in routes if route[1] == team and route[4] == "patrol"]
        route_lanes = {route[2] for route in patrol_routes}
        if len(patrol_routes) < 3 or len(route_lanes) < 2:
            raise SystemExit(
                f"team {team} has only {len(patrol_routes)} patrol routes in lanes "
                f"{sorted(route_lanes)}"
            )
        if any(route[8] < 2 for route in patrol_routes):
            raise SystemExit(f"team {team} has a patrol route with fewer than two waypoints")

    patrol_count = sum(route[4] == "patrol" for route in routes)
    print(
        f"passed: {len(assignments)} lane assignments, {len(lane_goals)} lane goals, "
        f"{patrol_count} patrol routes, teams {sorted(teams)}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--minimum-bots", type=int, default=24)
    parser.add_argument("--movement-only", action="store_true",
                        help="check sampled movement without requiring BSP lane coverage")
    args = parser.parse_args()
    if args.movement_only:
        text = args.log.read_text(errors="replace")
        samples: dict[int, int] = {}
        for match in STATUS_RE.finditer(text):
            bot = int(match.group(1))
            samples[bot] = samples.get(bot, 0) + 1
        if sum(count >= STATIONARY_SAMPLES for count in samples.values()) < args.minimum_bots:
            raise SystemExit("insufficient bot status samples for movement coverage")
        check_movement(text)
        print(f"passed: sampled movement for {len(samples)} bots (vehicle seats exempt)")
    else:
        check_log(args.log, args.minimum_bots)


if __name__ == "__main__":
    main()
