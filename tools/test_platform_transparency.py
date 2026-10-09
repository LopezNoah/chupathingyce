"""Real GL screenshots: transparent grid visible above, below, and grazing.

Requires owned maps. Keeps BMPs/logs outside the repo; uses no imaging dependency.
The underside check catches the old opaque collision shell masking the GLB.
Also checks transparent PNG selector rendering in unselected/selected states.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def cyan_pixels(path):
    image = path.read_bytes()
    assert image[:2] == b"BM"
    offset = struct.unpack_from("<I", image, 10)[0]
    width, signed_height = struct.unpack_from("<ii", image, 18)
    bits = struct.unpack_from("<H", image, 28)[0]
    compression = struct.unpack_from("<I", image, 30)[0]
    assert bits in (24, 32) and compression == 0
    height = abs(signed_height)
    assert width > 0 and height > 0
    pixel_bytes = bits // 8
    stride = ((width * pixel_bytes + 3) // 4) * 4
    count = 0
    for y in range(height * 35 // 100, height * 65 // 100):
        row = y if signed_height < 0 else height - 1 - y
        for x in range(width // 4, width * 3 // 4):
            at = offset + row * stride + x * pixel_bytes
            blue, green, red = image[at:at + 3]
            count += red < 90 and green > 110 and blue > 150
    # Normalize the threshold for hosts using a smaller render target.
    return count * 2560 * 1920 // (width * height)


def selector_ring_pixels(path):
    image = path.read_bytes()
    offset = struct.unpack_from("<I", image, 10)[0]
    width, signed_height = struct.unpack_from("<ii", image, 18)
    pixel_bytes = struct.unpack_from("<H", image, 28)[0] // 8
    height = abs(signed_height)
    stride = ((width * pixel_bytes + 3) // 4) * 4
    inner, outer = height * 0.020, height * 0.028
    count = 0
    for y in range(int(height / 2 - outer), int(height / 2 + outer) + 1):
        row = y if signed_height < 0 else height - 1 - y
        for x in range(int(width / 2 - outer), int(width / 2 + outer) + 1):
            radius_squared = (x - width / 2) ** 2 + (y - height / 2) ** 2
            if not inner ** 2 <= radius_squared <= outer ** 2:
                continue
            at = offset + row * stride + x * pixel_bytes
            color = image[at:at + 3]
            count += min(color) > 150 and max(color) - min(color) < 35
    return count * 2560 * 1920 // (width * height)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--maps", type=Path, required=True)
    parser.add_argument("--build", default="macos" if sys.platform == "darwin" else "linux64")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    maps = args.maps.expanduser().resolve()
    assert (maps / "bloodgulch.map").is_file() and (maps / "ui.map").is_file()
    root = Path(tempfile.mkdtemp(prefix="grid-alpha-"))
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
        "HALO_FORGE_SELECTOR_ASSET": str(repo / "blender/forge_gui.png"),
        "HALO_FORGE_SELECTED_SELECTOR_ASSET": str(repo / "blender/forge_selected_gui.png"),
        "HALO_FORGE_SELECTOR_SIZE": "32",
        "HALO_EXIT_AFTER": "14", "HALO_SCREENSHOT_EVERY": "1200",
    })
    for view in ("above", "below", "grazing", "selected"):
        screenshots = root / view
        screenshots.mkdir()
        env["HALO_SCREENSHOT_DIR"] = str(screenshots)
        env["HALO_FORGE_TEST"] = f"world-alpha-{view}"
        log = root / f"{view}.log"
        with log.open("w") as output:
            subprocess.run([str(repo / "build" / args.build / "halo")], cwd=root, env=env,
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=50)
        text = log.read_text(errors="replace")
        assert f"forge alpha test: {view} ready" in text
        assert ("selected bar" if view == "selected" else "unselected cross") in text
        assert "shader failed" not in text and "cannot decode PNG" not in text
        frame = sorted(screenshots.glob("*.bmp"))[-1]
        cyan = cyan_pixels(frame)
        assert cyan > 1000, f"{view}: grid hidden or opaque shell visible ({cyan} cyan pixels): {frame}"
        ring = selector_ring_pixels(frame)
        assert ring > 80, f"{view}: PNG ring missing, misplaced or wrongly sized: {frame}"
        for older in screenshots.glob("*.bmp"):
            if older != frame:
                older.unlink()
        print(f"PASS {view}: grid {cyan}, PNG ring {ring} normalized pixels; {frame}", flush=True)
    print(f"Transparency screenshots/logs: {root}")


if __name__ == "__main__":
    main()
