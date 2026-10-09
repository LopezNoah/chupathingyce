/* Host-local Forge adapter: stable slots, shared mesh, independent rigid poses.
 * Deletions retain their slots for undo; no tag/object datum is fabricated. */
enum { PLATFORM_INSTANCE_MAX = 64 };
static struct halo_world_edit_transform platform_instances[PLATFORM_INSTANCE_MAX];
static long platform_instance_count;

static long platform_edit_count(void) { return active ? platform_instance_count : 0; }

static boolean platform_edit_get(long index, struct halo_world_edit_transform *state)
{
    if (!active || index < 0 || index >= platform_instance_count) return FALSE;
    *state = platform_instances[index];
    return TRUE;
}

static boolean platform_edit_corners(long index, real_point3d *corners)
{
    struct halo_world_edit_transform *state;
    uint32_t vertex;
    if (!active || index < 0 || index >= platform_instance_count) return FALSE;
    state = &platform_instances[index];
    if (state->deleted) return FALSE;
    for (vertex = 0; vertex < PLATFORM_VERTEX_COUNT; vertex++)
        platform_shape_to_world(&board, state->position, state->rotation,
            board.vertices[vertex], corners[vertex].n);
    return TRUE;
}

static boolean platform_edit_set(long index, struct halo_world_edit_transform const *state)
{
    uint32_t axis, vertex;
    float point[3];
    if (!active || index < 0 || index > platform_instance_count || index >= PLATFORM_INSTANCE_MAX)
        return FALSE;
    for (axis = 0; axis < 3; axis++)
        if (!isfinite(state->position[axis]) || !isfinite(state->rotation[axis])) return FALSE;
    for (vertex = 0; vertex < PLATFORM_VERTEX_COUNT; vertex++) {
        platform_shape_to_world(&board, state->position, state->rotation, board.vertices[vertex], point);
        for (axis = 0; axis < 3; axis++) if (fabsf(point[axis]) >= 4900.0f) return FALSE;
    }
    platform_instances[index] = *state;
    if (index == platform_instance_count) platform_instance_count++;
    return TRUE;
}

static char const *platform_edit_name(long index)
{
    char const *asset = config_string("platform.asset");
    char const *name;
    (void)index;
    if (!asset[0]) return "solid platform";
    name = strrchr(asset, '/');
    return name ? name + 1 : asset;
}

static boolean platform_instance_ray(long index, real_point3d const *origin,
    real_vector3d const *ray, boolean front, boolean back, real *t, uint32_t *face)
{
    struct halo_world_edit_transform const *state = &platform_instances[index];
    float local_origin[3], local_ray[3];
    if (state->deleted) return FALSE;
    platform_shape_to_local(&board, state->position, state->rotation, origin->n, local_origin);
    platform_shape_unrotate(state->rotation, ray->n, local_ray);
    return platform_shape_ray(&board, local_origin, local_ray, front, back, t, face);
}

static boolean platform_edit_pick(real_point3d const *origin, real_vector3d const *ray,
    long ignored_index, real *t, long *index)
{
    boolean hit = FALSE;
    long slot;
    uint32_t face;
    if (!active) return FALSE;
    for (slot = 0; slot < platform_instance_count; slot++) {
        if (slot != ignored_index && platform_instance_ray(slot, origin, ray, TRUE, TRUE, t, &face)) {
            *index = slot;
            hit = TRUE;
        }
    }
    return hit;
}
