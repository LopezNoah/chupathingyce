#include "job_graph.h"

#include "../assert.h"
#include "../trace/trace.h"

#include <string.h>

ENGINE_STATIC_ASSERT(sizeof(struct engine_job_record) <= 128,
	"ADR 0049 sizes the job arena in 128-byte records");
ENGINE_STATIC_ASSERT(ENGINE_JOB_PRIORITY_COUNT == 4, "four priority classes");
ENGINE_STATIC_ASSERT(ENGINE_JOB_JOBS_PER_GRAPH_MAX < ENGINE_JOB_INVALID,
	"job handles never collide with the invalid handle");

static char const job_barrier_name[] = "job_barrier";

_Thread_local uint32_t engine_job_thread_slot;

uint32_t engine_job_partition_count(uint64_t item_count, uint32_t partition_size)
{
	uint64_t count;
	if (item_count == 0)
		return 1;
	ENGINE_ASSERT(partition_size > 0);
	count = item_count / partition_size + (item_count % partition_size != 0);
	/* Callers validate against the limit before relying on the narrowing. */
	return count > ENGINE_JOB_PARTITIONS_PER_JOB_MAX ? ENGINE_JOB_INVALID : (uint32_t)count;
}

static int job_is_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1u)) == 0;
}

enum engine_job_result engine_job_graph_init(struct engine_job_graph *graph,
	struct engine_job_graph_config const *config)
{
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(config != NULL);
	if (!config->jobs || config->jobs_capacity == 0 ||
		config->jobs_capacity > ENGINE_JOB_JOBS_PER_GRAPH_MAX)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (!config->edges || config->edges_capacity == 0 ||
		!config->accesses || config->accesses_capacity == 0 ||
		!config->readers || config->readers_capacity == 0)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (!config->resources || !job_is_power_of_two(config->resources_capacity) ||
		config->resources_capacity < 2u)
		return ENGINE_JOB_INVALID_ARGUMENT;
	memset(graph, 0, sizeof(*graph));
	graph->config = *config;
	memset(config->resources, 0, sizeof(*config->resources) * config->resources_capacity);
	engine_job_graph_reset(graph, ENGINE_JOB_GRAPH_DEFERRED);
	return ENGINE_JOB_OK;
}

void engine_job_graph_reset(struct engine_job_graph *graph, enum engine_job_graph_mode mode)
{
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(atomic_load(&graph->running) == 0);
	ENGINE_ASSERT(mode == ENGINE_JOB_GRAPH_DEFERRED || mode == ENGINE_JOB_GRAPH_DIRECT);
	graph->job_count = 0;
	graph->edge_count = 0;
	graph->access_count = 0;
	graph->reader_count = 0;
	graph->resource_count = 0;
	/* Generation 0 marks never-used slots, so live generations start at 1. */
	graph->generation += 1;
	if (graph->generation == 0)
		graph->generation = 1;
	graph->segment_first = 0;
	graph->barrier_job = ENGINE_JOB_INVALID;
	graph->mode = mode;
	graph->overflow = ENGINE_JOB_OK;
	memset(graph->ready_count, 0, sizeof(graph->ready_count));
	graph->partition_total = 0;
	atomic_store(&graph->failed, 0);
	atomic_store(&graph->first_failed_job, ENGINE_JOB_INVALID);
	atomic_store(&graph->partitions_executed, 0);
	atomic_store(&graph->partitions_skipped, 0);
	atomic_store(&graph->partitions_on_workers, 0);
}

static int job_access_valid(struct engine_job_access const *access)
{
	ENGINE_ASSERT(access != NULL);
	if (access->kind < ENGINE_JOB_RESOURCE_COMPONENT || access->kind > ENGINE_JOB_RESOURCE_USER)
		return 0;
	if (access->mode != ENGINE_JOB_ACCESS_READ && access->mode != ENGINE_JOB_ACCESS_WRITE)
		return 0;
	/* Component identifiers index a 64-bit component mask. */
	return access->kind != ENGINE_JOB_RESOURCE_COMPONENT || access->identifier < 64u;
}

static enum engine_job_result job_description_validate(struct engine_job_graph const *graph,
	struct engine_job_description const *description)
{
	uint32_t index;
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(description != NULL);
	if (!description->name || !description->execute || description->name[0] == 0)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (description->argument_size > ENGINE_JOB_ARGUMENT_BYTES_MAX ||
		(description->argument_size > 0 && !description->argument))
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (description->access_count > ENGINE_JOB_ACCESSES_PER_JOB_MAX ||
		(description->access_count > 0 && !description->accesses))
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (description->dependency_count > ENGINE_JOB_DEPENDENCIES_PER_JOB_MAX ||
		(description->dependency_count > 0 && !description->dependencies))
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (description->priority >= ENGINE_JOB_PRIORITY_COUNT ||
		(description->flags & ~(uint32_t)ENGINE_JOB_FLAG_MAIN_THREAD) != 0)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (description->item_count > 0 && description->partition_size == 0)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (engine_job_partition_count(description->item_count, description->partition_size) ==
		ENGINE_JOB_INVALID)
		return ENGINE_JOB_INVALID_ARGUMENT;
	for (index = 0; index < description->dependency_count; ++index)
		if (description->dependencies[index] >= graph->job_count)
			return ENGINE_JOB_INVALID_ARGUMENT;
	for (index = 0; index < description->access_count; ++index)
		if (!job_access_valid(&description->accesses[index]))
			return ENGINE_JOB_INVALID_ARGUMENT;
	return ENGINE_JOB_OK;
}

/* Returns the strongest declared mode for one component, or 0 if undeclared. */
static uint32_t job_component_mode(struct engine_job_description const *description,
	uint32_t component)
{
	uint32_t mode = 0;
	uint32_t index;
	ENGINE_ASSERT(component < 64u);
	ENGINE_ASSERT(description->access_count <= ENGINE_JOB_ACCESSES_PER_JOB_MAX);
	for (index = 0; index < description->access_count; ++index)
	{
		struct engine_job_access const *access = &description->accesses[index];
		if (access->kind == ENGINE_JOB_RESOURCE_COMPONENT && access->identifier == component &&
			access->mode > mode)
			mode = access->mode;
	}
	return mode;
}

/* Rejects query descriptors that read or write undeclared columns (ADR 0049). */
static enum engine_job_result job_query_validate(struct engine_job_description const *description)
{
	uint64_t const used = description->component_read_mask | description->component_write_mask;
	uint32_t component;
	ENGINE_ASSERT(description != NULL);
	ENGINE_ASSERT(description->access_count <= ENGINE_JOB_ACCESSES_PER_JOB_MAX);
	for (component = 0; component < 64u; ++component)
	{
		uint64_t const bit = (uint64_t)1 << component;
		uint32_t mode;
		if ((used & bit) == 0)
			continue;
		mode = job_component_mode(description, component);
		if (mode == 0)
			return ENGINE_JOB_UNDECLARED_ACCESS;
		if ((description->component_write_mask & bit) != 0 && mode != ENGINE_JOB_ACCESS_WRITE)
			return ENGINE_JOB_UNDECLARED_ACCESS;
	}
	return ENGINE_JOB_OK;
}

static enum engine_job_result job_overflow(struct engine_job_graph *graph,
	enum engine_job_result result)
{
	ENGINE_ASSERT(result != ENGINE_JOB_OK);
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DEFERRED);
	if (graph->overflow == ENGINE_JOB_OK)
		graph->overflow = result;
	return result;
}

static enum engine_job_result job_edge_add(struct engine_job_graph *graph,
	uint32_t source, uint32_t target, enum engine_job_edge_kind kind)
{
	struct engine_job_record *record;
	struct engine_job_edge *edge;
	ENGINE_ASSERT(source < target);
	ENGINE_ASSERT(target < graph->job_count + 1u);
	record = &graph->config.jobs[source];
	/* Edges for one target are added together, so a repeat is the last one. */
	if (record->last_successor == target)
		return ENGINE_JOB_OK;
	if (graph->edge_count >= graph->config.edges_capacity)
		return job_overflow(graph, ENGINE_JOB_EDGES_EXHAUSTED);
	edge = &graph->config.edges[graph->edge_count];
	edge->target = target;
	edge->next = record->successor_head;
	if (graph->config.edge_kinds)
		graph->config.edge_kinds[graph->edge_count] = (uint8_t)kind;
	record->successor_head = graph->edge_count;
	record->last_successor = target;
	graph->edge_count += 1;
	graph->config.jobs[target].predecessor_count += 1;
	return ENGINE_JOB_OK;
}

static uint32_t job_resource_hash(uint32_t kind, uint64_t identifier)
{
	uint64_t const mixed = (identifier ^ ((uint64_t)kind << 56)) * 0x9E3779B97F4A7C15ull;
	ENGINE_ASSERT(kind >= ENGINE_JOB_RESOURCE_COMPONENT);
	ENGINE_ASSERT(kind <= ENGINE_JOB_RESOURCE_USER);
	return (uint32_t)(mixed >> 32);
}

/* Finds or inserts a resource in the current generation; INVALID when full. */
static uint32_t job_resource_lookup(struct engine_job_graph *graph, uint32_t kind,
	uint64_t identifier)
{
	uint32_t const mask = graph->config.resources_capacity - 1u;
	uint32_t probe = job_resource_hash(kind, identifier) & mask;
	uint32_t attempt;
	ENGINE_ASSERT(graph->generation != 0);
	ENGINE_ASSERT(graph->resource_count * 2u <= graph->config.resources_capacity);
	for (attempt = 0; attempt < graph->config.resources_capacity; ++attempt)
	{
		struct engine_job_resource *resource = &graph->config.resources[probe];
		if (resource->generation != graph->generation)
		{
			/* Keep the table at most half full so probes stay short. */
			if ((graph->resource_count + 1u) * 2u > graph->config.resources_capacity)
				return ENGINE_JOB_INVALID;
			ENGINE_ASSERT(atomic_load(&resource->occupancy) == 0);
			resource->identifier = identifier;
			resource->kind = kind;
			resource->generation = graph->generation;
			resource->last_writer = ENGINE_JOB_INVALID;
			resource->reader_head = ENGINE_JOB_INVALID;
			graph->resource_count += 1;
			return probe;
		}
		if (resource->kind == kind && resource->identifier == identifier)
			return probe;
		probe = (probe + 1u) & mask;
	}
	ENGINE_ASSERT(0);
	return ENGINE_JOB_INVALID;
}

static enum engine_job_result job_order_write(struct engine_job_graph *graph,
	struct engine_job_resource *resource, uint32_t job)
{
	enum engine_job_result result = ENGINE_JOB_OK;
	uint32_t reader = resource->reader_head;
	uint32_t visited = 0;
	ENGINE_ASSERT(job < graph->config.jobs_capacity);
	ENGINE_ASSERT(resource->generation == graph->generation);
	/* Readers already follow the previous writer, so they suffice when present. */
	if (resource->last_writer != ENGINE_JOB_INVALID && reader == ENGINE_JOB_INVALID)
		result = job_edge_add(graph, resource->last_writer, job, ENGINE_JOB_EDGE_WAW);
	/* A write waits for every read since the previous write. */
	while (result == ENGINE_JOB_OK && reader != ENGINE_JOB_INVALID)
	{
		ENGINE_ASSERT(visited < graph->reader_count);
		result = job_edge_add(graph, graph->config.readers[reader].job, job, ENGINE_JOB_EDGE_WAR);
		reader = graph->config.readers[reader].next;
		visited += 1;
	}
	resource->last_writer = job;
	resource->reader_head = ENGINE_JOB_INVALID;
	return result;
}

static enum engine_job_result job_order_read(struct engine_job_graph *graph,
	struct engine_job_resource *resource, uint32_t job)
{
	struct engine_job_reader *reader;
	ENGINE_ASSERT(job < graph->config.jobs_capacity);
	ENGINE_ASSERT(resource->generation == graph->generation);
	if (resource->last_writer != ENGINE_JOB_INVALID)
	{
		enum engine_job_result const result = job_edge_add(graph, resource->last_writer, job,
			ENGINE_JOB_EDGE_RAW);
		if (result != ENGINE_JOB_OK)
			return result;
	}
	if (graph->reader_count >= graph->config.readers_capacity)
		return job_overflow(graph, ENGINE_JOB_RESOURCES_EXHAUSTED);
	reader = &graph->config.readers[graph->reader_count];
	reader->job = job;
	reader->next = resource->reader_head;
	resource->reader_head = graph->reader_count;
	graph->reader_count += 1;
	return ENGINE_JOB_OK;
}

/* Merges repeated declarations of one resource; a write dominates a read. */
static uint32_t job_accesses_normalize(struct engine_job_description const *description,
	struct engine_job_access *merged)
{
	uint32_t count = 0;
	uint32_t index;
	ENGINE_ASSERT(description->access_count <= ENGINE_JOB_ACCESSES_PER_JOB_MAX);
	ENGINE_ASSERT(merged != NULL);
	for (index = 0; index < description->access_count; ++index)
	{
		struct engine_job_access const *access = &description->accesses[index];
		uint32_t existing;
		for (existing = 0; existing < count; ++existing)
			if (merged[existing].kind == access->kind &&
				merged[existing].identifier == access->identifier)
				break;
		if (existing == count)
		{
			merged[count] = *access;
			count += 1;
		}
		else if (access->mode == ENGINE_JOB_ACCESS_WRITE)
			merged[existing].mode = ENGINE_JOB_ACCESS_WRITE;
	}
	return count;
}

static enum engine_job_result job_accesses_record(struct engine_job_graph *graph,
	struct engine_job_description const *description, uint32_t job)
{
	struct engine_job_access merged[ENGINE_JOB_ACCESSES_PER_JOB_MAX];
	uint32_t const count = job_accesses_normalize(description, merged);
	struct engine_job_record *record = &graph->config.jobs[job];
	uint32_t index;
	ENGINE_ASSERT(count <= description->access_count);
	ENGINE_ASSERT(record->access_count == 0);
	if (count > graph->config.accesses_capacity - graph->access_count)
		return job_overflow(graph, ENGINE_JOB_ACCESSES_EXHAUSTED);
	record->access_first = graph->access_count;
	for (index = 0; index < count; ++index)
	{
		struct engine_job_access_slot *slot = &graph->config.accesses[graph->access_count];
		uint32_t const resource = job_resource_lookup(graph, merged[index].kind,
			merged[index].identifier);
		enum engine_job_result result;
		if (resource == ENGINE_JOB_INVALID)
			return job_overflow(graph, ENGINE_JOB_RESOURCES_EXHAUSTED);
		slot->access = merged[index];
		slot->resource = resource;
		graph->access_count += 1;
		record->access_count += 1;
		result = merged[index].mode == ENGINE_JOB_ACCESS_WRITE ?
			job_order_write(graph, &graph->config.resources[resource], job) :
			job_order_read(graph, &graph->config.resources[resource], job);
		if (result != ENGINE_JOB_OK)
			return result;
	}
	return ENGINE_JOB_OK;
}

static void job_record_fill(struct engine_job_graph *graph, uint32_t job,
	struct engine_job_description const *description)
{
	struct engine_job_record *record = &graph->config.jobs[job];
	uint32_t const ring = (description->flags & ENGINE_JOB_FLAG_MAIN_THREAD) ?
		ENGINE_JOB_PRIORITY_COUNT : description->priority;
	ENGINE_ASSERT(job < graph->config.jobs_capacity);
	ENGINE_ASSERT(ring <= ENGINE_JOB_PRIORITY_COUNT);
	memset(record, 0, sizeof(*record));
	record->name = description->name;
	record->execute = description->execute;
	record->item_count = description->item_count;
	record->partition_size = description->partition_size;
	record->partition_count = engine_job_partition_count(description->item_count,
		description->partition_size);
	record->successor_head = ENGINE_JOB_INVALID;
	record->last_successor = ENGINE_JOB_INVALID;
	record->cost_hint_nanoseconds = description->cost_hint_nanoseconds;
	record->argument_size = (uint16_t)description->argument_size;
	record->priority = (uint8_t)description->priority;
	record->flags = (uint8_t)description->flags;
	if (description->argument_size > 0)
		memcpy(record->argument, description->argument, description->argument_size);
	graph->ready_count[ring] += 1;
	graph->partition_total += record->partition_count;
}

static enum engine_job_result job_add_deferred(struct engine_job_graph *graph,
	struct engine_job_description const *description, uint32_t *job)
{
	uint32_t const handle = graph->job_count;
	enum engine_job_result result = ENGINE_JOB_OK;
	uint32_t index;
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DEFERRED);
	ENGINE_ASSERT(graph->overflow == ENGINE_JOB_OK);
	if (handle >= graph->config.jobs_capacity)
		return job_overflow(graph, ENGINE_JOB_JOBS_EXHAUSTED);
	job_record_fill(graph, handle, description);
	graph->job_count += 1;
	if (graph->barrier_job != ENGINE_JOB_INVALID)
		result = job_edge_add(graph, graph->barrier_job, handle, ENGINE_JOB_EDGE_BARRIER);
	for (index = 0; result == ENGINE_JOB_OK && index < description->dependency_count; ++index)
		result = job_edge_add(graph, description->dependencies[index], handle,
			ENGINE_JOB_EDGE_EXPLICIT);
	if (result == ENGINE_JOB_OK)
		result = job_accesses_record(graph, description, handle);
	if (result == ENGINE_JOB_OK)
		*job = handle;
	return result;
}

#if defined(ENGINE_TRACE_ENABLED)
static void job_trace_begin(struct engine_job_record const *record, uint32_t partition)
{
	struct engine_trace_thread *thread = engine_trace_thread_current();
	if (thread)
		engine_trace_named_begin(thread, record->name, partition);
}

static void job_trace_end(struct engine_job_record const *record)
{
	struct engine_trace_thread *thread = engine_trace_thread_current();
	if (thread)
		engine_trace_named_end(thread, record->name);
}
#else
#define job_trace_begin(record, partition) ((void)0)
#define job_trace_end(record) ((void)0)
#endif

static int job_run_partition(struct engine_job_record const *record, uint32_t partition)
{
	struct engine_job_partition range;
	int succeeded;
	ENGINE_ASSERT(partition < record->partition_count);
	ENGINE_ASSERT(record->execute != NULL);
	range.index = partition;
	range.count = record->partition_count;
	range.item_begin = (uint64_t)partition * record->partition_size;
	range.item_end = range.item_begin + record->partition_size;
	if (record->item_count == 0)
	{
		range.item_begin = 0;
		range.item_end = 0;
	}
	else if (range.item_end > record->item_count)
		range.item_end = record->item_count;
	job_trace_begin(record, partition);
	succeeded = record->execute(record->argument, &range);
	job_trace_end(record);
	return succeeded;
}

/* Direct mode is the overflow fallback: each job runs completely, in order,
   as it is added, which is exactly the sequential oracle's order. */
static enum engine_job_result job_add_direct(struct engine_job_graph *graph,
	struct engine_job_description const *description, uint32_t *job)
{
	struct engine_job_record record;
	uint32_t partition;
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DIRECT);
	ENGINE_ASSERT(graph->job_count < ENGINE_JOB_JOBS_PER_GRAPH_MAX);
	if (atomic_load(&graph->failed))
		return ENGINE_JOB_FAILED;
	memset(&record, 0, sizeof(record));
	record.name = description->name;
	record.execute = description->execute;
	record.item_count = description->item_count;
	record.partition_size = description->partition_size;
	record.partition_count = engine_job_partition_count(description->item_count,
		description->partition_size);
	if (description->argument_size > 0)
		memcpy(record.argument, description->argument, description->argument_size);
	*job = graph->job_count;
	graph->job_count += 1;
	for (partition = 0; partition < record.partition_count; ++partition)
	{
		atomic_fetch_add(&graph->partitions_executed, 1);
		if (!job_run_partition(&record, partition))
		{
			atomic_store(&graph->failed, 1);
			atomic_store(&graph->first_failed_job, *job);
			return ENGINE_JOB_FAILED;
		}
	}
	return ENGINE_JOB_OK;
}

enum engine_job_result engine_job_graph_add(struct engine_job_graph *graph,
	struct engine_job_description const *description, uint32_t *job)
{
	enum engine_job_result result;
	ENGINE_ASSERT(graph != NULL && job != NULL);
	ENGINE_ASSERT(atomic_load(&graph->running) == 0);
	*job = ENGINE_JOB_INVALID;
	if (!description)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (graph->overflow != ENGINE_JOB_OK)
		return ENGINE_JOB_OVERFLOWED;
	result = job_description_validate(graph, description);
	if (result == ENGINE_JOB_OK)
		result = job_query_validate(description);
	if (result != ENGINE_JOB_OK)
		return result;
	if (graph->mode == ENGINE_JOB_GRAPH_DIRECT)
		return job_add_direct(graph, description, job);
	return job_add_deferred(graph, description, job);
}

enum engine_job_result engine_job_graph_barrier(struct engine_job_graph *graph)
{
	uint32_t barrier;
	uint32_t job;
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(atomic_load(&graph->running) == 0);
	if (graph->overflow != ENGINE_JOB_OK)
		return ENGINE_JOB_OVERFLOWED;
	/* Direct mode is already fully ordered, and an empty segment needs no join. */
	if (graph->mode == ENGINE_JOB_GRAPH_DIRECT || graph->job_count == graph->segment_first)
		return ENGINE_JOB_OK;
	barrier = graph->job_count;
	if (barrier >= graph->config.jobs_capacity)
		return job_overflow(graph, ENGINE_JOB_JOBS_EXHAUSTED);
	memset(&graph->config.jobs[barrier], 0, sizeof(graph->config.jobs[barrier]));
	graph->config.jobs[barrier].name = job_barrier_name;
	graph->config.jobs[barrier].partition_count = 1;
	graph->config.jobs[barrier].successor_head = ENGINE_JOB_INVALID;
	graph->config.jobs[barrier].last_successor = ENGINE_JOB_INVALID;
	graph->job_count += 1;
	graph->ready_count[ENGINE_JOB_PRIORITY_SIMULATION] += 1;
	graph->partition_total += 1;
	/* Joining the sinks suffices: every other job reaches a sink. */
	for (job = graph->segment_first; job < barrier; ++job)
	{
		if (graph->config.jobs[job].successor_head != ENGINE_JOB_INVALID)
			continue;
		if (job_edge_add(graph, job, barrier, ENGINE_JOB_EDGE_BARRIER) != ENGINE_JOB_OK)
			return ENGINE_JOB_EDGES_EXHAUSTED;
	}
	graph->segment_first = graph->job_count;
	graph->barrier_job = barrier;
	graph->resource_count = 0;
	graph->generation += 1;
	ENGINE_ASSERT(graph->generation != 0);
	return ENGINE_JOB_OK;
}

void engine_job_graph_prepare_run(struct engine_job_graph *graph, struct engine_job_system *system)
{
	uint32_t job;
	uint32_t expected = 0;
	ENGINE_ASSERT(graph->overflow == ENGINE_JOB_OK);
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DEFERRED);
	/* A graph has one executor at a time. */
	ENGINE_ASSERT(atomic_compare_exchange_strong(&graph->running, &expected, 1u));
	graph->system = system;
	atomic_store(&graph->complete, 0);
	atomic_store(&graph->remaining_jobs, graph->job_count);
	atomic_store(&graph->failed, 0);
	atomic_store(&graph->first_failed_job, ENGINE_JOB_INVALID);
	atomic_store(&graph->partitions_executed, 0);
	atomic_store(&graph->partitions_skipped, 0);
	atomic_store(&graph->partitions_on_workers, 0);
	for (job = 0; job < graph->job_count; ++job)
	{
		struct engine_job_record *record = &graph->config.jobs[job];
		atomic_store_explicit(&record->next_partition, 0, memory_order_relaxed);
		atomic_store_explicit(&record->completed_partitions, 0, memory_order_relaxed);
		atomic_store_explicit(&record->pending_predecessors, record->predecessor_count,
			memory_order_relaxed);
	}
	if (graph->config.profile)
	{
		struct engine_job_profile *profile = graph->config.profile;
		ENGINE_ASSERT(profile->clock_ns != NULL && profile->samples != NULL);
		profile->submit_ns = profile->clock_ns();
		profile->complete_ns = 0;
		for (job = 0; job <= ENGINE_JOB_WORKERS_MAX; ++job)
			atomic_store_explicit(&profile->thread_busy_ns[job], 0, memory_order_relaxed);
		for (job = 0; job < graph->job_count; ++job)
		{
			struct engine_job_profile_sample *sample = &profile->samples[job];
			atomic_store_explicit(&sample->ready_ns,
				graph->config.jobs[job].predecessor_count ? 0 : profile->submit_ns,
				memory_order_relaxed);
			atomic_store_explicit(&sample->start_ns, UINT64_MAX, memory_order_relaxed);
			atomic_store_explicit(&sample->end_ns, 0, memory_order_relaxed);
			atomic_store_explicit(&sample->busy_ns, 0, memory_order_relaxed);
			atomic_store_explicit(&sample->thread_mask_low, 0, memory_order_relaxed);
		}
	}
}

void engine_job_graph_profile_ready(struct engine_job_graph *graph, uint32_t job)
{
	ENGINE_ASSERT(job < graph->job_count);
	if (graph->config.profile)
		atomic_store_explicit(&graph->config.profile->samples[job].ready_ns,
			graph->config.profile->clock_ns(), memory_order_relaxed);
}

static void job_profile_partition(struct engine_job_graph *graph, uint32_t job,
	uint64_t begin, uint64_t end)
{
	struct engine_job_profile *profile = graph->config.profile;
	struct engine_job_profile_sample *sample = &profile->samples[job];
	uint32_t const slot = engine_job_thread_slot;
	uint64_t current;
	ENGINE_ASSERT(slot <= ENGINE_JOB_WORKERS_MAX);
	ENGINE_ASSERT(end >= begin);
	current = atomic_load_explicit(&sample->start_ns, memory_order_relaxed);
	while (begin < current && !atomic_compare_exchange_weak_explicit(&sample->start_ns,
		&current, begin, memory_order_relaxed, memory_order_relaxed))
		;
	current = atomic_load_explicit(&sample->end_ns, memory_order_relaxed);
	while (end > current && !atomic_compare_exchange_weak_explicit(&sample->end_ns,
		&current, end, memory_order_relaxed, memory_order_relaxed))
		;
	atomic_fetch_add_explicit(&sample->busy_ns, end - begin, memory_order_relaxed);
	atomic_fetch_add_explicit(&profile->thread_busy_ns[slot], end - begin, memory_order_relaxed);
	if (slot < 32u)
		atomic_fetch_or_explicit(&sample->thread_mask_low, 1u << slot, memory_order_relaxed);
}

static void job_record_failure(struct engine_job_graph *graph, uint32_t job)
{
	uint32_t current = atomic_load(&graph->first_failed_job);
	ENGINE_ASSERT(job < graph->job_count);
	atomic_store(&graph->failed, 1);
	/* Report the lowest failing handle so the report does not depend on timing. */
	while (job < current)
		if (atomic_compare_exchange_weak(&graph->first_failed_job, &current, job))
			break;
	ENGINE_ASSERT(atomic_load(&graph->first_failed_job) <= job);
}

enum engine_job_partition_outcome engine_job_graph_execute_partition(
	struct engine_job_graph *graph, uint32_t job, uint32_t partition)
{
	struct engine_job_record const *record = &graph->config.jobs[job];
	ENGINE_ASSERT(job < graph->job_count);
	ENGINE_ASSERT(partition < record->partition_count);
	if (!record->execute)
		return ENGINE_JOB_PARTITION_EMPTY;
	/* After a failure the rest of the graph is drained without running. */
	if (atomic_load_explicit(&graph->failed, memory_order_relaxed))
		return ENGINE_JOB_PARTITION_SKIPPED;
	if (graph->config.profile)
	{
		uint64_t const begin = graph->config.profile->clock_ns();
		int const succeeded = job_run_partition(record, partition);
		job_profile_partition(graph, job, begin, graph->config.profile->clock_ns());
		if (!succeeded)
			job_record_failure(graph, job);
		return ENGINE_JOB_PARTITION_EXECUTED;
	}
	if (!job_run_partition(record, partition))
		job_record_failure(graph, job);
	return ENGINE_JOB_PARTITION_EXECUTED;
}

void engine_job_graph_add_tallies(struct engine_job_graph *graph, uint64_t executed,
	uint64_t skipped, uint64_t on_workers)
{
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(on_workers <= executed + skipped);
	if (executed > 0)
		atomic_fetch_add_explicit(&graph->partitions_executed, executed, memory_order_relaxed);
	if (skipped > 0)
		atomic_fetch_add_explicit(&graph->partitions_skipped, skipped, memory_order_relaxed);
	if (on_workers > 0)
		atomic_fetch_add_explicit(&graph->partitions_on_workers, on_workers,
			memory_order_relaxed);
}

/* Checked exclusion: graph edges must already order conflicting jobs, so a
   failed acquisition is a scheduler bug, never an operating error. */
void engine_job_graph_acquire(struct engine_job_graph *graph, uint32_t job)
{
	struct engine_job_record const *record = &graph->config.jobs[job];
	uint32_t index;
	ENGINE_ASSERT(job < graph->job_count);
	ENGINE_ASSERT(record->access_first + record->access_count <= graph->access_count);
	for (index = 0; index < record->access_count; ++index)
	{
		struct engine_job_access_slot const *slot =
			&graph->config.accesses[record->access_first + index];
		_Atomic int32_t *occupancy = &graph->config.resources[slot->resource].occupancy;
		int32_t expected = 0;
		if (slot->access.mode == ENGINE_JOB_ACCESS_WRITE)
		{
			/* No two concurrent jobs write the same resource. */
			ENGINE_ASSERT(atomic_compare_exchange_strong(occupancy, &expected, -1));
			continue;
		}
		expected = atomic_load(occupancy);
		do
			ENGINE_ASSERT(expected >= 0);
		while (!atomic_compare_exchange_weak(occupancy, &expected, expected + 1));
	}
}

void engine_job_graph_release(struct engine_job_graph *graph, uint32_t job)
{
	struct engine_job_record const *record = &graph->config.jobs[job];
	uint32_t index;
	ENGINE_ASSERT(job < graph->job_count);
	ENGINE_ASSERT(record->access_first + record->access_count <= graph->access_count);
	for (index = 0; index < record->access_count; ++index)
	{
		struct engine_job_access_slot const *slot =
			&graph->config.accesses[record->access_first + index];
		_Atomic int32_t *occupancy = &graph->config.resources[slot->resource].occupancy;
		if (slot->access.mode == ENGINE_JOB_ACCESS_WRITE)
		{
			int32_t expected = -1;
			ENGINE_ASSERT(atomic_compare_exchange_strong(occupancy, &expected, 0));
			continue;
		}
		ENGINE_ASSERT(atomic_fetch_sub(occupancy, 1) > 0);
	}
}

void engine_job_graph_finish_report(struct engine_job_graph *graph,
	struct engine_job_run_report *report)
{
	uint64_t const claimed = atomic_load(&graph->partitions_executed) +
		atomic_load(&graph->partitions_skipped);
	uint64_t const workers = atomic_load(&graph->partitions_on_workers);
	ENGINE_ASSERT(atomic_load(&graph->remaining_jobs) == 0);
	ENGINE_ASSERT(atomic_load(&graph->complete) == 1);
	/* Worker claims include skipped partitions, so they never exceed all claims. */
	ENGINE_ASSERT(workers <= claimed);
	graph->system = NULL;
	if (graph->config.profile)
		graph->config.profile->complete_ns = graph->config.profile->clock_ns();
	if (report)
	{
		memset(report, 0, sizeof(*report));
		report->failed_job = atomic_load(&graph->first_failed_job);
		report->result = atomic_load(&graph->failed) ? ENGINE_JOB_FAILED : ENGINE_JOB_OK;
		report->jobs_completed = graph->job_count;
		report->partitions_executed = atomic_load(&graph->partitions_executed);
		report->partitions_skipped = atomic_load(&graph->partitions_skipped);
		report->partitions_on_workers = workers;
		report->partitions_on_caller = claimed - workers;
	}
	atomic_store_explicit(&graph->running, 0, memory_order_release);
}

enum engine_job_result engine_job_graph_run_sequential(struct engine_job_graph *graph,
	struct engine_job_run_report *report)
{
	uint32_t job;
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DEFERRED);
	if (graph->overflow != ENGINE_JOB_OK)
		return ENGINE_JOB_OVERFLOWED;
	uint64_t tallies[3] = { 0, 0, 0 };
	engine_job_graph_prepare_run(graph, NULL);
	for (job = 0; job < graph->job_count; ++job)
	{
		uint32_t const partitions = graph->config.jobs[job].partition_count;
		uint32_t partition;
		if (graph->config.jobs[job].predecessor_count)
			engine_job_graph_profile_ready(graph, job);
		engine_job_graph_acquire(graph, job);
		for (partition = 0; partition < partitions; ++partition)
			tallies[engine_job_graph_execute_partition(graph, job, partition)] += 1;
		engine_job_graph_release(graph, job);
	}
	engine_job_graph_add_tallies(graph, tallies[ENGINE_JOB_PARTITION_EXECUTED],
		tallies[ENGINE_JOB_PARTITION_SKIPPED], 0);
	atomic_store(&graph->remaining_jobs, 0);
	atomic_store(&graph->complete, 1);
	engine_job_graph_finish_report(graph, report);
	return atomic_load(&graph->failed) ? ENGINE_JOB_FAILED : ENGINE_JOB_OK;
}
