#include "job_phase.h"

#include "../assert.h"

#include <string.h>

static int job_phase_fallback(struct engine_job_phase *phase)
{
	int built;
	ENGINE_ASSERT(phase->graph->overflow != ENGINE_JOB_OK);
	ENGINE_ASSERT(phase->fallback_count < UINT64_MAX);
	phase->last_overflow = phase->graph->overflow;
	phase->fallback_count += 1;
	engine_job_graph_reset(phase->graph, ENGINE_JOB_GRAPH_DIRECT);
	built = phase->build(phase->build_context, phase->graph);
	memset(&phase->last_report, 0, sizeof(phase->last_report));
	phase->last_report.result = atomic_load(&phase->graph->failed) ? ENGINE_JOB_FAILED :
		ENGINE_JOB_OK;
	phase->last_report.jobs_completed = phase->graph->job_count;
	phase->last_report.partitions_executed = atomic_load(&phase->graph->partitions_executed);
	phase->last_report.partitions_on_caller = phase->last_report.partitions_executed;
	phase->last_report.failed_job = atomic_load(&phase->graph->first_failed_job);
	engine_job_graph_reset(phase->graph, ENGINE_JOB_GRAPH_DEFERRED);
	return built && phase->last_report.result == ENGINE_JOB_OK;
}

int engine_job_phase_execute(struct engine_job_phase *phase)
{
	enum engine_job_result result;
	int built;
	if (!phase || !phase->build || !phase->graph)
		return 0;
	ENGINE_ASSERT(atomic_load(&phase->graph->running) == 0);
	engine_job_graph_reset(phase->graph, ENGINE_JOB_GRAPH_DEFERRED);
	built = phase->build(phase->build_context, phase->graph);
	if (phase->graph->overflow != ENGINE_JOB_OK)
		return job_phase_fallback(phase);
	if (!built)
		return 0;
	phase->last_overflow = ENGINE_JOB_OK;
	result = ENGINE_JOB_SHUTTING_DOWN;
	if (phase->system)
		result = engine_job_system_run(phase->system, phase->graph, &phase->last_report);
	/* Refused before anything ran: the oracle runs the same graph once. */
	if (!phase->system || result == ENGINE_JOB_READY_EXHAUSTED || result == ENGINE_JOB_SHUTTING_DOWN)
	{
		if (phase->system)
			phase->sequential_count += 1;
		result = engine_job_graph_run_sequential(phase->graph, &phase->last_report);
	}
	ENGINE_ASSERT(result != ENGINE_JOB_OVERFLOWED);
	return result == ENGINE_JOB_OK;
}
