#ifndef ENGINE_CORE_TRACE_CAPTURE_H
#define ENGINE_CORE_TRACE_CAPTURE_H

/* Captures turn live rings into a self-contained, pointer-free record that
   tools serialize, export to the Chrome trace event format (which Perfetto
   also opens), and summarize for editors and AI clients (ADR 0051). */

#include "trace.h"

#include <stddef.h>
#include <stdint.h>

#define ENGINE_TRACE_CAPTURE_FILE_BYTES_MAX 96u
#define ENGINE_TRACE_CAPTURE_LOCATION_INVALID UINT32_MAX
#define ENGINE_TRACE_CAPTURE_MAGIC 0x43525445u /* "ETRC" */
#define ENGINE_TRACE_CAPTURE_VERSION 1u
#define ENGINE_TRACE_CAPTURE_ENDIAN_MARKER 0x01020304u
#define ENGINE_TRACE_SUMMARY_ZONES_MAX 16u
#define ENGINE_TRACE_SUMMARY_FRAME_SAMPLES_MAX 4096u

enum engine_trace_location_category
{
	ENGINE_TRACE_LOCATION_ZONE = 1,
	ENGINE_TRACE_LOCATION_NAMED = 2,
	ENGINE_TRACE_LOCATION_COUNTER = 3,
	ENGINE_TRACE_LOCATION_GPU = 4
};

struct engine_trace_capture_location
{
	char name[ENGINE_TRACE_NAME_BYTES_MAX];
	char file[ENGINE_TRACE_CAPTURE_FILE_BYTES_MAX];
	uint32_t line;
	uint32_t category;
};

struct engine_trace_capture_event
{
	uint64_t timestamp_nanoseconds;
	uint64_t argument;
	uint32_t location;
	uint32_t kind;
	uint32_t value32;
	uint32_t track;
};

struct engine_trace_capture_track
{
	char name[ENGINE_TRACE_NAME_BYTES_MAX];
	uint64_t lost_events;
	uint32_t event_first;
	uint32_t event_count;
};

struct engine_trace_capture_memory
{
	int64_t bytes_current;
	int64_t bytes_peak;
	int64_t bytes_budget;
	uint64_t allocation_count;
	uint64_t release_count;
	uint64_t budget_violation_count;
};

/* Pointer-to-index map used only while collecting. */
struct engine_trace_capture_slot
{
	uintptr_t key;
	uint32_t category;
	uint32_t index;
};

struct engine_trace_capture
{
	struct engine_trace_capture_event *events;
	uint32_t events_capacity;
	uint32_t event_count;
	struct engine_trace_capture_location *locations;
	uint32_t locations_capacity;
	uint32_t location_count;
	struct engine_trace_capture_slot *slots;
	uint32_t slots_capacity; /* Power of two, at least twice locations_capacity. */
	struct engine_trace_capture_track tracks[ENGINE_TRACE_TRACKS_MAX];
	uint32_t track_count;
	struct engine_trace_capture_memory memory[ENGINE_TRACE_MEMORY_TAG_COUNT];
	uint64_t gpu_unsupported_frame_count;
};

enum engine_trace_result engine_trace_capture_init(struct engine_trace_capture *capture,
	struct engine_trace_capture_event *events, uint32_t events_capacity,
	struct engine_trace_capture_location *locations, uint32_t locations_capacity,
	struct engine_trace_capture_slot *slots, uint32_t slots_capacity);

/* Copies every ring. Call it while recording threads are quiescent, such as
   between job graphs; the job system guarantees workers record nothing then.
   Events beyond capacity are reported as CAPACITY_EXCEEDED, not dropped silently. */
enum engine_trace_result engine_trace_capture_collect(struct engine_trace *trace,
	struct engine_trace_capture *capture);

/* Bounded text output shared by exporters. */
struct engine_trace_text
{
	char *data;
	size_t capacity;
	size_t length;
	int overflowed;
};
void engine_trace_text_init(struct engine_trace_text *text, char *data, size_t capacity);

/* Binary capture format in host byte order, tagged with an endianness marker.
   The reader validates every count, index, and string; malformed input returns
   MALFORMED and never reads outside `bytes`. */
enum engine_trace_result engine_trace_capture_serialize(struct engine_trace_capture const *capture,
	unsigned char *bytes, size_t capacity, size_t *size);
enum engine_trace_result engine_trace_capture_deserialize(struct engine_trace_capture *capture,
	unsigned char const *bytes, size_t size);
enum engine_trace_result engine_trace_file_write(char const *path,
	unsigned char const *bytes, size_t size);
enum engine_trace_result engine_trace_file_read(char const *path,
	unsigned char *bytes, size_t capacity, size_t *size);

enum engine_trace_result engine_trace_export_chrome(struct engine_trace_capture const *capture,
	struct engine_trace_text *text);

struct engine_trace_summary_zone
{
	uint32_t location;
	uint32_t track_kind; /* 0 CPU, 1 GPU */
	uint64_t count;
	uint64_t total_nanoseconds;
	uint64_t max_nanoseconds;
};

struct engine_trace_summary_counter
{
	uint32_t location;
	int64_t value;
};

struct engine_trace_summary
{
	struct engine_trace_summary_zone cpu_zones[ENGINE_TRACE_SUMMARY_ZONES_MAX];
	uint32_t cpu_zone_count;
	struct engine_trace_summary_zone gpu_zones[ENGINE_TRACE_SUMMARY_ZONES_MAX];
	uint32_t gpu_zone_count;
	struct engine_trace_summary_counter counters[ENGINE_TRACE_COUNTERS_MAX];
	uint32_t counter_count;
	uint32_t frame_sample_count;
	uint64_t frame_p95_nanoseconds;
	uint64_t frame_p99_nanoseconds;
	struct engine_trace_capture_memory memory[ENGINE_TRACE_MEMORY_TAG_COUNT];
	uint64_t lost_events;
	uint64_t unmatched_events;
	uint64_t ticks_completed;
	uint64_t frames_completed;
	uint64_t gpu_unsupported_frame_count;
};

/* `scratch` holds one accumulator per capture location. */
enum engine_trace_result engine_trace_summary_build(struct engine_trace_capture const *capture,
	struct engine_trace_summary_zone *scratch, uint32_t scratch_capacity,
	struct engine_trace_summary *summary);
/* Structured output for editors, the CLI, and AI clients (ADR 0106 profiler.*). */
enum engine_trace_result engine_trace_summary_write_json(struct engine_trace_summary const *summary,
	struct engine_trace_capture const *capture, struct engine_trace_text *text);

/* Best-effort crash report text: open zones, the newest events per thread,
   and memory by tag. It reads live rings without synchronization because the
   process is already failing; never use it for ordinary captures. */
void engine_trace_crash_context_write(struct engine_trace const *trace,
	uint32_t events_per_thread, struct engine_trace_text *text);

#endif
