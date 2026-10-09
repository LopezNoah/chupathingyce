"""Compile/run the host-only Infection rules harness; no game data required."""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cc, sanitize=False):
    with tempfile.TemporaryDirectory(prefix="infection-rules-test-") as directory:
        executable = Path(directory) / "infection_rules_test"
        command = [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
                   "-Wsign-conversion", "-Wshadow", "-Wstrict-prototypes",
                   "-Wmissing-prototypes", "-Wformat=2", "-Wundef", "-Werror",
                   "-I", str(ROOT / "source"),
                   str(ROOT / "source/features/infection/infection_rules.c"),
                   str(ROOT / "tools/test_infection_rules.c"), "-o", str(executable)]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=60)
        prototype = Path(directory) / "infection-prototype"
        prototype_command = [
            str(ROOT / "tools/infection_prototype.c")
            if arg == str(ROOT / "tools/test_infection_rules.c") else
            str(prototype) if arg == str(executable) else arg
            for arg in command
        ]
        subprocess.run(prototype_command, check=True, timeout=60)
        result = subprocess.run(
            [str(prototype), "seconds=10", "countdown=0", "results=0", "rounds=1"],
            input="tick 5\ndeath 1 0\ndeath 1 0\ntick 1\ntick 1\nquit\n",
            capture_output=True, text=True, check=True, timeout=60,
        )
        assert "Death queued" in result.stdout
        assert "Rejected." in result.stdout  # duplicate queued death
        assert "Round winner: Infected" in result.stdout
        assert "Beta Infected" in result.stdout
        assert "Match results" in result.stdout
        invalid = subprocess.run([str(prototype), "rounds=0"], capture_output=True, timeout=60)
        assert invalid.returncode == 1
        print("Infection prototype: scripted local match and invalid configuration passed")


def test_infection_rules():
    run(os.environ.get("CC", "clang"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not shutil.which(args.cc):
        parser.error(f"C compiler not found: {args.cc}")
    run(args.cc, args.sanitize)
