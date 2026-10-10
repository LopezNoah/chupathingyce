#ifndef ENGINE_CORE_JOB_GRAPH_H
#define ENGINE_CORE_JOB_GRAPH_H

/* Per-tick job graphs (ADR 0049). A graph is built in submission order, and
   that order is its sequential meaning: the sequential executor runs jobs and
   partitions in exactly that order and is the correctness oracle. Edges come
   from explicit dependencies, from barriers, and from conflicting declared
   access, so a parallel run can only reorder work that cannot observe the
   difference. All storage is caller-owned and reserved before the first add. */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ENGINE_JOB_INVALID UINT32_MAX
/* ADR 0049 fixes these; records stay near 128 bytes (65,536 records = 8 MiB). */
#define ENGINE_JOB_WORKERS_MAX 64u
#define ENGINE_JOB_DEPENDENCIES_PER_JOB_MAX 16u
#define ENGINE_JOB_ACCESSES_PER_JOB_MAX 16u
#define ENGINE_JOB_ARGUMENT_BYTES_MAX 48u
/* A million partitions of the 20 µs minimum is 20 s of work: far beyond a tick. */
#define ENGINE_JOB_PARTITIONS_PER_JOB_MAX (1u << 20)
#define ENGINE_JOB_JOBS_PER_GRAPH_MAX (1u << 20)

enum engine_job_result
{
	ENGINE_JOB_OK = 0,
	ENGINE_JOB_INVALID_ARGUMENT,
	ENGINE_JOB_UNDECLARED_ACCESS,
	ENGINE_JOB_JOBS_EXHAUSTED,
	ENGINE_JOB_EDGES_EXHAUSTED,
	ENGINE_JOB_ACCESSES_EXHAUSTED,
	ENGINE_JOB_RESOURCES_EXHAUSTED,
	ENGINE_JOB_READY_EXHAUSTED,
	ENGINE_JOB_OVERFLOWED,   /* An earlier add exhausted the graph; run sequentially. */
	ENGINE_JOB_FAILED,       /* A job returned failure. */
	ENGINE_JOB_SHUTTING_DOWN,
	ENGINE_JOB_PLATFORM_FAILED /* Thread or synchronization creation failed. */
};

enum engine_job_priority
{
	ENGINE_JOB_PRIORITY_SIMULATION = 0,
	ENGINE_JOB_PRIORITY_PRESENTATION,
	ENGINE_JOB_PRIORITY_STREAMING,
	ENGINE_JOB_PRIORITY_BACKGROUND,
	ENGINE_JOB_PRIORITY_COUNT
};

enum engine_job_resource_kind
{
	ENGINE_JOB_RESOURCE_COMPONENT = 1,   /* identifier: ECS component ID */
	ENGINE_JOB_RESOURCE_SUBSYSTEM = 2,   /* identifier: subsystem context */
	ENGINE_JOB_RESOURCE_EVENT_BUFFER = 3,
	ENGINE_JOB_RESOURCE_HANDLE = 4,      /* identifier: render or asset handle */
	ENGINE_JOB_RESOURCE_USER = 5
};

enum engine_job_access_mode
{
	ENGINE_JOB_ACCESS_READ = 1,
	ENGINE_JOB_ACCESS_WRITE = 2
};

enum engine_job_flag
{
	/* Platform calls that need the OS main thread (ADR 0049); no hidden locks. */
	ENGINE_JOB_FLAG_MAIN_THREAD = 1u
};

enum engine_job_graph_mode
{
	ENGINE_JOB_GRAPH_DEFERRED = 0, /* Record jobs; run them later. */
	ENGINE_JOB_GRAPH_DIRECT = 1    /* Sequential fallback: run each job as it is added. */
};

struct engine_job_access
{
	uint32_t kind;
	uint32_t mode;
	uint64_t identifier;
};

/* Partition boundaries depend only on item_count and partition_size, never on
   the worker count, so results are identical for 1 or N workers. A job sees no
   worker identity, clock, or completion order. */
struct engine_job_partition
{
	uint32_t index;
	uint32_t count;
	uint64_t item_begin;
	uint64_t item_end;
};

typedef int (*engine_job_fn)(void const *argument, struct engine_job_partition const *partition);

struct engine_job_description
{
	char const *name;  /* Static lifetime; names trace zones. */
	engine_job_fn execute;
	void const *argument;  /* Copied into the record; immutable afterwards. */
	uint32_t argument_size;
	struct engine_job_access const *accesses;
	uint32_t access_count;
	uint32_t const *dependencies;  /* Handles of earlier jobs in this graph. */
	uint32_t dependency_count;
	/* item_count 0 means one partition with an empty range. */
	uint64_t item_count;
	uint32_t partition_size;
	uint32_t priority;
	uint32_t flags;
	uint32_t cost_hint_nanoseconds;
	/* The query descriptors this job iterates. Every component it reads must
	   be declared, and every component it writes must be declared as a write. */
	uint64_t component_read_mask;
	uint64_t component_write_mask;
};

struct engine_job_record
{
	char const *name;
	engine_job_fn execute;
	uint64_t item_count;
	uint32_t partition_size;
	uint32_t partition_count;
	uint32_t access_first;
	uint32_t access_count;
	uint32_t successor_head;
	uint32_t last_successor;
	uint32_t predecessor_count;
	uint32_t cost_hint_nanoseconds;
	uint16_t argument_size;
	uint8_t priority;
	uint8_t flags;
	_Atomic uint32_t next_partition;
	_Atomic uint32_t completed_partitions;
	_Atomic uint32_t pending_predecessors;
	_Alignas(16) unsigned char argument[ENGINE_JOB_ARGUMENT_BYTES_MAX];
};

struct engine_job_edge
{
	uint32_t target;
	uint32_t next;
};

struct engine_job_access_slot
{
	struct engine_job_access access;
	uint32_t resource;
	uint32_t padding;
};

struct engine_job_resource
{
	uint64_t identifier;
	uint32_t kind;
	uint32_t generation;  /* Slots from an older generation are empty. */
	uint32_t last_writer;
	uint32_t reader_head;
	/* Checked-build exclusion: -1 one writer, >0 readers, 0 idle. */
	_Atomic int32_t occupancy;
	uint32_t padding;
};

struct engine_job_reader
{
	uint32_t job;
	uint32_t next;
};

/* Downstream (ChupathingyCE): why an edge exists, for trace export and graph
   visualization. An edge between one pair of jobs is stored once; when two
   reasons order the same pair, the first one recorded is kept. */
enum engine_job_edge_kind
{
	ENGINE_JOB_EDGE_EXPLICIT = 1, /* description->dependencies */
	ENGINE_JOB_EDGE_BARRIER = 2,  /* engine_job_graph_barrier */
	ENGINE_JOB_EDGE_RAW = 3,      /* read after write */
	ENGINE_JOB_EDGE_WAR = 4,      /* write after read */
	ENGINE_JOB_EDGE_WAW = 5       /* write after write */
};

/* Downstream (ChupathingyCE): optional per-job timing. Diagnostic only: jobs
   never see it, so it cannot change results. Times come from clock_ns. */
struct engine_job_profile_sample
{
	_Atomic uint64_t ready_ns;  /* all predecessors complete */
	_Atomic uint64_t start_ns;  /* first partition began (UINT64_MAX: never) */
	_Atomic uint64_t end_ns;    /* last partition ended */
	_Atomic uint64_t busy_ns;   /* sum of partition durations */
	_Atomic uint32_t thread_mask_low; /* bit per thread slot 0..31 that ran it */
	uint32_t padding;
};

struct engine_job_profile
{
	uint64_t (*clock_ns)(void);
	struct engine_job_profile_sample *samples; /* jobs_capacity entries */
	uint64_t submit_ns;
	uint64_t complete_ns;
	/* Slot 0 is the main (submitting) thread, slot 1 + n is worker n. */
	_Atomic uint64_t thread_busy_ns[ENGINE_JOB_WORKERS_MAX + 1u];
};

/* The profiling slot of the calling thread; set by the pool for workers. */
extern _Thread_local uint32_t engine_job_thread_slot;

struct engine_job_graph_config
{
	struct engine_job_record *jobs;
	uint32_t jobs_capacity;
	struct engine_job_edge *edges;
	uint32_t edges_capacity;
	struct engine_job_access_slot *accesses;
	uint32_t accesses_capacity;
	struct engine_job_resource *resources;
	uint32_t resources_capacity;  /* Power of two; at most half is used. */
	struct engine_job_reader *readers;
	uint32_t readers_capacity;
	/* Downstream, optional: edges_capacity kinds (enum engine_job_edge_kind). */
	uint8_t *edge_kinds;
	/* Downstream, optional: per-job timing for trace export. */
	struct engine_job_profile *profile;
};

struct engine_job_system;

struct engine_job_graph
{
	struct engine_job_graph_config config;
	uint32_t job_count;
	uint32_t edge_count;
	uint32_t access_count;
	uint32_t reader_count;
	uint32_t resource_count;
	uint32_t generation;
	uint32_t segment_first;
	uint32_t barrier_job;
	uint32_t mode;
	enum engine_job_result overflow;  /* First exhaustion since reset, or OK. */
	uint32_t ready_count[ENGINE_JOB_PRIORITY_COUNT + 1u];  /* Last slot: main thread. */
	uint64_t partition_total;
	/* Run state, owned by an executor between submit and completion. */
	struct engine_job_system *system;
	_Atomic uint32_t running;
	_Atomic uint32_t complete;
	_Atomic uint32_t remaining_jobs;
	_Atomic uint32_t failed;
	_Atomic uint32_t first_failed_job;
	_Atomic uint64_t partitions_executed;
	_Atomic uint64_t partitions_skipped;
	_Atomic uint64_t partitions_on_workers;
};

struct engine_job_run_report
{
	enum engine_job_result result;
	uint32_t jobs_completed;
	uint32_t failed_job;  /* Lowest failing job handle, or ENGINE_JOB_INVALID. */
	uint64_t partitions_executed;
	uint64_t partitions_skipped;  /* Not run because an earlier partition failed. */
	uint64_t partitions_on_workers;
	uint64_t partitions_on_caller;
};

enum engine_job_result engine_job_graph_init(struct engine_job_graph *graph,
	struct engine_job_graph_config const *config);
/* Discards all jobs. The graph must not be running. */
void engine_job_graph_reset(struct engine_job_graph *graph, enum engine_job_graph_mode mode);
/* Records a job (deferred) or runs it at once (direct). On exhaustion the graph
   is marked overflowed and must be reset; the caller falls back to a direct
   rebuild, which ADR 0049 requires to be reported. */
enum engine_job_result engine_job_graph_add(struct engine_job_graph *graph,
	struct engine_job_description const *description, uint32_t *job);
/* Every later job runs after every earlier job: structural barriers (ADR 0017)
   and event consumer phases. */
enum engine_job_result engine_job_graph_barrier(struct engine_job_graph *graph);
uint32_t engine_job_partition_count(uint64_t item_count, uint32_t partition_size);

/* The correctness oracle: every job and partition in submission order on the
   calling thread. */
enum engine_job_result engine_job_graph_run_sequential(struct engine_job_graph *graph,
	struct engine_job_run_report *report);

/* Shared by executors; not for callers. */
enum engine_job_partition_outcome
{
	ENGINE_JOB_PARTITION_EXECUTED = 0,
	ENGINE_JOB_PARTITION_SKIPPED = 1,  /* An earlier partition failed. */
	ENGINE_JOB_PARTITION_EMPTY = 2     /* A barrier: nothing to run. */
};
void engine_job_graph_prepare_run(struct engine_job_graph *graph, struct engine_job_system *system);
enum engine_job_partition_outcome engine_job_graph_execute_partition(
	struct engine_job_graph *graph, uint32_t job, uint32_t partition);
/* Executors count partitions per thread and add the tallies once per batch, so
   the shared counters are not written for every partition. */
void engine_job_graph_add_tallies(struct engine_job_graph *graph, uint64_t executed,
	uint64_t skipped, uint64_t on_workers);
/* Downstream: records the ready time of a job in the optional profile. */
void engine_job_graph_profile_ready(struct engine_job_graph *graph, uint32_t job);
void engine_job_graph_acquire(struct engine_job_graph *graph, uint32_t job);
void engine_job_graph_release(struct engine_job_graph *graph, uint32_t job);
void engine_job_graph_finish_report(struct engine_job_graph *graph,
	struct engine_job_run_report *report);

#endif
