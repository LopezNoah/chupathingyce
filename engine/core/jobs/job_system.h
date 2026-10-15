#ifndef ENGINE_CORE_JOB_SYSTEM_H
#define ENGINE_CORE_JOB_SYSTEM_H

/* The fixed worker pool that runs job graphs in parallel (ADR 0049). The host
   creates it once; every thread exists from init to shutdown. The thread that
   calls init is the main thread: only it submits, waits, runs main-thread
   jobs, and shuts down. Results equal engine_job_graph_run_sequential for any
   worker count because partitions and ordering never depend on workers. */

#include "job_graph.h"

#include <pthread.h>

struct engine_trace;

/* One ready entry per job that has all predecessors complete. It carries
   what claiming needs, so the locked claim never reads a job record that
   another core wrote last. */
struct engine_job_ready
{
	struct engine_job_graph *graph;
	uint32_t job;
	uint32_t ring;
	uint32_t partition_count;
	uint32_t cost_hint_nanoseconds;
};

struct engine_job_ready_ring
{
	struct engine_job_ready *entries;
	uint32_t capacity;
	uint32_t head;
	uint32_t count;
	uint32_t reserved;  /* Credits held by submitted graphs. */
};

struct engine_job_system_config
{
	/* Background threads; 0 runs everything on the main thread. */
	uint32_t worker_count;
	/* Workers that take only simulation jobs, so background work never starves
	   a tick. Must leave at least one general worker. */
	uint32_t simulation_reserved_workers;
	/* Idle workers spin this many polls before sleeping. */
	uint32_t spin_count;
	/* (ENGINE_JOB_PRIORITY_COUNT + 1) * ready_capacity entries: one ring per
	   priority plus the main-thread ring. */
	struct engine_job_ready *ready_entries;
	uint32_t ready_capacity;
	/* Optional: workers register trace threads and emit a zone per partition. */
	struct engine_trace *trace;
};

struct engine_job_worker
{
	struct engine_job_system *system;
	pthread_t thread;
	uint32_t index;
	uint32_t started;
};

struct engine_job_system
{
	struct engine_job_system_config config;
	pthread_t main_thread;
	/* Guards the rings and their credits. Held for a few ring operations, so
	   it is a spinlock: waiters read it until free instead of sleeping. */
	_Atomic uint32_t ring_lock;
	struct engine_job_ready_ring rings[ENGINE_JOB_PRIORITY_COUNT + 1u];
	/* Each ring's entry count, published for spinning and sleeping threads. */
	_Atomic uint32_t ring_ready[ENGINE_JOB_PRIORITY_COUNT + 1u];
	_Atomic uint32_t graphs_in_flight;
	_Atomic uint32_t stopping;
	_Atomic uint32_t exiting;
	/* Sleeping only: a waker takes the mutex only when a thread sleeps. */
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	_Atomic uint32_t sleepers;
	struct engine_job_worker workers[ENGINE_JOB_WORKERS_MAX];
	uint32_t workers_started;
	_Atomic uint64_t trace_registration_failures;
};

enum engine_job_result engine_job_system_init(struct engine_job_system *system,
	struct engine_job_system_config const *config);
/* Completes all in-flight graphs (running main-thread jobs as needed), then
   joins every worker. Pending work is finished, never abandoned. */
void engine_job_system_shutdown(struct engine_job_system *system);

/* Starts a graph without waiting; READY_EXHAUSTED when the rings cannot hold
   its jobs alongside graphs already in flight. */
enum engine_job_result engine_job_system_submit(struct engine_job_system *system,
	struct engine_job_graph *graph);
/* Helps run main-thread, simulation, and presentation jobs until the graph completes. */
enum engine_job_result engine_job_system_wait(struct engine_job_system *system,
	struct engine_job_graph *graph, struct engine_job_run_report *report);
enum engine_job_result engine_job_system_run(struct engine_job_system *system,
	struct engine_job_graph *graph, struct engine_job_run_report *report);
int engine_job_graph_complete(struct engine_job_graph const *graph);

#endif
