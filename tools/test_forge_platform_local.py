"""Live Forge GLB selection/edit and camera-to-biped handoff regression.

Uses isolated data/save roots, owned maps, and a fresh process for overlay reload.
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
    parser.add_argument("--visual", action="store_true", help="hold Forge open and save screenshots")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    maps = args.maps.expanduser().resolve()
    assert (maps / "bloodgulch.map").is_file() and (maps / "ui.map").is_file()
    root = Path(tempfile.mkdtemp(prefix="forge-glb-local-"))
    (root / "data").mkdir()
    (root / "saves").mkdir()
    (root / "data/maps").symlink_to(maps, target_is_directory=True)
    env = dict(os.environ)
    env.update({
        "HALO_DATA_ROOT": str(root / "data"), "HALO_SAVE_ROOT": str(root / "saves"),
        "HALO_NET_ONLINE": "false", "HALO_NET_REPORT_GAMES": "false",
        "HALO_NET_REPORT_EVENTS": "false", "HALO_NET_ALLOW_UPNP": "false",
        "HALO_NET_JOIN_FROM_CLIPBOARD": "false", "HALO_NET_BROWSER": "",
        "HALO_UPDATE_AUTO": "false", "HALO_BOTS": "0", "HALO_NETWORK_TEST": "",
        "HALO_HIDDEN_WINDOW": "1", "HALO_INFECTION_LOCAL": "false",
        "HALO_INFECTION_TEST_MAP": "bloodgulch", "HALO_INFECTION_TEST_SCENARIO": "local-play",
        "HALO_INFECTION_TEST_PLAYERS": "1", "HALO_PLATFORM_ENABLED": "true",
        "HALO_PLATFORM_ASSET": str(repo / "blender/forge_grid.glb"), "HALO_PLATFORM_TEST": "false",
        "HALO_EXIT_AFTER": "18",
    })
    if args.visual:
        screenshots = root / "screenshots"
        screenshots.mkdir()
        env["HALO_SCREENSHOT_DIR"] = str(screenshots)
        env["HALO_SCREENSHOT_EVERY"] = "300"
        print(f"Screenshots: {screenshots}", flush=True)
    for scenario in ("world-view" if args.visual else "world", "world-verify"):
        if args.visual and scenario == "world-verify":
            env["HALO_SCREENSHOT_EVERY"] = "0"
        env["HALO_FORGE_TEST"] = scenario
        log = root / f"{scenario}.log"
        print(f"Live Forge log: {log}", flush=True)
        with log.open("w") as output:
            subprocess.run([str(repo / "build" / args.build / "halo")], cwd=root, env=env,
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=65)
        text = log.read_text(errors="replace")
        assert "forge world test: FAIL" not in text, text
        assert f"forge world test: {scenario} PASS" in text, "Forge regression did not complete"
        assert "platform: loaded " in text
    print("PASS: GLB Forge view and overlay reload" if args.visual else
          "PASS: GLB selection/edit/undo/save/reload and safe avatar return")


if __name__ == "__main__":
    main()
