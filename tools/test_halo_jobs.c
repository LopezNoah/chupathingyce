/* ChupathingyCE job-graph integration tests (tools/test_halo_jobs.py).

   Links the downstream scheduler (engine/core/jobs), the platform adapter
   (port/linux/src/posix_jobs.c) and the game's real interpolation kernel and
   blend jobs (port/linux/game/render_interpolation_blend.inc) with stand-ins
   for the game's math types. The driver runs it once per configuration
   (HALO_JOBS=sequential, HALO_JOB_WORKERS=0/1/2/4/8) and compares the hashes
   it prints, so parallel results must equal the sequential oracle's bytes.

   Usage: test_halo_jobs [bench] */

#include "../engine/core/jobs/job_phase.h"
#include "../port/linux/src/halo_jobs.h"
#include "allocation_guard.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) \
	do { \
		if (!(condition)) \
		{ \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)

void platform_log(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

/* ---------- the game's types (source/math/real_math.h), as laid out there */

typedef float real;
typedef int boolean;
typedef unsigned char byte;
#define TRUE 1
#define FALSE 0
#define NONE (-1L)
#define NUMBEROF(array) (sizeof(array) / sizeof(array[0]))
#define MAXIMUM_OBJECTS_PER_MAP 8192

typedef struct { real i, j, k; } real_vector3d;
typedef struct { real x, y, z; } real_point3d;
typedef struct
{
	real scale;
	real_vector3d forward;
	real_vector3d left;
	real_vector3d up;
	real_point3d position;
} real_matrix4x3;

static real_vector3d const zero_vector3d = { 0.0f, 0.0f, 0.0f };
static real_vector3d const *const global_zero_vector3d = &zero_vector3d;

/* source/math/matrix_math.c, verbatim */
static real_point3d *matrix4x3_inverse_transform_point(real_matrix4x3 const *matrix,
	real_point3d const *point, real_point3d *result)
{
	if (matrix->scale != 0.f)
	{
		real x = point->x - matrix->position.x;
		real y = point->y - matrix->position.y;
		real z = point->z - matrix->position.z;

		if (matrix->scale != 1.f)
		{
			real scale = 1.f / matrix->scale;
			x *= scale;
			y *= scale;
			z *= scale;
		}
		result->x = x * matrix->forward.i + y * matrix->forward.j + z * matrix->forward.k;
		result->y = x * matrix->left.i + y * matrix->left.j + z * matrix->left.k;
		result->z = x * matrix->up.i + y * matrix->up.j + z * matrix->up.k;
	}
	else
	{
		result->x = 0.f;
		result->y = 0.f;
		result->z = 0.f;
	}
	return result;
}

#include "../port/linux/game/render_interpolation_blend.inc"

/* ---------- deterministic inputs */

static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint32_t rng_next(void)
{
	rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(rng_state >> 33);
}
static real rng_real(real low, real high)
{
	return low + (high - low) * (real)(rng_next() & 0xFFFFFF) / (real)0xFFFFFF;
}

static uint64_t hash_bytes(uint64_t hash, void const *data, size_t size)
{
	unsigned char const *bytes = data;
	size_t index;
	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 0x100000001B3ull;
	return hash;
}

/* ---------- 1. graph semantics through the adapter */

static int order_log[128];
static _Atomic int order_count;
static _Atomic int counters[8];
static _Atomic uint32_t partition_hits[4096];

struct record_argument { int id; };

static int record_order(void const *argument, struct halo_job_partition const *partition)
{
	struct record_argument const *record = argument;
	/* records run order, under the scheduler's guarantees only: each test
	   below declares accesses so that the conflicting ones are ordered */
	(void)partition;
	order_log[atomic_fetch_add(&order_count, 1)] = record->id;
	return 1;
}

/* The declarative example from the task: A snapshot -> B interpolate
   (partitioned) -> C prepare -> D submit (main thread). Only data
   dependencies are declared; the edges are inferred. */
static float example_simulation[1000];
static float example_snapshot[1000];
static float example_presentation[1000];
static double example_prepared;
static double example_submitted;

static int example_snapshot_job(void const *argument, struct halo_job_partition const *partition)
{
	(void)argument;
	memcpy(example_snapshot + partition->item_begin, example_simulation + partition->item_begin,
		(size_t)(partition->item_end - partition->item_begin) * sizeof(float));
	return 1;
}
static int example_interpolate_job(void const *argument, struct halo_job_partition const *partition)
{
	uint64_t index;
	(void)argument;
	for (index = partition->item_begin; index < partition->item_end; index++)
	{
		example_presentation[index] = example_snapshot[index] * 0.5f + 1.0f;
		atomic_fetch_add_explicit(&partition_hits[index], 1, memory_order_relaxed);
	}
	return 1;
}
static int example_prepare_job(void const *argument, struct halo_job_partition const *partition)
{
	double sum = 0.0;
	int index;
	(void)argument;
	(void)partition;
	for (index = 0; index < 1000; index++)
		sum += example_presentation[index];
	example_prepared = sum;
	return 1;
}
extern _Thread_local uint32_t engine_job_thread_slot;
static int example_submit_job(void const *argument, struct halo_job_partition const *partition)
{
	(void)argument;
	(void)partition;
	/* main-thread affinity: slot 0 is the submitting thread */
	CHECK(engine_job_thread_slot == 0);
	example_submitted = example_prepared;
	return 1;
}

static int example_build(void *context, struct halo_job_graph *graph)
{
	enum { SIMULATION = HALO_JOB_RESOURCE_SIMULATION_OBJECTS, SNAPSHOT = HALO_JOB_RESOURCE_INTERPOLATION_SNAPSHOTS,
		POSES = HALO_JOB_RESOURCE_PRESENTATION_POSES, PREPARED = HALO_JOB_RESOURCE_RENDER_PREPARATION,
		DEVICE = HALO_JOB_RESOURCE_GRAPHICS_DEVICE };
	struct halo_job_access const a[] = { { HALO_JOB_READ, SIMULATION }, { HALO_JOB_WRITE, SNAPSHOT } };
	struct halo_job_access const b[] = { { HALO_JOB_READ, SNAPSHOT }, { HALO_JOB_WRITE, POSES } };
	struct halo_job_access const c[] = { { HALO_JOB_READ, POSES }, { HALO_JOB_WRITE, PREPARED } };
	struct halo_job_access const d[] = { { HALO_JOB_READ, PREPARED }, { HALO_JOB_WRITE, DEVICE } };
	struct halo_job_description job;
	(void)context;
	memset(&job, 0, sizeof(job));
	job.name = "A_snapshot_transforms"; job.execute = example_snapshot_job;
	job.accesses = a; job.access_count = 2; job.item_count = 1000; job.partition_size = 100;
	job.priority = HALO_JOB_PRIORITY_SIMULATION;
	CHECK(halo_job_add(graph, &job) != HALO_JOB_INVALID);
	memset(&job, 0, sizeof(job));
	job.name = "B_interpolate_object_range"; job.execute = example_interpolate_job;
	job.accesses = b; job.access_count = 2; job.item_count = 1000; job.partition_size = 64;
	job.priority = HALO_JOB_PRIORITY_PRESENTATION;
	CHECK(halo_job_add(graph, &job) != HALO_JOB_INVALID);
	memset(&job, 0, sizeof(job));
	job.name = "C_prepare_visible_render_data"; job.execute = example_prepare_job;
	job.accesses = c; job.access_count = 2; job.priority = HALO_JOB_PRIORITY_PRESENTATION;
	CHECK(halo_job_add(graph, &job) != HALO_JOB_INVALID);
	memset(&job, 0, sizeof(job));
	job.name = "D_submit_rendering"; job.execute = example_submit_job;
	job.accesses = d; job.access_count = 2; job.main_thread = 1;
	return halo_job_add(graph, &job) != HALO_JOB_INVALID;
}

static void test_declarative_example(uint64_t *hash)
{
	int round, index;
	for (round = 0; round < 50; round++)
	{
		for (index = 0; index < 1000; index++)
			example_simulation[index] = (float)(index * (round + 1)) * 0.25f;
		for (index = 0; index < 1000; index++)
			atomic_store(&partition_hits[index], 0);
		CHECK(halo_jobs_phase_run(HALO_JOB_PHASE_TEST, example_build, NULL));
		for (index = 0; index < 1000; index++)
			CHECK(partition_hits[index] == 1); /* no duplicate or omitted partition */
		CHECK(example_submitted == example_prepared && example_prepared > 0.0);
		*hash = hash_bytes(*hash, example_presentation, sizeof(example_presentation));
		*hash = hash_bytes(*hash, &example_submitted, sizeof(example_submitted));
	}
}

/* RAW, WAR, WAW and read/read, fan-out and fan-in, on one resource R:
     0 W(R)   1 R(R)   2 R(R)   3 W(R)   4 W(R)   5 R(R)+dep(0)
   must order 0<1, 0<2 (RAW), 1<3, 2<3 (WAR), 3<4 (WAW), 4<5 (RAW); 1 and 2
   may overlap. */
static int hazards_build(void *context, struct halo_job_graph *graph)
{
	static uint32_t const zero = 0;
	int const modes[6] = { HALO_JOB_WRITE, HALO_JOB_READ, HALO_JOB_READ, HALO_JOB_WRITE, HALO_JOB_WRITE,
		HALO_JOB_READ };
	int index;
	(void)context;
	for (index = 0; index < 6; index++)
	{
		struct halo_job_access access;
		struct halo_job_description job;
		struct record_argument argument;
		access.mode = (uint32_t)modes[index];
		access.resource = HALO_JOB_RESOURCE_FIRST_USER;
		argument.id = index;
		memset(&job, 0, sizeof(job));
		job.name = "hazard"; job.execute = record_order;
		job.argument = &argument; job.argument_size = sizeof(argument);
		job.accesses = &access; job.access_count = 1;
		if (index == 5)
		{
			job.dependencies = &zero; /* explicit, and already implied */
			job.dependency_count = 1;
		}
		CHECK(halo_job_add(graph, &job) == (uint32_t)index);
	}
	return 1;
}

static void test_hazards(void)
{
	int round;
	for (round = 0; round < 200; round++)
	{
		int position[6], index;
		order_count = 0;
		CHECK(halo_jobs_phase_run(HALO_JOB_PHASE_TEST, hazards_build, NULL));
		CHECK(order_count == 6);
		for (index = 0; index < 6; index++)
			position[order_log[index]] = index;
		CHECK(position[0] < position[1] && position[0] < position[2]);
		CHECK(position[1] < position[3] && position[2] < position[3]);
		CHECK(position[3] < position[4] && position[4] < position[5]);
	}
}

/* The edges and their reasons, straight from the engine graph. */
static void test_edge_kinds(void)
{
	static struct engine_job_record jobs[16];
	static struct engine_job_edge edges[32];
	static uint8_t kinds[32];
	static struct engine_job_access_slot slots[32];
	static struct engine_job_resource resources[16];
	static struct engine_job_reader readers[32];
	struct engine_job_graph graph;
	struct engine_job_graph_config config;
	struct engine_job_access access[2];
	struct engine_job_description description;
	struct record_argument argument;
	uint32_t job, dependency, edge;
	int expected[8][8];
	int index;

	memset(&config, 0, sizeof(config));
	config.jobs = jobs; config.jobs_capacity = 16;
	config.edges = edges; config.edges_capacity = 32; config.edge_kinds = kinds;
	config.accesses = slots; config.accesses_capacity = 32;
	config.resources = resources; config.resources_capacity = 16;
	config.readers = readers; config.readers_capacity = 32;
	CHECK(engine_job_graph_init(&graph, &config) == ENGINE_JOB_OK);
	/* 0 W(x)  1 R(x)  2 W(x)  3 W(x)  4 R(y) dep(1)  [barrier]  6 R(x) */
	memset(expected, 0, sizeof(expected));
	for (index = 0; index < 5; index++)
	{
		memset(&description, 0, sizeof(description));
		argument.id = index;
		description.name = "edge"; description.execute = (engine_job_fn)(void *)0;
		description.argument = &argument; description.argument_size = sizeof(argument);
		access[0].kind = ENGINE_JOB_RESOURCE_USER;
		access[0].identifier = index == 4 ? 2u : 1u;
		access[0].mode = index == 1 || index == 4 ? ENGINE_JOB_ACCESS_READ : ENGINE_JOB_ACCESS_WRITE;
		description.accesses = access; description.access_count = 1;
		if (index == 4)
		{
			dependency = 1;
			description.dependencies = &dependency; description.dependency_count = 1;
		}
		description.execute = (engine_job_fn)record_order;
		CHECK(engine_job_graph_add(&graph, &description, &job) == ENGINE_JOB_OK);
	}
	expected[0][1] = ENGINE_JOB_EDGE_RAW;
	expected[1][2] = ENGINE_JOB_EDGE_WAR;
	expected[2][3] = ENGINE_JOB_EDGE_WAW;
	expected[1][4] = ENGINE_JOB_EDGE_EXPLICIT;
	CHECK(engine_job_graph_barrier(&graph) == ENGINE_JOB_OK); /* job 5 */
	access[0].identifier = 1; access[0].mode = ENGINE_JOB_ACCESS_READ;
	description.dependency_count = 0;
	CHECK(engine_job_graph_add(&graph, &description, &job) == ENGINE_JOB_OK && job == 6);
	/* the barrier joins the sinks (3 and 4); the new segment's read of x
	   follows the barrier, not job 3 (generation changed) */
	expected[3][5] = ENGINE_JOB_EDGE_BARRIER;
	expected[4][5] = ENGINE_JOB_EDGE_BARRIER;
	expected[5][6] = ENGINE_JOB_EDGE_BARRIER;
	for (job = 0; job < graph.job_count; job++)
	{
		int found = 0;
		for (edge = jobs[job].successor_head; edge != ENGINE_JOB_INVALID; edge = edges[edge].next)
		{
			CHECK(expected[job][edges[edge].target] == kinds[edge]);
			found++;
		}
		for (index = 0; index < 8; index++)
			found -= expected[job][index] != 0;
		CHECK(found == 0);
	}
	order_count = 0;
	CHECK(engine_job_graph_run_sequential(&graph, NULL) == ENGINE_JOB_OK);
	CHECK(order_count == 6);
}

/* More jobs than the phase arena holds: overflow, then the direct rebuild
   runs every job once, in order, and is counted. */
static int overflow_build(void *context, struct halo_job_graph *graph)
{
	int index;
	(void)context;
	for (index = 0; index < 100; index++)
	{
		struct halo_job_description job;
		struct record_argument argument;
		struct halo_job_access access = { HALO_JOB_WRITE, HALO_JOB_RESOURCE_FIRST_USER };
		argument.id = index;
		memset(&job, 0, sizeof(job));
		job.name = "overflow"; job.execute = record_order;
		job.argument = &argument; job.argument_size = sizeof(argument);
		job.accesses = &access; job.access_count = 1;
		halo_job_add(graph, &job); /* (INVALID once overflowed: the fallback reruns this builder) */
	}
	return 1;
}

static void test_overflow(void)
{
	int index;
	order_count = 0;
	CHECK(halo_jobs_phase_run(HALO_JOB_PHASE_TEST, overflow_build, NULL));
	/* the deferred build ran nothing; the direct rebuild ran all, in order */
	CHECK(order_count == 100);
	for (index = 0; index < 100; index++)
		CHECK(order_log[index] == index);
	CHECK(halo_jobs_phase_fallbacks(HALO_JOB_PHASE_TEST) == 1);
}

static int failing_job(void const *argument, struct halo_job_partition const *partition)
{
	(void)argument;
	atomic_fetch_add(&counters[0], 1);
	return partition->index != 3;
}
static int failure_build(void *context, struct halo_job_graph *graph)
{
	struct halo_job_description job;
	(void)context;
	memset(&job, 0, sizeof(job));
	job.name = "fails"; job.execute = failing_job;
	job.item_count = 8; job.partition_size = 1;
	return halo_job_add(graph, &job) != HALO_JOB_INVALID;
}

static void test_failure_not_rerun(void)
{
	counters[0] = 0;
	CHECK(!halo_jobs_phase_run(HALO_JOB_PHASE_TEST, failure_build, NULL));
	/* partitions ran at most once each; nothing was rerun after the failure */
	CHECK(counters[0] >= 1 && counters[0] <= 8);
}

static void test_resource_generations(void)
{
	uint32_t const handle = halo_job_resource_handle(7);
	CHECK(halo_job_resource_current(handle));
	CHECK(halo_job_resource_invalidate(7) != handle);
	CHECK(!halo_job_resource_current(handle));
	CHECK(halo_job_resource_current(halo_job_resource_handle(7)));
}

/* ---------- 2. the interpolation blend, jobs against the lazy path */

#define OBJECTS MAXIMUM_INTERPOLATED_OBJECTS
#define LIVE_OBJECTS 400

static struct interpolated_object records[OBJECTS];
static struct interpolated_object reference[OBJECTS];
static real_matrix4x3 node_storage[OBJECTS][3 * 20];
static struct interpolation_rotation rotation_storage[OBJECTS][2 * 20];
static real_matrix4x3 reference_nodes[OBJECTS][3 * 20];
static struct interpolation_rotation reference_rotations[OBJECTS][2 * 20];
/* the authoritative simulation pose, which nothing below may write */
static real_matrix4x3 simulation_pose[OBJECTS][20];
static long current_tick;
static long partition_frames[BLEND_PARTITIONS];

static uint64_t hash_bytes(uint64_t hash, void const *data, size_t size);
static uint64_t simulation_hash(void)
{
	uint64_t hash = 1;
	long object;
	for (object = 0; object < LIVE_OBJECTS; object++)
		hash = hash_bytes(hash, simulation_pose[(object * 7) % OBJECTS], sizeof(simulation_pose[0]));
	return hash;
}

static void random_rotation(real_matrix4x3 *matrix, real yaw)
{
	real c = (real)cos(yaw), s = (real)sin(yaw);
	matrix->scale = 1.0f;
	matrix->forward.i = c; matrix->forward.j = s; matrix->forward.k = 0.0f;
	matrix->left.i = -s; matrix->left.j = c; matrix->left.k = 0.0f;
	matrix->up.i = 0.0f; matrix->up.j = 0.0f; matrix->up.k = 1.0f;
}

static void simulation_step(long tick)
{
	long object;
	for (object = 0; object < LIVE_OBJECTS; object++)
	{
		long const index = (object * 7) % OBJECTS;
		int node;
		for (node = 0; node < 20; node++)
		{
			real_matrix4x3 *matrix = &simulation_pose[index][node];
			random_rotation(matrix, 0.05f * (real)tick + 0.1f * (real)node + rng_real(-0.01f, 0.01f));
			matrix->position.x = (real)object + 0.2f * (real)tick + 0.01f * (real)node;
			matrix->position.y = 0.01f * (real)node;
			matrix->position.z = 0.0f;
			if (object % 97 == 0 && tick % 13 == 0)
				matrix->position.x += 100.0f; /* a teleport: snaps */
			if (object % 89 == 0)
				matrix->scale = 1.5f;     /* a basis no quaternion holds */
		}
	}
}

/* render_interpolation_tick's bookkeeping for these records */
static void snapshot_tick(void)
{
	long object;
	current_tick++;
	for (object = 0; object < LIVE_OBJECTS; object++)
	{
		long const index = (object * 7) % OBJECTS;
		struct interpolated_object *record = &records[index];
		boolean const continuing = record->object_index == index && record->tick == current_tick - 1;
		if (continuing)
		{
			record->latest ^= 1;
			correction_advance(&record->correction, &record->correction_pending);
		}
		memcpy(record->nodes + record->latest * record->node_capacity, simulation_pose[index],
			(size_t)record->node_count * sizeof(real_matrix4x3));
		record->rotations_valid[record->latest] = FALSE;
		record->object_index = index;
		record->tick = current_tick;
		record->has_previous = continuing;
		record->blended_frame = NONE;
		if (object % 31 == 0 && continuing)
		{
			real_vector3d offset = { 0.5f, -0.25f, 0.0f };
			correction_add(&record->correction, &record->correction_pending, &offset);
		}
	}
}

static void records_init(void)
{
	long index;
	memset(records, 0, sizeof(records));
	for (index = 0; index < OBJECTS; index++)
	{
		records[index].object_index = NONE;
		records[index].nodes = node_storage[index];
		records[index].rotations = rotation_storage[index];
		records[index].node_capacity = 20;
		records[index].node_count = (short)(1 + index % 20);
	}
	current_tick = 0;
	memset(partition_frames, 0, sizeof(partition_frames));
}

/* The lazy path's result for a frame, on a copy: what the renderer would
   have got by asking for each record. */
static void lazy_reference(real fraction, long frame)
{
	long index;
	memcpy(reference, records, sizeof(records));
	for (index = 0; index < OBJECTS; index++)
	{
		struct interpolated_object *record = &reference[index];
		if (record->object_index == NONE)
			continue;
		memcpy(reference_nodes[index], node_storage[index], sizeof(node_storage[index]));
		memcpy(reference_rotations[index], rotation_storage[index], sizeof(rotation_storage[index]));
		record->nodes = reference_nodes[index];
		record->rotations = reference_rotations[index];
		if (record->object_index != NONE && record->tick == current_tick && record->has_previous &&
			record->blended_frame != frame)
			interpolated_object_blend(record, fraction, frame);
	}
}

/* Every blended position lies between its snapshots (plus the drawn
   correction), never beyond the latest: no extrapolation. */
static void check_no_extrapolation(real fraction)
{
	long index;
	CHECK(fraction >= 0.0f && fraction <= 1.0f);
	for (index = 0; index < OBJECTS; index++)
	{
		struct interpolated_object const *record = &records[index];
		real_matrix4x3 const *previous, *latest, *blended;
		real_vector3d drawn;
		real low, high, x;
		if (record->object_index == NONE || record->tick != current_tick || !record->has_previous)
			continue;
		previous = record->nodes + (record->latest ^ 1) * record->node_capacity;
		latest = record->nodes + record->latest * record->node_capacity;
		blended = record->nodes + 2 * record->node_capacity;
		correction_drawn(&record->correction, &record->correction_pending, fraction, &drawn);
		if (!correction_significant(&record->correction) && !correction_significant(&record->correction_pending))
			drawn.i = 0.0f;
		low = previous[0].position.x < latest[0].position.x ? previous[0].position.x : latest[0].position.x;
		high = previous[0].position.x < latest[0].position.x ? latest[0].position.x : previous[0].position.x;
		x = blended[0].position.x - drawn.i;
		CHECK(x >= low - 1e-3f && x <= high + 1e-3f);
	}
}

static uint64_t blend_hash(struct interpolated_object const *set, real_matrix4x3 (*nodes)[3 * 20])
{
	uint64_t hash = 1469598103934665603ull;
	long index;
	for (index = 0; index < OBJECTS; index++)
	{
		if (set[index].object_index == NONE || set[index].tick != current_tick || !set[index].has_previous)
			continue;
		hash = hash_bytes(hash, &nodes[index][2 * 20], (size_t)set[index].node_count * sizeof(real_matrix4x3));
	}
	return hash;
}

struct frame_clock
{
	char const *name;
	double interval;   /* seconds; 0: irregular */
};

static uint64_t run_frames(struct frame_clock const *frame_clock, double seconds, int check_reference,
	uint64_t *frame_ns, uint32_t *frame_count)
{
	double leftover = 0.0, elapsed = 0.0;
	long frame = 0, ticks = 0;
	uint64_t hash = 1469598103934665603ull;
	uint64_t simulation_hash_before;

	records_init();
	rng_state = 0x243F6A8885A308D3ull;
	simulation_step(0);
	snapshot_tick();
	*frame_count = 0;
	while (elapsed < seconds)
	{
		double const dt = frame_clock->interval > 0.0 ? frame_clock->interval : rng_real(0.002f, 0.09f);
		long tick_count, index;
		real fraction;
		struct blend_job_argument blend;
		/* game_time.c's arithmetic: whole ticks of the elapsed time, the rest kept */
		double const accumulated = dt + leftover;
		tick_count = (long)floor(accumulated * 30.0);
		leftover = accumulated - (double)tick_count / 30.0;
		elapsed += dt;
		for (index = 0; index < tick_count; index++)
		{
			ticks++;
			simulation_step(ticks);
			snapshot_tick();
		}
		fraction = (real)(leftover * 30.0);
		fraction = fraction >= 0.0f && fraction <= 1.0f ? fraction : fraction < 0.0f ? 0.0f : 1.0f;
		frame++;
		simulation_hash_before = simulation_hash();
		if (check_reference)
			lazy_reference(fraction, frame);
		blend.records = records;
		blend.partition_frames = partition_frames;
		blend.record_count = OBJECTS;
		blend.tick = current_tick;
		blend.frame = frame;
		blend.fraction = fraction;
		{
			struct timespec a, b;
			clock_gettime(CLOCK_MONOTONIC, &a);
			CHECK(halo_jobs_phase_run(HALO_JOB_PHASE_PRESENTATION, blend_graph_build, &blend));
			clock_gettime(CLOCK_MONOTONIC, &b);
			if (frame_ns && *frame_count < 100000u)
				frame_ns[*frame_count] = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull +
					(uint64_t)(b.tv_nsec - a.tv_nsec);
			*frame_count += 1;
		}
		/* presentation never writes the authoritative state */
		CHECK(simulation_hash() == simulation_hash_before);
		if (check_reference)
		{
			CHECK(blend_hash(records, node_storage) == blend_hash(reference, reference_nodes));
			check_no_extrapolation(fraction);
		}
		hash = hash_bytes(hash, &fraction, sizeof(fraction));
		hash = hash_bytes(hash, &current_tick, sizeof(current_tick));
		hash ^= blend_hash(records, node_storage);
	}
	/* the fixed schedule: ticks are the whole 30ths of the time elapsed */
	CHECK(ticks == (long)floor(elapsed * 30.0 + 1e-6) || ticks == (long)floor(elapsed * 30.0 + 1e-6) - 1);
	printf("frames %s: %ld frames, %ld ticks in %.2f s\n", frame_clock->name, frame, ticks, elapsed);
	return hash;
}

static int u64_compare(void const *a, void const *b)
{
	uint64_t x = *(uint64_t const *)a, y = *(uint64_t const *)b;
	return (x > y) - (x < y);
}

static uint64_t frame_samples[100000];

int main(int argc, char **argv)
{
	static struct frame_clock const clocks[] = {
		{ "30fps", 1.0 / 30.0 }, { "40fps", 1.0 / 40.0 }, { "60fps", 1.0 / 60.0 },
		{ "120fps", 1.0 / 120.0 }, { "144fps", 1.0 / 144.0 }, { "irregular", 0.0 },
		{ "multitick_10fps", 1.0 / 10.0 },
	};
	uint64_t hash = 1469598103934665603ull;
	uint32_t index, count;
	int const bench = argc > 1 && !strcmp(argv[1], "bench");
	/* quick: the sanitizer builds (simulated seconds per frame rate) */
	double const seconds = argc > 1 && !strcmp(argv[1], "quick") ? 0.4 : 1.5;

	CHECK(halo_jobs_mode() != HALO_JOBS_OFF);
	if (bench)
	{
		struct frame_clock const bench_clock = { "bench_144fps", 1.0 / 144.0 };
		run_frames(&bench_clock, 1.0, 0, NULL, &count); /* warm */
		run_frames(&bench_clock, 10.0, 0, frame_samples, &count);
		qsort(frame_samples, count, sizeof(frame_samples[0]), u64_compare);
		printf("bench workers=%u samples=%u p50_us=%.1f p95_us=%.1f p99_us=%.1f\n", halo_jobs_worker_count(),
			count, frame_samples[count / 2] / 1000.0, frame_samples[count * 95 / 100] / 1000.0,
			frame_samples[count * 99 / 100] / 1000.0);
		halo_jobs_shutdown();
		return 0;
	}
	allocation_guard_arm();
	test_edge_kinds();
	test_hazards();
	test_declarative_example(&hash);
	test_overflow();
	test_failure_not_rerun();
	test_resource_generations();
	for (index = 0; index < NUMBEROF(clocks); index++)
		hash ^= run_frames(&clocks[index], seconds, 1, NULL, &count) * (index + 1);
	allocation_guard_disarm();
	printf("hash %016" PRIx64 "\n", hash);
	halo_jobs_shutdown();
	return 0;
}
