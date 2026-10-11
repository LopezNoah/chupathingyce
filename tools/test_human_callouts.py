"""Run the real local-human callout module against fake cameras and damage events."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cc, sanitize=False):
    module = (ROOT / "source/features/bots/human_callouts.c").read_text()
    module = re.sub(r'^#include .*$', '', module, flags=re.MULTILINE)
    fixture = (ROOT / "tools/test_human_callouts.c.in").read_text()
    fixture = fixture.replace("/* PRODUCTION_MODULE */", module)
    with tempfile.TemporaryDirectory(prefix="human-callouts-") as directory:
        source = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        source.write_text(fixture)
        command = [cc, "-std=gnu89", "-Wall", "-Wextra", "-Werror", str(source), "-lm", "-o", str(executable)]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=30)


def test_human_callouts():
    run(os.environ.get("CC", "clang"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.cc, args.sanitize)
