# OpenGL texture residency: macOS experiment

## Configuration and invariants

`graphics.texture_cache_mb` / `HALO_TEXTURE_CACHE_MB` sets the estimated Xbox
texture-cache budget in MiB, from 1 to 16384. Desktop default: **512 MiB**.
Invalid values fall back to the default. Android shares the policy with a
256 MiB default, but this change is validated on macOS, not Android.

The existing byte estimate counts every mip, depth slice and cube face. Native
DXT uses block sizes; converted formats use RGBA8, not guest pitch or texel size.
LRU order updates on every request, including the recent-lookup shortcut.
Textures requested in the current frame cannot be evicted or recycled as palette
variants. A working set larger than the budget is allowed, with an over-budget
event per upload request that cannot fit. At the next frame boundary, protection
expires and excess residency is trimmed. The existing idle timeout still removes
old entries. Upload failures release partial storage and retry on the next request.

This is a **soft estimated residency limit**, not a guarantee of physical GPU
memory. Render targets, high-resolution HUD/menu/text replacements, driver
metadata, alignment and deferred deletion are outside its accounting.

## Automated correctness check

```sh
python3 tools/test_texture_cache.py --sanitize
python3 tools/test_trace.py
ninja build/macos/obj/port/linux/src/xbox_textures.o \
      build/macos/obj/port/linux/src/port_config.o \
      build/macos/obj/port/linux/src/posix_trace.o
ninja build/macos/halo
```

The cache test compiles the production geometry and cache (using the macOS LP64
rewrite) with fake GL/guest-memory services. It covers exact LRU ordering,
recent-pointer invalidation, current-frame protection, oversized textures,
budget changes/invalid values, palette variants, invalidated and failed uploads,
idle eviction, and accounting. It also checks RGBA8, DXT, mip, volume and cube
estimates. It does **not** measure real driver allocations or rendered output.

## Controlled real-driver comparison

1. Build once, then hold build, machine, resolution, vsync, window state and
   high-resolution settings fixed. Use the same local maps and isolated saves.
   Record macOS version, GPU, OpenGL renderer and configuration with each result.
2. Run **512, 64, 16, 4 and 1 MiB** budgets. The lowest two are deliberately
   pathological. Repeat each at least three times, alternating order. Do not
   enable texture dumping, per-upload logging, or `HALO_TEXTURE_NO_CACHE`.
3. Use separate runs for menu idle, a reproducible Blood Gulch camera route,
   a large CE map, and repeated map transitions. Include return visits to reveal
   reuploads. Warm up for 30 seconds, then record at least 90 seconds.
4. Set `HALO_TRACE_FILE` to a unique existing output directory's JSON path.
   Copy each report as it changes (every 300 presented frames); the file is
   overwritten. Use counter deltas between reports, not cumulative totals as
   rates. Retain screenshots and inspect for missing/corrupted textures.
5. Profile the same route using Instruments **Game Memory / VM Tracker** and
   **Time Profiler**. Record physical footprint and GPU-related VM categories
   such as IOAccelerator/IOSurface where exposed. If Metal Resource Events can
   observe this machine's OpenGL-to-Metal driver resources, also record resource
   allocation/deallocation history; verify visibility rather than assuming it.
   Apple's [Metal memory analysis guide](https://developer.apple.com/documentation/xcode/analyzing-the-memory-usage-of-your-metal-app)
   distinguishes allocation size from physical footprint. That guide describes
   Metal apps; resource visibility for this OpenGL application must be checked.
6. Keep profiler-on comparisons separate from profiler-off timing runs. On Apple
   Silicon, CPU/GPU use unified memory: RSS is not GPU residency, and total
   physical footprint is not exclusively textures. If driver-specific physical
   usage cannot be observed, report the physical GPU result as **inconclusive**.
   Do not substitute the cache estimate for it. Allow identical post-transition
   settling time in every run; deletion need not immediately free driver pages.

Example launch (use your existing legally supplied game data):

```sh
HALO_DATA_ROOT="$DATA_ROOT" HALO_SAVE_ROOT="$SAVE_ROOT" \
HALO_TEXTURE_CACHE_MB=16 HALO_TRACE_FILE="$OUTPUT/trace.json" \
HALO_NET_ONLINE=0 HALO_NET_ALLOW_UPNP=0 \
build/macos/halo
```

### Measurements to retain

| Metric | Interpretation |
| --- | --- |
| `texture_gpu_bytes`, tagged `render_gpu` current/peak | Estimated cache allocation only; snapshots can miss a temporary intra-frame peak |
| `texture_budget_bytes` | Effective configured limit |
| `texture_over_budget` delta | Upload requests unable to fit without deleting protected textures |
| `texture_uploads`, `texture_upload_bytes` deltas / second | Successful upload churn; bytes are estimated GPU allocation submitted, not bus traffic |
| `texture_evictions`, hits/misses deltas | Eviction and reuse pressure (evictions include idle cleanup) |
| `texture_upload_ns` delta / second | CPU decode/upload wall time, including synchronous GL call waits; includes failed attempts |
| `texture_upload_max_ns` | Largest upload-call duration since launch; use identical warm-up or separately annotate startup |
| `texture_upload` CPU zone count/total/max | Recent retained upload samples |
| `D3DDevice_Present` p95/p99, Time Profiler | Presentation/pacing and whole-renderer stalls respectively; Present alone excludes earlier texture lookups/uploads |
| Physical footprint, GPU-related VM/resource data | External evidence, with profiler/tool coverage and attribution limitations recorded |
| Screenshots and gameplay checks | Correct rendering under pressure |

The bounded trace ring overwrites old events (`lost_events`); CPU zone aggregates
and frame percentiles cover only the retained window. Upload time/byte counters
are cumulative and survive event loss. There is no isolated GPU timer in this
trace path. Deferred stalls may occur during draws or presentation, not inside
`glTexImage*`, so inspect whole-renderer stacks as well as upload duration.

### Decision criterion

A lower normal-play budget is justified only by repeatable reductions in measured
physical usage beyond run-to-run noise, with correct output and no meaningful
upload-rate, upload-latency or frame-time regression. If estimates drop but
physical use does not, while uploads or driver waits increase, residency limiting
is causing churn rather than delivering the desired saving. Keep the generous
default. Report peak and steady state separately; map transitions may benefit
without steady-state scenes benefiting.

**Status:** automated cache checks (including ASan/UBSan and no-cache mode),
port telemetry JSON checks, trace regression tests, and macOS executable build
passed. A [real Blood Gulch three-bot smoke/stress run](texture-residency/macos-arm64-2026-10-09/README.md)
found and fixed a zero-byte trace-release crash, demonstrated severe upload churn
at 4 MiB, and exposed a Forge/network-silence warning at the default budget.
The follow-up Forge clock regression reproduced and resolved that warning by
removing Forge's automatic simulation pause. A paired physical GPU-memory
comparison remains incomplete; no physical
saving is claimed. The prior [macOS baseline](baseline/macos-arm64-2026-10-09/README.md)
contains RSS and estimated texture bytes, not a physical GPU-memory baseline.
