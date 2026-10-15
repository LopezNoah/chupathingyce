#ifndef ENGINE_CORE_JOB_PHASE_H
#define ENGINE_CORE_JOB_PHASE_H

/* Runs one phase as a job graph (ADR 0049). Ported from LopezNoah/engine
   engine/simulation/job_phase.h with the ECS world and host tick replaced by
   an opaque context, so ChupathingyCE phases can use it.

   The builder only records jobs; it must not mutate state, because when the
   graph's arenas are exhausted the phase rebuilds it in direct mode, running
   each job in submission order on the calling thread. That fallback produces
   the same results as the sequential oracle and is counted, never silent.

   Downstream addition: when the pool cannot take the graph (READY_EXHAUSTED
   or SHUTTING_DOWN) nothing has run yet, so the already-built graph runs on
   the sequential oracle instead, also counted. A job that fails is reported
   and the phase is never rerun: its side effects may be partial. */

#include "job_system.h"

typedef int (*engine_job_phase_build_fn)(void *context, struct engine_job_graph *graph);

struct engine_job_phase
{
	engine_job_phase_build_fn build;
	void *build_context;
	struct engine_job_graph *graph;
	struct engine_job_system *system; /* Null runs the sequential executor. */
	uint64_t fallback_count;          /* direct rebuilds after overflow */
	uint64_t sequential_count;        /* pool refused a built graph */
	enum engine_job_result last_overflow;
	struct engine_job_run_report last_report;
};

int engine_job_phase_execute(struct engine_job_phase *phase);

#endif
