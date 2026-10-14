"""Compile/run procedural board geometry tests without game data."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(cc, sanitize=False):
    with tempfile.TemporaryDirectory(prefix="platform-shape-") as directory:
        executable = Path(directory) / "test"
        command = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
                   "-Wshadow", "-Wstrict-prototypes", "-Wmissing-prototypes", "-Werror",
                   str(ROOT / "source/features/platform/platform_shape.c"),
                   str(ROOT / "tools/test_platform_shape.c"), "-lm", "-o", str(executable)]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=60)


def test_platform_shape():
    run(os.environ.get("CC", "clang"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.cc, args.sanitize)
