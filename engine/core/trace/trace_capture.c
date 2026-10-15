#include "trace_capture.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ENGINE_STATIC_ASSERT(sizeof(struct engine_trace_capture_event) == 32,
	"capture events stay as small as ring events");

/* ---- Collection --------------------------------------------------------- */

static int capture_is_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

enum engine_trace_result engine_trace_capture_init(struct engine_trace_capture *capture,
	struct engine_trace_capture_event *events, uint32_t events_capacity,
	struct engine_trace_capture_location *locations, uint32_t locations_capacity,
	struct engine_trace_capture_slot *slots, uint32_t slots_capacity)
{
	ENGINE_ASSERT(capture != NULL);
	if (!events || events_capacity == 0 || !locations || locations_capacity == 0)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	/* Half-full open addressing keeps probe chains short. */
	if (!slots || !capture_is_power_of_two(slots_capacity) ||
		slots_capacity / 2u < locations_capacity)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	memset(capture, 0, sizeof(*capture));
	capture->events = events;
	capture->events_capacity = events_capacity;
	capture->locations = locations;
	capture->locations_capacity = locations_capacity;
	capture->slots = slots;
	capture->slots_capacity = slots_capacity;
	ENGINE_ASSERT(capture->event_count == 0);
	return ENGINE_TRACE_OK;
}

static void capture_copy_string(char *target, uint32_t capacity, char const *source)
{
	uint32_t length = 0;
	ENGINE_ASSERT(target != NULL);
	ENGINE_ASSERT(capacity > 0);
	if (source)
	{
		while (length + 1u < capacity && source[length] != 0)
			length += 1;
		memcpy(target, source, length);
	}
	memset(target + length, 0, capacity - length);
}

static uint32_t capture_category(uint32_t kind)
{
	ENGINE_ASSERT(kind != ENGINE_TRACE_EVENT_NONE);
	ENGINE_ASSERT(kind <= ENGINE_TRACE_EVENT_GPU_ZONE);
	if (kind == ENGINE_TRACE_EVENT_ZONE_BEGIN || kind == ENGINE_TRACE_EVENT_ZONE_END)
		return ENGINE_TRACE_LOCATION_ZONE;
	if (kind == ENGINE_TRACE_EVENT_NAMED_BEGIN || kind == ENGINE_TRACE_EVENT_NAMED_END)
		return ENGINE_TRACE_LOCATION_NAMED;
	if (kind == ENGINE_TRACE_EVENT_COUNTER)
		return ENGINE_TRACE_LOCATION_COUNTER;
	if (kind == ENGINE_TRACE_EVENT_GPU_ZONE)
		return ENGINE_TRACE_LOCATION_GPU;
	return 0;
}

static void capture_location_fill(struct engine_trace_capture_location *location,
	void const *payload, uint32_t category)
{
	ENGINE_ASSERT(payload != NULL);
	ENGINE_ASSERT(category >= ENGINE_TRACE_LOCATION_ZONE && category <= ENGINE_TRACE_LOCATION_GPU);
	location->category = category;
	if (category == ENGINE_TRACE_LOCATION_ZONE || category == ENGINE_TRACE_LOCATION_GPU)
	{
		struct engine_trace_location const *source = payload;
		capture_copy_string(location->name, ENGINE_TRACE_NAME_BYTES_MAX, source->name);
		capture_copy_string(location->file, ENGINE_TRACE_CAPTURE_FILE_BYTES_MAX, source->file);
		location->line = source->line;
		return;
	}
	capture_copy_string(location->name, ENGINE_TRACE_NAME_BYTES_MAX, payload);
	capture_copy_string(location->file, ENGINE_TRACE_CAPTURE_FILE_BYTES_MAX, NULL);
	location->line = 0;
}

/* Returns the location index, or INVALID when the location table is full. */
static uint32_t capture_location_intern(struct engine_trace_capture *capture,
	void const *payload, uint32_t category)
{
	uint32_t const mask = capture->slots_capacity - 1u;
	uint64_t const hash = ((uint64_t)(uintptr_t)payload ^ category) * 0x9E3779B97F4A7C15ull;
	uint32_t probe = (uint32_t)(hash >> 32) & mask;
	uint32_t attempt;
	ENGINE_ASSERT(payload != NULL);
	ENGINE_ASSERT(capture_is_power_of_two(capture->slots_capacity));
	for (attempt = 0; attempt < capture->slots_capacity; ++attempt)
	{
		struct engine_trace_capture_slot *slot = &capture->slots[probe];
		if (slot->key == (uintptr_t)payload && slot->category == category)
			return slot->index;
		if (slot->key == 0)
		{
			if (capture->location_count >= capture->locations_capacity)
				return ENGINE_TRACE_CAPTURE_LOCATION_INVALID;
			slot->key = (uintptr_t)payload;
			slot->category = category;
			slot->index = capture->location_count;
			capture_location_fill(&capture->locations[slot->index], payload, category);
			capture->location_count += 1;
			return slot->index;
		}
		probe = (probe + 1u) & mask;
	}
	/* Unreachable: the table is never more than half full. */
	ENGINE_ASSERT(0);
	return ENGINE_TRACE_CAPTURE_LOCATION_INVALID;
}

static enum engine_trace_result capture_append(struct engine_trace_capture *capture,
	struct engine_trace_event const *event, uint32_t track)
{
	struct engine_trace_capture_event *target;
	uint32_t const category = capture_category(event->kind);
	uint32_t location = ENGINE_TRACE_CAPTURE_LOCATION_INVALID;
	ENGINE_ASSERT(event != NULL);
	ENGINE_ASSERT(track < ENGINE_TRACE_TRACKS_MAX);
	if (capture->event_count >= capture->events_capacity)
		return ENGINE_TRACE_CAPACITY_EXCEEDED;
	if (category != 0)
	{
		location = capture_location_intern(capture, event->payload, category);
		if (location == ENGINE_TRACE_CAPTURE_LOCATION_INVALID)
			return ENGINE_TRACE_CAPACITY_EXCEEDED;
	}
	target = &capture->events[capture->event_count];
	target->timestamp_nanoseconds = event->timestamp_nanoseconds;
	target->argument = event->argument;
	target->location = location;
	target->kind = event->kind;
	target->value32 = event->value32;
	target->track = track;
	capture->event_count += 1;
	return ENGINE_TRACE_OK;
}

static enum engine_trace_result capture_track(struct engine_trace_capture *capture,
	struct engine_trace_thread const *thread, uint32_t track)
{
	struct engine_trace_capture_track *target = &capture->tracks[track];
	uint64_t const written = atomic_load_explicit(
		(_Atomic uint64_t *)&thread->write_count, memory_order_acquire);
	uint64_t const available = written < thread->capacity ? written : thread->capacity;
	uint64_t sequence;
	ENGINE_ASSERT(thread->ring != NULL);
	ENGINE_ASSERT(track < ENGINE_TRACE_TRACKS_MAX);
	capture_copy_string(target->name, ENGINE_TRACE_NAME_BYTES_MAX, thread->name);
	/* A full ring overwrote the oldest events; report them as lost. */
	target->lost_events = written - available;
	target->event_first = capture->event_count;
	target->event_count = 0;
	for (sequence = written - available; sequence < written; ++sequence)
	{
		struct engine_trace_event const *event =
			&thread->ring[sequence & (thread->capacity - 1u)];
		enum engine_trace_result const result = capture_append(capture, event, track);
		if (result != ENGINE_TRACE_OK)
			return result;
		target->event_count += 1;
	}
	return ENGINE_TRACE_OK;
}

static void capture_memory(struct engine_trace const *trace, struct engine_trace_capture *capture)
{
	uint32_t tag;
	ENGINE_ASSERT(trace != NULL);
	ENGINE_ASSERT(capture != NULL);
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
	{
		struct engine_trace_memory_tag_state *state =
			(struct engine_trace_memory_tag_state *)&trace->memory[tag];
		struct engine_trace_capture_memory *target = &capture->memory[tag];
		target->bytes_current = atomic_load(&state->bytes_current);
		target->bytes_peak = atomic_load(&state->bytes_peak);
		target->bytes_budget = state->bytes_budget;
		target->allocation_count = atomic_load(&state->allocation_count);
		target->release_count = atomic_load(&state->release_count);
		target->budget_violation_count = atomic_load(&state->budget_violation_count);
	}
}

enum engine_trace_result engine_trace_capture_collect(struct engine_trace *trace,
	struct engine_trace_capture *capture)
{
	uint32_t const cpu_count = atomic_load_explicit(&trace->thread_count, memory_order_acquire);
	enum engine_trace_result result = ENGINE_TRACE_OK;
	uint32_t index;
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(cpu_count <= trace->config.threads_max);
	capture->event_count = 0;
	capture->location_count = 0;
	memset(capture->slots, 0, sizeof(*capture->slots) * capture->slots_capacity);
	capture->track_count = cpu_count + 1u;
	for (index = 0; result == ENGINE_TRACE_OK && index < cpu_count; ++index)
		result = capture_track(capture, &trace->threads[index], index);
	if (result == ENGINE_TRACE_OK)
		result = capture_track(capture, engine_trace_gpu_track(trace), cpu_count);
	capture_memory(trace, capture);
	capture->gpu_unsupported_frame_count = trace->gpu_unsupported_frame_count;
	return result;
}

/* ---- Text output -------------------------------------------------------- */

void engine_trace_text_init(struct engine_trace_text *text, char *data, size_t capacity)
{
	ENGINE_ASSERT(text != NULL);
	ENGINE_ASSERT(data != NULL && capacity > 0);
	text->data = data;
	text->capacity = capacity;
	text->length = 0;
	text->overflowed = 0;
	data[0] = 0;
}

static void text_format(struct engine_trace_text *text, char const *format, ...)
{
	va_list arguments;
	size_t remaining;
	int written;
	ENGINE_ASSERT(text != NULL && text->data != NULL);
	ENGINE_ASSERT(text->length < text->capacity);
	if (text->overflowed)
		return;
	remaining = text->capacity - text->length;
	va_start(arguments, format);
	written = vsnprintf(text->data + text->length, remaining, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= remaining)
	{
		/* Keep the text terminated at its last complete write. */
		text->overflowed = 1;
		text->data[text->length] = 0;
		return;
	}
	text->length += (size_t)written;
}

static void text_json_string(struct engine_trace_text *text, char const *value, uint32_t capacity)
{
	uint32_t index;
	ENGINE_ASSERT(value != NULL);
	ENGINE_ASSERT(capacity > 0);
	text_format(text, "\"");
	for (index = 0; index < capacity && value[index] != 0; ++index)
	{
		unsigned char const character = (unsigned char)value[index];
		if (character == '"' || character == '\\')
			text_format(text, "\\%c", character);
		else if (character < 0x20)
			text_format(text, "\\u%04x", character);
		else
			text_format(text, "%c", character);
	}
	text_format(text, "\"");
}

static void text_microseconds(struct engine_trace_text *text, uint64_t nanoseconds)
{
	ENGINE_ASSERT(text != NULL);
	ENGINE_ASSERT(nanoseconds / 1000u <= UINT64_MAX);
	text_format(text, "%" PRIu64 ".%03" PRIu64, nanoseconds / 1000u, nanoseconds % 1000u);
}

/* ---- Chrome trace event export ----------------------------------------- */

static char const *capture_location_name(struct engine_trace_capture const *capture,
	uint32_t location)
{
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(location < capture->location_count);
	return capture->locations[location].name;
}

static void chrome_event_header(struct engine_trace_text *text, char const *phase,
	struct engine_trace_capture_event const *event)
{
	ENGINE_ASSERT(phase != NULL);
	ENGINE_ASSERT(event != NULL);
	text_format(text, ",\n{\"ph\":\"%s\",\"pid\":1,\"tid\":%" PRIu32 ",\"ts\":",
		phase, event->track);
	text_microseconds(text, event->timestamp_nanoseconds);
}

static void chrome_named(struct engine_trace_text *text, struct engine_trace_capture const *capture,
	struct engine_trace_capture_event const *event, char const *phase)
{
	ENGINE_ASSERT(event->location < capture->location_count);
	ENGINE_ASSERT(phase != NULL);
	chrome_event_header(text, phase, event);
	text_format(text, ",\"name\":");
	text_json_string(text, capture_location_name(capture, event->location),
		ENGINE_TRACE_NAME_BYTES_MAX);
}

static void chrome_event(struct engine_trace_text *text, struct engine_trace_capture const *capture,
	struct engine_trace_capture_event const *event)
{
	uint32_t const kind = event->kind;
	ENGINE_ASSERT(kind != ENGINE_TRACE_EVENT_NONE);
	ENGINE_ASSERT(kind <= ENGINE_TRACE_EVENT_GPU_ZONE);
	if (kind == ENGINE_TRACE_EVENT_ZONE_BEGIN || kind == ENGINE_TRACE_EVENT_NAMED_BEGIN)
	{
		chrome_named(text, capture, event, "B");
		text_format(text, ",\"args\":{\"value\":%" PRIu64 "}}", event->argument);
	}
	else if (kind == ENGINE_TRACE_EVENT_ZONE_END || kind == ENGINE_TRACE_EVENT_NAMED_END)
	{
		chrome_named(text, capture, event, "E");
		text_format(text, "}");
	}
	else if (kind == ENGINE_TRACE_EVENT_COUNTER)
	{
		chrome_named(text, capture, event, "C");
		text_format(text, ",\"args\":{\"value\":%" PRId64 "}}", (int64_t)event->argument);
	}
	else if (kind == ENGINE_TRACE_EVENT_GPU_ZONE)
	{
		chrome_named(text, capture, event, "X");
		text_format(text, ",\"dur\":");
		text_microseconds(text, event->argument);
		text_format(text, ",\"args\":{\"frame\":%" PRIu32 "}}", event->value32);
	}
	else
	{
		int const tick = kind == ENGINE_TRACE_EVENT_TICK_BEGIN ||
			kind == ENGINE_TRACE_EVENT_TICK_END;
		int const begin = kind == ENGINE_TRACE_EVENT_TICK_BEGIN ||
			kind == ENGINE_TRACE_EVENT_FRAME_BEGIN;
		chrome_event_header(text, begin ? "B" : "E", event);
		text_format(text, ",\"name\":\"%s\",\"args\":{\"%s\":%" PRIu64 "}}",
			tick ? "tick" : "frame", tick ? "tick" : "frame", event->argument);
	}
}

enum engine_trace_result engine_trace_export_chrome(struct engine_trace_capture const *capture,
	struct engine_trace_text *text)
{
	uint32_t track;
	uint32_t index;
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(text != NULL);
	text_format(text, "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n");
	text_format(text, "{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\","
		"\"args\":{\"name\":\"engine\"}}");
	for (track = 0; track < capture->track_count; ++track)
	{
		text_format(text, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%" PRIu32
			",\"name\":\"thread_name\",\"args\":{\"name\":", track);
		text_json_string(text, capture->tracks[track].name, ENGINE_TRACE_NAME_BYTES_MAX);
		text_format(text, ",\"lost_events\":%" PRIu64 "}}", capture->tracks[track].lost_events);
	}
	for (index = 0; index < capture->event_count; ++index)
		chrome_event(text, capture, &capture->events[index]);
	text_format(text, "\n]}\n");
	return text->overflowed ? ENGINE_TRACE_CAPACITY_EXCEEDED : ENGINE_TRACE_OK;
}

/* ---- Binary format ------------------------------------------------------ */

struct capture_cursor
{
	unsigned char *write;
	unsigned char const *read;
	size_t capacity;
	size_t offset;
	int failed;
};

static void cursor_put(struct capture_cursor *cursor, void const *value, size_t size)
{
	ENGINE_ASSERT(cursor->write != NULL);
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
	if (cursor->failed || size > cursor->capacity - cursor->offset)
	{
		cursor->failed = 1;
		return;
	}
	memcpy(cursor->write + cursor->offset, value, size);
	cursor->offset += size;
}

static void cursor_get(struct capture_cursor *cursor, void *value, size_t size)
{
	ENGINE_ASSERT(cursor->read != NULL);
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
	if (cursor->failed || size > cursor->capacity - cursor->offset)
	{
		cursor->failed = 1;
		memset(value, 0, size);
		return;
	}
	memcpy(value, cursor->read + cursor->offset, size);
	cursor->offset += size;
}

static void cursor_put_u32(struct capture_cursor *cursor, uint32_t value)
{
	ENGINE_ASSERT(cursor != NULL);
	cursor_put(cursor, &value, sizeof(value));
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
}

static void cursor_put_u64(struct capture_cursor *cursor, uint64_t value)
{
	ENGINE_ASSERT(cursor != NULL);
	cursor_put(cursor, &value, sizeof(value));
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
}

static uint32_t cursor_get_u32(struct capture_cursor *cursor)
{
	uint32_t value;
	ENGINE_ASSERT(cursor != NULL);
	cursor_get(cursor, &value, sizeof(value));
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
	return value;
}

static uint64_t cursor_get_u64(struct capture_cursor *cursor)
{
	uint64_t value;
	ENGINE_ASSERT(cursor != NULL);
	cursor_get(cursor, &value, sizeof(value));
	ENGINE_ASSERT(cursor->offset <= cursor->capacity);
	return value;
}

static void capture_put_memory(struct capture_cursor *cursor,
	struct engine_trace_capture_memory const *memory)
{
	ENGINE_ASSERT(memory->bytes_current >= 0);
	ENGINE_ASSERT(memory->bytes_peak >= memory->bytes_current);
	cursor_put_u64(cursor, (uint64_t)memory->bytes_current);
	cursor_put_u64(cursor, (uint64_t)memory->bytes_peak);
	cursor_put_u64(cursor, (uint64_t)memory->bytes_budget);
	cursor_put_u64(cursor, memory->allocation_count);
	cursor_put_u64(cursor, memory->release_count);
	cursor_put_u64(cursor, memory->budget_violation_count);
}

static void capture_put_body(struct capture_cursor *cursor,
	struct engine_trace_capture const *capture)
{
	uint32_t index;
	ENGINE_ASSERT(capture->track_count <= ENGINE_TRACE_TRACKS_MAX);
	ENGINE_ASSERT(capture->location_count <= capture->locations_capacity);
	for (index = 0; index < capture->track_count; ++index)
	{
		struct engine_trace_capture_track const *track = &capture->tracks[index];
		cursor_put(cursor, track->name, sizeof(track->name));
		cursor_put_u64(cursor, track->lost_events);
		cursor_put_u32(cursor, track->event_first);
		cursor_put_u32(cursor, track->event_count);
	}
	for (index = 0; index < capture->location_count; ++index)
	{
		struct engine_trace_capture_location const *location = &capture->locations[index];
		cursor_put(cursor, location->name, sizeof(location->name));
		cursor_put(cursor, location->file, sizeof(location->file));
		cursor_put_u32(cursor, location->line);
		cursor_put_u32(cursor, location->category);
	}
	for (index = 0; index < ENGINE_TRACE_MEMORY_TAG_COUNT; ++index)
		capture_put_memory(cursor, &capture->memory[index]);
	for (index = 0; index < capture->event_count; ++index)
	{
		struct engine_trace_capture_event const *event = &capture->events[index];
		cursor_put_u64(cursor, event->timestamp_nanoseconds);
		cursor_put_u64(cursor, event->argument);
		cursor_put_u32(cursor, event->location);
		cursor_put_u32(cursor, event->kind);
		cursor_put_u32(cursor, event->value32);
		cursor_put_u32(cursor, event->track);
	}
}

enum engine_trace_result engine_trace_capture_serialize(struct engine_trace_capture const *capture,
	unsigned char *bytes, size_t capacity, size_t *size)
{
	struct capture_cursor cursor = { bytes, NULL, capacity, 0, 0 };
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(size != NULL);
	if (!bytes)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	cursor_put_u32(&cursor, ENGINE_TRACE_CAPTURE_MAGIC);
	cursor_put_u32(&cursor, ENGINE_TRACE_CAPTURE_VERSION);
	cursor_put_u32(&cursor, ENGINE_TRACE_CAPTURE_ENDIAN_MARKER);
	cursor_put_u32(&cursor, capture->track_count);
	cursor_put_u32(&cursor, capture->location_count);
	cursor_put_u32(&cursor, capture->event_count);
	cursor_put_u32(&cursor, ENGINE_TRACE_MEMORY_TAG_COUNT);
	cursor_put_u32(&cursor, 0);
	cursor_put_u64(&cursor, capture->gpu_unsupported_frame_count);
	capture_put_body(&cursor, capture);
	*size = cursor.offset;
	return cursor.failed ? ENGINE_TRACE_CAPACITY_EXCEEDED : ENGINE_TRACE_OK;
}

static int capture_string_terminated(char const *value, size_t capacity)
{
	ENGINE_ASSERT(value != NULL);
	ENGINE_ASSERT(capacity > 0);
	return memchr(value, 0, capacity) != NULL;
}

static int capture_get_tracks(struct capture_cursor *cursor, struct engine_trace_capture *capture)
{
	uint32_t expected_first = 0;
	uint32_t index;
	ENGINE_ASSERT(capture->track_count <= ENGINE_TRACE_TRACKS_MAX);
	ENGINE_ASSERT(capture->event_count <= capture->events_capacity);
	for (index = 0; index < capture->track_count; ++index)
	{
		struct engine_trace_capture_track *track = &capture->tracks[index];
		cursor_get(cursor, track->name, sizeof(track->name));
		track->lost_events = cursor_get_u64(cursor);
		track->event_first = cursor_get_u32(cursor);
		track->event_count = cursor_get_u32(cursor);
		if (!capture_string_terminated(track->name, sizeof(track->name)) ||
			track->event_first != expected_first ||
			track->event_count > capture->event_count - expected_first)
			return 0;
		expected_first += track->event_count;
	}
	return expected_first == capture->event_count;
}

static int capture_get_locations(struct capture_cursor *cursor,
	struct engine_trace_capture *capture)
{
	uint32_t index;
	ENGINE_ASSERT(capture->location_count <= capture->locations_capacity);
	ENGINE_ASSERT(cursor->read != NULL);
	for (index = 0; index < capture->location_count; ++index)
	{
		struct engine_trace_capture_location *location = &capture->locations[index];
		cursor_get(cursor, location->name, sizeof(location->name));
		cursor_get(cursor, location->file, sizeof(location->file));
		location->line = cursor_get_u32(cursor);
		location->category = cursor_get_u32(cursor);
		if (!capture_string_terminated(location->name, sizeof(location->name)) ||
			!capture_string_terminated(location->file, sizeof(location->file)) ||
			location->category < ENGINE_TRACE_LOCATION_ZONE ||
			location->category > ENGINE_TRACE_LOCATION_GPU)
			return 0;
	}
	return 1;
}

static int capture_get_memory(struct capture_cursor *cursor, struct engine_trace_capture *capture)
{
	uint32_t tag;
	ENGINE_ASSERT(cursor->read != NULL);
	ENGINE_ASSERT(capture != NULL);
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
	{
		struct engine_trace_capture_memory *memory = &capture->memory[tag];
		memory->bytes_current = (int64_t)cursor_get_u64(cursor);
		memory->bytes_peak = (int64_t)cursor_get_u64(cursor);
		memory->bytes_budget = (int64_t)cursor_get_u64(cursor);
		memory->allocation_count = cursor_get_u64(cursor);
		memory->release_count = cursor_get_u64(cursor);
		memory->budget_violation_count = cursor_get_u64(cursor);
		if (memory->bytes_current < 0 || memory->bytes_peak < memory->bytes_current ||
			memory->bytes_budget < 0)
			return 0;
	}
	return 1;
}

static int capture_event_valid(struct engine_trace_capture const *capture,
	struct engine_trace_capture_event const *event, uint32_t track)
{
	uint32_t category;
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(track < capture->track_count);
	if (event->kind == ENGINE_TRACE_EVENT_NONE || event->kind > ENGINE_TRACE_EVENT_GPU_ZONE ||
		event->track != track)
		return 0;
	category = capture_category(event->kind);
	if (category == 0)
		return event->location == ENGINE_TRACE_CAPTURE_LOCATION_INVALID;
	if (event->location >= capture->location_count)
		return 0;
	return capture->locations[event->location].category == category;
}

static int capture_get_events(struct capture_cursor *cursor, struct engine_trace_capture *capture)
{
	uint32_t track;
	ENGINE_ASSERT(capture->event_count <= capture->events_capacity);
	ENGINE_ASSERT(capture->track_count <= ENGINE_TRACE_TRACKS_MAX);
	for (track = 0; track < capture->track_count; ++track)
	{
		uint32_t const first = capture->tracks[track].event_first;
		uint32_t index;
		for (index = first; index < first + capture->tracks[track].event_count; ++index)
		{
			struct engine_trace_capture_event *event = &capture->events[index];
			event->timestamp_nanoseconds = cursor_get_u64(cursor);
			event->argument = cursor_get_u64(cursor);
			event->location = cursor_get_u32(cursor);
			event->kind = cursor_get_u32(cursor);
			event->value32 = cursor_get_u32(cursor);
			event->track = cursor_get_u32(cursor);
			if (cursor->failed || !capture_event_valid(capture, event, track))
				return 0;
		}
	}
	return 1;
}

static int capture_get_header(struct capture_cursor *cursor, struct engine_trace_capture *capture)
{
	uint32_t magic;
	uint32_t version;
	uint32_t endian;
	uint32_t tag_count;
	uint32_t reserved;
	ENGINE_ASSERT(cursor->read != NULL);
	ENGINE_ASSERT(capture != NULL);
	magic = cursor_get_u32(cursor);
	version = cursor_get_u32(cursor);
	endian = cursor_get_u32(cursor);
	capture->track_count = cursor_get_u32(cursor);
	capture->location_count = cursor_get_u32(cursor);
	capture->event_count = cursor_get_u32(cursor);
	tag_count = cursor_get_u32(cursor);
	reserved = cursor_get_u32(cursor);
	capture->gpu_unsupported_frame_count = cursor_get_u64(cursor);
	if (cursor->failed || magic != ENGINE_TRACE_CAPTURE_MAGIC ||
		version != ENGINE_TRACE_CAPTURE_VERSION || endian != ENGINE_TRACE_CAPTURE_ENDIAN_MARKER ||
		tag_count != ENGINE_TRACE_MEMORY_TAG_COUNT || reserved != 0)
		return 0;
	return capture->track_count >= 1 && capture->track_count <= ENGINE_TRACE_TRACKS_MAX &&
		capture->location_count <= capture->locations_capacity &&
		capture->event_count <= capture->events_capacity;
}

enum engine_trace_result engine_trace_capture_deserialize(struct engine_trace_capture *capture,
	unsigned char const *bytes, size_t size)
{
	struct capture_cursor cursor = { NULL, bytes, size, 0, 0 };
	int valid;
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(capture->events != NULL && capture->locations != NULL);
	if (!bytes)
		return ENGINE_TRACE_INVALID_ARGUMENT;
	valid = capture_get_header(&cursor, capture);
	valid = valid && capture_get_tracks(&cursor, capture);
	valid = valid && capture_get_locations(&cursor, capture);
	valid = valid && capture_get_memory(&cursor, capture);
	valid = valid && capture_get_events(&cursor, capture);
	if (!valid || cursor.failed || cursor.offset != size)
	{
		/* Leave a malformed capture empty rather than half-read. */
		capture->track_count = 0;
		capture->location_count = 0;
		capture->event_count = 0;
		return ENGINE_TRACE_MALFORMED;
	}
	return ENGINE_TRACE_OK;
}

enum engine_trace_result engine_trace_file_write(char const *path,
	unsigned char const *bytes, size_t size)
{
	FILE *file;
	size_t written;
	int closed;
	ENGINE_ASSERT(path != NULL);
	ENGINE_ASSERT(bytes != NULL || size == 0);
	file = fopen(path, "wb");
	if (!file)
		return ENGINE_TRACE_IO_FAILED;
	written = fwrite(bytes, 1, size, file);
	closed = fclose(file);
	return written == size && closed == 0 ? ENGINE_TRACE_OK : ENGINE_TRACE_IO_FAILED;
}

enum engine_trace_result engine_trace_file_read(char const *path,
	unsigned char *bytes, size_t capacity, size_t *size)
{
	FILE *file;
	size_t length;
	int extra;
	ENGINE_ASSERT(path != NULL && size != NULL);
	ENGINE_ASSERT(bytes != NULL || capacity == 0);
	*size = 0;
	file = fopen(path, "rb");
	if (!file)
		return ENGINE_TRACE_IO_FAILED;
	length = fread(bytes, 1, capacity, file);
	extra = fgetc(file);
	if (ferror(file))
	{
		fclose(file);
		return ENGINE_TRACE_IO_FAILED;
	}
	fclose(file);
	if (extra != EOF)
		return ENGINE_TRACE_CAPACITY_EXCEEDED;
	*size = length;
	return ENGINE_TRACE_OK;
}

/* ---- Summaries ---------------------------------------------------------- */

struct summary_open_zone
{
	uint32_t location;
	uint64_t begin_nanoseconds;
};

static void summary_accumulate(struct engine_trace_summary_zone *zone, uint64_t duration)
{
	ENGINE_ASSERT(zone != NULL);
	ENGINE_ASSERT(zone->total_nanoseconds <= UINT64_MAX - duration);
	zone->count += 1;
	zone->total_nanoseconds += duration;
	if (duration > zone->max_nanoseconds)
		zone->max_nanoseconds = duration;
}

static void summary_track(struct engine_trace_capture const *capture, uint32_t track,
	struct engine_trace_summary_zone *scratch, struct engine_trace_summary *summary)
{
	struct summary_open_zone stack[ENGINE_TRACE_ZONE_DEPTH_MAX];
	uint32_t depth = 0;
	uint32_t const first = capture->tracks[track].event_first;
	uint32_t index;
	ENGINE_ASSERT(track < capture->track_count);
	ENGINE_ASSERT(first + capture->tracks[track].event_count <= capture->event_count);
	for (index = first; index < first + capture->tracks[track].event_count; ++index)
	{
		struct engine_trace_capture_event const *event = &capture->events[index];
		uint32_t const kind = event->kind;
		if (kind == ENGINE_TRACE_EVENT_ZONE_BEGIN || kind == ENGINE_TRACE_EVENT_NAMED_BEGIN)
		{
			if (depth == ENGINE_TRACE_ZONE_DEPTH_MAX)
			{
				summary->unmatched_events += 1;
				continue;
			}
			stack[depth].location = event->location;
			stack[depth].begin_nanoseconds = event->timestamp_nanoseconds;
			depth += 1;
		}
		else if (kind == ENGINE_TRACE_EVENT_ZONE_END || kind == ENGINE_TRACE_EVENT_NAMED_END)
		{
			/* An end without its begin was overwritten in a full ring. */
			if (depth == 0 || stack[depth - 1].location != event->location)
			{
				summary->unmatched_events += 1;
				continue;
			}
			depth -= 1;
			summary_accumulate(&scratch[event->location],
				event->timestamp_nanoseconds >= stack[depth].begin_nanoseconds ?
				event->timestamp_nanoseconds - stack[depth].begin_nanoseconds : 0);
		}
		else if (kind == ENGINE_TRACE_EVENT_GPU_ZONE)
			summary_accumulate(&scratch[event->location], event->argument);
		else if (kind == ENGINE_TRACE_EVENT_TICK_END)
			summary->ticks_completed += 1;
		else if (kind == ENGINE_TRACE_EVENT_FRAME_END)
			summary->frames_completed += 1;
	}
	summary->unmatched_events += depth;
}

/* Keeps the largest totals, ties broken by location index for stable output. */
static void summary_insert(struct engine_trace_summary_zone *top, uint32_t *count,
	struct engine_trace_summary_zone const *candidate)
{
	uint32_t position = *count;
	ENGINE_ASSERT(*count <= ENGINE_TRACE_SUMMARY_ZONES_MAX);
	ENGINE_ASSERT(candidate->count > 0);
	while (position > 0 && (top[position - 1].total_nanoseconds < candidate->total_nanoseconds ||
		(top[position - 1].total_nanoseconds == candidate->total_nanoseconds &&
		top[position - 1].location > candidate->location)))
		position -= 1;
	if (position >= ENGINE_TRACE_SUMMARY_ZONES_MAX)
		return;
	if (*count < ENGINE_TRACE_SUMMARY_ZONES_MAX)
		*count += 1;
	memmove(&top[position + 1], &top[position],
		sizeof(*top) * (*count - 1u - position));
	top[position] = *candidate;
}

static int summary_u64_compare(void const *left, void const *right)
{
	uint64_t const a = *(uint64_t const *)left;
	uint64_t const b = *(uint64_t const *)right;
	return (a > b) - (a < b);
}

static enum engine_trace_result summary_capture_details(struct engine_trace_capture const *capture,
	struct engine_trace_summary *summary)
{
	uint64_t frame_durations[ENGINE_TRACE_SUMMARY_FRAME_SAMPLES_MAX];
	uint32_t frame_duration_count = 0;
	uint32_t track;

	for (track = 0; track < capture->track_count; ++track)
	{
		uint32_t const first = capture->tracks[track].event_first;
		uint32_t const end = first + capture->tracks[track].event_count;
		uint32_t index;
		uint64_t frame_begin = 0;
		uint64_t frame_identity = 0;
		int frame_open = 0;

		for (index = first; index < end; ++index)
		{
			struct engine_trace_capture_event const *event = &capture->events[index];
			if (event->kind == ENGINE_TRACE_EVENT_COUNTER)
			{
				uint32_t counter;
				for (counter = 0; counter < summary->counter_count; ++counter)
					if (summary->counters[counter].location == event->location)
						break;
				if (counter == summary->counter_count)
				{
					if (summary->counter_count >= ENGINE_TRACE_COUNTERS_MAX)
						return ENGINE_TRACE_CAPACITY_EXCEEDED;
					summary->counters[counter].location = event->location;
					summary->counter_count += 1;
				}
				summary->counters[counter].value = (int64_t)event->argument;
			}
			else if (event->kind == ENGINE_TRACE_EVENT_FRAME_BEGIN)
			{
				frame_open = 1;
				frame_identity = event->argument;
				frame_begin = event->timestamp_nanoseconds;
			}
			else if (event->kind == ENGINE_TRACE_EVENT_FRAME_END && frame_open &&
				event->argument == frame_identity)
			{
				if (frame_duration_count >= ENGINE_TRACE_SUMMARY_FRAME_SAMPLES_MAX)
					return ENGINE_TRACE_CAPACITY_EXCEEDED;
				frame_durations[frame_duration_count] = event->timestamp_nanoseconds >= frame_begin ?
					event->timestamp_nanoseconds - frame_begin : 0;
				frame_duration_count += 1;
				frame_open = 0;
			}
		}
	}

	summary->frame_sample_count = frame_duration_count;
	if (frame_duration_count)
	{
		uint32_t p95_index;
		uint32_t p99_index;
		qsort(frame_durations, frame_duration_count, sizeof(frame_durations[0]), summary_u64_compare);
		p95_index = (frame_duration_count * 95u + 99u) / 100u - 1u;
		p99_index = (frame_duration_count * 99u + 99u) / 100u - 1u;
		summary->frame_p95_nanoseconds = frame_durations[p95_index];
		summary->frame_p99_nanoseconds = frame_durations[p99_index];
	}
	return ENGINE_TRACE_OK;
}

enum engine_trace_result engine_trace_summary_build(struct engine_trace_capture const *capture,
	struct engine_trace_summary_zone *scratch, uint32_t scratch_capacity,
	struct engine_trace_summary *summary)
{
	uint32_t index;
	ENGINE_ASSERT(capture != NULL);
	ENGINE_ASSERT(summary != NULL);
	if (!scratch || scratch_capacity < capture->location_count)
		return ENGINE_TRACE_CAPACITY_EXCEEDED;
	memset(summary, 0, sizeof(*summary));
	for (index = 0; index < capture->location_count; ++index)
	{
		memset(&scratch[index], 0, sizeof(scratch[index]));
		scratch[index].location = index;
		scratch[index].track_kind =
			capture->locations[index].category == ENGINE_TRACE_LOCATION_GPU;
	}
	for (index = 0; index < capture->track_count; ++index)
	{
		summary->lost_events += capture->tracks[index].lost_events;
		summary_track(capture, index, scratch, summary);
	}
	for (index = 0; index < capture->location_count; ++index)
	{
		if (scratch[index].count == 0)
			continue;
		if (scratch[index].track_kind)
			summary_insert(summary->gpu_zones, &summary->gpu_zone_count, &scratch[index]);
		else
			summary_insert(summary->cpu_zones, &summary->cpu_zone_count, &scratch[index]);
	}
	memcpy(summary->memory, capture->memory, sizeof(summary->memory));
	summary->gpu_unsupported_frame_count = capture->gpu_unsupported_frame_count;
	if (summary_capture_details(capture, summary) != ENGINE_TRACE_OK)
		return ENGINE_TRACE_CAPACITY_EXCEEDED;
	ENGINE_ASSERT(summary->cpu_zone_count <= ENGINE_TRACE_SUMMARY_ZONES_MAX);
	ENGINE_ASSERT(summary->gpu_zone_count <= ENGINE_TRACE_SUMMARY_ZONES_MAX);
	return ENGINE_TRACE_OK;
}

static void summary_zones_json(struct engine_trace_text *text,
	struct engine_trace_capture const *capture,
	struct engine_trace_summary_zone const *zones, uint32_t count)
{
	uint32_t index;
	ENGINE_ASSERT(count <= ENGINE_TRACE_SUMMARY_ZONES_MAX);
	ENGINE_ASSERT(zones != NULL);
	text_format(text, "[");
	for (index = 0; index < count; ++index)
	{
		text_format(text, "%s{\"name\":", index ? "," : "");
		text_json_string(text, capture_location_name(capture, zones[index].location),
			ENGINE_TRACE_NAME_BYTES_MAX);
		text_format(text, ",\"count\":%" PRIu64 ",\"total_ns\":%" PRIu64
			",\"max_ns\":%" PRIu64 "}", zones[index].count, zones[index].total_nanoseconds,
			zones[index].max_nanoseconds);
	}
	text_format(text, "]");
}

enum engine_trace_result engine_trace_summary_write_json(struct engine_trace_summary const *summary,
	struct engine_trace_capture const *capture, struct engine_trace_text *text)
{
	uint32_t tag;
	ENGINE_ASSERT(summary != NULL && capture != NULL);
	ENGINE_ASSERT(text != NULL);
	text_format(text, "{\"lost_events\":%" PRIu64 ",\"unmatched_events\":%" PRIu64
		",\"ticks\":%" PRIu64 ",\"frames\":%" PRIu64 ",\"gpu_unsupported_frames\":%" PRIu64,
		summary->lost_events, summary->unmatched_events, summary->ticks_completed,
		summary->frames_completed, summary->gpu_unsupported_frame_count);
	text_format(text, ",\"cpu_zones\":");
	summary_zones_json(text, capture, summary->cpu_zones, summary->cpu_zone_count);
	text_format(text, ",\"gpu_zones\":");
	summary_zones_json(text, capture, summary->gpu_zones, summary->gpu_zone_count);
	text_format(text, ",\"frame_samples\":%u,\"frame_p95_ns\":%" PRIu64
		",\"frame_p99_ns\":%" PRIu64 ",\"counters\":[",
		summary->frame_sample_count, summary->frame_p95_nanoseconds, summary->frame_p99_nanoseconds);
	for (tag = 0; tag < summary->counter_count; ++tag)
	{
		struct engine_trace_summary_counter const *counter = &summary->counters[tag];
		text_format(text, "%s{\"name\":", tag ? "," : "");
		text_json_string(text, capture_location_name(capture, counter->location),
			ENGINE_TRACE_NAME_BYTES_MAX);
		text_format(text, ",\"value\":%" PRId64 "}", counter->value);
	}
	text_format(text, "],\"memory\":[");
	for (tag = 0; tag < ENGINE_TRACE_MEMORY_TAG_COUNT; ++tag)
	{
		struct engine_trace_capture_memory const *memory = &summary->memory[tag];
		text_format(text, "%s{\"tag\":\"%s\",\"current\":%" PRId64 ",\"peak\":%" PRId64
			",\"budget\":%" PRId64 ",\"violations\":%" PRIu64 "}", tag ? "," : "",
			engine_trace_memory_tag_name((enum engine_trace_memory_tag)tag),
			memory->bytes_current, memory->bytes_peak, memory->bytes_budget,
			memory->budget_violation_count);
	}
	text_format(text, "]}\n");
	return text->overflowed ? ENGINE_TRACE_CAPACITY_EXCEEDED : ENGINE_TRACE_OK;
}

/* ---- Crash context ------------------------------------------------------ */

static char const *crash_payload_name(uint32_t kind, void const *payload)
{
	uint32_t category;
	ENGINE_ASSERT(kind <= ENGINE_TRACE_EVENT_GPU_ZONE);
	category = kind == ENGINE_TRACE_EVENT_NONE ? 0 : capture_category(kind);
	ENGINE_ASSERT(category <= ENGINE_TRACE_LOCATION_GPU);
	if (!payload || category == 0)
		return "-";
	if (category == ENGINE_TRACE_LOCATION_ZONE || category == ENGINE_TRACE_LOCATION_GPU)
		return ((struct engine_trace_location const *)payload)->name;
	return payload;
}

static void crash_thread(struct engine_trace_thread const *thread, uint32_t events_max,
	struct engine_trace_text *text)
{
	uint64_t const written = atomic_load_explicit(
		(_Atomic uint64_t *)&thread->write_count, memory_order_acquire);
	uint64_t const available = written < thread->capacity ? written : thread->capacity;
	uint64_t const shown = available < events_max ? available : events_max;
	uint64_t sequence;
	uint32_t level;
	ENGINE_ASSERT(thread->ring != NULL);
	ENGINE_ASSERT(thread->depth <= ENGINE_TRACE_ZONE_DEPTH_MAX);
	text_format(text, "thread %s: open zones", thread->name);
	for (level = 0; level < thread->depth; ++level)
		text_format(text, " > %s",
			crash_payload_name(thread->stack_kind[level], thread->stack[level]));
	text_format(text, "\n");
	for (sequence = written - shown; sequence < written; ++sequence)
	{
		struct engine_trace_event const *event = &thread->ring[sequence & (thread->capacity - 1u)];
		uint32_t const kind = event->kind <= ENGINE_TRACE_EVENT_GPU_ZONE ? event->kind : 0;
		text_format(text, "  %" PRIu64 " ns kind %" PRIu32 " %s %" PRIu64 "\n",
			event->timestamp_nanoseconds, kind, crash_payload_name(kind, event->payload),
			event->argument);
	}
}

void engine_trace_crash_context_write(struct engine_trace const *trace,
	uint32_t events_per_thread, struct engine_trace_text *text)
{
	uint32_t const count = atomic_load_explicit(
		(_Atomic uint32_t *)&trace->thread_count, memory_order_acquire);
	uint32_t index;
	ENGINE_ASSERT(text != NULL);
	ENGINE_ASSERT(count <= trace->config.threads_max);
	text_format(text, "engine trace crash context\n");
	for (index = 0; index < count; ++index)
		crash_thread(&trace->threads[index], events_per_thread, text);
	for (index = 0; index < ENGINE_TRACE_MEMORY_TAG_COUNT; ++index)
	{
		struct engine_trace_memory_tag_state *state =
			(struct engine_trace_memory_tag_state *)&trace->memory[index];
		text_format(text, "memory %s current %" PRId64 " peak %" PRId64 " budget %" PRId64 "\n",
			engine_trace_memory_tag_name((enum engine_trace_memory_tag)index),
			atomic_load(&state->bytes_current), atomic_load(&state->bytes_peak),
			state->bytes_budget);
	}
}
