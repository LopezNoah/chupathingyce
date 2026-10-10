# Job graph vs baseline: macOS arm64, 2026-10-09

This compares the job-graph integration ([docs/jobs.md](../../../docs/jobs.md))
against [the recorded baseline](../../baseline/macos-arm64-2026-10-09/README.md).

## Setup

- **Machine:** Apple M2 Pro, macOS 26.3, OpenGL 4.1 Metal.
- **Build:** native debug build, `python3 configure.py --bots`, working tree
  on commit `80b829aa` plus the job-graph changes.
- **Not the baseline's build.** The baseline was built from `0e982660`
  (before texture residency, commit `80b829aa`). That makes the
  "this build, jobs off" runs the fair control; the recorded baseline is
  context only.

## Runs

**Scenario:** the baseline's own. `run_baseline_equivalent.sh` reproduces it:

- Blood Gulch FFA Slayer with 3 spartan bots;
- `HALO_NETWORK_TEST=host:bloodgulch:slayer`, `HALO_SOLO_GAME=1`;
- networking off;
- vsync on, as in the baseline;
- the baseline's `config.toml` and shader cache;
- `HALO_EXIT_AFTER=45`;
- RSS sampled with `ps` every 5 s.

**Order:** `off`, `on`, then `on2`, `off2` (reversed to control order
effects). `on` means `HALO_JOBS=parallel` with the default 2 workers. `off`
means `HALO_JOBS=off`: the legacy lazy path, no graph built.

## Metrics (appended)

| Metric | Recorded baseline | Jobs off | Jobs off (2) | Jobs on | Jobs on (2) | Verdict |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Retained frame samples | 292 | 195 | 195 | 178 | 178 | Trace ring; not run length |
| `D3DDevice_Present` p95 | 13.479 ms | 13.631 | 13.971 | 13.515 | 13.593 | No change (vsync-paced) |
| `D3DDevice_Present` p99 | 14.535 ms | 15.761 | 14.870 | 15.839 | 13.833 | No change (noise) |
| `D3DDevice_Present` max | 14.783 ms | 17.282 | 15.404 | 24.085 | 13.945 | One 24 ms spike in `on`; not in `on2` |
| `render_frame` mean (prepare + `main_game_render`) | not instrumented | 1519 µs | 1219 µs | 1599 µs | 1514 µs | No improvement; within noise |
| `render_frame` max | not instrumented | 5558 µs | 1907 µs | 3364 µs | 2410 µs | Noise |
| `interpolation_prepare` mean | n/a | n/a | n/a | 27.9 µs | 17.6 µs | New cost, about 1–2% of `render_frame` |
| Presentation phase p50 / p95 / p99 | n/a | n/a | n/a | 21.2 / 45.4 / 81.3 µs | 16.3 / 36.2 / 55.0 µs | 5,247 and 5,237 runs |
| Phase fallbacks / failures | n/a | n/a | n/a | 0 / 0 | 0 / 0 | Correct |
| `bot_ai` mean / max | 28.7 / 82 µs | 29.3 / 64 | 51.9 / 132 | 49.7 / 142 | 23.7 / 41 | Untouched by jobs; noise |
| RSS 10–40 s, min / median / max | 424.3 / 440.8 / 444.5 MiB | 409.8 / 417.4 / 419.6 | 439.3 / 442.2 / 442.5 | 448.3 / 453.8 / 455.6 | 443.0 / 453.2 / 455.6 | +11 to +36 MiB with jobs on; unexplained (see below) |
| Tagged `game` memory | 312.1 MiB | 312.1 | 312.1 | 312.1 | 312.1 | Unchanged |
| Job arenas (static) | 0 | 0.13 MiB | 0.13 | 0.13 | 0.13 | As designed |
| Allocations in latest tick | 0 | 0 | 0 | 0 | 0 | Unchanged |
| Draw calls (latest frame) | 324 | 175 | — | 193 | — | Camera-dependent; not comparable |

## Same build, vsync off (earlier runs, `run_novsync.sh`, 50 s each)

| Config | Phase p50 / p95 / p99 | Main-thread busy in phase |
| --- | ---: | ---: |
| sequential oracle | 15.3 / 35.1 / 64.7 µs | 14.9 µs |
| 1 worker | 18.2 / 30.9 / 46.3 | 11.0 |
| 2 workers | 17.8 / 23.9 / 30.6 | 10.7 |
| 4 workers | 18.3 / 35.2 / 71.0 | 7.5 |

## Did we improve on the baseline?

No, not measurably. Frame pacing (Present p95/p99) matches the baseline and
the jobs-off control within noise. `render_frame` CPU time with jobs on is
not lower than with jobs off. Correctness held: no fallbacks, failures, or
assertions, and the simulation advanced on its fixed schedule.

## RSS

Median RSS with jobs on was 453–454 MiB in both runs. With jobs off it was
417 and 442 MiB, so the jobs-off control itself varies by 25 MiB. The job
arenas are 0.13 MiB, and two idle worker stacks touch little memory, so the
code accounts for at most about 1 MiB.

The difference is either noise (driver and texture state differed between
runs: `render_gpu` current was 8.0 vs 12.7 MiB) or something not yet found.
It needs more repeats and `vmmap`/footprint before anyone concludes jobs
cost RSS. Until then, treat "+11 MiB" as an unresolved risk, not a
measurement.

## Limits

- **Trace ring.** The bounded trace ring keeps only the last ~180–200
  frames (`lost_events` around 100k), so these windows are short.
- **Game state.** Bots and camera differ between runs (draw calls 175 vs
  193).
- **Repeats.** Two runs per configuration is too few for percentiles below
  about 5%.
- **Lazy cost.** The legacy lazy blend's cost is inside `render_frame` and
  was not isolated.

## Files

All run data was copied from `/tmp/jobsbench`:

- `bloodgulch-3bot-jobs-{off,on,on2,off2}-*`: the vsync-on runs (trace,
  jobs report, DOT graph, RSS, log);
- `novsync-*`: the vsync-off worker sweep;
- `run_baseline_equivalent.sh`, `run_novsync.sh`: the scripts.

## Against the last commit before the job graph (appended 2026-10-09)

**The control.** The last commit that isn't part of this work is
`80b829aa` (HEAD; all job-graph changes are uncommitted).

**Build.** It was built untouched in a separate worktree, so the working
tree and the gitignored `blender/` assets were never touched (checksums
verified before and after):

```sh
git worktree add --detach /tmp/chupa_head HEAD
cd /tmp/chupa_head && python3 configure.py --bots && ninja macos
```

That binary contains no job code (`nm` finds no `halo_jobs`/`engine_job_`
symbols). It ran the same scenario twice (`run_head_worktree.sh`; files
`bloodgulch-3bot-head80b829aa-*`).

| Metric | Recorded baseline (`0e982660`+trace) | HEAD `80b829aa` (2 runs) | Ours, jobs off (2 runs) | Ours, jobs on (2 runs) |
| --- | ---: | ---: | ---: | ---: |
| Retained frame samples | 292 | 215, 213 | 195, 195 | 178, 178 |
| `D3DDevice_Present` p95 | 13.479 ms | 13.452, 12.831 | 13.631, 13.971 | 13.515, 13.593 |
| `D3DDevice_Present` p99 | 14.535 ms | 14.039, 13.931 | 15.761, 14.870 | 15.839, 13.833 |
| `D3DDevice_Present` mean | 7.19 ms | 6.68, 5.81 | 6.57, 6.84 | 6.57, 6.64 |
| `D3DDevice_Present` max | 14.783 ms | 15.286, 18.183 | 17.282, 15.404 | 24.085, 13.945 |
| `bot_ai` mean / max | 28.7 / 82 µs | 33.0 / 47, 55.8 / 150 | 29.3 / 64, 51.9 / 132 | 49.7 / 142, 23.7 / 41 |
| RSS median (10–40 s) | 440.8 MiB | 418.5, 439.5 | 417.4, 442.2 | 453.8, 453.2 |
| Tagged `game` memory | 312.1 MiB | 312.1, 312.1 | 312.1, 312.1 | 312.1, 312.1 |
| Allocations in latest tick | 0 | 0, 0 | 0, 0 | 0, 0 |

**Pacing and CPU.** No improvement, and no regression.

- Present p95, p99, and mean overlap across HEAD, our build with jobs off,
  and our build with jobs on.
- HEAD's p99 is slightly lower (13.9–14.0 vs 13.8–15.8 ms), within run
  noise.
- Having the job code compiled in but off (`HALO_JOBS=off`) is
  indistinguishable from HEAD.

**Retained frames.** Fewer frames are retained in our build (195/178 vs
215). This is a trace artifact: the two new zones use slots in the
fixed-size trace ring, so it keeps fewer frames. It is not fewer frames
rendered.

**RSS.** With four jobs-off runs (HEAD 418.5 and 439.5; ours-off 417.4 and
442.2 MiB), jobs-off medians cluster at 417–442 MiB. Both jobs-on runs were
453 MiB, 11–36 MiB above that.

This is now more likely real than noise, but it is still unexplained:

- static arenas: 0.13 MiB;
- two idle 512 KiB worker stacks, mostly untouched;
- no heap allocation in jobs.

Next step: `footprint`/`vmmap -summary` on jobs-on vs jobs-off processes, and
`HALO_JOB_WORKERS=0` (pool with no threads) to separate threads from code.
Until then, count +11 MiB RSS as a possible cost of `HALO_JOBS=parallel`.

## Lobby scaling, 8–32 players (appended 2026-10-09)

See [docs/jobs.md §14](../../../docs/jobs.md#14-lobby-scaling-8-to-32-players-cpu-time-and-frame-rate-appended-2026-10-09)
and the raw data in `lobbies/`.

- **Bottleneck.** The main thread is the bottleneck at every size: 83–89%
  busy, with CPU per frame going from ~1.5 ms at 8 players to ~3.0 ms at 32.
- **Jobs on vs off.** No reliable median difference: run-to-run variance is
  up to ±60%, and the phase costs only 15–33 µs. Frame p99 was lower with
  jobs on in 6 of 7 pairs, which is suggestive but not proven.
