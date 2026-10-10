#ifndef HALO_TRACE_H
#define HALO_TRACE_H

#include <stdint.h>

/* Enabled when HALO_TRACE_FILE names a report destination. */
enum halo_trace_memory_tag
{
	HALO_TRACE_MEMORY_GAME,
	HALO_TRACE_MEMORY_RENDER_CPU,
	HALO_TRACE_MEMORY_RENDER_GPU
};

enum halo_trace_zone
{
	HALO_TRACE_ZONE_BOT_AI,
	HALO_TRACE_ZONE_MAP_PRECACHE,
	HALO_TRACE_ZONE_SCENARIO_LOAD,
	HALO_TRACE_ZONE_TEXTURE_UPLOAD,
	HALO_TRACE_ZONE_RENDER_FRAME,          /* prepare + main_game_render (main.c) */
	HALO_TRACE_ZONE_INTERPOLATION_PREPARE, /* the blend jobs (render_interpolation.c) */
	HALO_TRACE_ZONE_COUNT
};

enum halo_trace_counter
{
	HALO_TRACE_COUNTER_TEXTURE_CACHE_HITS,
	HALO_TRACE_COUNTER_TEXTURE_CACHE_MISSES,
	HALO_TRACE_COUNTER_TEXTURE_UPLOADS,
	HALO_TRACE_COUNTER_TEXTURE_EVICTIONS,
	HALO_TRACE_COUNTER_TEXTURE_GPU_BYTES,
	HALO_TRACE_COUNTER_TEXTURE_BUDGET_BYTES,
	HALO_TRACE_COUNTER_TEXTURE_OVER_BUDGET,
	HALO_TRACE_COUNTER_TEXTURE_UPLOAD_BYTES,
	HALO_TRACE_COUNTER_TEXTURE_UPLOAD_NS,
	HALO_TRACE_COUNTER_TEXTURE_UPLOAD_MAX_NS,
	HALO_TRACE_COUNTER_DRAW_CALLS,
	HALO_TRACE_COUNTER_ALLOCATIONS_PER_TICK,
	HALO_TRACE_COUNTER_MAP_PRECACHE_LAST_NS,
	HALO_TRACE_COUNTER_SCENARIO_LOAD_LAST_NS,
	HALO_TRACE_COUNTER_COUNT
};

void halo_trace_frame_begin(uint64_t frame);
void halo_trace_zone_begin(enum halo_trace_zone zone);
void halo_trace_zone_end(enum halo_trace_zone zone);
void halo_trace_game_tick_begin(void);
void halo_trace_game_tick_end(void);
void halo_trace_allocation(void);
void halo_trace_draw_call(void);
void halo_trace_frame_end(uint64_t frame);
void halo_trace_memory_reserve(enum halo_trace_memory_tag tag, uint64_t bytes);
void halo_trace_memory_release(enum halo_trace_memory_tag tag, uint64_t bytes);
void halo_trace_counter_add(enum halo_trace_counter counter, int64_t value);
void halo_trace_counter_set(enum halo_trace_counter counter, int64_t value);

#endif
