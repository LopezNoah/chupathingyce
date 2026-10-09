#ifndef ENGINE_CORE_TRACE_H
#define ENGINE_CORE_TRACE_H

/* Shared instrumentation stream (ADR 0051): CPU zones in per-thread rings,
   counters, tick/frame markers, GPU timestamp bookkeeping, and tagged memory
   budgets. All storage is caller-owned and reserved before the first event;
   recording never allocates and never takes a lock. */

#include "../assert.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* 64 job workers (ADR 0049), the main thread, and a few platform threads. */
#define ENGINE_TRACE_THREADS_MAX 72u
/* The GPU track is an extra pseudo-thread after the CPU threads. */
#define ENGINE_TRACE_TRACKS_MAX (ENGINE_TRACE_THREADS_MAX + 1u)
#define ENGINE_TRACE_ZONE_DEPTH_MAX 64u
#define ENGINE_TRACE_COUNTERS_MAX 1024u
#define ENGINE_TRACE_NAME_BYTES_MAX 64u
/* One frame of timestamps per pass begin/end pair, as ADR 0051 sizes it. */
#define ENGINE_TRACE_GPU_QUERIES_PER_FRAME_MAX 512u
#define ENGINE_TRACE_GPU_ZONES_PER_FRAME_MAX (ENGINE_TRACE_GPU_QUERIES_PER_FRAME_MAX / 2u)
/* Backends keep at most a few frames in flight (ADR 0044). */
#define ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX 4u
/* Ring capacities are powers of two so the write index masks instead of dividing. */
#define ENGINE_TRACE_EVENTS_PER_THREAD_MIN 64u
#define ENGINE_TRACE_EVENTS_PER_THREAD_MAX (1u << 20)

enum engine_trace_result
{
	ENGINE_TRACE_OK = 0,
	ENGINE_TRACE_INVALID_ARGUMENT,
	ENGINE_TRACE_THREADS_EXHAUSTED,
	ENGINE_TRACE_COUNTERS_EXHAUSTED,
	ENGINE_TRACE_BUDGET_EXCEEDED,
	ENGINE_TRACE_GPU_FRAMES_EXHAUSTED,
	ENGINE_TRACE_GPU_QUERIES_EXHAUSTED,
	ENGINE_TRACE_GPU_UNSUPPORTED,
	ENGINE_TRACE_CAPACITY_EXCEEDED,
	ENGINE_TRACE_IO_FAILED,
	ENGINE_TRACE_MALFORMED
};

/* A fixed hierarchy; ADR 0051 caps it at 64 tags. */
enum engine_trace_memory_tag
{
	ENGINE_TRACE_MEMORY_ECS = 0,
	ENGINE_TRACE_MEMORY_PHYSICS,
	ENGINE_TRACE_MEMORY_ANIMATION,
	ENGINE_TRACE_MEMORY_AUDIO,
	ENGINE_TRACE_MEMORY_RENDER_CPU,
	ENGINE_TRACE_MEMORY_RENDER_GPU,
	ENGINE_TRACE_MEMORY_STREAMING,
	ENGINE_TRACE_MEMORY_UI,
	ENGINE_TRACE_MEMORY_SCRIPTS,
	ENGINE_TRACE_MEMORY_GAME,
	ENGINE_TRACE_MEMORY_JOBS,
	ENGINE_TRACE_MEMORY_TRACE,
	ENGINE_TRACE_MEMORY_TAG_COUNT
};

enum engine_trace_event_kind
{
	ENGINE_TRACE_EVENT_NONE = 0,
	ENGINE_TRACE_EVENT_ZONE_BEGIN = 1,   /* payload: struct engine_trace_location */
	ENGINE_TRACE_EVENT_ZONE_END = 2,
	ENGINE_TRACE_EVENT_NAMED_BEGIN = 3,  /* payload: static NUL-terminated name */
	ENGINE_TRACE_EVENT_NAMED_END = 4,
	ENGINE_TRACE_EVENT_COUNTER = 5,      /* value32: counter index, argument: value */
	ENGINE_TRACE_EVENT_TICK_BEGIN = 6,   /* argument: simulation tick */
	ENGINE_TRACE_EVENT_TICK_END = 7,
	ENGINE_TRACE_EVENT_FRAME_BEGIN = 8,  /* argument: presentation frame */
	ENGINE_TRACE_EVENT_FRAME_END = 9,
	/* GPU track only: payload location, argument duration, value32 frame low bits. */
	ENGINE_TRACE_EVENT_GPU_ZONE = 10
};

/* Declared static at the instrumentation site so events store one pointer. */
struct engine_trace_location
{
	char const *name;
	char const *file;
	uint32_t line;
};

/* 32 bytes, so a 1 MiB ring holds 32,768 events (ADR 0051). */
struct engine_trace_event
{
	uint64_t timestamp_nanoseconds;
	uint64_t argument;
	void const *payload;
	uint32_t kind;
	uint32_t value32;
};

struct engine_trace;

struct engine_trace_thread
{
	struct engine_trace *trace;
	struct engine_trace_event *ring;
	/* Total events ever written; the ring holds the newest `capacity`. */
	_Atomic uint64_t write_count;
	uint32_t capacity;
	uint32_t index;
	uint32_t depth;
	char name[ENGINE_TRACE_NAME_BYTES_MAX];
	/* Explicit begin/end pairing stack; never shared between threads. */
	void const *stack[ENGINE_TRACE_ZONE_DEPTH_MAX];
	uint8_t stack_kind[ENGINE_TRACE_ZONE_DEPTH_MAX];
};

struct engine_trace_counter
{
	char name[ENGINE_TRACE_NAME_BYTES_MAX];
	_Atomic int64_t value;
};

struct engine_trace_memory_tag_state
{
	_Atomic int64_t bytes_current;
	_Atomic int64_t bytes_peak;
	_Atomic uint64_t allocation_count;
	_Atomic uint64_t release_count;
	_Atomic uint32_t budget_violation_count;
	int64_t bytes_budget; /* 0 means no budget for this platform. */
};

struct engine_trace_gpu_zone
{
	struct engine_trace_location const *location;
	uint32_t query_begin;
	uint32_t query_end;
};

struct engine_trace_gpu_frame
{
	uint64_t frame;
	uint32_t in_use;
	uint32_t query_count;
	uint32_t zone_count;
	uint32_t depth;
	uint32_t stack[ENGINE_TRACE_ZONE_DEPTH_MAX];
	struct engine_trace_gpu_zone zones[ENGINE_TRACE_GPU_ZONES_PER_FRAME_MAX];
};

struct engine_trace_config
{
	/* threads_max * events_per_thread events; events_per_thread is a power of two. */
	struct engine_trace_event *events;
	uint32_t events_capacity;
	uint32_t threads_max;
	uint32_t events_per_thread;
	/* Bytes per tag; 0 leaves a tag unbudgeted. */
	int64_t memory_budgets_bytes[ENGINE_TRACE_MEMORY_TAG_COUNT];
	/* Tests may make an exceeded budget a programmer error. */
	int budget_violation_fatal;
};

struct engine_trace
{
	struct engine_trace_config config;
	struct engine_trace_thread threads[ENGINE_TRACE_TRACKS_MAX];
	_Atomic uint32_t thread_count;
	struct engine_trace_counter counters[ENGINE_TRACE_COUNTERS_MAX];
	_Atomic uint32_t counter_count;
	struct engine_trace_memory_tag_state memory[ENGINE_TRACE_MEMORY_TAG_COUNT];
	struct engine_trace_gpu_frame gpu_frames[ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX];
	uint64_t gpu_unsupported_frame_count;
	uint64_t origin_nanoseconds;
};

enum engine_trace_result engine_trace_init(struct engine_trace *trace,
	struct engine_trace_config const *config);
/* Monotonic nanoseconds since engine_trace_init. */
uint64_t engine_trace_now(struct engine_trace const *trace);
char const *engine_trace_memory_tag_name(enum engine_trace_memory_tag tag);

/* Registration happens at thread start. The returned thread is also the
   calling thread's current thread for the ENGINE_TRACE_* macros. */
enum engine_trace_result engine_trace_thread_register(struct engine_trace *trace,
	char const *name, struct engine_trace_thread **thread);
struct engine_trace_thread *engine_trace_thread_current(void);
void engine_trace_thread_set_current(struct engine_trace_thread *thread);

void engine_trace_zone_begin(struct engine_trace_thread *thread,
	struct engine_trace_location const *location, uint64_t argument);
void engine_trace_zone_end(struct engine_trace_thread *thread,
	struct engine_trace_location const *location);
/* For zones named by data with static lifetime, such as job names. */
void engine_trace_named_begin(struct engine_trace_thread *thread, char const *name,
	uint64_t argument);
void engine_trace_named_end(struct engine_trace_thread *thread, char const *name);
void engine_trace_tick_begin(struct engine_trace_thread *thread, uint64_t tick);
void engine_trace_tick_end(struct engine_trace_thread *thread, uint64_t tick);
void engine_trace_frame_begin(struct engine_trace_thread *thread, uint64_t frame);
void engine_trace_frame_end(struct engine_trace_thread *thread, uint64_t frame);

enum engine_trace_result engine_trace_counter_register(struct engine_trace *trace,
	char const *name, uint32_t *counter);
void engine_trace_counter_set(struct engine_trace *trace, uint32_t counter, int64_t value);
void engine_trace_counter_add(struct engine_trace *trace, uint32_t counter, int64_t delta);
int64_t engine_trace_counter_get(struct engine_trace const *trace, uint32_t counter);
/* Writes one event per registered counter, normally at a tick or frame end. */
void engine_trace_counters_sample(struct engine_trace_thread *thread);

/* Returns BUDGET_EXCEEDED (still recorded) when the tag passes its budget. */
enum engine_trace_result engine_trace_memory_reserve(struct engine_trace *trace,
	enum engine_trace_memory_tag tag, int64_t bytes);
/* Sizes must pair with a previous reserve on the same tag. */
void engine_trace_memory_release(struct engine_trace *trace,
	enum engine_trace_memory_tag tag, int64_t bytes);
uint32_t engine_trace_memory_budget_violations(struct engine_trace const *trace);

/* GPU timing (ADR 0051): a backend opens a frame, asks for query indices at
   declared pass boundaries (ADR 0045), and resolves the frame once its
   submission retires (ADR 0044), possibly several frames later. */
enum engine_trace_result engine_trace_gpu_frame_begin(struct engine_trace *trace,
	uint64_t frame, uint32_t *slot);
enum engine_trace_result engine_trace_gpu_zone_begin(struct engine_trace *trace, uint32_t slot,
	struct engine_trace_location const *location, uint32_t *query);
enum engine_trace_result engine_trace_gpu_zone_end(struct engine_trace *trace, uint32_t slot,
	uint32_t *query);
/* timestamps[query] are raw GPU ticks; gpu_origin_ticks maps to cpu_origin.
   When the backend could not time the frame, pass supported = 0: the frame is
   dropped and counted, and no GPU events are fabricated. */
struct engine_trace_gpu_resolve
{
	uint64_t const *timestamps;
	uint32_t timestamp_count;
	int supported;
	uint64_t nanoseconds_per_tick_numerator;
	uint64_t nanoseconds_per_tick_denominator;
	uint64_t gpu_origin_ticks;
	uint64_t cpu_origin_nanoseconds;
};
enum engine_trace_result engine_trace_gpu_frame_resolve(struct engine_trace *trace,
	uint32_t slot, struct engine_trace_gpu_resolve const *resolve);
struct engine_trace_thread *engine_trace_gpu_track(struct engine_trace *trace);

/* Instrumentation sites use these macros. They expand to nothing unless the
   build defines ENGINE_TRACE_ENABLED, so shipping builds carry no zone code. */
#if defined(ENGINE_TRACE_ENABLED)
#define ENGINE_TRACE_LOCATION(variable, zone_name) \
	static struct engine_trace_location const variable = { zone_name, __FILE__, __LINE__ }
#define ENGINE_TRACE_ZONE_BEGIN(location, argument) \
	engine_trace_zone_begin(engine_trace_thread_current(), (location), (argument))
#define ENGINE_TRACE_ZONE_END(location) \
	engine_trace_zone_end(engine_trace_thread_current(), (location))
#define ENGINE_TRACE_NAMED_BEGIN(thread, zone_name, argument) \
	engine_trace_named_begin((thread), (zone_name), (argument))
#define ENGINE_TRACE_NAMED_END(thread, zone_name) \
	engine_trace_named_end((thread), (zone_name))
#define ENGINE_TRACE_COUNTER_ADD(trace, counter, delta) \
	engine_trace_counter_add((trace), (counter), (delta))
#else
#define ENGINE_TRACE_LOCATION(variable, zone_name) \
	ENGINE_STATIC_ASSERT(1, "trace disabled")
#define ENGINE_TRACE_ZONE_BEGIN(location, argument) ((void)0)
#define ENGINE_TRACE_ZONE_END(location) ((void)0)
#define ENGINE_TRACE_NAMED_BEGIN(thread, zone_name, argument) ((void)0)
#define ENGINE_TRACE_NAMED_END(thread, zone_name) ((void)0)
#define ENGINE_TRACE_COUNTER_ADD(trace, counter, delta) ((void)0)
#endif

#endif
