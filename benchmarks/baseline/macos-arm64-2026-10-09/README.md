# macOS ARM64 baseline — 2026-10-09

## Menu idle (90 seconds)

- Build: native macOS ARM64, debug configuration; startup reports base revision `0e982660`. The executable included the tracing changes from this working tree.
- Machine: Apple M2 Pro, macOS 26.3.1; renderer: OpenGL 4.1 Metal.
- Run: built `build/macos/halo`, with `HALO_DATA_ROOT` pointing at the existing maps folder and `HALO_TRACE_FILE` pointing at the trace output. No input after launch; terminated at 90 seconds.
- Presentation resolution changed during startup from 1280×960 to 2560×1662. Treat comparisons as valid only when resolution, build configuration, and machine are held constant.

### Captured results

| Measurement | Baseline |
| --- | ---: |
| Frames in trace report | 292 |
| `D3DDevice_Present` p95 | 14.200 ms |
| `D3DDevice_Present` p99 | 15.251 ms |
| `D3DDevice_Present` total / max | 2,351.596 ms / 16.423 ms |
| Process RSS, 10–90 s min / median / max | 178 / 194 / 234 MiB |
| Tagged `game` memory, current / peak | 312.1 / 312.1 MiB |
| Tagged `render_cpu` current / peak | 2.8 KiB / 259 KiB |
| Estimated texture GPU bytes, current / peak | 2.57 / 2.65 MiB |
| Texture cache hits / misses | 1,207,369 / 30 |
| Texture uploads / evictions | 43 / 9 |
| Draw calls (latest sampled frame) | 100 |
| Allocations in latest game tick | 0 (menu idle; no active simulation) |
| Bot/navigation AI zone | 74 samples, 0.196 ms total, 0.004 ms max (no bots active) |
| Last map pre-cache / scenario load | 0.001 ms / 11.826 ms (UI map startup; pre-cache was already warm) |
| GPU timing | Unsupported by current trace path |

The raw trace report and 5-second RSS samples are in this directory. RSS was sampled externally with `ps`; it is resident set size, not macOS physical footprint or PSS. The trace's `texture_gpu_bytes` is an estimate and excludes driver overhead, render targets, and high-resolution replacements. Frame percentiles describe the instrumented `D3DDevice_Present` interval, including swap/pacing behavior, not isolated GPU time or the whole game frame. These menu-idle results are only a starting reference: draw calls are for the latest sampled frame, allocations are the latest simulation tick, and the map pre-cache measurement was warm. The bot zone is overhead with no bots active; use the bot-enabled scenario for meaningful AI timing.

`lost_events` is high because the trace ring is bounded and overwrites older events during the run. The report still contains the retained frame samples used for these summaries; keep this limitation in mind when adding more zones or comparing event-level captures.

## Blood Gulch Slayer with three bots

- Build/machine/renderer: same native macOS ARM64 debug build and Apple M2 Pro / OpenGL 4.1 Metal as the menu run above.
- Run: local offline FFA Slayer, hosted with `HALO_NETWORK_TEST=host:bloodgulch:slayer`, `HALO_NETWORK_TEST_START=5`, `HALO_SOLO_GAME=1`, `HALO_BOTS=3`, and `HALO_BOT_SKILL=spartan`. Public/online networking and UPnP were disabled. Data root was the existing local CE installation; saves were isolated under `/tmp/chupathingy-play/saves`.
- Evidence: the log records all three bots joining, a 70-node/438-link navigation graph, movement/combat decisions, bot deaths/kills, and vehicle use. The captured simulation ran to at least tick 990 (about 33 game seconds at 30 ticks/s) with the human and three bots present. The process exited before a full 90-second gameplay sample; treat this as a short controlled gameplay sample, not a long-run stability result.

| Measurement | Captured value |
| --- | ---: |
| Retained frame samples; `D3DDevice_Present` p95 / p99 | 292; 13.479 / 14.535 ms |
| `D3DDevice_Present` total / max | 2,098.319 / 14.783 ms |
| Process RSS, 10–40 s min / median / max | 424.3 / 440.8 / 444.5 MiB |
| Tagged `game` memory, current / peak | 312.1 / 312.1 MiB |
| Tagged `render_cpu` current / peak | 30.5 KiB / 1.02 MiB |
| Estimated texture GPU bytes, current / peak | 15.81 / 15.91 MiB |
| Texture cache hits / misses; uploads / evictions | 2,999,007 / 294; 292 / 64 |
| Draw calls (latest sampled frame) | 324 |
| Allocations in latest game tick | 0 |
| `bot_ai` retained samples; total / maximum | 72; 2.065 / 0.082 ms |
| Last map pre-cache / scenario-load | 0.003 / 4.586 ms |
| GPU timing | Unsupported by current trace path |

Raw trace, RSS samples, and game log are `bloodgulch-3bot-trace.json`, `bloodgulch-3bot-rss.csv`, and `bloodgulch-3bot-game.log`. RSS statistics omit samples before 10 seconds to avoid startup/warm-up values. As with the menu run, `lost_events` indicates that the bounded trace ring overwrote events; percentile and zone aggregates describe the retained samples, not a guaranteed complete event history. Draw calls and allocations are latest-sample metrics. Texture GPU bytes are estimates, excluding driver overhead, render targets, and high-resolution replacements. Presentation timings include swap/pacing waits and are not isolated GPU or whole-frame timings.

## Other requested scenarios

Not measured: Blood Gulch Team Slayer, Foundation, Portent/large Custom Edition map, and repeated map transitions. Missing-texture counts and upload latency/spike distributions are not instrumented. The trace exposes draw calls, bot/navigation AI CPU zones, successful engine/game-state allocator calls per simulation tick, and latest map-precache and scenario-load durations.

## Follow-up: job-graph comparison (appended 2026-10-09)

This scenario was re-run with the job-graph integration on and off:
[benchmarks/jobs/macos-arm64-2026-10-09](../../jobs/macos-arm64-2026-10-09/README.md).

- **Pacing.** `D3DDevice_Present` p95 was 13.5–14.0 ms in all four runs,
  against this baseline's 13.48 ms, so frame pacing is unchanged.
- **CPU.** No measurable CPU frame-time improvement.
- **RSS.** Median RSS was 417–442 MiB with jobs off and 453–454 MiB with
  jobs on. That is unresolved, and within the jobs-off control's 25 MiB
  spread.
- **Unchanged.** Tagged `game` memory stayed at 312.1 MiB, and allocations
  per tick stayed at 0.
- **HEAD comparison (appended).** HEAD `80b829aa`, without the job graph,
  re-run twice in a separate worktree: Present p95 13.45/12.83 ms and RSS
  median 418.5/439.5 MiB, matching this build with jobs off. Details are in
  the jobs benchmark README.
