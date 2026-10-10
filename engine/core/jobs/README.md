# Job graphs and the worker pool

ADR 0049 runs parallel work through per-tick job graphs. The sequential
executor is the correctness oracle, and every parallel run produces the same
bytes as the oracle for any worker count. All storage is caller-owned and
reserved before the first job; nothing allocates afterwards.

## Building a graph

`engine_job_graph_add` records a job description:

- a static name, a function, an argument block (copied, at most 48 bytes);
- declared access: kind (component, subsystem, event buffer, handle, user),
  identifier, and read or write;
- explicit dependencies on earlier jobs;
- an item count and partition size, a priority class, flags, a cost hint, and
  the component masks of the queries it iterates.

Submission order is the sequential meaning of the graph. The graph adds an
edge from an earlier job whenever two jobs touch the same resource and at least
one writes it. Only non-conflicting work can therefore overlap. A write after
several reads waits for those reads; the reads already follow the previous
writer. `engine_job_graph_barrier` orders everything after it behind everything
before it. Use it for ADR 0017 structural barriers and event consumer phases.

Declared query masks are checked: a job that reads a component it did not
declare, or writes one it declared only for reading, is rejected with
`ENGINE_JOB_UNDECLARED_ACCESS`.

## Partitions and determinism

A job with `item_count > 0` runs `ceil(item_count / partition_size)`
partitions, whose boundaries never depend on the worker count. A partition
sees its index and item range, never a worker identity, clock, or completion
order. Reduce by writing one result per partition and combining them in
partition order in a later job. Emit events into one buffer per partition and
merge them in partition order (`ecs_event_buffer_append`). `ecs_spans_build`
numbers the rows of an ECS query so a partition can cover rows across chunk
and archetype boundaries.

## Running a graph

- `engine_job_graph_run_sequential` runs every job and partition in order on
  the calling thread.
- `engine_job_system_run` (or `_submit` and then `_wait`) runs the graph on the
  pool. The thread that called `engine_job_system_init` is the main thread.
  Only it submits, waits, runs `ENGINE_JOB_FLAG_MAIN_THREAD` jobs, and shuts
  down.
- Simulation, presentation, streaming, and background classes each have a
  ready ring. `simulation_reserved_workers` take only simulation jobs, so
  background work cannot starve a tick.
- A failing job (its function returns 0) fails the graph. Partitions that have
  not started are skipped, and the report names the lowest failing job.
- `engine_job_system_shutdown` finishes every submitted graph, including
  main-thread jobs, and then joins the workers. After shutdown, `_wait` still
  returns each graph's report without locking.

## Dispatch cost

ADR 0049 budgets 100–200 ns per job. The pool stays within it as follows:

- **Guided claims.** A thread claims several single-partition jobs, or a
  chunk of a job's partitions, under one lock. A claim takes at most a
  1/threads share of what remains and about 5 µs of declared work. Set
  `cost_hint_nanoseconds`: jobs without a hint count as 625 ns, at most 8 per
  claim, and jobs of 5 µs or more are always claimed one at a time.
- **Batch-level counting.** Tallies, completions, and predecessor decrements
  are counted per thread and published once per batch. A barrier or join fed
  by one batch is decremented once.
- **Continuations.** A single-partition successor that becomes ready runs on
  the same thread when nothing of higher priority is queued.
- **Locking and sleep.** The rings sit behind a test-and-test-and-set
  spinlock that yields after 256 polls. Sleeping uses a mutex and condition
  variable that wakers touch only when a thread is asleep. Idle threads spin
  on published per-ring counts first.

`python3 tools/test_engine_jobs.py --bench` prints dispatch costs for
independent jobs, one many-partition job, layered barriers, and a dependent
chain.

When an arena is exhausted, `engine_job_graph_add` returns the exhaustion and
marks the graph overflowed. `engine_job_phase_execute`
(`engine/simulation/job_phase.h`) then rebuilds the phase in direct mode, which
runs each job in order as it is added, and counts the fallback. A builder must
only record jobs, never mutate simulation state, so it can run twice.

## Limits

| Limit | Value | Reason |
| --- | ---: | --- |
| Workers | 64 | ADR 0049 |
| Dependencies per job | 16 | ADR 0049 |
| Accesses per job | 16 | Covers a system's columns, contexts, and buffers |
| Argument bytes | 48 | Keeps a record within 128 bytes |
| Partitions per job | 2^20 | 20 s of work at the 20 µs minimum partition |
| Idle spin polls | 2^20 | About 10 ms at 11 ns per pause |
| Jobs per claim | 16 | Guided, and capped by about 5 µs of declared work |
| Partitions per claim | 64 | Same |
| Ring lock polls before yielding | 256 | About 3 µs; longer means the holder was descheduled |
| Ready successors per publish | 128 | A 100-wide fan-out publishes in one critical section |

Checked builds (every build: assertions stay on) verify that no two running
jobs write one resource and that no writer overlaps a reader.

## Tests

```sh
python3 tools/test_engine_jobs.py --sanitize   # plain, traced, TSan, ASan/UBSan
python3 tools/test_engine_jobs.py --bench      # speedup curves (ADR 0049)
```

Clang sanitizer runtimes are not installed everywhere; `--cc gcc` works where
GCC's are.

## Downstream import (ChupathingyCE)

Imported from `LopezNoah/engine` commit `511a99b1a2e00eb414f1ad2880a8e16d03d00b81`
(CC0-1.0) as a source snapshot. Downstream changes, all opt-in and inert when
unused: optional `edge_kinds` (explicit, barrier, RAW, WAR, WAW) and
`engine_job_profile` in `engine_job_graph_config`, `engine_job_thread_slot`
set by workers, and `job_phase.{c,h}` ported from `engine/simulation` with an
opaque context and an oracle fallback when the pool refuses a built graph.
Integration, results and tests: [docs/jobs.md](../../../docs/jobs.md);
`python3 tools/test_halo_jobs.py --sanitize`.
