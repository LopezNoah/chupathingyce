/* engine_trace_tool: reads a binary capture (ADR 0051) and prints a structured
   JSON summary, or converts it to the Chrome trace event format that
   chrome://tracing and Perfetto open. Usage:
     engine_trace_tool summary CAPTURE
     engine_trace_tool chrome CAPTURE OUTPUT.json */
#include "../engine/core/trace/trace_capture.h"

#include <stdio.h>
#include <string.h>

/* One million events is 32 MiB in memory: about ten seconds of a busy frame. */
#define TOOL_EVENTS_MAX (1u << 20)
#define TOOL_LOCATIONS_MAX 65536u
#define TOOL_FILE_BYTES_MAX (64u << 20)
#define TOOL_TEXT_BYTES_MAX (256u << 20)

static struct engine_trace_capture_event events[TOOL_EVENTS_MAX];
static struct engine_trace_capture_location locations[TOOL_LOCATIONS_MAX];
static struct engine_trace_capture_slot slots[2u * TOOL_LOCATIONS_MAX];
static struct engine_trace_summary_zone scratch[TOOL_LOCATIONS_MAX];
static unsigned char file_bytes[TOOL_FILE_BYTES_MAX];
static char text_bytes[TOOL_TEXT_BYTES_MAX];
static struct engine_trace_capture capture;

static int tool_fail(char const *message, char const *path)
{
	fprintf(stderr, "engine_trace_tool: %s: %s\n", message, path);
	return 1;
}

static int tool_load(char const *path)
{
	size_t size = 0;
	enum engine_trace_result result;
	ENGINE_ASSERT(path != NULL);
	result = engine_trace_capture_init(&capture, events, TOOL_EVENTS_MAX, locations,
		TOOL_LOCATIONS_MAX, slots, 2u * TOOL_LOCATIONS_MAX);
	ENGINE_ASSERT(result == ENGINE_TRACE_OK);
	if (engine_trace_file_read(path, file_bytes, sizeof(file_bytes), &size) != ENGINE_TRACE_OK)
		return tool_fail("cannot read capture", path);
	if (engine_trace_capture_deserialize(&capture, file_bytes, size) != ENGINE_TRACE_OK)
		return tool_fail("malformed capture", path);
	return 0;
}

static int tool_summary(void)
{
	struct engine_trace_summary summary;
	struct engine_trace_text text;
	if (engine_trace_summary_build(&capture, scratch, TOOL_LOCATIONS_MAX, &summary) !=
		ENGINE_TRACE_OK)
		return tool_fail("cannot summarize", "capture");
	engine_trace_text_init(&text, text_bytes, sizeof(text_bytes));
	if (engine_trace_summary_write_json(&summary, &capture, &text) != ENGINE_TRACE_OK)
		return tool_fail("summary too large", "capture");
	fwrite(text.data, 1, text.length, stdout);
	return 0;
}

static int tool_chrome(char const *output)
{
	struct engine_trace_text text;
	ENGINE_ASSERT(output != NULL);
	engine_trace_text_init(&text, text_bytes, sizeof(text_bytes));
	if (engine_trace_export_chrome(&capture, &text) != ENGINE_TRACE_OK)
		return tool_fail("export too large", output);
	if (engine_trace_file_write(output, (unsigned char const *)text.data, text.length) !=
		ENGINE_TRACE_OK)
		return tool_fail("cannot write", output);
	return 0;
}

int main(int argument_count, char **arguments)
{
	if (argument_count == 3 && strcmp(arguments[1], "summary") == 0)
		return tool_load(arguments[2]) ? 1 : tool_summary();
	if (argument_count == 4 && strcmp(arguments[1], "chrome") == 0)
		return tool_load(arguments[2]) ? 1 : tool_chrome(arguments[3]);
	fprintf(stderr, "usage: engine_trace_tool summary CAPTURE\n"
		"       engine_trace_tool chrome CAPTURE OUTPUT.json\n");
	return 2;
}
