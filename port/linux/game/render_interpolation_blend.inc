/*
RENDER_INTERPOLATION_BLEND.INC

The pure part of port/linux/game/render_interpolation.c: its constants, an
object's snapshot record, and the blend of one record. Included by
render_interpolation.c and by tools/test_halo_jobs.c (which supplies the
game's math types), so the parallel and the sequential paths are tested on
the very code the game runs. Nothing here reads a global but
global_zero_vector3d.
*/

/* ---------- constants */

/* (by absolute index: the object array's all) */
#define MAXIMUM_INTERPOLATED_OBJECTS MAXIMUM_OBJECTS_PER_MAP
#define MAXIMUM_INTERPOLATED_NODES 64

/* world units (10 feet each) a node may move in one tick before it snaps:
well beyond any vehicle, short of any teleport */
#define OBJECT_SNAP_DISTANCE 10.0f
/* ... a node may move in the object's root node's frame in one tick before
the pose is taken for a new one, not blended to: further than any limb or
part of a model moves in 33 ms (37 m/s), short of the game changing a pose
at once (an actor waking from dormancy, a model swapped), which blended
sweeps the vertices through poses it never had. (In the root's frame, so
that the whole object turning moves nothing: measured in the world, a fast
turn swung the head and weapon far enough to snap, and characters moved
like robots.) */
#define NODE_SNAP_DISTANCE 0.4f
/* ... a first-person node may move relative to the camera */
#define FIRST_PERSON_SNAP_DISTANCE 0.25f
/* a correction's difference left drawn after each tick (of 1) */
#define CORRECTION_DECAY 0.6f
/* ... and small enough to be none */
#define CORRECTION_NEGLIGIBLE 0.001f
/* ... or so large that it is no glide but a jump (corrections come one on
another: their sum is drawn): past the largest a client's own unit or
vehicle is corrected by (3 and 4 world units), short of a snap */
#define CORRECTION_MAXIMUM 8.0f
/* a camera cut: a jump or turn no player or scripted camera makes in 33 ms */
#define CAMERA_CUT_DISTANCE 3.0f
#define CAMERA_CUT_COSINE 0.5f

/* ---------- structures */

struct interpolation_quaternion
{
	real i, j, k, w;
};

/* a node's rotation: whether its basis is one a quaternion can hold, and
that quaternion */
struct interpolation_rotation
{
	struct interpolation_quaternion quaternion;
	boolean is_rotation;
};

struct interpolated_object
{
	long object_index; /* NONE when unused */
	long tick; /* the tick of the latest snapshot */
	short node_count;
	short node_capacity;
	boolean has_previous;
	byte latest; /* which snapshot is the latest */
	long blended_frame;
	/* where it is drawn from where it is: a correction fading, and those
	since the last tick, drawn whole until the next begins to fade them */
	real_vector3d correction;
	real_vector3d correction_pending;
	/* [0] and [1]: the two snapshots, [2]: the blend drawn this frame */
	real_matrix4x3 *nodes;
	/* ... the two snapshots' rotations, found when first blended */
	struct interpolation_rotation *rotations;
	boolean rotations_valid[2];
};

/* ---------- blending */

static real lerp(real a, real b, real t)
{
	return a + (b - a) * t;
}

static void point_lerp(real_point3d const *a, real_point3d const *b, real t, real_point3d *result)
{
	result->x = lerp(a->x, b->x, t);
	result->y = lerp(a->y, b->y, t);
	result->z = lerp(a->z, b->z, t);
}

static real vector_length(real_vector3d const *v)
{
	return (real)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);
}

static void vector_nlerp(real_vector3d const *a, real_vector3d const *b, real t, real_vector3d *result)
{
	real length;

	result->i = lerp(a->i, b->i, t);
	result->j = lerp(a->j, b->j, t);
	result->k = lerp(a->k, b->k, t);
	length = vector_length(result);
	if (length > 1e-6f)
	{
		result->i /= length;
		result->j /= length;
		result->k /= length;
	}
	else
	{
		*result = *b;
	}
}

/* an orthonormal right-handed basis, which a quaternion can represent */
static boolean basis_is_rotation(real_matrix4x3 const *matrix)
{
	real_vector3d cross;
	real determinant;

	if (fabs(vector_length(&matrix->forward) - 1.0f) > 1e-2f ||
		fabs(vector_length(&matrix->left) - 1.0f) > 1e-2f ||
		fabs(vector_length(&matrix->up) - 1.0f) > 1e-2f)
	{
		return FALSE;
	}
	cross.i = matrix->forward.j * matrix->left.k - matrix->forward.k * matrix->left.j;
	cross.j = matrix->forward.k * matrix->left.i - matrix->forward.i * matrix->left.k;
	cross.k = matrix->forward.i * matrix->left.j - matrix->forward.j * matrix->left.i;
	determinant = cross.i * matrix->up.i + cross.j * matrix->up.j + cross.k * matrix->up.k;
	return determinant > 0.5f;
}

/* the basis vectors are the matrix's columns (x forward, y left, z up) */
static void quaternion_from_basis(real_matrix4x3 const *matrix, struct interpolation_quaternion *q)
{
	real m00 = matrix->forward.i, m10 = matrix->forward.j, m20 = matrix->forward.k;
	real m01 = matrix->left.i, m11 = matrix->left.j, m21 = matrix->left.k;
	real m02 = matrix->up.i, m12 = matrix->up.j, m22 = matrix->up.k;
	real trace = m00 + m11 + m22;
	real s;

	if (trace > 0.0f)
	{
		s = 0.5f / (real)sqrt(trace + 1.0f);
		q->w = 0.25f / s;
		q->i = (m21 - m12) * s;
		q->j = (m02 - m20) * s;
		q->k = (m10 - m01) * s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		s = 2.0f * (real)sqrt(1.0f + m00 - m11 - m22);
		q->w = (m21 - m12) / s;
		q->i = 0.25f * s;
		q->j = (m01 + m10) / s;
		q->k = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		s = 2.0f * (real)sqrt(1.0f + m11 - m00 - m22);
		q->w = (m02 - m20) / s;
		q->i = (m01 + m10) / s;
		q->j = 0.25f * s;
		q->k = (m12 + m21) / s;
	}
	else
	{
		s = 2.0f * (real)sqrt(1.0f + m22 - m00 - m11);
		q->w = (m10 - m01) / s;
		q->i = (m02 + m20) / s;
		q->j = (m12 + m21) / s;
		q->k = 0.25f * s;
	}
}

static void basis_from_quaternion(struct interpolation_quaternion const *q, real_matrix4x3 *matrix)
{
	real ii = q->i * q->i, jj = q->j * q->j, kk = q->k * q->k;
	real ij = q->i * q->j, ik = q->i * q->k, jk = q->j * q->k;
	real wi = q->w * q->i, wj = q->w * q->j, wk = q->w * q->k;

	matrix->forward.i = 1.0f - 2.0f * (jj + kk);
	matrix->forward.j = 2.0f * (ij + wk);
	matrix->forward.k = 2.0f * (ik - wj);
	matrix->left.i = 2.0f * (ij - wk);
	matrix->left.j = 1.0f - 2.0f * (ii + kk);
	matrix->left.k = 2.0f * (jk + wi);
	matrix->up.i = 2.0f * (ik + wj);
	matrix->up.j = 2.0f * (jk - wi);
	matrix->up.k = 1.0f - 2.0f * (ii + jj);
}

static void rotation_from_matrix(real_matrix4x3 const *matrix, struct interpolation_rotation *rotation)
{
	rotation->is_rotation = basis_is_rotation(matrix);
	if (rotation->is_rotation)
		quaternion_from_basis(matrix, &rotation->quaternion);
}

/* a matrix a fraction t of the way from a to b, their rotations found */
static void matrix_blend_rotations(
	real_matrix4x3 const *a,
	real_matrix4x3 const *b,
	struct interpolation_rotation const *rotation_a,
	struct interpolation_rotation const *rotation_b,
	real t,
	real_matrix4x3 *result)
{
	result->scale = lerp(a->scale, b->scale, t);
	point_lerp(&a->position, &b->position, t, &result->position);
	if (rotation_a->is_rotation && rotation_b->is_rotation)
	{
		struct interpolation_quaternion qa = rotation_a->quaternion, qb = rotation_b->quaternion, q;
		real length;

		/* q and -q are the same rotation: take the shorter way round */
		if (qa.i * qb.i + qa.j * qb.j + qa.k * qb.k + qa.w * qb.w < 0.0f)
		{
			qb.i = -qb.i;
			qb.j = -qb.j;
			qb.k = -qb.k;
			qb.w = -qb.w;
		}
		q.i = lerp(qa.i, qb.i, t);
		q.j = lerp(qa.j, qb.j, t);
		q.k = lerp(qa.k, qb.k, t);
		q.w = lerp(qa.w, qb.w, t);
		length = (real)sqrt(q.i * q.i + q.j * q.j + q.k * q.k + q.w * q.w);
		if (length > 1e-6f)
		{
			q.i /= length;
			q.j /= length;
			q.k /= length;
			q.w /= length;
			basis_from_quaternion(&q, result);
			return;
		}
	}
	/* a basis a quaternion cannot hold (scaled or mirrored): blend it as is */
	result->forward.i = lerp(a->forward.i, b->forward.i, t);
	result->forward.j = lerp(a->forward.j, b->forward.j, t);
	result->forward.k = lerp(a->forward.k, b->forward.k, t);
	result->left.i = lerp(a->left.i, b->left.i, t);
	result->left.j = lerp(a->left.j, b->left.j, t);
	result->left.k = lerp(a->left.k, b->left.k, t);
	result->up.i = lerp(a->up.i, b->up.i, t);
	result->up.j = lerp(a->up.j, b->up.j, t);
	result->up.k = lerp(a->up.k, b->up.k, t);
}

/* the vector's parts' sum, large enough to be a correction (so written that
one not a number is none) */
static boolean correction_significant(real_vector3d const *correction)
{
	return fabs(correction->i) + fabs(correction->j) + fabs(correction->k) >= CORRECTION_NEGLIGIBLE;
}

/* a correction a tick on: what was drawn fades, what came since is drawn
whole from now on, fading from the next */
static void correction_advance(real_vector3d *correction, real_vector3d *pending)
{
	correction->i = correction->i * CORRECTION_DECAY + pending->i;
	correction->j = correction->j * CORRECTION_DECAY + pending->j;
	correction->k = correction->k * CORRECTION_DECAY + pending->k;
	*pending = *global_zero_vector3d;
	if (!correction_significant(correction))
		*correction = *global_zero_vector3d;
}

/* a correction as drawn this frame: fading through the tick as it does tick
to tick, and those since the tick whole */
static void correction_drawn(real_vector3d const *correction, real_vector3d const *pending, real fraction,
	real_vector3d *drawn)
{
	real fade = lerp(1.0f, CORRECTION_DECAY, fraction);

	drawn->i = correction->i * fade + pending->i;
	drawn->j = correction->j * fade + pending->j;
	drawn->k = correction->k * fade + pending->k;
}

/* a correction added (offset): all of it dropped when the sum is too large
to glide (or not a number) */
static void correction_add(real_vector3d *correction, real_vector3d *pending, real_vector3d const *offset)
{
	real i, j, k;

	pending->i += offset->i;
	pending->j += offset->j;
	pending->k += offset->k;
	i = correction->i + pending->i;
	j = correction->j + pending->j;
	k = correction->k + pending->k;
	if (!(i * i + j * j + k * k <= CORRECTION_MAXIMUM * CORRECTION_MAXIMUM))
	{
		*correction = *global_zero_vector3d;
		*pending = *global_zero_vector3d;
	}
}

static real distance_squared(real_point3d const *a, real_point3d const *b)
{
	real x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;

	return x * x + y * y + z * z;
}

/* ---------- one object's blend

The frame's pose of one object, from its two snapshots, into its own third
buffer (record->nodes + 2 * node_capacity). It reads and writes nothing but
the record (its snapshots, its rotations found once, its blend), so blends of
distinct records may run at once (render_interpolation_prepare_frame). */
static void interpolated_object_blend(struct interpolated_object *record, real fraction, long frame)
{
	real_matrix4x3 const *previous = record->nodes + (record->latest ^ 1) * record->node_capacity;
	real_matrix4x3 const *latest = record->nodes + record->latest * record->node_capacity;
	real_matrix4x3 *blended = record->nodes + 2 * record->node_capacity;
	short node_index;
	/* (so written that a position not a number snaps) */
	boolean snap = !(distance_squared(&previous[0].position, &latest[0].position) <=
		OBJECT_SNAP_DISTANCE * OBJECT_SNAP_DISTANCE);

	/* a node moved further in the root's frame than a tick allows: the
	two snapshots are different poses, not one moving (so written that
	a position not a number snaps) */
	for (node_index = 1; !snap && node_index < record->node_count; node_index++)
	{
		real_point3d previous_local, latest_local;

		matrix4x3_inverse_transform_point(&previous[0], &previous[node_index].position, &previous_local);
		matrix4x3_inverse_transform_point(&latest[0], &latest[node_index].position, &latest_local);
		snap = !(distance_squared(&previous_local, &latest_local) <= NODE_SNAP_DISTANCE * NODE_SNAP_DISTANCE);
	}
	if (!snap)
	{
		struct interpolation_rotation *previous_rotations =
			record->rotations + (record->latest ^ 1) * record->node_capacity;
		struct interpolation_rotation *latest_rotations =
			record->rotations + record->latest * record->node_capacity;
		short snapshot;

		/* (each snapshot's rotations found once, not every frame: its
		nodes' positions may move with a correction, their rotations
		never) */
		for (snapshot = 0; snapshot < 2; snapshot++)
		{
			if (!record->rotations_valid[snapshot])
			{
				real_matrix4x3 const *nodes = record->nodes + snapshot * record->node_capacity;
				struct interpolation_rotation *rotations = record->rotations + snapshot * record->node_capacity;

				for (node_index = 0; node_index < record->node_count; node_index++)
					rotation_from_matrix(&nodes[node_index], &rotations[node_index]);
				record->rotations_valid[snapshot] = TRUE;
			}
		}
		for (node_index = 0; node_index < record->node_count; node_index++)
		{
			matrix_blend_rotations(&previous[node_index], &latest[node_index], &previous_rotations[node_index],
				&latest_rotations[node_index], fraction, &blended[node_index]);
		}
	}
	if (snap)
		memcpy(blended, latest, record->node_count * sizeof(real_matrix4x3));
	if (correction_significant(&record->correction) || correction_significant(&record->correction_pending))
	{
		real_vector3d drawn;

		correction_drawn(&record->correction, &record->correction_pending, fraction, &drawn);
		for (node_index = 0; node_index < record->node_count; node_index++)
		{
			blended[node_index].position.x += drawn.i;
			blended[node_index].position.y += drawn.j;
			blended[node_index].position.z += drawn.k;
		}
	}
	record->blended_frame = frame;
}

/* ---------- the frame's blends as jobs (render_interpolation_prepare_frame) */

#define BLEND_PARTITION_RECORDS 256
#define BLEND_PARTITIONS ((MAXIMUM_INTERPOLATED_OBJECTS + BLEND_PARTITION_RECORDS - 1) / BLEND_PARTITION_RECORDS)
/* measured per eligible record: ~0.3 us (render_interpolation_prepare_frame) */
#define BLEND_PARTITION_COST_NS 20000

struct blend_job_argument
{
	struct interpolated_object *records;
	/* one stamp per partition, the frame it last ran for */
	long *partition_frames;
	/* (32 bits: the Xbox's long, and the argument fits a job record) */
	int32_t record_count;
	int32_t tick;
	int32_t frame;
	real fraction;
};

/* (C89's static assertion: a negative array size fails to compile) */
typedef char blend_job_argument_fits[sizeof(struct blend_job_argument) <= HALO_JOB_ARGUMENT_BYTES_MAX ? 1 : -1];

static int blend_job_execute(void const *argument, struct halo_job_partition const *partition)
{
	struct blend_job_argument const *blend = argument;
	long index;

	for (index = (long)partition->item_begin; index < (long)partition->item_end; index++)
	{
		struct interpolated_object *record = &blend->records[index];

		if (record->object_index != NONE && record->tick == blend->tick && record->has_previous &&
			record->blended_frame != blend->frame)
		{
			interpolated_object_blend(record, blend->fraction, blend->frame);
		}
	}
	/* (a partition's own slot: no two partitions write one) */
	if (blend->partition_frames[partition->index] == blend->frame)
		return 0; /* run twice */
	blend->partition_frames[partition->index] = blend->frame;
	return 1;
}

/* the consumer side, on the main thread: every partition ran, once, for
this frame (an omitted or repeated one fails the phase) */
static int publish_job_execute(void const *argument, struct halo_job_partition const *partition)
{
	struct blend_job_argument const *blend = argument;
	long index;

	(void)partition;
	for (index = 0; index < (blend->record_count + BLEND_PARTITION_RECORDS - 1) / BLEND_PARTITION_RECORDS; index++)
	{
		if (blend->partition_frames[index] != blend->frame)
			return 0;
	}
	return 1;
}

static int blend_graph_build(void *context, struct halo_job_graph *graph)
{
	static struct halo_job_access const blend_accesses[] = {
		{ HALO_JOB_READ, HALO_JOB_RESOURCE_INTERPOLATION_SNAPSHOTS },
		{ HALO_JOB_WRITE, HALO_JOB_RESOURCE_INTERPOLATION_ROTATIONS },
		{ HALO_JOB_WRITE, HALO_JOB_RESOURCE_PRESENTATION_POSES },
	};
	static struct halo_job_access const publish_accesses[] = {
		{ HALO_JOB_READ, HALO_JOB_RESOURCE_PRESENTATION_POSES },
	};
	struct blend_job_argument const *blend = context;
	struct halo_job_description description;

	memset(&description, 0, sizeof(description));
	description.name = "interpolation_blend_objects";
	description.execute = blend_job_execute;
	description.argument = blend;
	description.argument_size = sizeof(*blend);
	description.accesses = blend_accesses;
	description.access_count = NUMBEROF(blend_accesses);
	description.item_count = (uint64_t)blend->record_count;
	description.partition_size = BLEND_PARTITION_RECORDS;
	description.priority = HALO_JOB_PRIORITY_PRESENTATION;
	description.cost_hint_nanoseconds = BLEND_PARTITION_COST_NS;
	if (halo_job_add(graph, &description) == HALO_JOB_INVALID)
		return FALSE;
	/* (ordered after the blends by its read of the poses: no explicit edge) */
	memset(&description, 0, sizeof(description));
	description.name = "interpolation_publish_poses";
	description.execute = publish_job_execute;
	description.argument = blend;
	description.argument_size = sizeof(*blend);
	description.accesses = publish_accesses;
	description.access_count = NUMBEROF(publish_accesses);
	description.priority = HALO_JOB_PRIORITY_PRESENTATION;
	description.main_thread = 1;
	return halo_job_add(graph, &description) != HALO_JOB_INVALID;
}

