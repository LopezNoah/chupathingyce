#include "../engine/core/trace/trace.h"

ENGINE_TRACE_LOCATION(trace_probe_location, "trace_probe");

void trace_instrumentation_probe(struct engine_trace_thread *thread)
{
#if defined(ENGINE_TRACE_ENABLED)
	engine_trace_zone_begin(thread, &trace_probe_location, 0);
	engine_trace_zone_end(thread, &trace_probe_location);
#else
	(void)thread;
#endif
}
