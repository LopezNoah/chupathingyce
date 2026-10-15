# Data-aware job graph (ADR 0049 port)

The deterministic job scheduler from `LopezNoah/engine`
(`engine/core/jobs`, commit `511a99b1`) runs in ChupathingyCE. Its first
workload is the presentation interpolation blend. Simulation timing (30 Hz),
authoritative state, network formats, and save layouts are unchanged.

## 1. Reused directly

`job_graph.{c,h}`, `job_system.{c,h}`, and README are copied with their
semantics unchanged:

- fixed worker pool and per-priority ready rings;
- reserved simulation workers;
- explicit dependencies plus RAW/WAR/WAW edges inferred from declared access;
- barriers;
- partitions that don't depend on the worker count;
- main-thread jobs;
- the sequential oracle;
- caller-owned bounded arenas;
- overflow handling: mark the graph, rebuild in direct mode;
- checked-exclusion assertions (always on).

`engine/simulation/job_phase` is ported as `engine/core/jobs/job_phase` with
the ECS world and tick replaced by an opaque context.

## 2. What changed for ChupathingyCE

| Change | Where | Why |
| --- | --- | --- |
| Optional `edge_kinds` array: EXPLICIT, BARRIER, RAW, WAR, WAW | `job_graph.{c,h}` | Graph export tells explicit edges from inferred hazard edges. When two reasons order one pair of jobs, the first one recorded is kept. |
| Optional `engine_job_profile`: per-job ready/start/end/busy times, per-thread busy time | `job_graph.{c,h}`, two lines in `job_system.c` | Stage B tracing. Jobs never see it. Costs nothing when `NULL`. |
| `job_phase`: a built graph the pool refuses (READY_EXHAUSTED or SHUTTING_DOWN) runs on the oracle, counted | `job_phase.c` | Nothing has run yet, so this is not a rerun. A failed job is never rerun. |
| C89-clean adapter: trampoline arguments, static arenas (135 KB total), CPU-aware worker count, env switch, JSON/DOT report | `port/linux/src/halo_jobs.h`, `posix_jobs.c` | Game sources are gnu89 with the Xbox ABI and can't include `<stdatomic.h>`. |
| Build wiring with `-DHALO_JOBS_ENABLED` | `tools/linux_build.py`, `tools/lp64_build.py` | Same pattern as the trace import. Windows and Android get stub functions and keep the legacy paths. |

The upstream suite still passes against the modified scheduler. Dispatch
cost is unchanged when profiling is off.

## 3. First workload: interpolation blend

`render_interpolation.c` already blended each object lazily, on first
access while drawing. The per-object blend is now
`interpolated_object_blend()` in `render_interpolation_blend.inc`. It reads
and writes only its own record: the two snapshots, the cached rotations, and
the third (presentation) buffer.

`render_interpolation_prepare_frame()` is called in `main.c` between
`render_interpolation_frame_begin()` and `main_game_render()`. It blends
every eligible record in 32 partitions of 256 absolute indices. The renderer
then finds the blends done. The lazy path is untouched and still covers:

- `HALO_JOBS=off`;
- editors;
- `game_frame`'s accesses;
- records a network correction invalidates after the prepare step;
- any phase failure.

Results are bit-identical between the two paths: the test compares them
every frame.

## 4. Graph

```text
render frame (main loop, main.c)
  game_time_update: 0..N fixed ticks -> render_interpolation_tick (snapshots)   [legacy, main thread]
        |  explicit barrier: the phase is built only after every tick has run
        v
  interpolation_blend_objects  (presentation priority, 32 partitions)
      reads  INTERPOLATION_SNAPSHOTS
      writes INTERPOLATION_ROTATIONS, PRESENTATION_POSES (each partition: its own records)
        |  inferred RAW on PRESENTATION_POSES (dashed red in jobs.json.dot)
        v
  interpolation_publish_poses  (main thread) checks every partition ran exactly once
        |  phase completes before halo_jobs_phase_run returns
        v
  main_game_render -> D3D8-on-OpenGL submission -> Present          [legacy, main thread]
```

`tools/test_halo_jobs.c` builds the declarative A→D example from the task
using only data declarations, and checks that D runs on the main thread.

## 5. Ownership, lifetime, and thread affinity

- **Record ownership.** The interpolation record array belongs to
  `render_interpolation.c` and the main thread. Every entry point (tick,
  reset, frame begin, the accessor) asserts no presentation phase is active.
- **Lifetime.** Phases are synchronous: built, run, and completed inside
  `render_interpolation_prepare_frame`. No job can outlive the frame stage
  that built it, so these are safe by construction:
  - map change (`render_interpolation_reset`);
  - snapshot `realloc` in the tick;
  - device reset;
  - suspend/resume;
  - shutdown.
- **Shutdown.** `atexit` drains and joins the pool, but only on the main
  thread. Otherwise the workers are idle sleepers.
- **What workers can touch.** Workers touch only snapshot copies. They never
  touch:
  - `object_datum` or guest memory;
  - globals: the argument carries the fraction, frame, and tick;
  - OpenGL.
- **Graphics device.** `GRAPHICS_DEVICE` is declared for main-thread jobs
  only.
- **Network correction.** `render_interpolation_correct_object` stays on the
  main thread during ticks. It invalidates `blended_frame` so the lazy path
  re-blends.
- **Interpolation rules.** The fraction is clamped to [0, 1] (NaN becomes 1)
  in `frame_begin`, so presentation never extrapolates. Snap, teleport,
  camera-cut, and first-person logic is unchanged.
- **Limits of validation.** Occupancy assertions check declared resources,
  not raw pointers. The blend job's record-locality is established by
  reading the code (an audit), and backed by the per-partition stamp check
  and the test's bitwise comparison.

## 6. Results

Apple M2 Pro: 8 performance and 4 efficiency cores, macOS 26.3, debug build.

**Isolated** (`tools/test_halo_jobs.py --bench`; 400 live objects, 144 FPS
clock; phase wall time per frame):

| Config | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| sequential oracle | 57 µs | 78 | 92 |
| 0 workers | 58 | 77 | 89 |
| 1 worker | 44 | 54 | 58 |
| 2 workers | 40 | 48 | 51 |
| 4 workers | 44 | 77 | 111 |
| 8 workers | 53 | 119 | 146 |

**In game** (Blood Gulch Slayer with 3 spartan bots, vsync off, 50 s per
run; `jobs.json` covers ~11k phase runs per configuration):

| Config | phase p50/p95/p99 | last blend busy | main-thread busy | fallbacks / failures |
| --- | ---: | ---: | ---: | ---: |
| sequential | 15.3 / 35.1 / 64.7 µs | 14.9 µs | 14.9 µs | 0 / 0 |
| 1 worker | 18.2 / 30.9 / 46.3 | 16.3 | 11.0 | 0 / 0 |
| 2 workers | 17.8 / 23.9 / 30.6 | 17.2 | 10.7 | 0 / 0 |
| 4 workers | 18.3 / 35.2 / 71.0 | 18.5 | 7.5 | 0 / 0 |

- **Median vs tail.** Dispatch adds about 3 µs to the median. Two workers
  roughly halve p99 and take about 4 µs off the main thread.
- **Whole frame.** The `render_frame` zone is about 1 ms, and its run-to-run
  spread (game state, the trace ring keeping only the last ~200 frames) is
  far larger than this phase. No whole-frame improvement is claimed.
- **Default.** The default is capped at 2 workers:
  `min(performance cores − 2, 2)`.
- **Eager vs lazy.** The eager path blends every live object, including ones
  never drawn. The legacy lazy cost was not isolated, so no claim is made
  that eager+parallel beats lazy either.

**Memory.** 135,320 bytes of static arenas. No heap allocation during frames:
the allocation guard is armed for all frame tests.

**Dispatch.** The upstream bench on this machine ranges from 3 ns to 62 ns
per job or partition, depending on graph shape.

## 7. Tests

```sh
python3 configure.py --bots && ninja macos
python3 tools/test_halo_jobs.py --sanitize   # plain: oracle + 0/1/2/4/8 workers; TSan, ASan/UBSan: oracle + 1/4
python3 tools/test_halo_jobs.py --bench
# upstream suite against the downstream scheduler:
cp -R ../engine /tmp/e && cp engine/core/jobs/job_{graph,system}.{c,h} /tmp/e/engine/core/jobs/ &&
  (cd /tmp/e && python3 tools/test_engine_jobs.py --sanitize && python3 tools/test_engine_jobs.py --bench 4)
```

What the tests cover:

- RAW, WAR, WAW, read/read overlap, fan-out and fan-in (200 rounds);
- edge kinds, including barriers and the generation change after a barrier;
- no duplicate or omitted partitions;
- main-thread affinity;
- overflow, then a direct rebuild that runs all jobs once, in order, and is
  counted;
- a failing job that is not rerun;
- generational handles;
- frame clocks at 30/40/60/120/144 FPS, irregular intervals, and 10 FPS
  (multiple ticks per frame);
- tick count equals whole 30ths of elapsed time;
- fraction within [0, 1], with no blended position outside its snapshot
  interval;
- authoritative state unchanged by presentation;
- jobs bit-identical to the lazy path every frame;
- one hash across all worker counts.

The in-game runs logged no assertion failures.

## 8. Unresolved risks

- **Platforms.** Only macOS arm64 was built and run. The Linux 32/64-bit
  build edits are untested here (no Docker daemon or Linux host). Windows
  and Android use the stub functions. The Windows pthread shim has no
  `pthread_join`, which must be added before enabling jobs there.
- **Frame clock.** The frame-clock test reproduces `game_time.c`'s
  arithmetic; it does not execute `game_time.c`.
- **Clock source.** On Linux the profile clock is `CLOCK_MONOTONIC`.
- **Worker count.** The default is chosen from one machine.

## 9. Next workloads

1. Make the prepare step lazy-aware: blend only objects that pass
   visibility. This needs a visibility list produced before blending.
2. Read-only bot perception and spatial queries with per-partition results,
   at simulation priority with reserved workers.
3. Overlapping the blend with early `main_game_render` work. This needs a
   submit/join split, with the join on first record access.

## 10. Metrics vs baseline (appended 2026-10-09)

Full table and raw data:
[benchmarks/jobs/macos-arm64-2026-10-09](../benchmarks/jobs/macos-arm64-2026-10-09/README.md).
The scenario is the recorded baseline's (Blood Gulch, 3 spartan bots, vsync
on). Two runs each with jobs off and jobs on, on the same build.

| Metric | Recorded baseline | Jobs off (2 runs) | Jobs on (2 runs) |
| --- | ---: | ---: | ---: |
| Present p95 | 13.48 ms | 13.63, 13.97 | 13.52, 13.59 |
| Present p99 | 14.54 ms | 15.76, 14.87 | 15.84, 13.83 |
| `render_frame` mean | — | 1519, 1219 µs | 1599, 1514 µs |
| `interpolation_prepare` mean | — | — | 27.9, 17.6 µs |
| Phase p99 | — | — | 81.3, 55.0 µs |
| RSS median (10–40 s) | 440.8 MiB | 417.4, 442.2 | 453.8, 453.2 |
| Fallbacks / failures | — | — | 0 / 0 |

**Verdict:**

- **Frame time.** No measurable frame-time improvement over the baseline or
  the jobs-off control.
- **Pacing and correctness.** No pacing regression and no correctness
  failures.
- **RSS.** RSS with jobs on is 11–36 MiB higher in these runs. The job code
  accounts for about 1 MiB at most, so the rest is unexplained (see the
  benchmark README).

## 11. Work log and decisions (appended 2026-10-09)

### Order of work

1. Ran the upstream suite and bench on the untouched engine repo: all pass.
2. Copied the scheduler and added the edge kinds and profile hooks.
3. Re-ran the upstream suite against the modified files, in a scratch copy
   of the engine tree (`/tmp/engine_dj`) so they compile with downstream
   headers: all pass, no dispatch regression.
4. Ported `job_phase`.
5. Wrote the adapter and wired the builds.
6. Extracted the blend kernel and added the prepare phase.

### Bugs and fixes

- **Argument too large.** The test caught it: adding the partition-stamp
  pointer grew `struct blend_job_argument` to 48 bytes, over the adapter's
  40-byte limit. `halo_job_add` rejected the job and the lazy path silently
  covered for it. Fixes:
  - compact `int32_t` fields;
  - a C89 compile-time size check;
  - `platform_log` on a phase's first failure.
- **Clock resolution.** macOS `CLOCK_MONOTONIC` has µs resolution, so the
  profile now uses `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` there.
- **Validation job cost.** The validation consumer first scanned all 8192
  records: 8 µs, half the phase. It now checks 32 per-partition stamps,
  which also catches a partition that ran twice.
- **Test runtime.** The integration test first took minutes: per-frame
  50 MB copies and a full-array hash. It now copies and hashes only live
  records (400 objects, 1.5 s simulated per clock; 0.4 s and a subset of
  configurations under sanitizers): about 20 s total.

### Decisions

- **Default workers.** `min(performance cores − 2, 2)`. Four workers
  worsened p99 both in isolation and in game.
- **Reserved workers.** No reserved simulation workers: there are no
  simulation jobs yet.
- **Default mode.** `parallel`; `HALO_JOBS=off` restores the legacy path.
  Because no frame-time win was shown, switching the default to `off` until
  a visibility-limited blend exists is a reasonable alternative.

### Environment notes

- The build was reconfigured with `python3 configure.py --bots` to match
  the baseline. Re-run `configure.py` with your usual flags if needed.
- Nothing is committed. `debug.txt` was already untracked.
- Windows needs `pthread_join` in `port/windows/src/win32_posix.c` before
  `HALO_JOBS_ENABLED` is turned on there.
- The Linux build edits are unverified.

## 12. Against HEAD `80b829aa`, the last commit before this work (appended 2026-10-09)

HEAD was built untouched in a separate worktree (`/tmp/chupa_head`); the
working tree and the gitignored `blender/` assets were never touched. It ran
the baseline scenario twice. Results are in
[benchmarks/jobs/macos-arm64-2026-10-09](../benchmarks/jobs/macos-arm64-2026-10-09/README.md#against-the-last-commit-before-the-job-graph-appended-2026-10-09).

- **Pacing.** Present p95 was 13.45/12.83 ms on HEAD, 13.63/13.97 with jobs
  off, and 13.52/13.59 with jobs on: no improvement and no regression.
- **Code compiled in but off.** Indistinguishable from HEAD.
- **RSS medians.** HEAD 418.5/439.5, jobs off 417.4/442.2, jobs on
  453.8/453.2 MiB. That is a possible +11 MiB with jobs on, unexplained by
  the job code. To investigate: `vmmap -summary`/`footprint`, and
  `HALO_JOB_WORKERS=0`.
- **Safety backups.** Taken before the comparison, in `/tmp/chupa_backup`:
  - `ours_tracked.patch`: `git diff`;
  - `ours_untracked.tgz`: new files;
  - `blender_assets.tgz`: the gitignored Blender assets.

## 13. Why there is no visible improvement (appended 2026-10-09)

### It is hooked up

- The presentation phase ran 5,237 times in a 45 s run (11,731 with vsync
  off).
- The blend partitions ran on 3 threads: main 14.4 µs, worker 1 8.8 µs,
  worker 2 7.7 µs in the last run.
- There were no fallbacks or failures.

### Where the main thread's time goes

Source: `sample` of the running game, 12 s at 1 ms, Blood Gulch with 3 bots,
vsync off, jobs on (`benchmarks/jobs/macos-arm64-2026-10-09/main-thread-sample-novsync.txt`).

| Main thread, inclusive | Share |
| --- | ---: |
| `D3DDevice_Present` | 72.2% |
| … of which `platform_video_swap` → `SDL_DelayPrecise` → `nanosleep` (the frame limiter sleeping) | ~55% |
| … of which the OpenGL swap (`Cocoa_GL_SwapWindow`) | ~13% |
| `render_frame` (draw-call submission: shadows, objects, structure, HUD) | 22.6% |
| `network_game_server_idle` | 2.1% |
| `game_time_update` (all fixed 30 Hz ticks, bots and AI included) | 1.5% |
| `render_interpolation_prepare_frame` (the job-graph phase) | 0.2% |

### Why the job system can't help yet

1. **The game isn't CPU-bound on this machine.** With vsync off,
   `display.max_fps = 0` caps frames at twice the display rate. The main
   thread spends over half its time asleep in the limiter. CPU time saved
   anywhere becomes more sleep, not more frames or lower latency.
2. **The parallelized workload is 0.2% of the main thread.** By Amdahl's law,
   an infinitely fast blend saves at most that, which is what we measured
   (no change).
3. **The real CPU work is single-threaded by contract.** Draw submission
   (22.6%) goes through the OpenGL-backed D3D8 layer, which must stay on the
   context's thread. Simulation is 1.5% and full of sequential legacy
   globals.
4. **Halo Infinite's gains came from a CPU-bound modern engine.** Halo CE on
   an M2 Pro uses about 1–2 ms of CPU per frame.

### What would show a gain

- **Measure CPU time per frame, not frame rate.** That is,
  `render_frame` + `game_time_update`, uncapped with `HALO_MAX_FPS`. Or test
  on CPU-limited targets: Android, older x86.
- **Move CPU work off the GL path first.** Build the render object list,
  shadow lists, and per-object skinning into buffers in jobs, then have the
  main thread only submit. This is the only CPU block large enough (~20% of
  the main thread) to matter here.
- **Keep scaling the simulation down the list.** Only bot perception and
  spatial queries would grow with many bots.

## 14. Lobby scaling: 8 to 32 players, CPU time and frame rate (appended 2026-10-09)

### Setup

- **Bots raised to 31.** Bots were capped at 3: they shared one
  pseudo-machine (index 127), whose player list has
  `MAXIMUM_LOCAL_PLAYERS` (4) slots. Now `BOTS_MAXIMUM = 31`, and bot *n*
  joins pseudo-machine `127 − n/4` (machines 120–127).
  - **Untouched:** no packet format or game-state layout changes. The
    port's limits were already 128 players and 128 machines. Bots never
    join a game another machine is in.
  - **Files:** `source/features/bots/bots.c`, `settings.inc` (now "0 to
    31").
- **New recorder.** `port/linux/src/posix_frame_stats.c`, enabled by
  `HALO_FRAME_STATS=<csv>`. At every `D3DDevice_Present` it records:
  - the wall interval (the frame rate);
  - main-thread CPU (`CLOCK_THREAD_CPUTIME_ID`): the game plus GL driver
    work on that thread, not the limiter's sleep;
  - process CPU (`getrusage`).
- **Scenario.** Blood Gulch FFA Slayer, *n* − 1 spartan bots, vsync off and
  uncapped (`HALO_MAX_FPS=-1`). Runs last until every bot has joined, plus
  40 s. The window is the final 30 s.
- **Data.** `benchmarks/jobs/macos-arm64-2026-10-09/lobbies/` (CSVs, job
  reports, logs, scripts, `analyze.py`).
- **Invalid run.** The first 16-player/jobs-off run was discarded: its
  window was closed after 19 s. It was re-run. No match ended in any run
  (top score at most 6 of 15 kills).

| Players | Jobs | FPS | Frame p50 / p99 µs | Main CPU p50 / p99 µs | Process CPU p50 µs | Main busy | Phase p50 / p99 µs |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | off | 453 | 1761 / 6436 | 1553 / 3414 | 1721 | 83% | – |
| 8 | on | 542 | 1599 / 5706 | 1497 / 3048 | 1702 | 84% | 15 / 28 |
| 12 | off | 424 | 2033 / 5852 | 1897 / 4239 | 2092 | 86% | – |
| 12 | on | 432 | 2117 / 5555 | 1940 / 4132 | 2201 | 86% | 19 / 42 |
| 16 | off | 400 | 2100 / 7388 | 1989 / 3968 | 2149 | 87% | – |
| 16 | on | 415 | 2151 / 5428 | 1833 / 4081 | 2134 | 85% | 19 / 55 |
| 24 | off | 266 | 3734 / 8406 | 3267 / 6154 | 3670 | 88% | – |
| 24 | on | 295 | 3100 / 7354 | 2740 / 5933 | 3152 | 89% | 26 / 70 |
| 24 (repeat) | off | 430 | 1967 / 6304 | 1747 / 5078 | 1944 | 87% | – |
| 24 (repeat) | on | 312 | 2781 / 7607 | 2517 / 5976 | 2860 | 86% | 26 / 68 |
| 32 | off | 254 | 3378 / 9347 | 2997 / 8433 | 3335 | 88% | – |
| 32 | on | 274 | 3558 / 8369 | 2902 / 7231 | 3459 | 88% | 33 / 80 |
| 32 (repeat) | off | 263 | 3221 / 9100 | 2969 / 8067 | 3278 | 88% | – |
| 32 (repeat) | on | 255 | 3498 / 8096 | 3245 / 6890 | 3666 | 88% | 29 / 74 |

### Findings

- **The main thread is the bottleneck at every size.** It is busy 83–89%
  of each frame. Its CPU time per frame roughly doubles from 8 players
  (~1.5 ms) to 32 (~3.0 ms). Process CPU is only 0.1–0.4 ms above main
  CPU: the other threads (job workers, driver, audio) are mostly idle.
- **Jobs on vs off: no reliable median difference.** Run-to-run variance is
  bigger than any effect. 24 players with jobs off gave 266 FPS in one run
  and 430 in the repeat, depending on what the host's stationary camera
  sees. The 32-player result reversed between runs (+8%, then −3%).
- **The phase can't explain the gaps.** The parallelized phase costs only
  15–33 µs p50 per frame, so it can't cause the 100–800 µs gaps either way.
- **Weak tail signal.** Frame p99 was lower with jobs on in 6 of 7 pairs,
  and main-CPU p99 in 5 of 7. That's suggestive (the lazy blend runs
  scattered inside rendering), but with one run per pair it's not proven.
- **The phase grows with players.** From 15 µs at 8 players to 33 µs at 32,
  still about 1% of main-thread CPU.

### Draw submission (main-thread `render_frame`, 3-bot profile)

| Share of `render_frame` | |
| --- | ---: |
| Game code, including the D3D8→GL layer | ~35% |
| Apple's OpenGL-on-Metal stack (`AppleMetalOpenGLRenderer` 22%, AGX 9%, GLEngine 6%, IOKit 5%, Metal 5%, objc 3%) | ~51% |
| `memmove`/malloc (buffer uploads) | ~9% |

Most draw-submission CPU is Apple translating GL into Metal, synchronously,
on the game's thread. The D3D8 layer's streaming is already tuned for that
driver (orphaned stream buffers, unsynchronized maps), so there's no cheap
fix inside it. The options, from least to most effort:

1. **Fewer GL calls.** Batch and sort state in the D3D8 layer. Moderate
   gain, low risk; needs per-call profiling at 32 players first.
2. **A dedicated GL submission thread** (the render thread in Halo
   Infinite's game → render → submit split). The game thread records D3D8
   calls into a bounded command ring; one GL thread owns the context and
   replays them.
   - GL still runs on a single thread, but not the game's.
   - The ~51% driver share and the swap move off the main thread, and run
     in parallel with the next frame's simulation and render preparation.
   - Sync points: buffer locks, visibility queries, screenshots, device
     reset.
3. **A native Metal backend.** Removes the GL→Metal translation and allows
   encoding command buffers from several threads, which the job graph
   could then drive. This is the largest effort.

**Next step:** profile at 32 players to confirm the driver share at scale,
then prototype option 2 behind a switch.

### Noise control for future comparisons

Use several repeats per configuration, alternating order. Better still, use
a fixed spectator camera or demo playback, so draw load doesn't depend on
where the bots fight.
