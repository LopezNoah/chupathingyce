"""Run a hidden, loopback-only Blood Gulch Forge-flight clock regression.

Uses legally supplied maps, isolated saves/overlays and no desktop input injection.
Requires a native executable built with Forge. --bots 3 adds the full bot scenario.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--build", default="macos" if sys.platform == "darwin" else "linux64")
    parser.add_argument("--bots", type=int, choices=(0, 3), default=0)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    maps = args.maps.expanduser().resolve()
    if not all((maps / name).is_file() for name in ("bloodgulch.map", "ui.map")):
        parser.error("--maps must contain bloodgulch.map and ui.map")
    root = Path(tempfile.mkdtemp(prefix="forge-clock-"))
    (root / "data").mkdir()
    (root / "saves").mkdir()
    (root / "data/maps").symlink_to(maps, target_is_directory=True)
    env = {key: value for key, value in os.environ.items() if not key.startswith("HALO_")}
    env.update({
        "HALO_DATA_ROOT": str(root / "data"), "HALO_SAVE_ROOT": str(root / "saves"),
        "HALO_NET_ADDRESS": "127.0.0.1", "HALO_NET_BROADCAST": "127.0.0.1",
        "HALO_NET_ONLINE": "false", "HALO_PUBLIC_LOBBY": "false", "HALO_HOST_PUBLIC": "false",
        "HALO_NET_REPORT_GAMES": "false", "HALO_NET_REPORT_EVENTS": "false",
        "HALO_NET_ALLOW_UPNP": "false", "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
        "HALO_NET_BROWSER": "", "HALO_UPDATE_AUTO": "false", "HALO_DISCORD_APPLICATION": "",
        "HALO_NETWORK_TEST": "host:bloodgulch:slayer", "HALO_NETWORK_TEST_START": "3",
        "HALO_NETWORK_TEST_SCORE": "100", "HALO_SOLO_GAME": "true",
        "HALO_BOTS": str(args.bots), "HALO_BOT_SKILL": "spartan",
        "HALO_FORGE_TEST": "flight-clock", "HALO_TEST_INPUT": "",
        "HALO_HIDDEN_WINDOW": "1", "HALO_FULLSCREEN": "false", "HALO_GL_DEBUG": "1",
        "HALO_EXIT_AFTER": "30", "HALO_TEXTURE_CACHE_MB": "512",
    })
    log = root / "run.log"
    print(f"Forge clock evidence: {root}", flush=True)
    with log.open("w") as output:
        process = subprocess.run([str(repo / "build" / args.build / "halo")], cwd=root,
                                 env=env, stdout=output, stderr=subprocess.STDOUT, timeout=50)
    text = log.read_text(errors="replace")
    debug = root / "data/debug.txt"
    text += "\n" + (debug.read_text(errors="replace") if debug.exists() else "")
    lines = [line for line in text.splitlines() if "forge clock test:" in line or
             "dangerously long" in line or "assertion failed" in line]
    # debug.txt also includes platform output; avoid printing duplicate samples.
    print("\n".join(dict.fromkeys(lines)))
    assert process.returncode == 0, f"game exited {process.returncode}: {log}"
    assert "forge clock test: FAIL" not in text, f"clock stopped during Forge: {log}"
    assert "forge clock test: PASS flight-clock" in text, f"fixture did not complete: {log}"
    assert "forge clock test: PASS explicit pause preserved" in text, f"Forge overwrote pause state: {log}"
    assert "dangerously long amount of time" not in text, f"loopback connection went silent: {debug}"
    assert "network_connection_idle_client_reliable_endpoint failed" not in text, debug
    if args.bots:
        for index in range(1, 4):
            assert f"bots: Bot {index} joined" in text, f"Bot {index} did not join"
    progress = re.search(r"PASS flight advanced (\d+) ticks", text)
    assert progress and int(progress[1]) >= 180
    print(f"PASS: clock advances in flight and after return, no network-silence warning ({args.bots} bots)")


if __name__ == "__main__":
    main()
