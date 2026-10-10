/* The job graph's platform adapter (halo_jobs.h): one fixed worker pool,
   static per-phase graph arenas, CPU-aware worker count, fallback reporting
   and trace export. The scheduler is LopezNoah/engine's (engine/core/jobs).

   Builds that compile engine/core/jobs define HALO_JOBS_ENABLED (the Linux
   and 64-bit builds: tools/linux_build.py, tools/lp64_build.py). The others
   (Windows, Android) get the same API with every phase reported unavailable,
   so their callers keep the legacy sequential paths. */

#include "halo_jobs.h"

#if defined(HALO_JOBS_ENABLED)

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../engine/core/assert.h"
#include "../../../engine/core/jobs/job_phase.h"

#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif

/* ---------- limits */

/* Phases are a handful of jobs (one partitioned job per stage), so these
   arenas are small; overflow is reported and falls back, never allocates. */
#define JOBS_PER_PHASE 64u
#define EDGES_PER_PHASE 256u
#define ACCESSES_PER_PHASE 256u
#define RESOURCES_PER_PHASE 128u /* power of two; half usable */
#define READERS_PER_PHASE 256u
#define READY_CAPACITY 128u /* per ring, shared by all phases in flight */
#define WALL_SAMPLES 4096u  /* per phase, for percentiles */
/* measured (docs/jobs.md): the presentation phase's tail is best at 2 and
   worse at 4 (wake-up latency against a ~20 us phase) */
#define DEFAULT_WORKERS_MAX 2u
#define SPIN_COUNT 4000u    /* ~40 us of polling before a worker sleeps */
#define RESOURCE_HANDLES_MAX 256u
#define REPORT_EVERY_RUNS 1800u

struct halo_job_graph
{
	struct engine_job_graph graph;
};

struct job_phase_storage
{
	struct halo_job_graph graph;
	struct engine_job_record jobs[JOBS_PER_PHASE];
	struct engine_job_edge edges[EDGES_PER_PHASE];
	uint8_t edge_kinds[EDGES_PER_PHASE];
	struct engine_job_access_slot accesses[ACCESSES_PER_PHASE];
	struct engine_job_resource resources[RESOURCES_PER_PHASE];
	struct engine_job_reader readers[READERS_PER_PHASE];
	struct engine_job_profile profile;
	struct engine_job_profile_sample samples[JOBS_PER_PHASE];
	struct engine_job_phase phase;
	/* statistics */
	uint64_t runs;
	uint64_t failures;
	uint64_t build_ns_total;
	uint64_t wall_ns_total;
	uint64_t busy_ns_total;
	uint64_t wall_ns[WALL_SAMPLES];
	uint32_t wall_count;
	int active;
};

/* A job's argument: the game's function and its argument bytes. */
struct job_trampoline
{
	halo_job_fn execute;
	unsigned char argument[HALO_JOB_ARGUMENT_BYTES_MAX];
};

ENGINE_STATIC_ASSERT(sizeof(struct job_trampoline) <= ENGINE_JOB_ARGUMENT_BYTES_MAX,
	"a game job's argument fits a job record");
ENGINE_STATIC_ASSERT(HALO_JOB_ACCESSES_MAX <= ENGINE_JOB_ACCESSES_PER_JOB_MAX, "access limit");
ENGINE_STATIC_ASSERT(HALO_JOB_DEPENDENCIES_MAX <= ENGINE_JOB_DEPENDENCIES_PER_JOB_MAX, "dependency limit");

static struct job_phase_storage job_phases[HALO_JOB_PHASE_COUNT];
static struct engine_job_ready job_ready_entries[(ENGINE_JOB_PRIORITY_COUNT + 1u) * READY_CAPACITY];
static struct engine_job_system job_system;
static int job_mode = -1;
static int job_system_started;
static uint32_t job_workers;
static uint32_t job_performance_cores;
static char const *job_trace_path;
static uint16_t job_resource_generations[RESOURCE_HANDLES_MAX];

static char const *const job_phase_names[HALO_JOB_PHASE_COUNT] = { "presentation", "test" };

static uint64_t job_clock_ns(void)
{
#if defined(__APPLE__)
	/* (CLOCK_MONOTONIC is microseconds on macOS) */
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
#endif
}

/* ---------- CPU topology */

/* Cores worth giving work to: performance cores where the system tells them
   apart (Apple silicon's perflevel0, Intel hybrid's cpu_core), physical cores
   where SMT doubles the logical count, and never more than the process may
   run on. */
static uint32_t job_performance_core_count(void)
{
	long online = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t cores = online > 0 ? (uint32_t)online : 1u;
#if defined(__APPLE__)
	{
		int value = 0;
		size_t size = sizeof(value);
		if (sysctlbyname("hw.perflevel0.physicalcpu", &value, &size, NULL, 0) == 0 && value > 0)
			cores = (uint32_t)value;
		else if (sysctlbyname("hw.physicalcpu", &value, &size, NULL, 0) == 0 && value > 0)
			cores = (uint32_t)value;
	}
#elif defined(__linux__)
	{
		cpu_set_t set;
		FILE *file;
		if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0)
			cores = (uint32_t)CPU_COUNT(&set);
		/* Intel hybrid: the performance cores' list, as "0-15" */
		file = fopen("/sys/devices/cpu_core/cpus", "r");
		if (file)
		{
			unsigned first, last;
			if (fscanf(file, "%u-%u", &first, &last) == 2 && last >= first && last - first + 1u < cores)
				cores = last - first + 1u;
			fclose(file);
		}
		file = fopen("/sys/devices/system/cpu/smt/active", "r");
		if (file)
		{
			int active = 0;
			if (fscanf(file, "%d", &active) == 1 && active == 1 && cores > 1)
				cores /= 2;
			fclose(file);
		}
	}
#endif
	return cores ? cores : 1u;
}

/* The main thread runs the game and the OpenGL driver's own threads run
   beside it: leave both a core, and cap the pool where the presentation
   workload stops scaling (DEFAULT_WORKERS_MAX). */
static uint32_t job_default_workers(uint32_t cores)
{
	uint32_t workers = cores > 2u ? cores - 2u : 0u;
	return workers > DEFAULT_WORKERS_MAX ? DEFAULT_WORKERS_MAX : workers;
}

/* ---------- lifetime */

static void job_phase_storage_init(struct job_phase_storage *storage)
{
	struct engine_job_graph_config config;
	enum engine_job_result result;
	memset(&config, 0, sizeof(config));
	config.jobs = storage->jobs;
	config.jobs_capacity = JOBS_PER_PHASE;
	config.edges = storage->edges;
	config.edges_capacity = EDGES_PER_PHASE;
	config.edge_kinds = storage->edge_kinds;
	config.accesses = storage->accesses;
	config.accesses_capacity = ACCESSES_PER_PHASE;
	config.resources = storage->resources;
	config.resources_capacity = RESOURCES_PER_PHASE;
	config.readers = storage->readers;
	config.readers_capacity = READERS_PER_PHASE;
	storage->profile.clock_ns = job_clock_ns;
	storage->profile.samples = storage->samples;
	config.profile = &storage->profile;
	result = engine_job_graph_init(&storage->graph.graph, &config);
	ENGINE_ASSERT(result == ENGINE_JOB_OK);
	storage->phase.graph = &storage->graph.graph;
}

static void job_initialize(void)
{
	char const *mode = getenv("HALO_JOBS");
	char const *workers = getenv("HALO_JOB_WORKERS");
	struct engine_job_system_config config;
	uint32_t index;

	job_mode = HALO_JOBS_PARALLEL;
	if (mode && (!strcmp(mode, "off") || !strcmp(mode, "0") || !strcmp(mode, "legacy")))
		job_mode = HALO_JOBS_OFF;
	else if (mode && !strcmp(mode, "sequential"))
		job_mode = HALO_JOBS_SEQUENTIAL;
	job_trace_path = getenv("HALO_JOB_TRACE");
	if (job_trace_path && !job_trace_path[0])
		job_trace_path = NULL;
	job_performance_cores = job_performance_core_count();
	job_workers = job_default_workers(job_performance_cores);
	if (workers && workers[0])
	{
		long requested = strtol(workers, NULL, 10);
		job_workers = requested < 0 ? 0u : requested > (long)ENGINE_JOB_WORKERS_MAX ?
			ENGINE_JOB_WORKERS_MAX : (uint32_t)requested;
	}
	for (index = 0; index < HALO_JOB_PHASE_COUNT; index++)
		job_phase_storage_init(&job_phases[index]);
	if (job_mode == HALO_JOBS_PARALLEL)
	{
		memset(&config, 0, sizeof(config));
		config.worker_count = job_workers;
		/* No simulation jobs exist yet: reserving a worker would idle it. */
		config.simulation_reserved_workers = 0;
		config.spin_count = SPIN_COUNT;
		config.ready_entries = job_ready_entries;
		config.ready_capacity = READY_CAPACITY;
		if (engine_job_system_init(&job_system, &config) == ENGINE_JOB_OK)
		{
			job_system_started = 1;
			for (index = 0; index < HALO_JOB_PHASE_COUNT; index++)
				job_phases[index].phase.system = &job_system;
		}
		else
		{
			platform_log("jobs: could not start %u workers; running graphs sequentially", job_workers);
			job_mode = HALO_JOBS_SEQUENTIAL;
		}
	}
	platform_log("jobs: %s, %u workers (%u performance cores), %u KiB of graph arenas",
		job_mode == HALO_JOBS_OFF ? "off (legacy paths)" :
		job_mode == HALO_JOBS_SEQUENTIAL ? "sequential oracle" : "parallel",
		job_mode == HALO_JOBS_PARALLEL ? job_workers : 0u, job_performance_cores,
		(unsigned)((sizeof(job_phases) + sizeof(job_ready_entries)) / 1024u));
	atexit(halo_jobs_shutdown);
}

int halo_jobs_mode(void)
{
	if (job_mode < 0)
		job_initialize();
	return job_mode;
}

uint32_t halo_jobs_worker_count(void)
{
	halo_jobs_mode();
	return job_system_started ? job_workers : 0u;
}

/* ---------- building */

static int job_trampoline_execute(void const *argument, struct engine_job_partition const *partition)
{
	struct job_trampoline const *trampoline = argument;
	struct halo_job_partition range;
	range.index = partition->index;
	range.count = partition->count;
	range.item_begin = partition->item_begin;
	range.item_end = partition->item_end;
	return trampoline->execute(trampoline->argument, &range);
}

uint32_t halo_job_add(struct halo_job_graph *graph, struct halo_job_description const *description)
{
	struct engine_job_description engine;
	struct engine_job_access accesses[HALO_JOB_ACCESSES_MAX];
	struct job_trampoline trampoline;
	uint32_t index;
	uint32_t job = HALO_JOB_INVALID;

	if (!graph || !description || !description->execute ||
		description->argument_size > HALO_JOB_ARGUMENT_BYTES_MAX ||
		description->access_count > HALO_JOB_ACCESSES_MAX ||
		description->dependency_count > HALO_JOB_DEPENDENCIES_MAX)
		return HALO_JOB_INVALID;
	memset(&trampoline, 0, sizeof(trampoline));
	trampoline.execute = description->execute;
	if (description->argument_size)
		memcpy(trampoline.argument, description->argument, description->argument_size);
	for (index = 0; index < description->access_count; index++)
	{
		accesses[index].kind = ENGINE_JOB_RESOURCE_SUBSYSTEM;
		accesses[index].mode = description->accesses[index].mode == HALO_JOB_WRITE ?
			ENGINE_JOB_ACCESS_WRITE : ENGINE_JOB_ACCESS_READ;
		accesses[index].identifier = description->accesses[index].resource;
	}
	memset(&engine, 0, sizeof(engine));
	engine.name = description->name;
	engine.execute = job_trampoline_execute;
	engine.argument = &trampoline;
	engine.argument_size = sizeof(trampoline);
	engine.accesses = accesses;
	engine.access_count = description->access_count;
	engine.dependencies = description->dependencies;
	engine.dependency_count = description->dependency_count;
	engine.item_count = description->item_count;
	engine.partition_size = description->partition_size;
	engine.priority = description->priority;
	engine.flags = description->main_thread ? ENGINE_JOB_FLAG_MAIN_THREAD : 0u;
	engine.cost_hint_nanoseconds = description->cost_hint_nanoseconds;
	if (engine_job_graph_add(&graph->graph, &engine, &job) != ENGINE_JOB_OK)
		return HALO_JOB_INVALID;
	return job;
}

int halo_job_barrier(struct halo_job_graph *graph)
{
	return graph && engine_job_graph_barrier(&graph->graph) == ENGINE_JOB_OK;
}

/* ---------- running */

struct job_build_context
{
	halo_job_build_fn build;
	void *context;
	struct halo_job_graph *graph;
};

static int job_build_adapter(void *context, struct engine_job_graph *graph)
{
	struct job_build_context *build = context;
	ENGINE_ASSERT(graph == &build->graph->graph);
	return build->build(build->context, build->graph);
}

static void job_report_write(void);

int halo_jobs_phase_run(enum halo_job_phase phase, halo_job_build_fn build, void *context)
{
	struct job_phase_storage *storage;
	struct job_build_context adapter;
	uint64_t begin, end;
	int succeeded;

	if (halo_jobs_mode() == HALO_JOBS_OFF || (unsigned)phase >= HALO_JOB_PHASE_COUNT || !build)
		return 0;
	storage = &job_phases[phase];
	/* phases do not nest: a job never builds a graph */
	ENGINE_ASSERT(!storage->active);
	storage->active = 1;
	adapter.build = build;
	adapter.context = context;
	adapter.graph = &storage->graph;
	storage->phase.build = job_build_adapter;
	storage->phase.build_context = &adapter;
	begin = job_clock_ns();
	succeeded = engine_job_phase_execute(&storage->phase);
	end = job_clock_ns();
	storage->active = 0;
	storage->runs++;
	if (!succeeded && storage->failures++ == 0)
		platform_log("jobs: the %s phase failed (first time; HALO_JOB_TRACE reports later ones)",
			job_phase_names[phase]);
	if (storage->phase.last_overflow == ENGINE_JOB_OK && storage->profile.submit_ns >= begin)
	{
		uint32_t job;
		storage->build_ns_total += storage->profile.submit_ns - begin;
		for (job = 0; job < storage->graph.graph.job_count; job++)
			storage->busy_ns_total += atomic_load(&storage->samples[job].busy_ns);
	}
	storage->wall_ns_total += end - begin;
	storage->wall_ns[storage->wall_count++ % WALL_SAMPLES] = end - begin;
	if (job_trace_path && storage->runs % REPORT_EVERY_RUNS == 0)
		job_report_write();
	return succeeded;
}

int halo_jobs_phase_active(enum halo_job_phase phase)
{
	return (unsigned)phase < HALO_JOB_PHASE_COUNT && job_phases[phase].active;
}

uint32_t halo_jobs_phase_fallbacks(enum halo_job_phase phase)
{
	if ((unsigned)phase >= HALO_JOB_PHASE_COUNT)
		return 0;
	return (uint32_t)(job_phases[phase].phase.fallback_count + job_phases[phase].phase.sequential_count);
}

void halo_jobs_shutdown(void)
{
	if (job_mode < 0)
		return;
	if (job_trace_path)
		job_report_write();
	/* Only the main thread may join; every phase completed before it
	   returned, so a process ended elsewhere leaves only sleeping workers. */
	if (job_system_started && pthread_equal(pthread_self(), job_system.main_thread))
	{
		uint32_t index;
		engine_job_system_shutdown(&job_system);
		job_system_started = 0;
		for (index = 0; index < HALO_JOB_PHASE_COUNT; index++)
			job_phases[index].phase.system = NULL;
	}
}

/* ---------- resource handles */

uint32_t halo_job_resource_handle(uint32_t slot)
{
	ENGINE_ASSERT(slot < RESOURCE_HANDLES_MAX);
	return slot | ((uint32_t)job_resource_generations[slot] << 16);
}

uint32_t halo_job_resource_invalidate(uint32_t slot)
{
	uint32_t index;
	ENGINE_ASSERT(slot < RESOURCE_HANDLES_MAX);
	/* storage is replaced only while no job can hold the old handle */
	for (index = 0; index < HALO_JOB_PHASE_COUNT; index++)
		ENGINE_ASSERT(!job_phases[index].active);
	job_resource_generations[slot]++;
	return halo_job_resource_handle(slot);
}

int halo_job_resource_current(uint32_t handle)
{
	uint32_t const slot = handle & 0xFFFFu;
	return slot < RESOURCE_HANDLES_MAX && (handle >> 16) == job_resource_generations[slot];
}

/* ---------- report */

static int job_u64_compare(void const *a, void const *b)
{
	uint64_t x = *(uint64_t const *)a, y = *(uint64_t const *)b;
	return (x > y) - (x < y);
}

static unsigned job_bit_count(uint32_t bits)
{
	unsigned count = 0;
	for (; bits; bits &= bits - 1u)
		count++;
	return count;
}

static char const *job_edge_kind_name(uint8_t kind)
{
	switch (kind)
	{
	case ENGINE_JOB_EDGE_EXPLICIT: return "explicit";
	case ENGINE_JOB_EDGE_BARRIER: return "barrier";
	case ENGINE_JOB_EDGE_RAW: return "raw";
	case ENGINE_JOB_EDGE_WAR: return "war";
	case ENGINE_JOB_EDGE_WAW: return "waw";
	default: return "unknown";
	}
}

/* The last run of one phase: jobs, edges by reason, critical path, waits. */
static void job_report_graph(FILE *file, FILE *dot, struct job_phase_storage *storage, char const *name)
{
	struct engine_job_graph const *graph = &storage->graph.graph;
	struct engine_job_profile const *profile = &storage->profile;
	uint64_t path[JOBS_PER_PHASE];
	uint64_t critical = 0;
	uint64_t edge_counts[6] = { 0 };
	uint64_t dependency_wait = 0, queue_wait = 0, last_end = 0;
	uint32_t job, slot;
	int first = 1;

	/* Edges only point forward, so submission order is a topological order. */
	memset(path, 0, sizeof(path));
	for (job = 0; job < graph->job_count; job++)
	{
		uint32_t edge;
		uint64_t const busy = atomic_load(&profile->samples[job].busy_ns);
		path[job] += busy;
		if (path[job] > critical)
			critical = path[job];
		for (edge = graph->config.jobs[job].successor_head; edge != ENGINE_JOB_INVALID;
			edge = graph->config.edges[edge].next)
		{
			uint32_t const target = graph->config.edges[edge].target;
			if (path[job] > path[target])
				path[target] = path[job];
			edge_counts[graph->config.edge_kinds[edge] < 6 ? graph->config.edge_kinds[edge] : 0]++;
		}
	}
	fprintf(file, "{\"phase\":\"%s\",\"jobs\":[", name);
	fprintf(dot, "  subgraph cluster_%s { label=\"%s\";\n", name, name);
	for (job = 0; job < graph->job_count; job++)
	{
		struct engine_job_profile_sample const *sample = &profile->samples[job];
		struct engine_job_record const *record = &graph->config.jobs[job];
		uint64_t const ready = atomic_load(&sample->ready_ns);
		uint64_t start = atomic_load(&sample->start_ns);
		uint64_t const stop = atomic_load(&sample->end_ns);
		if (start == UINT64_MAX)
			start = ready; /* a barrier: nothing ran */
		if (ready >= profile->submit_ns)
			dependency_wait += ready - profile->submit_ns;
		if (start >= ready)
			queue_wait += start - ready;
		if (stop > last_end)
			last_end = stop;
		fprintf(file, "%s{\"id\":%u,\"name\":\"%s\",\"priority\":%u,\"main_thread\":%u,"
			"\"partitions\":%u,\"ready_ns\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"busy_ns\":%llu,"
			"\"threads\":%u}",
			job ? "," : "", job, record->name, record->priority,
			(record->flags & ENGINE_JOB_FLAG_MAIN_THREAD) != 0, record->partition_count,
			(unsigned long long)(ready >= profile->submit_ns ? ready - profile->submit_ns : 0),
			(unsigned long long)(start >= profile->submit_ns ? start - profile->submit_ns : 0),
			(unsigned long long)(stop >= profile->submit_ns ? stop - profile->submit_ns : 0),
			(unsigned long long)atomic_load(&sample->busy_ns),
			job_bit_count(atomic_load(&sample->thread_mask_low)));
		fprintf(dot, "    %s_%u [label=\"%s\\n%llu us%s\"%s];\n", name, job, record->name,
			(unsigned long long)(atomic_load(&sample->busy_ns) / 1000u),
			(record->flags & ENGINE_JOB_FLAG_MAIN_THREAD) ? "\\nmain thread" : "",
			(record->flags & ENGINE_JOB_FLAG_MAIN_THREAD) ? " shape=box" : "");
	}
	fprintf(file, "],\"edges\":[");
	for (job = 0; job < graph->job_count; job++)
	{
		uint32_t edge;
		for (edge = graph->config.jobs[job].successor_head; edge != ENGINE_JOB_INVALID;
			edge = graph->config.edges[edge].next)
		{
			uint8_t const kind = graph->config.edge_kinds[edge];
			fprintf(file, "%s{\"from\":%u,\"to\":%u,\"kind\":\"%s\",\"inferred\":%s}", first ? "" : ",",
				job, graph->config.edges[edge].target, job_edge_kind_name(kind),
				kind >= ENGINE_JOB_EDGE_RAW ? "true" : "false");
			/* explicit edges solid, inferred hazards dashed and labelled */
			fprintf(dot, "    %s_%u -> %s_%u [%s];\n", name, job, name, graph->config.edges[edge].target,
				kind == ENGINE_JOB_EDGE_EXPLICIT ? "style=solid" :
				kind == ENGINE_JOB_EDGE_BARRIER ? "style=bold" :
				kind == ENGINE_JOB_EDGE_RAW ? "style=dashed color=red label=RAW" :
				kind == ENGINE_JOB_EDGE_WAR ? "style=dashed color=blue label=WAR" :
				"style=dashed color=purple label=WAW");
			first = 0;
		}
	}
	fprintf(dot, "  }\n");
	fprintf(file, "],\"edge_counts\":{\"explicit\":%llu,\"barrier\":%llu,\"raw\":%llu,\"war\":%llu,\"waw\":%llu},",
		(unsigned long long)edge_counts[1], (unsigned long long)edge_counts[2],
		(unsigned long long)edge_counts[3], (unsigned long long)edge_counts[4],
		(unsigned long long)edge_counts[5]);
	fprintf(file, "\"critical_path_ns\":%llu,\"dependency_wait_ns\":%llu,\"ready_queue_wait_ns\":%llu,"
		"\"completion_ns\":%llu,\"drain_ns\":%llu,\"thread_busy_ns\":[",
		(unsigned long long)critical, (unsigned long long)dependency_wait, (unsigned long long)queue_wait,
		(unsigned long long)(profile->complete_ns >= profile->submit_ns ? profile->complete_ns - profile->submit_ns : 0),
		(unsigned long long)(profile->complete_ns >= last_end && last_end ? profile->complete_ns - last_end : 0));
	for (slot = 0; slot <= job_workers && slot <= ENGINE_JOB_WORKERS_MAX; slot++)
		fprintf(file, "%s%llu", slot ? "," : "", (unsigned long long)atomic_load(&profile->thread_busy_ns[slot]));
	fprintf(file, "]}");
}

static void job_report_write(void)
{
	char dot_path[1024];
	FILE *file, *dot;
	uint32_t index;

	if (!job_trace_path)
		return;
	file = fopen(job_trace_path, "w");
	if (!file)
		return;
	snprintf(dot_path, sizeof(dot_path), "%s.dot", job_trace_path);
	dot = fopen(dot_path, "w");
	if (!dot)
		dot = fopen("/dev/null", "w");
	fprintf(dot, "digraph jobs {\n  rankdir=LR;\n");
	fprintf(file, "{\"mode\":\"%s\",\"workers\":%u,\"performance_cores\":%u,\"arena_bytes\":%llu,"
		"\"phases\":[",
		job_mode == HALO_JOBS_PARALLEL ? "parallel" : job_mode == HALO_JOBS_SEQUENTIAL ? "sequential" : "off",
		job_system_started ? job_workers : 0u, job_performance_cores,
		(unsigned long long)(sizeof(job_phases) + sizeof(job_ready_entries) + sizeof(job_system)));
	for (index = 0; index < HALO_JOB_PHASE_COUNT; index++)
	{
		struct job_phase_storage *storage = &job_phases[index];
		static uint64_t sorted[WALL_SAMPLES];
		uint32_t const count = storage->wall_count < WALL_SAMPLES ? storage->wall_count : WALL_SAMPLES;
		uint64_t p50 = 0, p95 = 0, p99 = 0;
		if (count)
		{
			memcpy(sorted, storage->wall_ns, count * sizeof(sorted[0]));
			qsort(sorted, count, sizeof(sorted[0]), job_u64_compare);
			p50 = sorted[(count - 1u) / 2u];
			p95 = sorted[(count * 95u + 99u) / 100u - 1u];
			p99 = sorted[(count * 99u + 99u) / 100u - 1u];
		}
		fprintf(file, "%s{\"name\":\"%s\",\"runs\":%llu,\"failures\":%llu,\"overflow_fallbacks\":%llu,"
			"\"sequential_fallbacks\":%llu,\"wall_ns_total\":%llu,\"build_ns_total\":%llu,\"busy_ns_total\":%llu,"
			"\"wall_p50_ns\":%llu,\"wall_p95_ns\":%llu,\"wall_p99_ns\":%llu,\"last\":",
			index ? "," : "", job_phase_names[index], (unsigned long long)storage->runs,
			(unsigned long long)storage->failures, (unsigned long long)storage->phase.fallback_count,
			(unsigned long long)storage->phase.sequential_count, (unsigned long long)storage->wall_ns_total,
			(unsigned long long)storage->build_ns_total, (unsigned long long)storage->busy_ns_total,
			(unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99);
		if (storage->runs && storage->phase.last_overflow == ENGINE_JOB_OK)
			job_report_graph(file, dot, storage, job_phase_names[index]);
		else
			fprintf(file, "null");
		fprintf(file, "}");
	}
	fprintf(file, "]}\n");
	fprintf(dot, "}\n");
	fclose(file);
	fclose(dot);
}

#else /* !HALO_JOBS_ENABLED: this build does not compile engine/core/jobs */

int halo_jobs_mode(void) { return HALO_JOBS_OFF; }
uint32_t halo_jobs_worker_count(void) { return 0; }
uint32_t halo_job_add(struct halo_job_graph *graph, struct halo_job_description const *description)
{
	(void)graph; (void)description;
	return HALO_JOB_INVALID;
}
int halo_job_barrier(struct halo_job_graph *graph) { (void)graph; return 0; }
int halo_jobs_phase_run(enum halo_job_phase phase, halo_job_build_fn build, void *context)
{
	(void)phase; (void)build; (void)context;
	return 0;
}
int halo_jobs_phase_active(enum halo_job_phase phase) { (void)phase; return 0; }
uint32_t halo_jobs_phase_fallbacks(enum halo_job_phase phase) { (void)phase; return 0; }
void halo_jobs_shutdown(void) {}
uint32_t halo_job_resource_handle(uint32_t slot) { return slot; }
uint32_t halo_job_resource_invalidate(uint32_t slot) { return slot; }
int halo_job_resource_current(uint32_t handle) { (void)handle; return 1; }

#endif
