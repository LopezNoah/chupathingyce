#ifndef HALO_JOBS_H
#define HALO_JOBS_H

/* The game's door to the job graph (engine/core/jobs, ADR 0049; implemented
   by port/linux/src/posix_jobs.c). Plain C89 types only: game sources are
   gnu89 and, on 64-bit builds, compiled with the Xbox's ABI, so nothing here
   may use long, atomics or engine structures.

   Everything runs on the thread that first calls halo_jobs_mode() (the game's
   main thread), and every phase completes before halo_jobs_phase_run returns:
   no job outlives the frame stage that built it, so map changes, device
   resets and shutdown never meet a job in flight.

   Runtime switch (environment):
     HALO_JOBS=off          legacy code paths only (no graph is built)
     HALO_JOBS=sequential   graphs run on the sequential oracle
     HALO_JOBS=parallel     graphs run on the worker pool (default)
     HALO_JOB_WORKERS=N     worker count (default from the CPU's performance cores)
     HALO_JOB_TRACE=path    write the job report (JSON) and graph (DOT) there */

#include <stdint.h>

#define HALO_JOB_INVALID 0xFFFFFFFFu
#define HALO_JOB_ARGUMENT_BYTES_MAX 40u
#define HALO_JOB_ACCESSES_MAX 8u
#define HALO_JOB_DEPENDENCIES_MAX 8u

enum halo_jobs_mode
{
	HALO_JOBS_OFF = 0,
	HALO_JOBS_SEQUENTIAL = 1,
	HALO_JOBS_PARALLEL = 2
};

enum halo_job_phase
{
	HALO_JOB_PHASE_PRESENTATION = 0, /* interpolation and render preparation */
	HALO_JOB_PHASE_TEST,             /* tools/test_halo_jobs.c */
	HALO_JOB_PHASE_COUNT
};

/* engine_job_priority, in order */
enum halo_job_priority
{
	HALO_JOB_PRIORITY_SIMULATION = 0,
	HALO_JOB_PRIORITY_PRESENTATION,
	HALO_JOB_PRIORITY_STREAMING,
	HALO_JOB_PRIORITY_BACKGROUND
};

enum halo_job_access_mode
{
	HALO_JOB_READ = 1,
	HALO_JOB_WRITE = 2 /* also: produce, invalidate (see halo_job_resource_invalidate) */
};

/* Conservative, subsystem- or buffer-sized resources (ENGINE_JOB_RESOURCE_SUBSYSTEM).
   Two jobs that name no common resource with a write may run at once, so a job
   must name every shared structure it touches. Distinct names never imply
   distinct memory: name the containing structure when unsure. */
enum halo_job_resource
{
	HALO_JOB_RESOURCE_SIMULATION_OBJECTS = 1, /* the object data array: main thread only */
	HALO_JOB_RESOURCE_INTERPOLATION_SNAPSHOTS, /* render_interpolation.c's two snapshots */
	HALO_JOB_RESOURCE_INTERPOLATION_ROTATIONS, /* ... their quaternions, found lazily */
	HALO_JOB_RESOURCE_PRESENTATION_POSES,      /* ... the blend drawn this frame */
	HALO_JOB_RESOURCE_RENDER_PREPARATION,
	HALO_JOB_RESOURCE_GRAPHICS_DEVICE,         /* OpenGL context: main-thread jobs only */
	HALO_JOB_RESOURCE_FIRST_USER = 64
};

struct halo_job_partition
{
	uint32_t index;
	uint32_t count;
	uint64_t item_begin;
	uint64_t item_end;
};

/* Returns 0 on failure, which fails the graph; the phase is not rerun. */
typedef int (*halo_job_fn)(void const *argument, struct halo_job_partition const *partition);

struct halo_job_access
{
	uint32_t mode;
	uint32_t resource; /* enum halo_job_resource, or a handle's slot */
};

struct halo_job_description
{
	char const *name; /* static lifetime */
	halo_job_fn execute;
	void const *argument; /* copied: at most HALO_JOB_ARGUMENT_BYTES_MAX */
	uint32_t argument_size;
	struct halo_job_access const *accesses;
	uint32_t access_count;
	uint32_t const *dependencies; /* handles returned by earlier halo_job_add */
	uint32_t dependency_count;
	uint64_t item_count; /* 0: one partition with an empty range */
	uint32_t partition_size;
	uint32_t priority;
	uint32_t main_thread; /* nonzero: only the main thread runs it (OpenGL) */
	uint32_t cost_hint_nanoseconds; /* per partition */
};

struct halo_job_graph;
/* Records jobs only; it may run twice (overflow rebuilds in direct mode). */
typedef int (*halo_job_build_fn)(void *context, struct halo_job_graph *graph);

int halo_jobs_mode(void);
uint32_t halo_jobs_worker_count(void);
/* HALO_JOB_INVALID when the graph overflowed or the description is invalid. */
uint32_t halo_job_add(struct halo_job_graph *graph, struct halo_job_description const *description);
int halo_job_barrier(struct halo_job_graph *graph);
/* Builds and runs one phase to completion on the calling (main) thread. */
int halo_jobs_phase_run(enum halo_job_phase phase, halo_job_build_fn build, void *context);
/* Whether a phase is between build and completion (ownership assertions). */
int halo_jobs_phase_active(enum halo_job_phase phase);
/* Overflow rebuilds plus pool refusals, since start. */
uint32_t halo_jobs_phase_fallbacks(enum halo_job_phase phase);
void halo_jobs_shutdown(void);

/* Compact generational handles for buffers whose storage can be replaced.
   A job captures the handle it was built against and checks it when it runs;
   a write to the slot (halo_job_resource_invalidate) orders the replacement
   after every earlier reader. */
uint32_t halo_job_resource_handle(uint32_t slot);
uint32_t halo_job_resource_invalidate(uint32_t slot); /* main thread, no phase active */
int halo_job_resource_current(uint32_t handle);

#endif
