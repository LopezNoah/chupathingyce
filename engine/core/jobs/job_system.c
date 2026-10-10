#include "job_system.h"

#include "../assert.h"
#include "../trace/trace.h"

#include <sched.h>
#include <stdio.h>
#include <string.h>

/* A million idle polls is about 10 ms at 11 ns per pause; more only burns a core. */
#define JOB_SPIN_COUNT_MAX (1u << 20)
#define JOB_RING_MAIN ENGINE_JOB_PRIORITY_COUNT
#define JOB_RING_COUNT (ENGINE_JOB_PRIORITY_COUNT + 1u)

ENGINE_STATIC_ASSERT(JOB_RING_COUNT == 5, "four priority rings and one main-thread ring");

/* Claims amortize the lock and shared counters over several jobs or
   partitions (guided self-scheduling): a claim takes at most a 1/threads
   share of what remains, so the end of a graph still spreads across threads,
   and at most about JOB_CLAIM_NANOSECONDS of declared work, so long jobs are
   claimed one at a time. 5 µs is 25 times the 200 ns dispatch budget. */
#define JOB_CLAIM_NANOSECONDS 5000u
/* A job without a cost hint counts as 625 ns: at most 8 per claim. */
#define JOB_COST_UNKNOWN_NANOSECONDS 625u
#define JOB_BATCH_JOBS_MAX 16u
#define JOB_CHUNK_PARTITIONS_MAX 64u
/* Critical sections are a few ring operations, so a held ring lock frees
   within a few hundred cycles; after 256 polls (about 3 µs) the holder was
   probably descheduled, and waiters yield the core instead. */
#define JOB_LOCK_SPIN_MAX 256u
/* A ring lock still held after a billion attempts (minutes of yields) is a
   deadlock, not contention. */
#define JOB_LOCK_ATTEMPTS_MAX (1u << 30)
/* Ready successors gathered before one locked push: a 100-wide fan-out
   (a barrier releasing a layer) publishes in one critical section. */
#define JOB_READY_LOCAL_MAX 128u
/* Distinct successors whose predecessor counts a batch decrements together. */
#define JOB_DECREMENTS_MAX 32u

ENGINE_STATIC_ASSERT(JOB_CLAIM_NANOSECONDS / JOB_COST_UNKNOWN_NANOSECONDS <= JOB_BATCH_JOBS_MAX,
	"unknown costs never fill a whole batch");

/* What one executing thread may run, in priority order. */
struct job_thread
{
	struct engine_job_system *system;
	uint32_t const *rings;
	uint32_t ring_count;
	int on_worker;
};

/* Work claimed under one lock: single-partition jobs of one graph, or a chunk
   of one job's partitions. */
struct job_batch
{
	struct engine_job_graph *graph;
	uint32_t ring;
	uint32_t job_count;
	uint32_t jobs[JOB_BATCH_JOBS_MAX];
	uint32_t chunk_job;
	uint32_t chunk_begin;
	uint32_t chunk_end;
};

/* Per-thread state while running a batch; shared counters are written once. */
struct job_run
{
	struct job_thread const *thread;
	struct engine_job_graph *graph;
	uint32_t continuation;
	uint32_t ready_count;
	struct engine_job_ready ready[JOB_READY_LOCAL_MAX];
	/* A batch whose jobs share a successor (a barrier, a join) decrements its
	   count once, instead of once per job on one contended cache line. */
	uint32_t decrement_count;
	uint32_t decrement_jobs[JOB_DECREMENTS_MAX];
	uint32_t decrement_amounts[JOB_DECREMENTS_MAX];
	uint64_t tallies[3];
	uint32_t jobs_completed;
};

/* Reserved workers take only simulation work; ADR 0049 reserves capacity so
   background work cannot starve a tick. */
static uint32_t const job_rings_reserved[] = { ENGINE_JOB_PRIORITY_SIMULATION };
static uint32_t const job_rings_general[] = {
	ENGINE_JOB_PRIORITY_SIMULATION, ENGINE_JOB_PRIORITY_PRESENTATION,
	ENGINE_JOB_PRIORITY_STREAMING, ENGINE_JOB_PRIORITY_BACKGROUND,
};
static uint32_t const job_rings_main[] = {
	JOB_RING_MAIN, ENGINE_JOB_PRIORITY_SIMULATION, ENGINE_JOB_PRIORITY_PRESENTATION,
};
static uint32_t const job_rings_main_only[] = {
	JOB_RING_MAIN, ENGINE_JOB_PRIORITY_SIMULATION, ENGINE_JOB_PRIORITY_PRESENTATION,
	ENGINE_JOB_PRIORITY_STREAMING, ENGINE_JOB_PRIORITY_BACKGROUND,
};

/* ---- Locking, sleeping, waking ------------------------------------------ */

static void job_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#endif
}

/* Test and test-and-set: waiters poll with plain loads, so they do not bounce
   the line while the holder works, and only race when it looks free. */
static void job_lock(struct engine_job_system *system)
{
	uint32_t attempt = 0;
	ENGINE_ASSERT(system != NULL);
	for (;;)
	{
		if (atomic_load_explicit(&system->ring_lock, memory_order_relaxed) == 0 &&
			atomic_exchange_explicit(&system->ring_lock, 1, memory_order_acquire) == 0)
			return;
		attempt += 1;
		ENGINE_ASSERT(attempt < JOB_LOCK_ATTEMPTS_MAX);
		if (attempt < JOB_LOCK_SPIN_MAX)
			job_pause();
		else
			sched_yield();
	}
}

static void job_unlock(struct engine_job_system *system)
{
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(atomic_load_explicit(&system->ring_lock, memory_order_relaxed) == 1);
	atomic_store_explicit(&system->ring_lock, 0, memory_order_release);
}

/* Lock held. Publishes a ring's count to threads that spin or sleep on it.
   Sequentially consistent, pairing with the sleeper count in job_sleep. */
static void job_ring_publish(struct engine_job_system *system, uint32_t ring)
{
	ENGINE_ASSERT(ring < JOB_RING_COUNT);
	ENGINE_ASSERT(system->rings[ring].count <= system->rings[ring].capacity);
	atomic_store(&system->ring_ready[ring], system->rings[ring].count);
}

/* After publishing work, a completion, or exit. Takes the mutex only when a
   thread is asleep, so a busy pool never touches it. */
static void job_wake(struct engine_job_system *system)
{
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(atomic_load_explicit(&system->ring_lock, memory_order_relaxed) <= 1);
	if (atomic_load(&system->sleepers) == 0)
		return;
	pthread_mutex_lock(&system->mutex);
	pthread_cond_broadcast(&system->wake);
	pthread_mutex_unlock(&system->mutex);
}

static int job_rings_ready(struct job_thread const *thread)
{
	uint32_t index;
	ENGINE_ASSERT(thread->ring_count <= JOB_RING_COUNT);
	ENGINE_ASSERT(thread->rings != NULL);
	for (index = 0; index < thread->ring_count; ++index)
		if (atomic_load(&thread->system->ring_ready[thread->rings[index]]) > 0)
			return 1;
	return 0;
}

/* Whether a waiting thread has something to do. The main thread also waits
   for its graph (or, in shutdown, for every graph) to complete. */
static int job_awake(struct job_thread const *thread, struct engine_job_graph const *graph,
	int draining)
{
	struct engine_job_system *system = thread->system;
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(thread->on_worker || graph != NULL || draining);
	if (atomic_load(&system->exiting) || job_rings_ready(thread))
		return 1;
	if (thread->on_worker)
		return 0;
	if (graph)
		return engine_job_graph_complete(graph);
	return atomic_load(&system->graphs_in_flight) == 0;
}

/* Spins a bounded count before sleeping (ADR 0049). */
static void job_spin(struct job_thread const *thread, struct engine_job_graph const *graph,
	int draining)
{
	uint32_t spin;
	ENGINE_ASSERT(thread->system->config.spin_count <= JOB_SPIN_COUNT_MAX);
	ENGINE_ASSERT(thread->ring_count > 0);
	for (spin = 0; spin < thread->system->config.spin_count; ++spin)
	{
		if (job_awake(thread, graph, draining))
			return;
		job_pause();
	}
}

/* The sleeper raises the sleeper count, then reads the rings; a waker
   publishes the rings, then reads the sleeper count. Both are sequentially
   consistent, so either the waker sees the sleeper or the sleeper sees the work. */
static void job_sleep(struct job_thread const *thread, struct engine_job_graph const *graph,
	int draining)
{
	struct engine_job_system *system = thread->system;
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(atomic_load(&system->sleepers) <= ENGINE_JOB_WORKERS_MAX);
	pthread_mutex_lock(&system->mutex);
	atomic_fetch_add(&system->sleepers, 1);
	if (!job_awake(thread, graph, draining))
		pthread_cond_wait(&system->wake, &system->mutex);
	atomic_fetch_sub(&system->sleepers, 1);
	pthread_mutex_unlock(&system->mutex);
}

/* ---- Rings ---------------------------------------------------------------- */

static void job_assert_main(struct engine_job_system const *system)
{
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(pthread_equal(pthread_self(), system->main_thread));
}

static uint32_t job_ring_of(struct engine_job_record const *record)
{
	ENGINE_ASSERT(record != NULL);
	ENGINE_ASSERT(record->priority < ENGINE_JOB_PRIORITY_COUNT);
	if (record->flags & ENGINE_JOB_FLAG_MAIN_THREAD)
		return JOB_RING_MAIN;
	return record->priority;
}

static struct engine_job_ready job_ready_entry(struct engine_job_graph *graph, uint32_t job)
{
	struct engine_job_record const *record = &graph->config.jobs[job];
	struct engine_job_ready entry;
	ENGINE_ASSERT(job < graph->job_count);
	ENGINE_ASSERT(record->partition_count >= 1);
	entry.graph = graph;
	entry.job = job;
	entry.ring = job_ring_of(record);
	entry.partition_count = record->partition_count;
	entry.cost_hint_nanoseconds = record->cost_hint_nanoseconds;
	return entry;
}

/* Lock held. Submission credits guarantee capacity; the caller publishes. */
static void job_ring_push(struct engine_job_system *system, struct engine_job_ready const *entry)
{
	struct engine_job_ready_ring *ring = &system->rings[entry->ring];
	uint32_t const slot = (ring->head + ring->count) % ring->capacity;
	ENGINE_ASSERT(entry->ring < JOB_RING_COUNT);
	ENGINE_ASSERT(ring->count < ring->capacity);
	ENGINE_ASSERT(ring->count < ring->reserved);
	ring->entries[slot] = *entry;
	ring->count += 1;
}

/* Lock held. Removes the front entry, which this thread just claimed. */
static void job_ring_pop(struct engine_job_system *system, uint32_t ring_index,
	struct engine_job_graph const *graph, uint32_t job)
{
	struct engine_job_ready_ring *ring = &system->rings[ring_index];
	ENGINE_ASSERT(ring->count > 0);
	ENGINE_ASSERT(ring->entries[ring->head].graph == graph &&
		ring->entries[ring->head].job == job);
	ring->head = (ring->head + 1u) % ring->capacity;
	ring->count -= 1;
}

static uint32_t job_claim_size(struct engine_job_system const *system,
	uint32_t cost_hint_nanoseconds, uint32_t remaining, uint32_t limit)
{
	uint32_t const cost = cost_hint_nanoseconds ? cost_hint_nanoseconds :
		JOB_COST_UNKNOWN_NANOSECONDS;
	uint32_t const by_cost = JOB_CLAIM_NANOSECONDS / cost;
	uint32_t size = remaining / (system->config.worker_count + 1u);
	ENGINE_ASSERT(limit >= 1);
	ENGINE_ASSERT(remaining >= 1);
	if (size > by_cost)
		size = by_cost;
	if (size > limit)
		size = limit;
	return size ? size : 1u;
}

/* Lock held. Claims a chunk of the front job's partitions; the claimer of the
   last partition pops the entry in the same critical section. */
static int job_ring_claim_chunk(struct engine_job_system *system, uint32_t ring_index,
	struct engine_job_ready entry, struct job_batch *batch)
{
	struct engine_job_record *record = &entry.graph->config.jobs[entry.job];
	uint32_t const count = entry.partition_count;
	uint32_t begin = atomic_load_explicit(&record->next_partition, memory_order_relaxed);
	uint32_t size;
	ENGINE_ASSERT(count > 1);
	ENGINE_ASSERT(batch->job_count == 0);
	/* Exhausted: the thread that claimed its last partition is about to pop it. */
	if (begin >= count)
		return 0;
	size = job_claim_size(system, entry.cost_hint_nanoseconds, count - begin,
		JOB_CHUNK_PARTITIONS_MAX);
	begin = atomic_fetch_add_explicit(&record->next_partition, size, memory_order_acq_rel);
	if (begin >= count)
		return 0;
	batch->chunk_job = entry.job;
	batch->chunk_begin = begin;
	batch->chunk_end = count - begin > size ? begin + size : count;
	if (batch->chunk_end == count)
	{
		job_ring_pop(system, ring_index, entry.graph, entry.job);
		job_ring_publish(system, ring_index);
	}
	return 1;
}

/* Lock held. Pops consecutive single-partition jobs of one graph at once, so
   no other thread ever sees them claimed. */
static void job_ring_claim_jobs(struct engine_job_system *system, uint32_t ring_index,
	struct job_batch *batch)
{
	struct engine_job_ready_ring *ring = &system->rings[ring_index];
	struct engine_job_ready const first = ring->entries[ring->head];
	uint32_t const limit = job_claim_size(system, first.cost_hint_nanoseconds, ring->count,
		JOB_BATCH_JOBS_MAX);
	ENGINE_ASSERT(first.graph == batch->graph);
	ENGINE_ASSERT(limit <= JOB_BATCH_JOBS_MAX);
	while (batch->job_count < limit && ring->count > 0)
	{
		struct engine_job_ready const entry = ring->entries[ring->head];
		if (entry.graph != batch->graph || entry.partition_count != 1)
			break;
		job_ring_pop(system, ring_index, entry.graph, entry.job);
		batch->jobs[batch->job_count] = entry.job;
		batch->job_count += 1;
	}
	job_ring_publish(system, ring_index);
}

/* Lock held. */
static int job_ring_take(struct engine_job_system *system, uint32_t ring_index,
	struct job_batch *batch)
{
	struct engine_job_ready_ring *ring = &system->rings[ring_index];
	struct engine_job_ready entry;
	ENGINE_ASSERT(ring_index < JOB_RING_COUNT);
	ENGINE_ASSERT(batch != NULL);
	if (ring->count == 0)
		return 0;
	entry = ring->entries[ring->head];
	batch->graph = entry.graph;
	batch->ring = ring_index;
	batch->job_count = 0;
	batch->chunk_job = ENGINE_JOB_INVALID;
	if (entry.partition_count > 1)
		return job_ring_claim_chunk(system, ring_index, entry, batch);
	job_ring_claim_jobs(system, ring_index, batch);
	ENGINE_ASSERT(batch->job_count >= 1);
	return 1;
}

static int job_take(struct job_thread const *thread, struct job_batch *batch)
{
	struct engine_job_system *system = thread->system;
	uint32_t index;
	int taken = 0;
	ENGINE_ASSERT(thread->rings != NULL);
	ENGINE_ASSERT(thread->ring_count <= JOB_RING_COUNT);
	/* Skip the lock entirely when no eligible ring has entries. */
	if (!job_rings_ready(thread))
		return 0;
	job_lock(system);
	for (index = 0; !taken && index < thread->ring_count; ++index)
		taken = job_ring_take(system, thread->rings[index], batch);
	job_unlock(system);
	return taken;
}

/* Lock held. The last job of a graph returns its ring credits; after this
   nothing may touch the graph except its owner. The caller wakes sleepers. */
static void job_graph_finished(struct engine_job_system *system, struct engine_job_graph *graph)
{
	uint32_t ring;
	ENGINE_ASSERT(atomic_load(&system->graphs_in_flight) > 0);
	ENGINE_ASSERT(atomic_load(&graph->remaining_jobs) == 0);
	for (ring = 0; ring < JOB_RING_COUNT; ++ring)
	{
		ENGINE_ASSERT(system->rings[ring].reserved >= graph->ready_count[ring]);
		system->rings[ring].reserved -= graph->ready_count[ring];
	}
	atomic_fetch_sub(&system->graphs_in_flight, 1);
	atomic_store(&graph->complete, 1);
}

/* ---- Running batches ------------------------------------------------------ */

static void job_ready_flush(struct job_run *run)
{
	struct engine_job_system *system = run->thread->system;
	uint32_t touched = 0;
	uint32_t index;
	ENGINE_ASSERT(run->ready_count <= JOB_READY_LOCAL_MAX);
	if (run->ready_count == 0)
		return;
	job_lock(system);
	for (index = 0; index < run->ready_count; ++index)
	{
		job_ring_push(system, &run->ready[index]);
		touched |= 1u << run->ready[index].ring;
	}
	for (index = 0; index < JOB_RING_COUNT; ++index)
		if (touched & (1u << index))
			job_ring_publish(system, index);
	job_unlock(system);
	job_wake(system);
	run->ready_count = 0;
}

/* A ready successor runs on the thread that readied it when this thread may
   run it, it has one partition, and no ring this thread prefers holds work.
   The thread would otherwise push it, wake others, and likely take it back. */
static int job_continuable(struct job_run const *run, struct engine_job_record const *record)
{
	uint32_t const ring = job_ring_of(record);
	uint32_t index;
	ENGINE_ASSERT(run->continuation == ENGINE_JOB_INVALID);
	ENGINE_ASSERT(run->thread->ring_count <= JOB_RING_COUNT);
	if (record->partition_count != 1)
		return 0;
	for (index = 0; index < run->thread->ring_count; ++index)
	{
		uint32_t const preferred = run->thread->rings[index];
		if (preferred == ring)
			return 1;
		if (atomic_load_explicit(&run->thread->system->ring_ready[preferred],
			memory_order_relaxed) > 0)
			return 0;
	}
	return 0;
}

static void job_ready(struct job_run *run, uint32_t job)
{
	struct engine_job_record const *record = &run->graph->config.jobs[job];
	ENGINE_ASSERT(job < run->graph->job_count);
	ENGINE_ASSERT(atomic_load_explicit(&run->graph->config.jobs[job].pending_predecessors,
		memory_order_relaxed) == 0);
	engine_job_graph_profile_ready(run->graph, job);
	engine_job_graph_acquire(run->graph, job);
	if (run->continuation == ENGINE_JOB_INVALID && job_continuable(run, record))
	{
		run->continuation = job;
		return;
	}
	if (run->ready_count == JOB_READY_LOCAL_MAX)
		job_ready_flush(run);
	run->ready[run->ready_count] = job_ready_entry(run->graph, job);
	run->ready_count += 1;
}

/* Applies the combined decrements; successors that reach zero are ready. */
static void job_decrements_apply(struct job_run *run)
{
	uint32_t index;
	ENGINE_ASSERT(run->decrement_count <= JOB_DECREMENTS_MAX);
	ENGINE_ASSERT(run->graph != NULL);
	for (index = 0; index < run->decrement_count; ++index)
	{
		uint32_t const job = run->decrement_jobs[index];
		uint32_t const amount = run->decrement_amounts[index];
		uint32_t const before = atomic_fetch_sub_explicit(
			&run->graph->config.jobs[job].pending_predecessors, amount, memory_order_acq_rel);
		ENGINE_ASSERT(before >= amount);
		if (before == amount)
			job_ready(run, job);
	}
	run->decrement_count = 0;
}

static void job_decrement(struct job_run *run, uint32_t job)
{
	uint32_t index = run->decrement_count;
	ENGINE_ASSERT(job < run->graph->job_count);
	ENGINE_ASSERT(index <= JOB_DECREMENTS_MAX);
	/* Newest first: jobs in a batch usually share their latest successor. */
	while (index > 0)
	{
		index -= 1;
		if (run->decrement_jobs[index] != job)
			continue;
		run->decrement_amounts[index] += 1;
		return;
	}
	if (run->decrement_count == JOB_DECREMENTS_MAX)
		job_decrements_apply(run);
	run->decrement_jobs[run->decrement_count] = job;
	run->decrement_amounts[run->decrement_count] = 1;
	run->decrement_count += 1;
}

static void job_finish(struct job_run *run, uint32_t job)
{
	struct engine_job_graph *graph = run->graph;
	uint32_t edge = graph->config.jobs[job].successor_head;
	uint32_t visited = 0;
	ENGINE_ASSERT(job < graph->job_count);
	engine_job_graph_release(graph, job);
	while (edge != ENGINE_JOB_INVALID)
	{
		ENGINE_ASSERT(visited < graph->edge_count);
		job_decrement(run, graph->config.edges[edge].target);
		edge = graph->config.edges[edge].next;
		visited += 1;
	}
	run->jobs_completed += 1;
}

static void job_tally(struct job_run *run, enum engine_job_partition_outcome outcome)
{
	ENGINE_ASSERT((uint32_t)outcome < 3u);
	ENGINE_ASSERT(run->graph != NULL);
	run->tallies[outcome] += 1;
}

static void job_run_single(struct job_run *run, uint32_t job)
{
	ENGINE_ASSERT(job < run->graph->job_count);
	ENGINE_ASSERT(run->graph->config.jobs[job].partition_count == 1);
	job_tally(run, engine_job_graph_execute_partition(run->graph, job, 0));
	job_finish(run, job);
}

static void job_run_tallies_flush(struct job_run *run)
{
	uint64_t const claimed = run->tallies[ENGINE_JOB_PARTITION_EXECUTED] +
		run->tallies[ENGINE_JOB_PARTITION_SKIPPED];
	ENGINE_ASSERT(run->graph != NULL);
	ENGINE_ASSERT(claimed <= ENGINE_JOB_PARTITIONS_PER_JOB_MAX * (uint64_t)run->graph->job_count);
	engine_job_graph_add_tallies(run->graph, run->tallies[ENGINE_JOB_PARTITION_EXECUTED],
		run->tallies[ENGINE_JOB_PARTITION_SKIPPED], run->thread->on_worker ? claimed : 0);
	memset(run->tallies, 0, sizeof(run->tallies));
}

/* Runs the claimed chunk, then claims more of the same job without the lock.
   Completed partitions are added once, after the job's partitions are all
   claimed, so the job cannot finish while this thread still touches it. */
static void job_run_chunk(struct job_run *run, uint32_t ring, uint32_t job,
	uint32_t begin, uint32_t end)
{
	struct engine_job_system *system = run->thread->system;
	struct engine_job_record *record = &run->graph->config.jobs[job];
	uint32_t const count = record->partition_count;
	uint32_t claimed = 0;
	ENGINE_ASSERT(begin < end && end <= count);
	for (;;)
	{
		for (; begin < end; ++begin, ++claimed)
			job_tally(run, engine_job_graph_execute_partition(run->graph, job, begin));
		ENGINE_ASSERT(claimed <= count);
		begin = atomic_load_explicit(&record->next_partition, memory_order_relaxed);
		if (begin >= count)
			break;
		end = job_claim_size(system, record->cost_hint_nanoseconds, count - begin,
			JOB_CHUNK_PARTITIONS_MAX);
		begin = atomic_fetch_add_explicit(&record->next_partition, end, memory_order_acq_rel);
		if (begin >= count)
			break;
		end = count - begin > end ? begin + end : count;
		if (end < count)
			continue;
		job_lock(system);
		job_ring_pop(system, ring, run->graph, job);
		job_ring_publish(system, ring);
		job_unlock(system);
	}
	/* Publish the tallies first: once the other claimers' completions arrive,
	   the graph may finish and must not be touched again by this thread. */
	job_run_tallies_flush(run);
	if (atomic_fetch_add_explicit(&record->completed_partitions, claimed,
		memory_order_acq_rel) + claimed == count)
		job_finish(run, job);
}

/* Adds the batch's tallies, then its completed jobs; the thread that brings
   the count to zero finishes the graph. Tallies always precede the completion
   that could finish the graph, and nothing touches the graph after it. */
static void job_run_flush(struct job_run *run)
{
	struct engine_job_system *system = run->thread->system;
	ENGINE_ASSERT(run->ready_count == 0 && run->decrement_count == 0);
	ENGINE_ASSERT(run->continuation == ENGINE_JOB_INVALID);
	if (run->jobs_completed == 0)
	{
		/* Only a chunk that did not finish its job; it flushed already. */
		ENGINE_ASSERT(run->tallies[0] == 0 && run->tallies[1] == 0);
		return;
	}
	job_run_tallies_flush(run);
	if (atomic_fetch_sub_explicit(&run->graph->remaining_jobs, run->jobs_completed,
		memory_order_acq_rel) != run->jobs_completed)
		return;
	job_lock(system);
	job_graph_finished(system, run->graph);
	job_unlock(system);
	job_wake(system);
}

static void job_run_batch(struct job_thread const *thread, struct job_batch const *batch)
{
	struct job_run run;
	uint32_t steps = 0;
	uint32_t index;
	ENGINE_ASSERT(batch->job_count <= JOB_BATCH_JOBS_MAX);
	ENGINE_ASSERT(batch->job_count > 0 || batch->chunk_job != ENGINE_JOB_INVALID);
	run.thread = thread;
	run.graph = batch->graph;
	run.continuation = ENGINE_JOB_INVALID;
	run.ready_count = 0;
	run.decrement_count = 0;
	memset(run.tallies, 0, sizeof(run.tallies));
	run.jobs_completed = 0;
	for (index = 0; index < batch->job_count; ++index)
		job_run_single(&run, batch->jobs[index]);
	if (batch->chunk_job != ENGINE_JOB_INVALID)
		job_run_chunk(&run, batch->ring, batch->chunk_job, batch->chunk_begin, batch->chunk_end);
	/* Publish ready successors before running a continuation, so other
	   threads can start on them meanwhile. */
	job_decrements_apply(&run);
	job_ready_flush(&run);
	while (run.continuation != ENGINE_JOB_INVALID)
	{
		uint32_t const job = run.continuation;
		ENGINE_ASSERT(steps < run.graph->job_count);
		run.continuation = ENGINE_JOB_INVALID;
		job_run_single(&run, job);
		job_decrements_apply(&run);
		job_ready_flush(&run);
		steps += 1;
	}
	job_run_flush(&run);
}

/* Takes and runs work until `job_awake`'s exit condition holds; spins a
   bounded count, then sleeps, whenever nothing is ready. */
static void job_serve(struct job_thread const *thread, struct engine_job_graph *graph,
	int draining)
{
	struct engine_job_system *system = thread->system;
	struct job_batch batch;
	int spun = 0;
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(thread->on_worker == (graph == NULL && !draining));
	for (;;)
	{
		if (atomic_load(&system->exiting))
			return;
		if (!thread->on_worker && (graph ? engine_job_graph_complete(graph) :
			atomic_load(&system->graphs_in_flight) == 0))
			return;
		if (job_take(thread, &batch))
		{
			job_run_batch(thread, &batch);
			spun = 0;
			continue;
		}
		if (!spun && system->config.spin_count > 0)
		{
			job_spin(thread, graph, draining);
			spun = 1;
			continue;
		}
		job_sleep(thread, graph, draining);
		spun = 0;
	}
}

/* ---- Workers and lifetime ------------------------------------------------- */

/* Workers trace only in builds that compile zones in (ADR 0051); shipping
   builds keep no reference to the trace API. */
#if defined(ENGINE_TRACE_ENABLED)
static void job_worker_trace_register(struct engine_job_worker *worker)
{
	struct engine_trace_thread *thread = NULL;
	char name[ENGINE_TRACE_NAME_BYTES_MAX];
	int const length = snprintf(name, sizeof(name), "job_worker_%02u", worker->index);
	ENGINE_ASSERT(length > 0 && (size_t)length < sizeof(name));
	ENGINE_ASSERT(worker->system->config.trace != NULL);
	if (engine_trace_thread_register(worker->system->config.trace, name, &thread) ==
		ENGINE_TRACE_OK)
		return;
	/* Tracing is diagnostic: run untraced and report the shortfall. */
	atomic_fetch_add(&worker->system->trace_registration_failures, 1);
}
#else
static void job_worker_trace_register(struct engine_job_worker *worker)
{
	ENGINE_ASSERT(worker != NULL);
	ENGINE_ASSERT(worker->system->config.trace != NULL);
}
#endif

static void *job_worker_main(void *argument)
{
	struct engine_job_worker *worker = argument;
	struct engine_job_system *system = worker->system;
	int const reserved = worker->index < system->config.simulation_reserved_workers;
	struct job_thread thread;
	ENGINE_ASSERT(worker->index < system->config.worker_count);
	thread.system = system;
	thread.rings = reserved ? job_rings_reserved : job_rings_general;
	thread.ring_count = reserved ? 1u : 4u;
	thread.on_worker = 1;
	engine_job_thread_slot = worker->index + 1u;
	if (system->config.trace)
		job_worker_trace_register(worker);
	job_serve(&thread, NULL, 0);
	/* Shutdown drains every graph before workers are told to exit. */
	ENGINE_ASSERT(atomic_load(&system->graphs_in_flight) == 0);
	return NULL;
}

static enum engine_job_result job_config_validate(struct engine_job_system_config const *config)
{
	ENGINE_ASSERT(config != NULL);
	ENGINE_ASSERT(JOB_RING_COUNT <= UINT32_MAX / ENGINE_JOB_JOBS_PER_GRAPH_MAX);
	if (config->worker_count > ENGINE_JOB_WORKERS_MAX ||
		config->spin_count > JOB_SPIN_COUNT_MAX)
		return ENGINE_JOB_INVALID_ARGUMENT;
	/* At least one general worker must exist, or none at all. */
	if (config->worker_count == 0 ? config->simulation_reserved_workers != 0 :
		config->simulation_reserved_workers >= config->worker_count)
		return ENGINE_JOB_INVALID_ARGUMENT;
	if (!config->ready_entries || config->ready_capacity == 0 ||
		config->ready_capacity > ENGINE_JOB_JOBS_PER_GRAPH_MAX)
		return ENGINE_JOB_INVALID_ARGUMENT;
	return ENGINE_JOB_OK;
}

static int job_synchronization_init(struct engine_job_system *system)
{
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(system->workers_started == 0);
	if (pthread_mutex_init(&system->mutex, NULL) != 0)
		return 0;
	if (pthread_cond_init(&system->wake, NULL) != 0)
	{
		pthread_mutex_destroy(&system->mutex);
		return 0;
	}
	return 1;
}

static void job_workers_exit(struct engine_job_system *system)
{
	uint32_t index;
	ENGINE_ASSERT(system->workers_started <= ENGINE_JOB_WORKERS_MAX);
	atomic_store(&system->exiting, 1);
	pthread_mutex_lock(&system->mutex);
	pthread_cond_broadcast(&system->wake);
	pthread_mutex_unlock(&system->mutex);
	for (index = 0; index < system->workers_started; ++index)
	{
		int const joined = pthread_join(system->workers[index].thread, NULL);
		ENGINE_ASSERT(joined == 0);
	}
	system->workers_started = 0;
}

static void job_synchronization_destroy(struct engine_job_system *system)
{
	ENGINE_ASSERT(system->workers_started == 0);
	ENGINE_ASSERT(atomic_load(&system->graphs_in_flight) == 0);
	pthread_cond_destroy(&system->wake);
	pthread_mutex_destroy(&system->mutex);
}

enum engine_job_result engine_job_system_init(struct engine_job_system *system,
	struct engine_job_system_config const *config)
{
	enum engine_job_result const result = job_config_validate(config);
	uint32_t ring;
	ENGINE_ASSERT(system != NULL);
	if (result != ENGINE_JOB_OK)
		return result;
	memset(system, 0, sizeof(*system));
	system->config = *config;
	system->main_thread = pthread_self();
	for (ring = 0; ring < JOB_RING_COUNT; ++ring)
	{
		system->rings[ring].entries = config->ready_entries + (size_t)ring * config->ready_capacity;
		system->rings[ring].capacity = config->ready_capacity;
	}
	if (!job_synchronization_init(system))
		return ENGINE_JOB_PLATFORM_FAILED;
	for (; system->workers_started < config->worker_count; system->workers_started += 1)
	{
		struct engine_job_worker *worker = &system->workers[system->workers_started];
		worker->system = system;
		worker->index = system->workers_started;
		if (pthread_create(&worker->thread, NULL, job_worker_main, worker) != 0)
			break;
		worker->started = 1;
	}
	if (system->workers_started == config->worker_count)
	{
		ENGINE_ASSERT(atomic_load(&system->graphs_in_flight) == 0);
		return ENGINE_JOB_OK;
	}
	job_workers_exit(system);
	job_synchronization_destroy(system);
	return ENGINE_JOB_PLATFORM_FAILED;
}

/* Main-thread help: the main ring first, then tick work; everything when no
   worker exists or during shutdown. */
static struct job_thread job_main_thread(struct engine_job_system *system, int all_rings)
{
	struct job_thread thread;
	ENGINE_ASSERT(sizeof(job_rings_main_only) / sizeof(job_rings_main_only[0]) == JOB_RING_COUNT);
	ENGINE_ASSERT(sizeof(job_rings_main) / sizeof(job_rings_main[0]) == 3u);
	thread.system = system;
	thread.on_worker = 0;
	thread.rings = job_rings_main;
	thread.ring_count = 3u;
	if (all_rings || system->config.worker_count == 0)
	{
		thread.rings = job_rings_main_only;
		thread.ring_count = JOB_RING_COUNT;
	}
	return thread;
}

static int job_credits_available(struct engine_job_system const *system,
	struct engine_job_graph const *graph)
{
	uint32_t ring;
	ENGINE_ASSERT(system != NULL);
	ENGINE_ASSERT(graph != NULL);
	for (ring = 0; ring < JOB_RING_COUNT; ++ring)
		if (graph->ready_count[ring] > system->rings[ring].capacity -
			system->rings[ring].reserved)
			return 0;
	return 1;
}

/* Lock held. Roots never conflict with each other: a conflict would have
   added an edge. */
static void job_push_roots(struct engine_job_system *system, struct engine_job_graph *graph)
{
	uint32_t ring;
	uint32_t job;
	ENGINE_ASSERT(atomic_load(&graph->running) == 1);
	ENGINE_ASSERT(graph->system == system);
	for (job = 0; job < graph->job_count; ++job)
	{
		struct engine_job_ready entry;
		if (graph->config.jobs[job].predecessor_count != 0)
			continue;
		engine_job_graph_acquire(graph, job);
		entry = job_ready_entry(graph, job);
		job_ring_push(system, &entry);
	}
	for (ring = 0; ring < JOB_RING_COUNT; ++ring)
		job_ring_publish(system, ring);
	if (graph->job_count == 0)
		job_graph_finished(system, graph);
}

enum engine_job_result engine_job_system_submit(struct engine_job_system *system,
	struct engine_job_graph *graph)
{
	uint32_t ring;
	job_assert_main(system);
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(graph->mode == ENGINE_JOB_GRAPH_DEFERRED);
	if (graph->overflow != ENGINE_JOB_OK)
		return ENGINE_JOB_OVERFLOWED;
	if (atomic_load(&system->stopping))
		return ENGINE_JOB_SHUTTING_DOWN;
	job_lock(system);
	if (!job_credits_available(system, graph))
	{
		job_unlock(system);
		return ENGINE_JOB_READY_EXHAUSTED;
	}
	for (ring = 0; ring < JOB_RING_COUNT; ++ring)
		system->rings[ring].reserved += graph->ready_count[ring];
	atomic_fetch_add(&system->graphs_in_flight, 1);
	job_unlock(system);
	engine_job_graph_prepare_run(graph, system);
	job_lock(system);
	job_push_roots(system, graph);
	job_unlock(system);
	job_wake(system);
	return ENGINE_JOB_OK;
}

int engine_job_graph_complete(struct engine_job_graph const *graph)
{
	uint32_t complete;
	ENGINE_ASSERT(graph != NULL);
	complete = atomic_load((_Atomic uint32_t *)&graph->complete);
	/* Paired with job_graph_finished: completion implies no remaining jobs. */
	ENGINE_ASSERT(!complete || atomic_load_explicit(
		(_Atomic uint32_t *)&graph->remaining_jobs, memory_order_relaxed) == 0);
	return complete != 0;
}

enum engine_job_result engine_job_system_wait(struct engine_job_system *system,
	struct engine_job_graph *graph, struct engine_job_run_report *report)
{
	job_assert_main(system);
	ENGINE_ASSERT(graph != NULL);
	ENGINE_ASSERT(graph->system == system);
	/* A completed graph needs no help, so reports can be collected after shutdown. */
	if (!engine_job_graph_complete(graph))
	{
		struct job_thread const thread = job_main_thread(system, 0);
		job_serve(&thread, graph, 0);
	}
	ENGINE_ASSERT(engine_job_graph_complete(graph));
	engine_job_graph_finish_report(graph, report);
	return atomic_load(&graph->failed) ? ENGINE_JOB_FAILED : ENGINE_JOB_OK;
}

enum engine_job_result engine_job_system_run(struct engine_job_system *system,
	struct engine_job_graph *graph, struct engine_job_run_report *report)
{
	enum engine_job_result result;
	ENGINE_ASSERT(system != NULL);
	result = engine_job_system_submit(system, graph);
	job_assert_main(system);
	ENGINE_ASSERT(graph != NULL);
	if (result != ENGINE_JOB_OK)
		return result;
	return engine_job_system_wait(system, graph, report);
}

void engine_job_system_shutdown(struct engine_job_system *system)
{
	struct job_thread const thread = job_main_thread(system, 1);
	job_assert_main(system);
	ENGINE_ASSERT(system->workers_started == system->config.worker_count);
	atomic_store(&system->stopping, 1);
	/* Finish pending work, including main-thread jobs only this thread can run. */
	job_serve(&thread, NULL, 1);
	ENGINE_ASSERT(atomic_load(&system->graphs_in_flight) == 0);
	job_workers_exit(system);
	ENGINE_ASSERT(system->workers_started == 0);
	job_synchronization_destroy(system);
}
