/* Opt-in CPU frame tracing for the Linux port.

   Set HALO_TRACE_FILE to a writable JSON path. The process writes one summary
   every 300 presented frames, replacing the previous report. */
#include "halo_trace.h"
#include "../../../engine/core/trace/trace.h"
#include "../../../engine/core/trace/trace_capture.h"

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_THREADS_MAX 1u
#define TRACE_EVENTS_PER_THREAD 4096u
#define TRACE_EVENT_STORAGE_COUNT ((TRACE_THREADS_MAX + 1u) * TRACE_EVENTS_PER_THREAD)
#define TRACE_CAPTURE_EVENTS_MAX 4096u
#define TRACE_CAPTURE_LOCATIONS_MAX 32u
#define TRACE_CAPTURE_SLOTS_MAX 64u
#define TRACE_SUMMARY_SCRATCH_MAX TRACE_CAPTURE_LOCATIONS_MAX
#define TRACE_REPORT_BYTES_MAX 16384u
#define TRACE_REPORT_EVERY_FRAMES 300u

enum
{
	TRACE_UNINITIALIZED = 0,
	TRACE_DISABLED,
	TRACE_READY,
	TRACE_FAILED
};

static struct engine_trace trace_state;
static struct engine_trace_thread *trace_main_thread;
static struct engine_trace_event trace_event_storage[TRACE_EVENT_STORAGE_COUNT];
static struct engine_trace_capture trace_capture;
static struct engine_trace_capture_event trace_capture_events[TRACE_CAPTURE_EVENTS_MAX];
static struct engine_trace_capture_location trace_capture_locations[TRACE_CAPTURE_LOCATIONS_MAX];
static struct engine_trace_capture_slot trace_capture_slots[TRACE_CAPTURE_SLOTS_MAX];
static struct engine_trace_summary_zone trace_summary_scratch[TRACE_SUMMARY_SCRATCH_MAX];
static struct engine_trace_summary trace_summary;
static char trace_report[TRACE_REPORT_BYTES_MAX];
static struct engine_trace_text trace_report_text;
static struct engine_trace_location const trace_frame_location = {
	"D3DDevice_Present", "port/linux/src/d3d8_gl.c", 0
};
static struct engine_trace_location const trace_zone_locations[HALO_TRACE_ZONE_COUNT] = {
	{ "bot_ai", "source/features/bots/bots.c", 0 },
	{ "map_precache", "source/game/game.c", 0 },
	{ "scenario_load", "source/game/game.c", 0 },
	{ "texture_upload", "port/linux/src/xbox_textures.c", 0 },
	{ "render_frame", "source/main/main.c", 0 },
	{ "interpolation_prepare", "port/linux/game/render_interpolation.c", 0 }
};
static uint64_t trace_tick_index;
static uint64_t trace_tick_allocations;
static uint64_t trace_texture_upload_max_ns;
static uint64_t trace_zone_started[HALO_TRACE_ZONE_COUNT];
static int trace_zone_started_valid[HALO_TRACE_ZONE_COUNT];
static int trace_tick_active;
static char const *trace_path;
static uint32_t trace_state_status = TRACE_UNINITIALIZED;
static uint32_t trace_counter_ids[HALO_TRACE_COUNTER_COUNT];
static enum engine_trace_result trace_initialization_error = ENGINE_TRACE_OK;
static int trace_startup_message_written;

static int trace_platform_register_counters(void)
{
	static char const *const names[HALO_TRACE_COUNTER_COUNT] = {
		"texture_cache_hits", "texture_cache_misses", "texture_uploads",
		"texture_evictions", "texture_gpu_bytes", "texture_budget_bytes",
		"texture_over_budget", "texture_upload_bytes", "texture_upload_ns",
		"texture_upload_max_ns", "draw_calls",
		"allocations_per_tick", "map_precache_last_ns", "scenario_load_last_ns"
	};
	uint32_t index;

	for (index = 0; index < HALO_TRACE_COUNTER_COUNT; ++index)
	{
		enum engine_trace_result const result = engine_trace_counter_register(&trace_state,
			names[index], &trace_counter_ids[index]);
		if (result != ENGINE_TRACE_OK)
		{
			trace_initialization_error = result;
			return 0;
		}
	}
	return 1;
}

static int trace_platform_initialize(void)
{
	struct engine_trace_config config;
	enum engine_trace_result result;

	if (trace_state_status != TRACE_UNINITIALIZED)
		return trace_state_status == TRACE_READY;

	trace_path = getenv("HALO_TRACE_FILE");
	if (!trace_path || !trace_path[0])
	{
		trace_state_status = TRACE_DISABLED;
		return 0;
	}
	trace_state_status = TRACE_FAILED;

	memset(&config, 0, sizeof(config));
	config.events = trace_event_storage;
	config.events_capacity = TRACE_EVENT_STORAGE_COUNT;
	config.threads_max = TRACE_THREADS_MAX;
	config.events_per_thread = TRACE_EVENTS_PER_THREAD;
	result = engine_trace_init(&trace_state, &config);
	if (result != ENGINE_TRACE_OK)
	{
		trace_initialization_error = result;
		return 0;
	}
	result = engine_trace_thread_register(&trace_state, "main", &trace_main_thread);
	if (result != ENGINE_TRACE_OK)
	{
		trace_initialization_error = result;
		return 0;
	}
	if (!trace_platform_register_counters())
		return 0;
	result = engine_trace_capture_init(&trace_capture, trace_capture_events,
		TRACE_CAPTURE_EVENTS_MAX, trace_capture_locations, TRACE_CAPTURE_LOCATIONS_MAX,
		trace_capture_slots, TRACE_CAPTURE_SLOTS_MAX);
	if (result != ENGINE_TRACE_OK)
	{
		trace_initialization_error = result;
		return 0;
	}
	trace_state_status = TRACE_READY;
	return 1;
}

static void trace_platform_startup(void) __attribute__((constructor(102)));

static void trace_platform_startup(void)
{
	(void)trace_platform_initialize();
}

void halo_trace_frame_begin(uint64_t frame)
{
	if (!trace_platform_initialize())
	{
		if (trace_state_status == TRACE_FAILED && !trace_startup_message_written)
			platform_log("trace: initialization failed (%d)", trace_initialization_error);
		trace_startup_message_written = 1;
		return;
	}
	if (!trace_startup_message_written)
	{
		platform_log("trace: recording frame summaries to %s", trace_path);
		trace_startup_message_written = 1;
	}
	engine_trace_frame_begin(trace_main_thread, frame);
	engine_trace_zone_begin(trace_main_thread, &trace_frame_location, frame);
}

static void trace_platform_write_report(void)
{
	FILE *file;
	enum engine_trace_result result;

	result = engine_trace_capture_collect(&trace_state, &trace_capture);
	if (result != ENGINE_TRACE_OK)
	{
		platform_log("trace: capture failed (%d)", result);
		return;
	}
	result = engine_trace_summary_build(&trace_capture, trace_summary_scratch,
		TRACE_SUMMARY_SCRATCH_MAX, &trace_summary);
	if (result != ENGINE_TRACE_OK)
	{
		platform_log("trace: summary failed (%d)", result);
		return;
	}
	engine_trace_text_init(&trace_report_text, trace_report, sizeof(trace_report));
	result = engine_trace_summary_write_json(&trace_summary, &trace_capture, &trace_report_text);
	if (result != ENGINE_TRACE_OK || trace_report_text.overflowed)
	{
		platform_log("trace: JSON report exceeded its fixed buffer");
		return;
	}
	file = fopen(trace_path, "w");
	if (!file)
	{
		platform_log("trace: cannot open report %s", trace_path);
		return;
	}
	{
		int write_failed = fwrite(trace_report, 1, trace_report_text.length, file) != trace_report_text.length;
		int close_failed = fclose(file) != 0;
		if (write_failed || close_failed)
		{
			platform_log("trace: could not write report %s", trace_path);
			return;
		}
	}

	/* The ring stays live: counters remain cumulative and tagged memory stays
	   correct across reports. Capture collection retains only the newest ring. */
}

void halo_trace_frame_end(uint64_t frame)
{
	if (trace_state_status != TRACE_READY)
		return;
	engine_trace_zone_end(trace_main_thread, &trace_frame_location);
	engine_trace_frame_end(trace_main_thread, frame);
	engine_trace_counters_sample(trace_main_thread);
	if (frame != 0 && frame % TRACE_REPORT_EVERY_FRAMES == 0)
		trace_platform_write_report();
	engine_trace_counter_set(&trace_state, trace_counter_ids[HALO_TRACE_COUNTER_DRAW_CALLS], 0);
}

static enum engine_trace_memory_tag trace_platform_memory_tag(enum halo_trace_memory_tag tag)
{
	switch (tag)
	{
	case HALO_TRACE_MEMORY_GAME: return ENGINE_TRACE_MEMORY_GAME;
	case HALO_TRACE_MEMORY_RENDER_CPU: return ENGINE_TRACE_MEMORY_RENDER_CPU;
	case HALO_TRACE_MEMORY_RENDER_GPU: return ENGINE_TRACE_MEMORY_RENDER_GPU;
	default: return ENGINE_TRACE_MEMORY_TAG_COUNT;
	}
}

void halo_trace_memory_reserve(enum halo_trace_memory_tag tag, uint64_t bytes)
{
	enum engine_trace_memory_tag const engine_tag = trace_platform_memory_tag(tag);
	if (trace_state_status == TRACE_READY && engine_tag < ENGINE_TRACE_MEMORY_TAG_COUNT &&
		bytes <= (uint64_t)INT64_MAX)
		(void)engine_trace_memory_reserve(&trace_state, engine_tag, (int64_t)bytes);
}

void halo_trace_memory_release(enum halo_trace_memory_tag tag, uint64_t bytes)
{
	enum engine_trace_memory_tag const engine_tag = trace_platform_memory_tag(tag);
	if (trace_state_status == TRACE_READY && engine_tag < ENGINE_TRACE_MEMORY_TAG_COUNT &&
		bytes <= (uint64_t)INT64_MAX)
		engine_trace_memory_release(&trace_state, engine_tag, (int64_t)bytes);
}

void halo_trace_zone_begin(enum halo_trace_zone zone)
{
	if (trace_state_status == TRACE_READY && (uint32_t)zone < HALO_TRACE_ZONE_COUNT)
	{
		trace_zone_started[zone] = engine_trace_now(&trace_state);
		trace_zone_started_valid[zone] = 1;
		engine_trace_zone_begin(trace_main_thread, &trace_zone_locations[zone], 0);
	}
}

void halo_trace_zone_end(enum halo_trace_zone zone)
{
	if (trace_state_status == TRACE_READY && (uint32_t)zone < HALO_TRACE_ZONE_COUNT)
	{
		if (trace_zone_started_valid[zone])
		{
			uint64_t const now = engine_trace_now(&trace_state);
			uint64_t const elapsed = now >= trace_zone_started[zone] ? now - trace_zone_started[zone] : 0;
			enum halo_trace_counter counter = HALO_TRACE_COUNTER_COUNT;
			if (zone == HALO_TRACE_ZONE_MAP_PRECACHE)
				counter = HALO_TRACE_COUNTER_MAP_PRECACHE_LAST_NS;
			else if (zone == HALO_TRACE_ZONE_SCENARIO_LOAD)
				counter = HALO_TRACE_COUNTER_SCENARIO_LOAD_LAST_NS;
			if (zone == HALO_TRACE_ZONE_TEXTURE_UPLOAD && elapsed <= (uint64_t)INT64_MAX)
			{
				engine_trace_counter_add(&trace_state,
					trace_counter_ids[HALO_TRACE_COUNTER_TEXTURE_UPLOAD_NS], (int64_t)elapsed);
				if (elapsed > trace_texture_upload_max_ns)
				{
					trace_texture_upload_max_ns = elapsed;
					engine_trace_counter_set(&trace_state,
						trace_counter_ids[HALO_TRACE_COUNTER_TEXTURE_UPLOAD_MAX_NS], (int64_t)elapsed);
				}
			}
			if (counter < HALO_TRACE_COUNTER_COUNT && elapsed <= (uint64_t)INT64_MAX)
				engine_trace_counter_set(&trace_state, trace_counter_ids[counter], (int64_t)elapsed);
			trace_zone_started_valid[zone] = 0;
		}
		engine_trace_zone_end(trace_main_thread, &trace_zone_locations[zone]);
	}
}

void halo_trace_game_tick_begin(void)
{
	if (trace_state_status != TRACE_READY)
		return;
	trace_tick_active = 1;
	trace_tick_allocations = 0;
	engine_trace_tick_begin(trace_main_thread, ++trace_tick_index);
}

void halo_trace_game_tick_end(void)
{
	if (trace_state_status != TRACE_READY || !trace_tick_active)
		return;
	engine_trace_tick_end(trace_main_thread, trace_tick_index);
	engine_trace_counter_set(&trace_state,
		trace_counter_ids[HALO_TRACE_COUNTER_ALLOCATIONS_PER_TICK], (int64_t)trace_tick_allocations);
	trace_tick_active = 0;
}

void halo_trace_allocation(void)
{
	if (trace_state_status == TRACE_READY && trace_tick_active && trace_tick_allocations < (uint64_t)INT64_MAX)
		++trace_tick_allocations;
}

void halo_trace_draw_call(void)
{
	if (trace_state_status == TRACE_READY)
		engine_trace_counter_add(&trace_state, trace_counter_ids[HALO_TRACE_COUNTER_DRAW_CALLS], 1);
}

void halo_trace_counter_add(enum halo_trace_counter counter, int64_t value)
{
	if (trace_state_status == TRACE_READY && (uint32_t)counter < HALO_TRACE_COUNTER_COUNT)
		engine_trace_counter_add(&trace_state, trace_counter_ids[counter], value);
}

void halo_trace_counter_set(enum halo_trace_counter counter, int64_t value)
{
	if (trace_state_status == TRACE_READY && (uint32_t)counter < HALO_TRACE_COUNTER_COUNT)
		engine_trace_counter_set(&trace_state, trace_counter_ids[counter], value);
}
