/* ADR 0051 implementation gates for the shared instrumentation stream. */
#define _POSIX_C_SOURCE 200809L

#include "../engine/core/trace/trace.h"
#include "../engine/core/trace/trace_capture.h"
#include "allocation_guard.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)

#define THREADS 8u
#define EVENTS_PER_THREAD 4096u
#define LOCATIONS 256u

static struct engine_trace trace;
static struct engine_trace_event events[(THREADS + 1u) * EVENTS_PER_THREAD];
static struct engine_trace_capture_event capture_events[(THREADS + 1u) * EVENTS_PER_THREAD];
static struct engine_trace_capture_location capture_locations[LOCATIONS];
static struct engine_trace_capture_slot capture_slots[2u * LOCATIONS];
static struct engine_trace_capture capture;
static struct engine_trace_capture_event loaded_events[(THREADS + 1u) * EVENTS_PER_THREAD];
static struct engine_trace_capture_location loaded_locations[LOCATIONS];
static struct engine_trace_capture_slot loaded_slots[2u * LOCATIONS];
static struct engine_trace_capture loaded;
static unsigned char serialized[4u << 20];
static unsigned char mutated[4u << 20];
static char text_storage[8u << 20];

static struct engine_trace_location const zone_frame = { "frame", "test_engine_trace.c", 10 };
static struct engine_trace_location const zone_physics = { "physics", "test_engine_trace.c", 11 };
static struct engine_trace_location const zone_quote = { "needs \"escaping\"\n", "x.c", 12 };
static struct engine_trace_location const gpu_shadows = { "shadow_pass", "renderer.c", 1 };
static struct engine_trace_location const gpu_opaque = { "opaque_pass", "renderer.c", 2 };

static void trace_start(uint32_t threads_max, int fatal_budgets)
{
	struct engine_trace_config config;
	memset(&config, 0, sizeof(config));
	config.events = events;
	config.events_capacity = (threads_max + 1u) * EVENTS_PER_THREAD;
	config.threads_max = threads_max;
	config.events_per_thread = EVENTS_PER_THREAD;
	config.memory_budgets_bytes[ENGINE_TRACE_MEMORY_PHYSICS] = 1000;
	config.budget_violation_fatal = fatal_budgets;
	CHECK(engine_trace_init(&trace, &config) == ENGINE_TRACE_OK);
}

static void capture_start(struct engine_trace_capture *target,
	struct engine_trace_capture_event *target_events,
	struct engine_trace_capture_location *locations, struct engine_trace_capture_slot *slots)
{
	CHECK(engine_trace_capture_init(target, target_events, (THREADS + 1u) * EVENTS_PER_THREAD,
		locations, LOCATIONS, slots, 2u * LOCATIONS) == ENGINE_TRACE_OK);
}

static int child_aborts(void (*body)(void))
{
	pid_t const child = fork();
	int status = 0;
	CHECK(child >= 0);
	if (child == 0)
	{
		if (!freopen("/dev/null", "w", stderr))
			_exit(2);
		body();
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child);
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void test_configuration(void)
{
	struct engine_trace_config config;
	struct engine_trace_thread *thread;
	struct engine_trace_thread *third;
	memset(&config, 0, sizeof(config));
	config.events = events;
	config.events_capacity = 3u * EVENTS_PER_THREAD;
	config.threads_max = 2;
	config.events_per_thread = 1000;  /* Not a power of two. */
	CHECK(engine_trace_init(&trace, &config) == ENGINE_TRACE_INVALID_ARGUMENT);
	config.events_per_thread = EVENTS_PER_THREAD;
	config.events_capacity = 2u * EVENTS_PER_THREAD;  /* No room for the GPU track. */
	CHECK(engine_trace_init(&trace, &config) == ENGINE_TRACE_INVALID_ARGUMENT);
	config.events_capacity = 3u * EVENTS_PER_THREAD;
	config.memory_budgets_bytes[0] = -1;
	CHECK(engine_trace_init(&trace, &config) == ENGINE_TRACE_INVALID_ARGUMENT);
	config.memory_budgets_bytes[0] = 0;
	CHECK(engine_trace_init(&trace, &config) == ENGINE_TRACE_OK);
	CHECK(engine_trace_thread_register(&trace, "a", &thread) == ENGINE_TRACE_OK);
	CHECK(engine_trace_thread_current() == thread);
	CHECK(engine_trace_thread_register(&trace, "b", &thread) == ENGINE_TRACE_OK);
	CHECK(engine_trace_thread_register(&trace, "c", &third) == ENGINE_TRACE_THREADS_EXHAUSTED);
	CHECK(third == NULL);
	CHECK(engine_trace_thread_register(&trace,
		"a name that is far too long to fit in the sixty-four byte name field", &third) ==
		ENGINE_TRACE_INVALID_ARGUMENT);
}

static void test_zones_counters_and_markers(void)
{
	struct engine_trace_thread *thread;
	struct engine_trace_summary_zone scratch[LOCATIONS];
	struct engine_trace_summary summary;
	uint32_t entities;
	uint32_t draws;
	uint32_t tick;
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	CHECK(engine_trace_counter_register(&trace, "visible_entities", &entities) == ENGINE_TRACE_OK);
	CHECK(engine_trace_counter_register(&trace, "draw_calls", &draws) == ENGINE_TRACE_OK);
	allocation_guard_arm();
	for (tick = 0; tick < 10; ++tick)
	{
		engine_trace_tick_begin(thread, tick);
		engine_trace_zone_begin(thread, &zone_frame, tick);
		engine_trace_zone_begin(thread, &zone_physics, 0);
		engine_trace_zone_end(thread, &zone_physics);
		engine_trace_named_begin(thread, "named_job", 3);
		engine_trace_named_end(thread, "named_job");
		engine_trace_zone_end(thread, &zone_frame);
		engine_trace_counter_set(&trace, entities, 100 + tick);
		engine_trace_counter_add(&trace, draws, 2);
		engine_trace_counters_sample(thread);
		engine_trace_tick_end(thread, tick);
	}
	CHECK(allocation_guard_disarm() == 0);
	CHECK(engine_trace_counter_get(&trace, entities) == 109);
	CHECK(engine_trace_counter_get(&trace, draws) == 20);
	CHECK(thread->depth == 0);
	capture_start(&capture, capture_events, capture_locations, capture_slots);
	CHECK(engine_trace_capture_collect(&trace, &capture) == ENGINE_TRACE_OK);
	CHECK(capture.track_count == 2);  /* main and the GPU track */
	CHECK(capture.tracks[0].event_count == 10u * 10u);
	CHECK(capture.tracks[0].lost_events == 0);
	CHECK(capture.location_count == 5);
	CHECK(engine_trace_summary_build(&capture, scratch, LOCATIONS, &summary) == ENGINE_TRACE_OK);
	CHECK(summary.ticks_completed == 10 && summary.unmatched_events == 0);
	CHECK(summary.cpu_zone_count == 3);
	/* The enclosing frame zone has the largest inclusive total. */
	CHECK(strcmp(capture.locations[summary.cpu_zones[0].location].name, "frame") == 0);
	CHECK(summary.cpu_zones[0].count == 10);
	CHECK(summary.cpu_zones[0].total_nanoseconds >= summary.cpu_zones[1].total_nanoseconds);
}

static void mismatched_end(void)
{
	struct engine_trace_thread *thread;
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	engine_trace_zone_begin(thread, &zone_frame, 0);
	engine_trace_zone_end(thread, &zone_physics);
}

static void unopened_end(void)
{
	struct engine_trace_thread *thread;
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	engine_trace_zone_end(thread, &zone_frame);
}

static void test_zone_pairing(void)
{
	CHECK(child_aborts(mismatched_end));
	CHECK(child_aborts(unopened_end));
}

static void test_ring_overflow(void)
{
	struct engine_trace_thread *thread;
	struct engine_trace_summary_zone scratch[LOCATIONS];
	struct engine_trace_summary summary;
	uint32_t index;
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	/* An open zone whose begin will be overwritten. */
	engine_trace_zone_begin(thread, &zone_frame, 0);
	for (index = 0; index < EVENTS_PER_THREAD; ++index)
	{
		engine_trace_zone_begin(thread, &zone_physics, index);
		engine_trace_zone_end(thread, &zone_physics);
	}
	engine_trace_zone_end(thread, &zone_frame);
	/* A trailing marker aligns the surviving window on a zone begin. */
	engine_trace_tick_begin(thread, 0);
	capture_start(&capture, capture_events, capture_locations, capture_slots);
	CHECK(engine_trace_capture_collect(&trace, &capture) == ENGINE_TRACE_OK);
	/* A full ring overwrites the oldest events and counts the loss. */
	CHECK(capture.tracks[0].event_count == EVENTS_PER_THREAD);
	CHECK(capture.tracks[0].lost_events == EVENTS_PER_THREAD + 3u);
	CHECK(engine_trace_summary_build(&capture, scratch, LOCATIONS, &summary) == ENGINE_TRACE_OK);
	CHECK(summary.lost_events == EVENTS_PER_THREAD + 3u);
	CHECK(summary.unmatched_events == 1);  /* The frame end without its begin. */
}

static void test_capture_capacity(void)
{
	struct engine_trace_thread *thread;
	struct engine_trace_capture small;
	struct engine_trace_capture_event small_events[4];
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	engine_trace_zone_begin(thread, &zone_frame, 0);
	engine_trace_zone_begin(thread, &zone_physics, 0);
	engine_trace_zone_end(thread, &zone_physics);
	engine_trace_zone_end(thread, &zone_frame);
	engine_trace_tick_begin(thread, 0);
	CHECK(engine_trace_capture_init(&small, small_events, 4, capture_locations, LOCATIONS,
		capture_slots, 2u * LOCATIONS) == ENGINE_TRACE_OK);
	CHECK(engine_trace_capture_collect(&trace, &small) == ENGINE_TRACE_CAPACITY_EXCEEDED);
	CHECK(engine_trace_capture_init(&small, small_events, 4, capture_locations, LOCATIONS,
		capture_slots, LOCATIONS) == ENGINE_TRACE_INVALID_ARGUMENT);
}

/* ---- Memory tags --------------------------------------------------------- */

static void test_memory_tags(void)
{
	trace_start(THREADS, 0);
	CHECK(engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_ECS, 4096) == ENGINE_TRACE_OK);
	CHECK(engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 600) == ENGINE_TRACE_OK);
	/* Exceeding a budget is reported and still recorded. */
	CHECK(engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 600) ==
		ENGINE_TRACE_BUDGET_EXCEEDED);
	CHECK(engine_trace_memory_budget_violations(&trace) == 1);
	engine_trace_memory_release(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 600);
	engine_trace_memory_release(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 600);
	CHECK(atomic_load(&trace.memory[ENGINE_TRACE_MEMORY_PHYSICS].bytes_current) == 0);
	CHECK(atomic_load(&trace.memory[ENGINE_TRACE_MEMORY_PHYSICS].bytes_peak) == 1200);
	CHECK(atomic_load(&trace.memory[ENGINE_TRACE_MEMORY_ECS].bytes_current) == 4096);
	CHECK(strcmp(engine_trace_memory_tag_name(ENGINE_TRACE_MEMORY_RENDER_GPU), "render_gpu") == 0);
}

static void negative_bytes(void)
{
	trace_start(THREADS, 0);
	CHECK(engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_UI, 10) == ENGINE_TRACE_OK);
	engine_trace_memory_release(&trace, ENGINE_TRACE_MEMORY_UI, 11);
}

static void fatal_budget(void)
{
	trace_start(THREADS, 1);
	(void)engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 2000);
}

static void test_memory_assertions(void)
{
	CHECK(child_aborts(negative_bytes));
	CHECK(child_aborts(fatal_budget));
}

/* ---- GPU timing ----------------------------------------------------------- */

static void test_gpu_timing(void)
{
	struct engine_trace_gpu_resolve resolve;
	struct engine_trace_summary_zone scratch[LOCATIONS];
	struct engine_trace_summary summary;
	uint64_t timestamps[ENGINE_TRACE_GPU_QUERIES_PER_FRAME_MAX];
	uint32_t slots[ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX];
	uint32_t extra;
	uint32_t queries[4];
	uint32_t index;
	trace_start(THREADS, 0);
	for (index = 0; index < ENGINE_TRACE_GPU_FRAMES_IN_FLIGHT_MAX; ++index)
		CHECK(engine_trace_gpu_frame_begin(&trace, 100u + index, &slots[index]) ==
			ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_frame_begin(&trace, 200, &extra) == ENGINE_TRACE_GPU_FRAMES_EXHAUSTED);
	/* Frame 100: a shadow pass containing an opaque pass. */
	CHECK(engine_trace_gpu_zone_begin(&trace, slots[0], &gpu_shadows, &queries[0]) ==
		ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_begin(&trace, slots[0], &gpu_opaque, &queries[1]) ==
		ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_end(&trace, slots[0], &queries[2]) == ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_end(&trace, slots[0], &queries[3]) == ENGINE_TRACE_OK);
	CHECK(queries[0] == 0 && queries[1] == 2 && queries[2] == 3 && queries[3] == 1);
	/* Frame 101 ran out of queries mid-frame: the overflow is reported. */
	for (index = 0; index < ENGINE_TRACE_GPU_ZONES_PER_FRAME_MAX; ++index)
	{
		uint32_t query;
		CHECK(engine_trace_gpu_zone_begin(&trace, slots[1], &gpu_opaque, &query) ==
			ENGINE_TRACE_OK);
		CHECK(engine_trace_gpu_zone_end(&trace, slots[1], &query) == ENGINE_TRACE_OK);
	}
	CHECK(engine_trace_gpu_zone_begin(&trace, slots[1], &gpu_opaque, &extra) ==
		ENGINE_TRACE_GPU_QUERIES_EXHAUSTED);
	/* Frame 102's backend could not time it: nothing is fabricated. */
	memset(&resolve, 0, sizeof(resolve));
	CHECK(engine_trace_gpu_frame_resolve(&trace, slots[2], &resolve) == ENGINE_TRACE_GPU_UNSUPPORTED);
	/* Frame 100 resolves after later frames began (delayed completion). */
	timestamps[0] = 1000;
	timestamps[1] = 5000;
	timestamps[2] = 2000;
	timestamps[3] = 4000;
	resolve.timestamps = timestamps;
	resolve.timestamp_count = 4;
	resolve.supported = 1;
	resolve.nanoseconds_per_tick_numerator = 1;
	resolve.nanoseconds_per_tick_denominator = 1;
	resolve.gpu_origin_ticks = 1000;
	resolve.cpu_origin_nanoseconds = 50000;
	CHECK(engine_trace_gpu_frame_resolve(&trace, slots[0], &resolve) == ENGINE_TRACE_OK);
	/* Frame 103: a timestamp before the origin is malformed, not trusted. */
	CHECK(engine_trace_gpu_zone_begin(&trace, slots[3], &gpu_opaque, &queries[0]) ==
		ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_end(&trace, slots[3], &queries[1]) == ENGINE_TRACE_OK);
	timestamps[0] = 10;
	CHECK(engine_trace_gpu_frame_resolve(&trace, slots[3], &resolve) == ENGINE_TRACE_MALFORMED);
	CHECK(engine_trace_gpu_frame_begin(&trace, 300, &extra) == ENGINE_TRACE_OK);
	capture_start(&capture, capture_events, capture_locations, capture_slots);
	CHECK(engine_trace_capture_collect(&trace, &capture) == ENGINE_TRACE_OK);
	CHECK(capture.track_count == 1);  /* Only the GPU track. */
	CHECK(capture.tracks[0].event_count == 2);
	CHECK(capture.events[0].kind == ENGINE_TRACE_EVENT_GPU_ZONE);
	CHECK(capture.events[0].timestamp_nanoseconds == 50000 && capture.events[0].argument == 4000);
	CHECK(capture.events[1].timestamp_nanoseconds == 51000 && capture.events[1].argument == 2000);
	CHECK(capture.events[0].value32 == 100 && capture.events[1].value32 == 100);
	CHECK(capture.gpu_unsupported_frame_count == 1);
	CHECK(engine_trace_summary_build(&capture, scratch, LOCATIONS, &summary) == ENGINE_TRACE_OK);
	CHECK(summary.gpu_zone_count == 2 && summary.gpu_zones[0].total_nanoseconds == 4000);
}

/* ---- Contention ---------------------------------------------------------- */

#define CONTENTION_ZONES 50000u

static void *contention_thread(void *argument)
{
	struct engine_trace_thread *thread;
	char name[32];
	uint32_t index;
	snprintf(name, sizeof(name), "worker_%u", (unsigned)(uintptr_t)argument);
	CHECK(engine_trace_thread_register(&trace, name, &thread) == ENGINE_TRACE_OK);
	for (index = 0; index < CONTENTION_ZONES; ++index)
	{
		engine_trace_zone_begin(thread, &zone_physics, index);
		engine_trace_named_begin(thread, "inner", index);
		engine_trace_named_end(thread, "inner");
		engine_trace_zone_end(thread, &zone_physics);
		engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_JOBS, 64);
		engine_trace_memory_release(&trace, ENGINE_TRACE_MEMORY_JOBS, 64);
	}
	return NULL;
}

static void test_contention(void)
{
	pthread_t threads[THREADS];
	struct engine_trace_summary_zone scratch[LOCATIONS];
	struct engine_trace_summary summary;
	uint32_t index;
	trace_start(THREADS, 0);
	for (index = 0; index < THREADS; ++index)
		CHECK(pthread_create(&threads[index], NULL, contention_thread,
			(void *)(uintptr_t)index) == 0);
	for (index = 0; index < THREADS; ++index)
		CHECK(pthread_join(threads[index], NULL) == 0);
	capture_start(&capture, capture_events, capture_locations, capture_slots);
	CHECK(engine_trace_capture_collect(&trace, &capture) == ENGINE_TRACE_OK);
	CHECK(capture.track_count == THREADS + 1u);
	for (index = 0; index < THREADS; ++index)
	{
		CHECK(capture.tracks[index].event_count == EVENTS_PER_THREAD);
		CHECK(capture.tracks[index].lost_events == 4u * CONTENTION_ZONES - EVENTS_PER_THREAD);
	}
	CHECK(atomic_load(&trace.memory[ENGINE_TRACE_MEMORY_JOBS].bytes_current) == 0);
	CHECK(atomic_load(&trace.memory[ENGINE_TRACE_MEMORY_JOBS].allocation_count) ==
		(uint64_t)THREADS * CONTENTION_ZONES);
	CHECK(engine_trace_summary_build(&capture, scratch, LOCATIONS, &summary) == ENGINE_TRACE_OK);
	/* Each ring starts on a pair boundary, so every surviving zone matches. */
	CHECK(summary.unmatched_events == 0);
	CHECK(summary.cpu_zones[0].count == (uint64_t)THREADS * EVENTS_PER_THREAD / 4u);
}

/* ---- Serialization, export, and crash context --------------------------- */

static void record_mixed_capture(void)
{
	struct engine_trace_thread *thread;
	struct engine_trace_gpu_resolve resolve;
	uint64_t timestamps[2] = { 10, 30 };
	uint32_t counter;
	uint32_t slot;
	uint32_t query;
	trace_start(THREADS, 0);
	CHECK(engine_trace_thread_register(&trace, "main", &thread) == ENGINE_TRACE_OK);
	CHECK(engine_trace_counter_register(&trace, "resimulated_ticks", &counter) == ENGINE_TRACE_OK);
	CHECK(engine_trace_memory_reserve(&trace, ENGINE_TRACE_MEMORY_PHYSICS, 2000) ==
		ENGINE_TRACE_BUDGET_EXCEEDED);
	engine_trace_frame_begin(thread, 1);
	engine_trace_tick_begin(thread, 7);
	engine_trace_zone_begin(thread, &zone_frame, 1);
	engine_trace_zone_begin(thread, &zone_quote, 2);
	engine_trace_zone_end(thread, &zone_quote);
	engine_trace_zone_end(thread, &zone_frame);
	engine_trace_counter_set(&trace, counter, -3);
	engine_trace_counters_sample(thread);
	engine_trace_tick_end(thread, 7);
	engine_trace_frame_end(thread, 1);
	CHECK(engine_trace_gpu_frame_begin(&trace, 1, &slot) == ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_begin(&trace, slot, &gpu_shadows, &query) == ENGINE_TRACE_OK);
	CHECK(engine_trace_gpu_zone_end(&trace, slot, &query) == ENGINE_TRACE_OK);
	memset(&resolve, 0, sizeof(resolve));
	resolve.timestamps = timestamps;
	resolve.timestamp_count = 2;
	resolve.supported = 1;
	resolve.nanoseconds_per_tick_numerator = 125;
	resolve.nanoseconds_per_tick_denominator = 3;
	CHECK(engine_trace_gpu_frame_resolve(&trace, slot, &resolve) == ENGINE_TRACE_OK);
	/* Leave one zone open for the crash context. */
	engine_trace_zone_begin(thread, &zone_physics, 9);
	capture_start(&capture, capture_events, capture_locations, capture_slots);
	CHECK(engine_trace_capture_collect(&trace, &capture) == ENGINE_TRACE_OK);
}

static void check_captures_equal(struct engine_trace_capture const *left,
	struct engine_trace_capture const *right)
{
	uint32_t index;
	CHECK(left->track_count == right->track_count);
	CHECK(left->location_count == right->location_count);
	CHECK(left->event_count == right->event_count);
	CHECK(memcmp(left->tracks, right->tracks, sizeof(left->tracks[0]) * left->track_count) == 0);
	CHECK(memcmp(left->memory, right->memory, sizeof(left->memory)) == 0);
	CHECK(left->gpu_unsupported_frame_count == right->gpu_unsupported_frame_count);
	for (index = 0; index < left->location_count; ++index)
		CHECK(memcmp(&left->locations[index], &right->locations[index],
			sizeof(left->locations[index])) == 0);
	for (index = 0; index < left->event_count; ++index)
		CHECK(memcmp(&left->events[index], &right->events[index], sizeof(left->events[index])) == 0);
}

/* Every parser of external input is fuzzed (ADR 0089). */
static void fuzz_deserialize(size_t size)
{
	uint32_t seed = 0x1234567u;
	uint32_t round;
	uint32_t accepted = 0;
	for (round = 0; round < 4000; ++round)
	{
		size_t length = size;
		uint32_t flips;
		memcpy(mutated, serialized, size);
		seed = seed * 1664525u + 1013904223u;
		for (flips = 1 + seed % 4u; flips > 0; --flips)
		{
			seed = seed * 1664525u + 1013904223u;
			mutated[seed % size] ^= (unsigned char)(1u + (seed >> 24) % 255u);
		}
		if (round % 7 == 0)
			length = seed % size;
		{
			enum engine_trace_result const result =
				engine_trace_capture_deserialize(&loaded, mutated, length);
			CHECK(result == ENGINE_TRACE_OK || result == ENGINE_TRACE_MALFORMED);
			accepted += result == ENGINE_TRACE_OK;
		}
	}
	/* Mutations of payload bytes such as timestamps stay well-formed. */
	CHECK(accepted > 0 && accepted < 4000);
}

static void test_serialization(char const *directory)
{
	char path[512];
	size_t size;
	size_t read_size;
	unsigned char bad[8] = { 0 };
	record_mixed_capture();
	CHECK(engine_trace_capture_serialize(&capture, serialized, sizeof(serialized), &size) ==
		ENGINE_TRACE_OK);
	CHECK(engine_trace_capture_serialize(&capture, serialized, size - 1, &read_size) ==
		ENGINE_TRACE_CAPACITY_EXCEEDED);
	CHECK(engine_trace_capture_serialize(&capture, serialized, sizeof(serialized), &size) ==
		ENGINE_TRACE_OK);
	capture_start(&loaded, loaded_events, loaded_locations, loaded_slots);
	CHECK(engine_trace_capture_deserialize(&loaded, serialized, size) == ENGINE_TRACE_OK);
	check_captures_equal(&capture, &loaded);
	CHECK(engine_trace_capture_deserialize(&loaded, serialized, size - 1) ==
		ENGINE_TRACE_MALFORMED);
	CHECK(loaded.event_count == 0);
	CHECK(engine_trace_capture_deserialize(&loaded, bad, sizeof(bad)) == ENGINE_TRACE_MALFORMED);
	snprintf(path, sizeof(path), "%s/capture.etrace", directory);
	CHECK(engine_trace_file_write(path, serialized, size) == ENGINE_TRACE_OK);
	memset(mutated, 0, size);
	CHECK(engine_trace_file_read(path, mutated, sizeof(mutated), &read_size) == ENGINE_TRACE_OK);
	CHECK(read_size == size && memcmp(mutated, serialized, size) == 0);
	CHECK(engine_trace_file_read(path, mutated, size - 1, &read_size) ==
		ENGINE_TRACE_CAPACITY_EXCEEDED);
	CHECK(engine_trace_file_read("/nonexistent/capture.etrace", mutated, sizeof(mutated),
		&read_size) == ENGINE_TRACE_IO_FAILED);
	CHECK(engine_trace_capture_deserialize(&loaded, mutated, size) == ENGINE_TRACE_OK);
	check_captures_equal(&capture, &loaded);
	fuzz_deserialize(size);
}

static void write_text_file(char const *directory, char const *name,
	struct engine_trace_text const *text)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	CHECK(engine_trace_file_write(path, (unsigned char const *)text->data, text->length) ==
		ENGINE_TRACE_OK);
}

static void test_exports(char const *directory)
{
	struct engine_trace_text text;
	struct engine_trace_summary_zone scratch[LOCATIONS];
	struct engine_trace_summary summary;
	char small[64];
	char path[512];
	size_t size;
	record_mixed_capture();
	engine_trace_text_init(&text, text_storage, sizeof(text_storage));
	CHECK(engine_trace_export_chrome(&capture, &text) == ENGINE_TRACE_OK);
	CHECK(strstr(text.data, "\"traceEvents\"") != NULL);
	CHECK(strstr(text.data, "needs \\\"escaping\\\"\\u000a") != NULL);
	write_text_file(directory, "chrome.json", &text);
	engine_trace_text_init(&text, small, sizeof(small));
	CHECK(engine_trace_export_chrome(&capture, &text) == ENGINE_TRACE_CAPACITY_EXCEEDED);
	CHECK(text.length < sizeof(small) && small[text.length] == 0);
	CHECK(engine_trace_summary_build(&capture, scratch, LOCATIONS, &summary) == ENGINE_TRACE_OK);
	CHECK(summary.frames_completed == 1 && summary.ticks_completed == 1);
	CHECK(summary.frame_sample_count == 1);
	CHECK(summary.frame_p95_nanoseconds == summary.frame_p99_nanoseconds);
	CHECK(summary.counter_count == 1);
	CHECK(summary.counters[0].value == -3);
	CHECK(strcmp(capture.locations[summary.counters[0].location].name, "resimulated_ticks") == 0);
	/* The physics zone is still open when the capture is taken. */
	CHECK(summary.unmatched_events == 1);
	CHECK(summary.memory[ENGINE_TRACE_MEMORY_PHYSICS].budget_violation_count == 1);
	engine_trace_text_init(&text, text_storage, sizeof(text_storage));
	CHECK(engine_trace_summary_write_json(&summary, &capture, &text) == ENGINE_TRACE_OK);
	write_text_file(directory, "summary.json", &text);
	/* The CLI driver re-summarizes this exact capture from disk. */
	CHECK(engine_trace_capture_serialize(&capture, serialized, sizeof(serialized), &size) ==
		ENGINE_TRACE_OK);
	snprintf(path, sizeof(path), "%s/capture.etrace", directory);
	CHECK(engine_trace_file_write(path, serialized, size) == ENGINE_TRACE_OK);
	engine_trace_text_init(&text, text_storage, sizeof(text_storage));
	engine_trace_crash_context_write(&trace, 4, &text);
	CHECK(strstr(text.data, "thread main: open zones > physics") != NULL);
	CHECK(strstr(text.data, "memory physics current 2000") != NULL);
	CHECK(!text.overflowed);
}

int main(int argument_count, char **arguments)
{
	char const *directory = argument_count > 1 ? arguments[1] : ".";
	test_configuration();
	test_zones_counters_and_markers();
	test_zone_pairing();
	test_ring_overflow();
	test_capture_capacity();
	test_memory_tags();
	test_memory_assertions();
	test_gpu_timing();
	test_contention();
	test_serialization(directory);
	test_exports(directory);
	puts("engine trace tests passed");
	return 0;
}
