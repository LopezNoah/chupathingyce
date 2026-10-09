"""Compile/run the independent BSP polygon navigation core (no game/map required)."""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cc, sanitize=False):
    with tempfile.TemporaryDirectory(prefix="surface-navigation-test-") as directory:
        executable = Path(directory) / "test_surface_navigation"
        command = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
                   "-Wsign-conversion", "-Wshadow", "-Wstrict-prototypes",
                   "-Wmissing-prototypes", "-Wformat=2", "-Wundef", "-Wwrite-strings",
                   "-Werror", str(ROOT / "source/engine_ai/surface_navigation.c"),
                   str(ROOT / "tools/test_surface_navigation.c"), "-lm", "-o", str(executable)]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=60)


def test_surface_navigation():
    run(os.environ.get("CC", "clang"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not shutil.which(args.cc):
        parser.error(f"C compiler not found: {args.cc}")
    run(args.cc, args.sanitize)
