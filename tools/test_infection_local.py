"""Run real, isolated local Blood Gulch integration tests (requires owned maps)."""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="macos" if sys.platform == "darwin" else "linux64")
    parser.add_argument("--maps", required=True, type=Path, help="existing CE Xbox-format maps directory")
    parser.add_argument("--scenario", choices=("lifecycle", "melee", "melee-full", "slayer-control"), default="lifecycle")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    maps = args.maps.expanduser().resolve()
    if not (maps / "bloodgulch.map").is_file() or not (maps / "ui.map").is_file():
        parser.error("maps must contain bloodgulch.map and ui.map")
    if not args.no_build:
        subprocess.run(["ninja", "-j4", args.build], cwd=repo, check=True)
    game = repo / "build" / args.build / "halo"
    if not game.is_file():
        parser.error(f"game executable not found: {game}")
    root = Path(tempfile.mkdtemp(prefix="infection-local-"))
    (root / "data").mkdir()
    (root / "saves").mkdir()
    (root / "data" / "maps").symlink_to(maps, target_is_directory=True)
    env = dict(os.environ)
    env.update({
        "HALO_DATA_ROOT": str(root / "data"), "HALO_SAVE_ROOT": str(root / "saves"),
        "HALO_NET_ADDRESS": "127.0.0.1", "HALO_NET_BROADCAST": "127.0.0.1",
        "HALO_NET_ONLINE": "false", "HALO_NET_PUBLIC_LOBBY": "false",
        "HALO_NET_HOST_PUBLIC": "false", "HALO_NET_LIST_GAMES": "false",
        "HALO_NET_REPORT_GAMES": "false", "HALO_NET_REPORT_EVENTS": "false",
        "HALO_NET_ALLOW_UPNP": "false", "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
        "HALO_NET_BROWSER": "", "HALO_NET_PROTOCOL": "opence",
        "HALO_UPDATE_AUTO": "false", "HALO_CRASH_REPORTS": "no", "HALO_BOTS": "0",
        "HALO_NETWORK_TEST": "", "HALO_HIDDEN_WINDOW": "1",
        "HALO_INFECTION_LOCAL": "false" if args.scenario == "slayer-control" else "true", "HALO_INFECTION_TEST_MAP": "bloodgulch",
        "HALO_INFECTION_TEST_SCENARIO": args.scenario, "HALO_INFECTION_SEED": "42",
        "HALO_INFECTION_ALPHAS": "0", "HALO_INFECTION_RESPAWN": "1",
        "HALO_INFECTION_SHOTGUN_ROUNDS": "18", "HALO_INFECTION_PISTOL_ROUNDS": "36",
        "HALO_INFECTION_TEST_PLAYERS": "3" if args.scenario == "lifecycle" else "2",
        "HALO_INFECTION_SECONDS": {"lifecycle": "10", "melee-full": "15"}.get(args.scenario, "8"),
        "HALO_INFECTION_ROUNDS": "3" if args.scenario == "lifecycle" else "1",
        "HALO_EXIT_AFTER": {"lifecycle": "70", "melee": "25", "melee-full": "35", "slayer-control": "35"}[args.scenario],
    })
    log = root / "run.log"
    print(f"Actual local game log: {log}", flush=True)
    with log.open("w") as output:
        subprocess.run([str(game)], cwd=root, env=env, stdout=output,
                       stderr=subprocess.STDOUT, timeout=100, check=True)
    subprocess.run([sys.executable, str(repo / "tools" / "check_infection_local_log.py"),
                    str(log), f"--{args.scenario}"], cwd=repo, check=True)
    print(f"PASS: local {args.scenario}; log retained at {log}")


if __name__ == "__main__":
    main()
