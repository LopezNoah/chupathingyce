/* Forge's adapter for geometry without engine object datums. The provider owns
 * slots and collision; the existing command stack owns all edit history. */
static struct halo_world_geometry const *forge_world_geometry(struct forge_target const *target)
{
    struct halo_extension const *extension;
    if (target->kind != _forge_target_world || target->type < 0 ||
        target->type >= halo_extensions_count()) return NULL;
    extension = halo_extensions_get(target->type);
    return extension ? extension->world_geometry : NULL;
}

static boolean forge_world_get(struct forge_target const *target, struct forge_transform *state)
{
    struct halo_world_geometry const *geometry = forge_world_geometry(target);
    struct halo_world_edit_transform pose;
    if (!geometry || !geometry->edit_get || !geometry->edit_get(target->index, &pose)) return FALSE;
    memcpy(state->position.n, pose.position, sizeof(pose.position));
    memcpy(state->rotation.n, pose.rotation, sizeof(pose.rotation));
    state->deleted = pose.deleted;
    return TRUE;
}

static boolean forge_world_set(struct forge_target const *target, struct forge_transform const *state)
{
    struct halo_world_geometry const *geometry = forge_world_geometry(target);
    struct halo_world_edit_transform pose;
    if (!geometry || !geometry->edit_set) return FALSE;
    memcpy(pose.position, state->position.n, sizeof(pose.position));
    memcpy(pose.rotation, state->rotation.n, sizeof(pose.rotation));
    pose.deleted = state->deleted;
    return geometry->edit_set(target->index, &pose);
}

static boolean forge_hover_target(struct forge_target *target)
{
    if (forge.hover_world.kind != _forge_target_none) {
        *target = forge.hover_world;
        return TRUE;
    }
    return forge_target_of_object(forge.hover_object_index, target);
}

static void forge_world_pick(real_vector3d const *ray, real t)
{
    long provider, slot;
    for (provider = 0; provider < halo_extensions_count(); provider++) {
        struct halo_extension const *extension = halo_extensions_get(provider);
        struct halo_world_geometry const *geometry = extension->world_geometry;
        long ignored = forge.carrying && forge.selected.kind == _forge_target_world &&
            forge.selected.type == provider ? forge.selected.index : NONE;
        /* Mesh collision may already be the hit at t. Permit equal hits while
         * retaining nearer native hits. This epsilon is a segment fraction. */
        real candidate = MIN(1.000001f, t + 0.000001f);
        if (!geometry || !geometry->edit_pick ||
            !geometry->edit_pick(&forge.camera.position, ray, ignored, &candidate, &slot)) continue;
        if (candidate > t + 0.000001f) continue;
        t = candidate;
        forge.hover_object_index = NONE;
        forge.hover_world.kind = _forge_target_world;
        forge.hover_world.type = (short)provider;
        forge.hover_world.index = slot;
        forge.hover_hit = TRUE;
        point_from_line3d(&forge.camera.position, ray, t, &forge.hover_point);
    }
}

static boolean forge_world_sphere(struct forge_target const *target, real_point3d *center, real *radius)
{
    struct halo_world_geometry const *geometry = forge_world_geometry(target);
    real_point3d corners[8];
    short vertex, axis;
    if (!geometry || !geometry->edit_corners || !geometry->edit_corners(target->index, corners)) return FALSE;
    set_real_point3d(center, 0.0f, 0.0f, 0.0f);
    for (vertex = 0; vertex < 8; vertex++)
        for (axis = 0; axis < 3; axis++) center->n[axis] += corners[vertex].n[axis] * 0.125f;
    *radius = 0.1f;
    for (vertex = 0; vertex < 8; vertex++)
        *radius = MAX(*radius, distance3d(center, &corners[vertex]) + 0.01f);
    return TRUE;
}

static void forge_world_outline(struct forge_target const *target, real_argb_color const *color)
{
    real_point3d center;
    real radius;
    /* Same spherical highlight as native boulders, with the existing grey
     * hover / yellow selection / cyan carry colors. Picking stays exact. */
    if (forge_world_sphere(target, &center, &radius))
        render_debug_sphere(TRUE, &center, radius, color);
}
