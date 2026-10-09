"""Exercise the production texture geometry/cache with fake GL and guest memory.

No game assets or window required. The generated test unit uses the same LP64
rewrite as macOS; only decode/upload and external services are stubbed.
"""
import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

from lp64_rewrite import rewrite

ROOT = Path(__file__).resolve().parent.parent


def check_trace(cc: str, directory: Path) -> None:
    """Verify the port's new counters and upload timing reach its JSON report."""
    probe = directory / "trace_probe.c"
    probe.write_text('''
#include "halo_trace.h"
#include <time.h>
void platform_log(const char *format, ...) { (void)format; }
int main(void) {
    struct timespec pause = {0, 1000000};
    halo_trace_frame_begin(300);
    for (int i = 0; i < 2; i++) {
        halo_trace_zone_begin(HALO_TRACE_ZONE_TEXTURE_UPLOAD);
        nanosleep(&pause, 0);
        halo_trace_zone_end(HALO_TRACE_ZONE_TEXTURE_UPLOAD);
        halo_trace_counter_add(HALO_TRACE_COUNTER_TEXTURE_UPLOADS, 1);
        halo_trace_counter_add(HALO_TRACE_COUNTER_TEXTURE_UPLOAD_BYTES, 1024);
    }
    halo_trace_counter_set(HALO_TRACE_COUNTER_TEXTURE_BUDGET_BYTES, 1048576);
    halo_trace_counter_add(HALO_TRACE_COUNTER_TEXTURE_OVER_BUDGET, 1);
    halo_trace_frame_end(300);
    return 0;
}
''')
    executable = directory / "trace_probe"
    sources = ["port/linux/src/posix_trace.c", "engine/core/trace/trace.c",
               "engine/core/trace/trace_capture.c"]
    subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                    "-Werror", "-O1", "-I", str(directory), str(probe),
                    *[str(ROOT / source) for source in sources], "-o", str(executable)],
                   check=True)
    report = directory / "trace.json"
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, "HALO_TRACE_FILE": str(report)})
    summary = json.loads(report.read_text())
    values = {counter["name"]: counter["value"] for counter in summary["counters"]}
    assert values["texture_uploads"] == 2
    assert values["texture_upload_bytes"] == 2048
    assert values["texture_budget_bytes"] == 1048576
    assert values["texture_over_budget"] == 1
    assert values["texture_upload_ns"] >= values["texture_upload_max_ns"] > 0
    zone = next(zone for zone in summary["cpu_zones"] if zone["name"] == "texture_upload")
    assert zone["count"] == 2
    print("port texture telemetry JSON passed")


def run(cc: str, sanitize: bool) -> None:
    source = (ROOT / "port/linux/src/xbox_textures.c").read_text()
    header = (ROOT / "port/linux/src/xgpu.h").read_text()
    description = header[header.index("struct xgpu_texture_description\n{"):]
    description = description[:description.index("};") + 2]
    constants = (ROOT / "port/include/xdk/xdk_d3d8.h").read_text()
    constants = "\n".join(re.findall(
        r"^#define (?:D3DFORMAT_|D3DSIZE_|D3DTEXTURE_|D3DCOMMON_PORT_)\w+[^\n]*",
        constants, re.M))
    geometry = source[source.index("/* ---------- formats */"):
                      source.index("/* ---------- swizzling */")]
    cache = source[source.index("/* ---------- cache */"):]
    with tempfile.TemporaryDirectory(prefix="texture-cache-test-") as name:
        directory = Path(name)
        (directory / "production_cache.h").write_text(rewrite(
            constants + "\n" + geometry + "\n" + cache))
        (directory / "halo_trace.h").write_text(
            (ROOT / "port/linux/src/halo_trace.h").read_text())
        harness = (ROOT / "tools/tests/texture_cache_test.c.in").read_text()
        # The description must precede upload's declaration; cache comes later.
        (directory / "description.h").write_text(rewrite(description))
        (directory / "test.c").write_text(rewrite(harness))
        variants = [([], "plain")]
        if sanitize:
            variants.append((["-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
                             "asan-ubsan"))
        for flags, variant in variants:
            executable = directory / variant
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-O1", "-g", *flags,
                            str(directory / "test.c"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
            subprocess.run([str(executable), "--no-cache"], check=True)
            print(f"texture cache tests passed ({variant})")
        check_trace(cc, directory)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    run(args.cc, args.sanitize)
