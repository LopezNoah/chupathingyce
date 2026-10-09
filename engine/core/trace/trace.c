#define _POSIX_C_SOURCE 200809L

#include "trace.h"

#include <string.h>
#include <time.h>

ENGINE_STATIC_ASSERT(sizeof(struct engine_trace_event) == 32,
	"ADR 0051 sizes rings in 32-byte events");
ENGINE_STATIC_ASSERT(ENGINE_TRACE_MEMORY_TAG_COUNT <= 64, "ADR 0051 fixes at most 64 tags");
ENGINE_STATIC_ASSERT((ENGINE_TRACE_EVENTS_PER_THREAD_MAX &
	(ENGINE_TRACE_EVENTS_PER_THREAD_MAX - 1)) == 0, "ring capacities are powers of two");
ENGINE_STATIC_ASSERT(ENGINE_TRACE_GPU_ZONES_PER_FRAME_MAX * 2 ==
	ENGINE_TRACE_GPU_QUERIES_PER_FRAME_MAX, "each GPU zone reserves two queries");

static _Thread_local struct engine_trace_thread *trace_thread_current;

static char const *const trace_memory_tag_names[ENGINE_TRACE_MEMORY_TAG_COUNT] = {
	"ecs", "physics", "animation", "audio", "render_cpu", "render_gpu",
	"streaming", "ui", "scripts", "game", "jobs", "trace",
};

static uint64_t trace_clock_nanoseconds(void)
{
	struct timespec now;
	int const status = clock_gettime(CLOCK_MONOTONIC, &now);
	ENGINE_ASSERT(status == 0);
	ENGINE_ASSERT(now.tv_sec >= 0);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static int trace_is_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

/* Returns the length, or `capacity` when no terminator fits. */
static uint32_t trace_name_length(char const *name, uint32_t capacity)
{
	char const *end;
	ENGINE_ASSERT(name != NULL);
	ENGINE_ASSERT(capacity > 0);
	end = memchr(name, 0, capacity);
	return end ? (uint32_t)(end - name) : capacity;
}

static void trace_thread_bind(struct engine_trace *trace, uint32_t index, char const *name)
{
	struct engine_trace_thread *thread = &trace->threads[index];
	uint32_t const length = trace_name_length(name, ENGINE_TRACE_NAME_BYTES_MAX);
	ENGINE_ASSERT(index < ENGINE_TRACE_TRACKS_MAX);
	ENGINE_ASSERT(length < ENGINE_TRACE_NAME_BYTES_MAX);
	thread->trace = trace;
	thread->ring = trace->config.events + (size_t)index * trace->config.events_per_thread;
	thread->capacity = trace->config.events_per_thread;
	thread->index = index;
	thread->depth = 0;
	atomic_store_explicit(&thread->write_count, 0, memory_order_relaxed);
	memcpy(thread->name, name, length);
	thread->name[length] = 0;
}

static enum engine_trace_result trace_config_validate(struct engine_trace_config const *config)
{
	uint64_t required;
	uint32_t tag;
	ENGINE_ASSERT(config != NULL);
	if (!config->events || config->threads_max == 0 ||
		config->threads_max > ENGINE_TRACE_THREADS_MAX)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	if (!trace_is_power_of_two(config->events_per_thread) ||
		config->events_per_thread < ENGINE_TRACE_EVENTS_PER_THREAD_MIN ||
		config->events_per_thread > ENGINE_TRACE_EVENTS_PER_THREAD_MAX)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	/* One ring per CPU thread plus the GPU track. */
	required = (uint64_t)(config->threads_max + 1u) * config->events_per_thread;
	ENGINE_ASSERT(required > config->events_per_thread);
	if (required > config->events_capacity)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
		if (config->memory_budgets_bytes[tag] < 0)
			return ENGINE_TRACE_INVALID_ARGUMENT;
	return ENGINE_TRACE_OK;
}

enum engine_trace_result engine_trace_init(struct engine_trace *trace,
	struct engine_trace_config const *config)
{
	enum engine_trace_result result;
	uint32_t tag;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(config != NULL);
	result = trace_config_validate(config);
	if (result != ENGINE_TRACE_OK)
		return result;
	memset(trace, 0, sizeof(*trace));
	trace->config = *config;
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
		trace->memory[tag].bytes_budget = config->memory_budgets_bytes[tag];
	trace_thread_bind(trace, config->threads_max, "GPU");
	trace->origin_nanoseconds = trace_clock_nanoseconds();
	return ENGINE_TRACE_OK;
}

uint64_t engine_trace_now(struct engine_trace const *trace)
{
	uint64_t const now = trace_clock_nanoseconds();
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(now >= trace->origin_nanoseconds);
	return now - trace->origin_nanoseconds;
}

char const *engine_trace_memory_tag_name(enum engine_trace_memory_tag tag)
{
	ENGINE_ASSERT((uint32_t)tag < ENGINE_TRACE_MEMORY_TAG_COUNT);
	ENGINE_ASSERT(trace_memory_tag_names[tag] != NULL);
	return trace_memory_tag_names[tag];
}

enum engine_trace_result engine_trace_thread_register(struct engine_trace *trace,
	char const *name, struct engine_trace_thread **thread)
{
	uint32_t index;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(thread != NULL);
	*thread = NULL;
	if (!name)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	if (trace_name_length(name, ENGINE_TRACE_NAME_BYTES_MAX) >= ENGINE_TRACE_NAME_BYTES_MAX)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	index = atomic_load_explicit(&trace->thread_count, memory_order_relaxed);
	do
	{
		if (index >= trace->config.threads_max)
			return ENGINE_TRACE_THREADS_EXHAUSTED;
	} while (!atomic_compare_exchange_weak_explicit(&trace->thread_count, &index, index + 1,
		memory_order_acq_rel, memory_order_relaxed));
	trace_thread_bind(trace, index, name);
	*thread = &trace->threads[index];
	trace_thread_current = *thread;
	return ENGINE_TRACE_OK;
}

struct engine_trace_thread *engine_trace_thread_current(void)
{
	return trace_thread_current;
}

void engine_trace_thread_set_current(struct engine_trace_thread *thread)
{
	ENGINE_ASSERT(thread == NULL || thread->trace != NULL);
	ENGINE_ASSERT(thread == NULL || thread->ring != NULL);
	trace_thread_current = thread;
}

/* Single producer per ring: only the owning thread writes, so the slot store
   needs no lock; the release store publishes it to a quiescent collector. */
static void trace_write(struct engine_trace_thread *thread, uint32_t kind,
	void const *payload, uint64_t argument, uint32_t value32, uint64_t timestamp)
{
	uint64_t const index = atomic_load_explicit(&thread->write_count, memory_order_relaxed);
	struct engine_trace_event *event = &thread->ring[index & (thread->capacity - 1u)];
	ENGINE_ASSERT(kind != ENGINE_TRACE_EVENT_NONE);
	ENGINE_ASSERT(trace_is_power_of_two(thread->capacity));
	event->timestamp_nanoseconds = timestamp;
	event->argument = argument;
	event->payload = payload;
	event->kind = kind;
	event->value32 = value32;
	atomic_store_explicit(&thread->write_count, index + 1u, memory_order_release);
}

static void trace_push(struct engine_trace_thread *thread, uint32_t kind,
	void const *payload, uint64_t argument)
{
	ENGINE_ASSERT(thread != NULL);
	ENGINE_ASSERT(payload != NULL);
	ENGINE_ASSERT(thread->depth < ENGINE_TRACE_ZONE_DEPTH_MAX);
	thread->stack[thread->depth] = payload;
	thread->stack_kind[thread->depth] = (uint8_t)kind;
	thread->depth += 1;
	trace_write(thread, kind, payload, argument, 0, engine_trace_now(thread->trace));
}

static void trace_pop(struct engine_trace_thread *thread, uint32_t kind, void const *payload)
{
	ENGINE_ASSERT(thread != NULL);
	ENGINE_ASSERT(thread->depth > 0);
	/* Begin and end pair per thread on an explicit stack (ADR 0051). */
	ENGINE_ASSERT(thread->stack[thread->depth - 1] == payload);
	thread->depth -= 1;
	trace_write(thread, kind, payload, 0, 0, engine_trace_now(thread->trace));
}

void engine_trace_zone_begin(struct engine_trace_thread *thread,
	struct engine_trace_location const *location, uint64_t argument)
{
	ENGINE_ASSERT(location != NULL);
	ENGINE_ASSERT(location->name != NULL);
	trace_push(thread, ENGINE_TRACE_EVENT_ZONE_BEGIN, location, argument);
}

void engine_trace_zone_end(struct engine_trace_thread *thread,
	struct engine_trace_location const *location)
{
	ENGINE_ASSERT(location != NULL);
	ENGINE_ASSERT(location->name != NULL);
	trace_pop(thread, ENGINE_TRACE_EVENT_ZONE_END, location);
}

void engine_trace_named_begin(struct engine_trace_thread *thread, char const *name,
	uint64_t argument)
{
	ENGINE_ASSERT(name != NULL);
	ENGINE_ASSERT(name[0] != 0);
	trace_push(thread, ENGINE_TRACE_EVENT_NAMED_BEGIN, name, argument);
}

void engine_trace_named_end(struct engine_trace_thread *thread, char const *name)
{
	ENGINE_ASSERT(name != NULL);
	ENGINE_ASSERT(name[0] != 0);
	trace_pop(thread, ENGINE_TRACE_EVENT_NAMED_END, name);
}

static void trace_marker(struct engine_trace_thread *thread, uint32_t kind, uint64_t value)
{
	ENGINE_ASSERT(thread != NULL);
	ENGINE_ASSERT(kind >= ENGINE_TRACE_EVENT_TICK_BEGIN && kind <= ENGINE_TRACE_EVENT_FRAME_END);
	trace_write(thread, kind, NULL, value, 0, engine_trace_now(thread->trace));
}

void engine_trace_tick_begin(struct engine_trace_thread *thread, uint64_t tick)
{
	trace_marker(thread, ENGINE_TRACE_EVENT_TICK_BEGIN, tick);
}

void engine_trace_tick_end(struct engine_trace_thread *thread, uint64_t tick)
{
	trace_marker(thread, ENGINE_TRACE_EVENT_TICK_END, tick);
}

void engine_trace_frame_begin(struct engine_trace_thread *thread, uint64_t frame)
{
	trace_marker(thread, ENGINE_TRACE_EVENT_FRAME_BEGIN, frame);
}

void engine_trace_frame_end(struct engine_trace_thread *thread, uint64_t frame)
{
	trace_marker(thread, ENGINE_TRACE_EVENT_FRAME_END, frame);
}

enum engine_trace_result engine_trace_counter_register(struct engine_trace *trace,
	char const *name, uint32_t *counter)
{
	uint32_t index;
	uint32_t length;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(counter != NULL);
	if (!name || name[0] == 0)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	length = trace_name_length(name, ENGINE_TRACE_NAME_BYTES_MAX);
	if (length >= ENGINE_TRACE_NAME_BYTES_MAX)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	index = atomic_load_explicit(&trace->counter_count, memory_order_relaxed);
	do
	{
		if (index >= ENGINE_TRACE_COUNTERS_MAX)
			return ENGINE_TRACE_COUNTERS_EXHAUSTED;
	} while (!atomic_compare_exchange_weak_explicit(&trace->counter_count, &index, index + 1,
		memory_order_acq_rel, memory_order_relaxed));
	memcpy(trace->counters[index].name, name, length);
	trace->counters[index].name[length] = 0;
	atomic_store_explicit(&trace->counters[index].value, 0, memory_order_relaxed);
	*counter = index;
	return ENGINE_TRACE_OK;
}

void engine_trace_counter_set(struct engine_trace *trace, uint32_t counter, int64_t value)
{
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(counter < atomic_load_explicit(&trace->counter_count, memory_order_relaxed));
	atomic_store_explicit(&trace->counters[counter].value, value, memory_order_relaxed);
}

void engine_trace_counter_add(struct engine_trace *trace, uint32_t counter, int64_t delta)
{
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(counter < atomic_load_explicit(&trace->counter_count, memory_order_relaxed));
	atomic_fetch_add_explicit(&trace->counters[counter].value, delta, memory_order_relaxed);
}

int64_t engine_trace_counter_get(struct engine_trace const *trace, uint32_t counter)
{
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(counter < atomic_load_explicit(&trace->counter_count, memory_order_relaxed));
	return atomic_load_explicit(&trace->counters[counter].value, memory_order_relaxed);
}

void engine_trace_counters_sample(struct engine_trace_thread *thread)
{
	struct engine_trace *trace;
	uint32_t count;
	uint32_t index;
	uint64_t now;
	ENGINE_ASSERT(thread != NULL);
	ENGINE_ASSERT(thread->trace != NULL);
	trace = thread->trace;
	count = atomic_load_explicit(&trace->counter_count, memory_order_acquire);
	now = engine_trace_now(trace);
	for (index = 0; index < count; ++index)
	{
		int64_t const value = atomic_load_explicit(&trace->counters[index].value,
			memory_order_relaxed);
		trace_write(thread, ENGINE_TRACE_EVENT_COUNTER, trace->counters[index].name,
			(uint64_t)value, index, now);
	}
}

static void trace_memory_peak_raise(struct engine_trace_memory_tag_state *state, int64_t current)
{
	int64_t peak = atomic_load_explicit(&state->bytes_peak, memory_order_relaxed);
	ENGINE_ASSERT(current >= 0);
	while (current > peak)
	{
		if (atomic_compare_exchange_weak_explicit(&state->bytes_peak, &peak, current,
			memory_order_relaxed, memory_order_relaxed))
			break;
	}
	ENGINE_ASSERT(atomic_load_explicit(&state->bytes_peak, memory_order_relaxed) >= current);
}

enum engine_trace_result engine_trace_memory_reserve(struct engine_trace *trace,
	enum engine_trace_memory_tag tag, int64_t bytes)
{
	struct engine_trace_memory_tag_state *state;
	int64_t current;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT((uint32_t)tag < ENGINE_TRACE_MEMORY_TAG_COUNT);
	ENGINE_ASSERT(bytes > 0);
	state = &trace->memory[tag];
	current = atomic_fetch_add_explicit(&state->bytes_current, bytes, memory_order_relaxed);
	ENGINE_ASSERT(current <= INT64_MAX - bytes);
	current += bytes;
	atomic_fetch_add_explicit(&state->allocation_count, 1, memory_order_relaxed);
	trace_memory_peak_raise(state, current);
	if (state->bytes_budget == 0 || current <= state->bytes_budget)
		return ENGINE_TRACE_OK;
	atomic_fetch_add_explicit(&state->budget_violation_count, 1, memory_order_relaxed);
	/* A test build may decide that a budget overrun is a programmer error. */
	ENGINE_ASSERT(!trace->config.budget_violation_fatal);
	return ENGINE_TRACE_BUDGET_EXCEEDED;
}

void engine_trace_memory_release(struct engine_trace *trace,
	enum engine_trace_memory_tag tag, int64_t bytes)
{
	struct engine_trace_memory_tag_state *state;
	int64_t previous;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT((uint32_t)tag < ENGINE_TRACE_MEMORY_TAG_COUNT);
	ENGINE_ASSERT(bytes > 0);
	state = &trace->memory[tag];
	previous = atomic_fetch_sub_explicit(&state->bytes_current, bytes, memory_order_relaxed);
	/* Per-tag byte counters never go negative: releases pair with reserves. */
	ENGINE_ASSERT(previous >= bytes);
	atomic_fetch_add_explicit(&state->release_count, 1, memory_order_relaxed);
	ENGINE_ASSERT(atomic_load_explicit(&state->release_count, memory_order_relaxed) <=
		atomic_load_explicit(&state->allocation_count, memory_order_relaxed));
}

uint32_t engine_trace_memory_budget_violations(struct engine_trace const *trace)
{
	uint32_t total = 0;
	uint32_t tag;
	ENGINE_ASSERT(trace != NULL);
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
		total += atomic_load_explicit(&trace->memory[tag].budget_violation_count,
			memory_order_relaxed);
	ENGINE_ASSERT(total < UINT32_MAX);
	return total;
}

struct engine_trace_thread *engine_trace_gpu_track(struct engine_trace *trace)
{
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(trace->threads[trace->config.threads_max].ring != NULL);
	return &trace->threads[trace->config.threads_max];
}

enum engine_trace_result engine_trace_gpu_frame_begin(struct engine_trace *trace,
	uint64_t frame, uint32_t *slot)
{
	uint32_t index;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(slot != NULL);
	for (index = 0; index < ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX; ++index)
	{
		struct engine_trace_gpu_frame *gpu = &trace->gpu_frames[index];
		if (gpu->in_use)
			continue;
		gpu->in_use = 1;
		gpu->frame = frame;
		gpu->query_count = 0;
		gpu->zone_count = 0;
		gpu->depth = 0;
		*slot = index;
		return ENGINE_TRACE_OK;
	}
	return ENGINE_TRACE_GPU_FRAMES_EXHAUSTED;
}

static struct engine_trace_gpu_frame *trace_gpu_frame(struct engine_trace *trace, uint32_t slot)
{
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(slot < ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX);
	ENGINE_ASSERT(trace->gpu_frames[slot].in_use);
	return &trace->gpu_frames[slot];
}

enum engine_trace_result engine_trace_gpu_zone_begin(struct engine_trace *trace, uint32_t slot,
	struct engine_trace_location const *location, uint32_t *query)
{
	struct engine_trace_gpu_frame *gpu = trace_gpu_frame(trace, slot);
	struct engine_trace_gpu_zone *zone;
	ENGINE_ASSERT(location != NULL);
	ENGINE_ASSERT(query != NULL);
	if (gpu->zone_count >= ENGINE_TRACE_GPU_ZONES_PER_FRAME_MAX ||
		gpu->depth >= ENGINE_TRACE_ZONE_DEPTH_MAX)
		return ENGINE_TRACE_GPU_QUERIES_EXHAUSTED;
	ENGINE_ASSERT(gpu->query_count + 2u <= ENGINE_TRACE_GPU_QUERIES_PER_FRAME_MAX);
	/* Reserve the end query now so a begun zone can always be closed. */
	zone = &gpu->zones[gpu->zone_count];
	zone->location = location;
	zone->query_begin = gpu->query_count;
	zone->query_end = gpu->query_count + 1u;
	gpu->query_count += 2u;
	gpu->stack[gpu->depth] = gpu->zone_count;
	gpu->depth += 1;
	gpu->zone_count += 1;
	*query = zone->query_begin;
	return ENGINE_TRACE_OK;
}

enum engine_trace_result engine_trace_gpu_zone_end(struct engine_trace *trace, uint32_t slot,
	uint32_t *query)
{
	struct engine_trace_gpu_frame *gpu = trace_gpu_frame(trace, slot);
	ENGINE_ASSERT(query != NULL);
	ENGINE_ASSERT(gpu->depth > 0);
	gpu->depth -= 1;
	*query = gpu->zones[gpu->stack[gpu->depth]].query_end;
	ENGINE_ASSERT(*query < gpu->query_count);
	return ENGINE_TRACE_OK;
}

static enum engine_trace_result trace_gpu_validate(struct engine_trace_gpu_frame const *gpu,
	struct engine_trace_gpu_resolve const *resolve)
{
	uint32_t index;
	ENGINE_ASSERT(gpu->depth == 0);
	ENGINE_ASSERT(resolve != NULL);
	if (!resolve->timestamps || resolve->timestamp_count < gpu->query_count ||
		resolve->nanoseconds_per_tick_numerator == 0 ||
		resolve->nanoseconds_per_tick_denominator == 0)
		return ENGINE_TRACE_MALFORMED;
	for (index = 0; index < gpu->zone_count; ++index)
	{
		uint64_t const begin = resolve->timestamps[gpu->zones[index].query_begin];
		uint64_t const end = resolve->timestamps[gpu->zones[index].query_end];
		if (begin < resolve->gpu_origin_ticks || end < begin)
			return ENGINE_TRACE_MALFORMED;
		/* Bound the conversion so the scaled product cannot overflow. */
		if (end - resolve->gpu_origin_ticks > UINT64_MAX /
			resolve->nanoseconds_per_tick_numerator)
			return ENGINE_TRACE_MALFORMED;
	}
	return ENGINE_TRACE_OK;
}

static uint64_t trace_gpu_nanoseconds(struct engine_trace_gpu_resolve const *resolve,
	uint64_t ticks)
{
	ENGINE_ASSERT(ticks >= resolve->gpu_origin_ticks);
	ENGINE_ASSERT(resolve->nanoseconds_per_tick_denominator != 0);
	return resolve->cpu_origin_nanoseconds + (ticks - resolve->gpu_origin_ticks) *
		resolve->nanoseconds_per_tick_numerator / resolve->nanoseconds_per_tick_denominator;
}

enum engine_trace_result engine_trace_gpu_frame_resolve(struct engine_trace *trace,
	uint32_t slot, struct engine_trace_gpu_resolve const *resolve)
{
	struct engine_trace_gpu_frame *gpu = trace_gpu_frame(trace, slot);
	struct engine_trace_thread *track = engine_trace_gpu_track(trace);
	enum engine_trace_result result = ENGINE_TRACE_OK;
	uint32_t index;
	ENGINE_ASSERT(resolve != NULL);
	if (!resolve->supported)
	{
		/* Never fabricate timings for a frame the backend could not measure. */
		trace->gpu_unsupported_frame_count += 1;
		gpu->in_use = 0;
		return ENGINE_TRACE_GPU_UNSUPPORTED;
	}
	result = trace_gpu_validate(gpu, resolve);
	for (index = 0; result == ENGINE_TRACE_OK && index < gpu->zone_count; ++index)
	{
		struct engine_trace_gpu_zone const *zone = &gpu->zones[index];
		uint64_t const begin = trace_gpu_nanoseconds(resolve,
			resolve->timestamps[zone->query_begin]);
		uint64_t const end = trace_gpu_nanoseconds(resolve, resolve->timestamps[zone->query_end]);
		ENGINE_ASSERT(end >= begin);
		trace_write(track, ENGINE_TRACE_EVENT_GPU_ZONE, zone->location, end - begin,
			(uint32_t)gpu->frame, begin);
	}
	ENGINE_ASSERT(result != ENGINE_TRACE_OK || index == gpu->zone_count);
	gpu->in_use = 0;
	return result;
}
