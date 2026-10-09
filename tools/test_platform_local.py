"""Live offline Blood Gulch platform smoke test; requires owned CE Xbox-format maps.

The existing Infection test launcher starts an ordinary local Slayer game;
Infection rules stay disabled. Build with --platform --infection first.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--build", default="macos" if sys.platform == "darwin" else "linux64")
    parser.add_argument("--disabled", action="store_true", help="verify no platform is created")
    parser.add_argument("--asset", type=Path, help="also verify loading a GLB visual")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    maps = args.maps.expanduser().resolve()
    if not (maps / "bloodgulch.map").is_file() or not (maps / "ui.map").is_file():
        parser.error("maps must contain bloodgulch.map and ui.map")
    game = repo / "build" / args.build / "halo"
    if not game.is_file():
        parser.error("build with --platform --infection first")
    root = Path(tempfile.mkdtemp(prefix="platform-local-"))
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
        "HALO_INFECTION_LOCAL": "false", "HALO_INFECTION_TEST_MAP": "bloodgulch",
        "HALO_INFECTION_TEST_SCENARIO": "slayer-control", "HALO_INFECTION_TEST_PLAYERS": "2",
        "HALO_PLATFORM_ENABLED": "false" if args.disabled else "true",
        "HALO_PLATFORM_TEST": "true", "HALO_PLATFORM_WIDTH": "4.0",
        "HALO_PLATFORM_DEPTH": "4.0", "HALO_PLATFORM_ELEVATION": "0.6",
        "HALO_PLATFORM_THICKNESS": "0.15", "HALO_PLATFORM_FORWARD_OFFSET": "3.0",
        "HALO_EXIT_AFTER": "25",
    })
    if args.asset:
        env["HALO_PLATFORM_ASSET"] = str(args.asset.expanduser().resolve())
    log = root / "run.log"
    print(f"Live platform log: {log}", flush=True)
    with log.open("w") as output:
        subprocess.run([str(game)], cwd=root, env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True, timeout=70)
    text = log.read_text(errors="replace")
    assert "as an ordinary Slayer control" in text, "offline game was not launched"
    if args.disabled:
        assert "platform: board at" not in text and "platform test: standing" not in text
    else:
        assert text.count("platform: board at") == 2, "platform was not recreated after map reset"
        assert "platform test: resetting map" in text, "map reset was not requested"
        assert text.count("platform test: standing PASS") == 2, "biped did not stand before and after reset"
        assert "platform test: standing FAIL" not in text
        if args.asset:
            assert "platform: loaded " in text, "GLB visual was not uploaded"
            assert "platform: shader failed" not in text
            assert "platform: GLB upload failed" not in text
    print("PASS: disabled control" if args.disabled else "PASS: live standing biped and map reset")


if __name__ == "__main__":
    main()
