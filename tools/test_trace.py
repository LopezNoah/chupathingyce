"""Build and run the imported trace tests and check the shipping-build rule.

The C test writes a Chrome trace export and a JSON summary, which this driver
parses as JSON. A local probe verifies trace macros compile out when disabled.
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path

from allocation_guard_build import ALLOCATION_GUARD_FLAGS  # pyright: ignore[reportMissingImports]

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    "engine/core/trace/trace.c",
    "engine/core/trace/trace_capture.c",
    "tools/allocation_guard.c",
    "tools/test_engine_trace.c",
]
WRAP = ALLOCATION_GUARD_FLAGS
VARIANTS = {
    "plain": ["-O2"],
    "thread": ["-O1", "-g", "-fsanitize=thread"],
    "address": ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer"],
}
# ChupathingyCE probe whose zones must vanish unless tracing is enabled.
INSTRUMENTED = ["tools/trace_instrumentation_probe.c"]


def tsan_prefix() -> list[str]:
    """ThreadSanitizer cannot map its shadow memory under high ASLR entropy
    (as on Ubuntu 24.04 hosts), so run it without address randomization."""
    setarch = shutil.which("setarch")
    if setarch and subprocess.run([setarch, platform.machine(), "-R", "true"],
                                  capture_output=True).returncode == 0:
        return [setarch, platform.machine(), "-R"]
    return []


def check_exports(directory: Path) -> None:
    chrome = json.loads((directory / "chrome.json").read_text(encoding="utf-8"))
    events = chrome["traceEvents"]
    names = {event.get("name") for event in events}
    assert {"frame", "tick", "resimulated_ticks", "shadow_pass"} <= names, names
    assert any(event.get("ph") == "X" and event.get("name") == "shadow_pass" for event in events)
    assert any(event.get("ph") == "M" and event["args"].get("name") == "GPU" for event in events)
    summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
    assert summary["ticks"] == 1 and summary["frames"] == 1, summary
    assert summary["frame_samples"] == 1, summary
    assert summary["frame_p95_ns"] == summary["frame_p99_ns"], summary
    assert {counter["name"]: counter["value"] for counter in summary["counters"]} == {
        "resimulated_ticks": -3
    }, summary
    assert summary["cpu_zones"][0]["name"] == "frame", summary
    assert summary["gpu_zones"][0]["name"] == "shadow_pass", summary
    physics = next(tag for tag in summary["memory"] if tag["tag"] == "physics")
    assert physics["violations"] == 1 and physics["budget"] == 1000, physics


def check_tool(cc: str, directory: Path) -> None:
    """The CLI must reproduce the in-process summary from the saved capture."""
    tool = directory / "engine_trace_tool"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
                    str(ROOT / "engine/core/trace/trace_capture.c"),
                    str(ROOT / "engine/core/trace/trace.c"),
                    str(ROOT / "tools/engine_trace_tool.c"), "-o", str(tool)],
                   cwd=ROOT, check=True, timeout=120)
    capture = directory / "capture.etrace"
    output = subprocess.run([str(tool), "summary", str(capture)], check=True,
                            capture_output=True, text=True, timeout=60).stdout
    in_process = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
    from_file = json.loads(output)
    assert from_file["cpu_zones"] == in_process["cpu_zones"], (from_file, in_process)
    assert from_file["frame_p95_ns"] == in_process["frame_p95_ns"]
    assert from_file["frame_p99_ns"] == in_process["frame_p99_ns"]
    assert from_file["counters"] == in_process["counters"]
    assert from_file["memory"] == in_process["memory"]
    subprocess.run([str(tool), "chrome", str(capture), str(directory / "tool.json")],
                   check=True, timeout=60)
    json.loads((directory / "tool.json").read_text(encoding="utf-8"))
    broken = directory / "broken.etrace"
    broken.write_bytes(capture.read_bytes()[:-3])
    result = subprocess.run([str(tool), "summary", str(broken)], capture_output=True, timeout=60)
    assert result.returncode == 1 and b"malformed" in result.stderr, result
    print("engine_trace_tool round trip passed")


def check_shipping_symbols(cc: str, directory: Path) -> None:
    for enabled in (False, True):
        referenced = False
        for source in INSTRUMENTED:
            obj = directory / (Path(source).stem + (".traced.o" if enabled else ".o"))
            command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-c",
                       str(ROOT / source), "-o", str(obj)]
            if enabled:
                command.insert(1, "-DENGINE_TRACE_ENABLED")
            subprocess.run(command, cwd=ROOT, check=True, timeout=60)
            symbols = subprocess.run(["nm", "-u", str(obj)], check=True, capture_output=True,
                                     text=True).stdout
            if not enabled and "engine_trace_" in symbols:
                raise SystemExit(f"{source} references the trace API without ENGINE_TRACE_ENABLED")
            referenced = referenced or "engine_trace_" in symbols
        # Positive control: the same check must see zones when they are enabled.
        if enabled and not referenced:
            raise SystemExit("traced build has no trace references; the check is broken")
    print("shipping builds contain no trace zone references")


def run(cc: str, sanitize: bool) -> None:
    variants = ["plain"] + (["thread", "address"] if sanitize else [])
    with tempfile.TemporaryDirectory(prefix="open-ce-engine-trace-test-") as name:
        directory = Path(name)
        for variant in variants:
            executable = directory / f"test_engine_trace_{variant}"
            command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
                       *VARIANTS[variant], *[str(ROOT / source) for source in SOURCES],
                       *WRAP, "-o", str(executable)]
            subprocess.run(command, cwd=ROOT, check=True, timeout=120)
            prefix = tsan_prefix() if variant == "thread" else []
            subprocess.run([*prefix, str(executable), str(directory)], cwd=ROOT, check=True,
                           timeout=300)
            check_exports(directory)
            print(f"engine trace tests passed ({variant})")
        check_tool(cc, directory)
        check_shipping_symbols(cc, directory)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true",
                        help="also run ThreadSanitizer and ASan/UBSan builds")
    arguments = parser.parse_args()
    if not shutil.which(arguments.cc):
        parser.error(f"C compiler not found: {arguments.cc}")
    run(arguments.cc, arguments.sanitize)
