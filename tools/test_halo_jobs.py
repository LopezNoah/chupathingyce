"""Build and run the ChupathingyCE job-graph integration tests.

Builds tools/test_halo_jobs.c (the downstream scheduler in engine/core/jobs,
the platform adapter port/linux/src/posix_jobs.c, and the game's own
interpolation kernel and blend jobs) plain, under ThreadSanitizer and under
ASan/UBSan, and runs each build for the sequential oracle and 0, 1, 2, 4 and
8 workers. Every configuration must print the oracle's hash. The allocation
guard fails any heap allocation while frames run.

  python3 tools/test_halo_jobs.py [--sanitize] [--bench]
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from allocation_guard_build import ALLOCATION_GUARD_FLAGS  # pyright: ignore[reportMissingImports]

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    "engine/core/jobs/job_graph.c",
    "engine/core/jobs/job_system.c",
    "engine/core/jobs/job_phase.c",
    "port/linux/src/posix_jobs.c",
    "tools/allocation_guard.c",
    "tools/test_halo_jobs.c",
]
VARIANTS = {
    "plain": ["-O2"],
    "thread": ["-O1", "-g", "-fsanitize=thread"],
    "address": ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer"],
}
CONFIGURATIONS = [("sequential", None), ("parallel", 0), ("parallel", 1), ("parallel", 2),
                  ("parallel", 4), ("parallel", 8)]


def tsan_prefix() -> list[str]:
    setarch = shutil.which("setarch")
    if setarch and subprocess.run([setarch, platform.machine(), "-R", "true"],
                                  capture_output=True).returncode == 0:
        return [setarch, platform.machine(), "-R"]
    return []


def build(cc: str, variant: str, directory: Path, guard: bool = True) -> Path:
    executable = directory / f"test_halo_jobs_{variant}{'' if guard else '_bench'}"
    command = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-pthread", "-DHALO_JOBS_ENABLED", *VARIANTS[variant],
               *[str(ROOT / source) for source in SOURCES],
               *(ALLOCATION_GUARD_FLAGS if guard else []), "-lm", "-o", str(executable)]
    subprocess.run(command, cwd=ROOT, check=True, timeout=300)
    return executable


def run(executable: Path, variant: str, mode: str, workers: int | None, *arguments: str) -> str:
    env = dict(os.environ, HALO_JOBS=mode)
    env.pop("HALO_JOB_TRACE", None)
    if workers is not None:
        env["HALO_JOB_WORKERS"] = str(workers)
    prefix = []
    if variant == "thread":
        env["TSAN_OPTIONS"] = "halt_on_error=1:second_deadlock_stack=1"
        prefix = tsan_prefix()
    return subprocess.run([*prefix, str(executable), *arguments], cwd=ROOT, check=True, timeout=900,
                          env=env, capture_output=True, text=True).stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--bench", action="store_true")
    arguments = parser.parse_args()
    variants = ["plain"] + (["thread", "address"] if arguments.sanitize else [])
    with tempfile.TemporaryDirectory(prefix="chupathingyce-jobs-test-") as name:
        directory = Path(name)
        for variant in variants:
            executable = build(arguments.cc, variant, directory)
            hashes = {}
            sanitized = variant != "plain"
            for mode, workers in CONFIGURATIONS:
                if sanitized and workers not in (None, 1, 4):
                    continue
                output = run(executable, variant, mode, workers, *(["quick"] if sanitized else []))
                found = re.search(r"^hash ([0-9a-f]+)$", output, re.MULTILINE)
                assert found, output
                hashes[(mode, workers)] = found.group(1)
                if variant == "plain" and (mode, workers) == CONFIGURATIONS[0]:
                    print(output.strip())
            assert len(set(hashes.values())) == 1, hashes
            print(f"halo job tests passed ({variant}): {len(hashes)} configurations, hash "
                  f"{next(iter(hashes.values()))}")
        if arguments.bench:
            executable = build(arguments.cc, "plain", directory, guard=False)
            for mode, workers in CONFIGURATIONS:
                for line in run(executable, "plain", mode, workers, "bench").splitlines():
                    if line.startswith("bench"):
                        print(f"{mode:10} {line}")


if __name__ == "__main__":
    main()
