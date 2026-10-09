# Blood Gulch bot smoke/stress test — macOS ARM64, 2026-10-09

## Setup

Native `build/macos/halo`, current texture-residency working tree plus the
zero-byte tracing-release fix described below. Apple M2 Pro, macOS 26.3.1,
OpenGL 4.1 Metal - 90.5. The renderer reported 2560x1662 after startup, despite
requesting a 1280x960 window. Both runs used the same settings and executable.

Automated local host: `HALO_NETWORK_TEST=host:bloodgulch:slayer`, start delay 5,
score limit 100, solo-game enabled, three Spartan bots. Online play/public lobby
and UPnP disabled; fresh isolated saves for each run. High-resolution replacements
were enabled. `HALO_GL_DEBUG=1`, screenshots every 600 frames, trace reports
archived as the overwritten JSON changed. No scripted player input in the fixed
runs (`HALO_TEST_INPUT=""`). The user interacted with the default-budget run,
including entering/exiting Forge; this is **not a controlled camera-route A/B**.

Raw local artifacts, screenshots, launch environment and all report snapshots:
`/tmp/chupathingy-texture-match-fixed/`. Failed pre-fix runs:
`/tmp/chupathingy-texture-match/`. No game map files are included here.

## A real bug found by this test

Both initial traced runs aborted at `engine/core/trace/trace.c` with
`assertion failed: bytes > 0`. The new cache release helper unconditionally called
the tracing allocator for entries with a GL name but zero estimated allocation
(e.g. replacement-only entries). The trace API requires positive byte counts.

Fixed by skipping the traced memory release when the estimate is zero, while
still deleting the GL name and invalidating renderer state. The production-cache
regression test now exercises eviction of a nonresident entry and enforces the
real trace API's positive-size contract. It failed before the fix and passed
with ASan/UBSan after it. The original fake trace service accepted zero releases,
which is why the earlier unit tests missed this integration defect.

The initial camera motion was deliberate `HALO_TEST_INPUT=look:7` set by the test
launcher, not a camera-input regression. It was removed for subsequent runs.

## Fixed-run observations

| Measurement | 512 MiB default | 4 MiB stress limit |
| --- | ---: | ---: |
| Process lifetime | 41.21 s, window closed, exit 0 | 120.42 s, timed exit, exit 0 |
| Last logged simulation tick | 510 | 3363 |
| Bots joining | 3 | 3 |
| Archived trace reports | 14 | 46 |
| Final report time since launch | 37.53 s | 119.19 s |
| Estimated texture residency, final report | 13.69 MiB | 3.88 MiB |
| Estimated texture residency, lifetime peak | 15.68 MiB | 10.85 MiB |
| Successful uploads, cumulative | 313 | 1,236,766 |
| Evictions, cumulative (includes nonresident/idle entries) | 48 | 1,427,335 |
| Unable-to-fit upload requests, cumulative | 0 | 563,054 |
| Estimated allocation bytes submitted by successful uploads | 23.53 MiB | 95.98 GiB |
| CPU decode/upload time, cumulative | 16.469 ms | 51.522 s |
| Maximum CPU upload-call duration | 0.837 ms | 14.129 ms |
| Latest retained Present p95 / p99 | 12.742 / 13.084 ms | 10.132 / 10.413 ms |
| RSS after 10 s, min / median / max | 449.1 / 471.1 / 480.5 MiB | 234.0 / 250.6 / 469.0 MiB |

Totals describe unequal lifetimes and different views; do not directly compare
those totals as rates. For the stress run, between the first archived report
at/after 30 s and the last report (an 86.94 s interval), the upload rate was
**10,770 successful uploads/s**, consuming **437.9 ms/s** in CPU decode/upload.
Current-frame resources exceeded the limit, then trimming caused repeat uploads
in subsequent frames. This is deliberate pathological pressure, not a sensible
normal-play limit.

Screenshots inspected from both runs showed textured terrain/buildings and HUD;
the stress run also rendered the held weapon. No exhaustive pixel comparison was
performed. All three bots moved/fought, with vehicle use and deaths in the logs.
The stress run continued to tick 3363 and exited normally. No further tracer
abort occurred after the fix. Present timings exclude uploads occurring earlier
in the renderer: the lower stress-run Present percentile is **not evidence of a
performance improvement**, especially with different scenes and substantial
upload CPU time.

### Physical-memory limitations

The stress run's `vmmap` at approximately 95 s reported 702.8 MiB physical
footprint (802.2 MiB peak). `footprint` around 105 s reported approximately
704 MiB, including 179 MiB dirty IOAccelerator graphics, 81 MiB IOSurface, and
41 MiB unmapped graphics footprint. These include more than this texture cache
and may include compressed memory/deferred driver allocations.

No matching default-budget physical-footprint sample was collected: that run
closed before the scheduled probes. `xctrace list templates` was killed by the
host environment; no Instruments capture was obtained. **Physical GPU-memory
reduction remains unestablished.** RSS and estimated residency are not substitutes
for the missing paired physical measurement.

## Forge/system-link warning (resolved in follow-up)

During the **512 MiB** fixed run, the user reported a system-link-disconnect alert
while flying in Forge. The full game log records:

- 16:22:04: entered Forge.
- 16:22:09: `network client connection has been silent for a dangerously long
  amount of time`.
- Returned from Forge; simulation logging resumed at tick 510.
- 16:22:13: local player removal requested; no local players remained; game exited
  to the menu; client disposed; `network_connection_idle_client_reliable_endpoint
  failed`.
- Window then closed normally.

This establishes a network-silence warning during Forge, not its cause. Later
endpoint failure may be teardown rather than the initiating failure. There were
no over-budget requests in this run. Do **not** mark this original interactive
run as an unqualified gameplay pass.

Follow-up: the isolated `tools/test_forge_clock.py` regression reproduced the
warning with no bots at the default cache budget: Forge advanced zero ticks in
12 seconds. Forge's automatic pause on entry and every-frame re-pause were
removed. The same real-engine test now advances 360 ticks in 12 seconds with
zero or three bots, with no network-silence warning and safe return to the
character. Explicit user pauses are preserved. See
[Forge validation notes](../../../FORGE_IMPLEMENTATION_NOTES.md#live-clocknetwork-regression-2026-10-09).
