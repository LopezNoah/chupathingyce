# Instrumentation stream

ADR 0051's shared trace layer: CPU zones, counters, tick and frame markers,
GPU timestamp bookkeeping, and tagged memory budgets. One stream serves tests,
tools, and AI clients. Storage is caller-owned; recording neither allocates
nor locks.

## Recording

- Register each thread once with `engine_trace_thread_register`. It writes
  only to its own ring of 32-byte events, which must be a power of two in
  size. A full ring overwrites its oldest events, and captures report them as
  lost.
- Instrument with `ENGINE_TRACE_LOCATION`, `ENGINE_TRACE_ZONE_BEGIN`, and
  `ENGINE_TRACE_ZONE_END`. These expand to nothing unless the build defines
  `ENGINE_TRACE_ENABLED`, so shipping builds carry no zone code. Begin and end
  must pair on each thread, or an assertion fails.
- Mark ticks and frames with `engine_trace_tick_begin`/`_end` and
  `engine_trace_frame_begin`/`_end`. Sample counters into the stream with
  `engine_trace_counters_sample`.
- Memory tags record reserves and releases per subsystem, with peaks and
  per-platform budgets. An over-budget reserve returns `BUDGET_EXCEEDED`, or
  aborts when the configuration makes budgets fatal.
- GPU backends call `engine_trace_gpu_frame_begin`, take query indices with
  `engine_trace_gpu_zone_begin`/`_end` at declared pass boundaries, and call
  `engine_trace_gpu_frame_resolve` once the frame retires. If the backend
  cannot time a frame, pass `supported = 0`: the frame is counted and no
  timings are invented.

## Reading

`engine_trace_capture_collect` copies all rings into a pointer-free capture
while producers are quiescent, for example between job graphs. From a capture:

- `engine_trace_capture_serialize` and `_deserialize` use a versioned binary
  format. The reader validates every field and is fuzzed.
- `engine_trace_export_chrome` writes Chrome trace event JSON, which
  chrome://tracing and https://ui.perfetto.dev open.
- `engine_trace_summary_build` and `_write_json` give the structured summary
  for editors and AI clients: top CPU and GPU zones, latest sampled counters,
  frame p95/p99, ticks, frames, lost and unmatched events, and memory by tag.
- `tools/engine_trace_tool.c` does the same from a saved capture:
  `engine_trace_tool summary CAPTURE` or
  `engine_trace_tool chrome CAPTURE OUT.json`.

`engine_trace_crash_context_write` formats open zones, the newest events, and
memory by tag for a crash report. It reads live rings without synchronization,
so use it only while the process is failing.

## Limits

72 threads plus the GPU track, zone depth 64, 1,024 counters, 64-byte names,
4 GPU frames in flight with 512 queries each, and 12 memory tags (ADR 0051
allows at most 64).

## Tests

```sh
python3 tools/test_trace.py --sanitize
```

## Downstream import

Imported from `LopezNoah/engine` commit
`511a99b1a2e00eb414f1ad2880a8e16d03d00b81` (CC0-1.0). This is a source
snapshot, not a linked submodule; downstream changes should be validated with
the local test runner above and recorded here when rebasing from engine.
