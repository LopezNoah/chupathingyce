/* Opt-in real-engine regression, not used by normal local-play launches. */
static boolean forge_world_test_ok;
static boolean forge_world_test_check(boolean ok, char const *what)
{
    platform_log("forge world test: %s %s", ok ? "PASS" : "FAIL", what);
    if (!ok) forge_world_test_ok = FALSE;
    return ok;
}

static void forge_world_test_key(short key, boolean control)
{
    csmemset(&forge.input, 0, sizeof(forge.input));
    csmemset(&forge.previous_input, 0, sizeof(forge.previous_input));
    if (key != NONE) forge.input.keys[key] = TRUE;
    forge.input.keys[HALO_FORGE_KEY_CTRL] = control;
    forge_pick();
    forge_edit(0.1f);
}

static void forge_world_test_edit(struct forge_target target, struct halo_world_geometry const *geometry)
{
    struct forge_transform before, after;
    if (!forge_world_test_check(forge_world_get(&target, &before), "read original")) return;
    forge.camera.position = before.position;
    forge.camera.position.z += 0.3f;
    forge.camera.facing.yaw = 0.0f;
    forge.camera.facing.pitch = -_pi * 0.5f;
    forge_world_test_key(HALO_FORGE_KEY_MOUSE_LEFT, FALSE);
    forge_world_test_check(forge.selected.kind == _forge_target_world && forge.selected.index == 0,
        "ray and click select GLB");
    {
        real_point3d center, corners[8];
        real radius;
        short vertex;
        boolean bounded = forge_world_sphere(&target, &center, &radius) &&
            geometry->edit_corners(target.index, corners);
        if (bounded) {
            bounded = distance3d(&center, &before.position) < 0.001f;
            for (vertex = 0; vertex < 8; vertex++)
                if (distance3d(&center, &corners[vertex]) > radius) bounded = FALSE;
        }
        forge_world_test_check(bounded, "spherical GLB highlight encloses asset bounds");
    }
    forge_world_test_key(HALO_FORGE_KEY_E, FALSE);
    forge_world_test_check(forge.carrying, "grab GLB");
    forge.camera.position.x += 0.75f;
    forge.camera.position.y += 0.25f;
    forge.camera.position.z += 0.5f;
    forge_world_test_key(NONE, FALSE);
    forge_world_test_key(HALO_FORGE_KEY_MOUSE_LEFT, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) &&
        fabsf(before.position.x - after.position.x) > 0.4f &&
        fabsf(before.position.y - after.position.y) > 0.1f &&
        fabsf(before.position.z - after.position.z) > 0.1f, "carry and drop moves GLB on all three axes");
    forge.snap = TRUE;
    forge_world_test_key(HALO_FORGE_KEY_Q, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) && fabsf(after.rotation.yaw) > 0.1f, "rotate GLB yaw/Z");
    forge_world_test_key(HALO_FORGE_KEY_Y, FALSE);
    forge_world_test_key(HALO_FORGE_KEY_Q, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) && fabsf(after.rotation.pitch) > 0.1f, "rotate GLB pitch/Y");
    forge_world_test_key(HALO_FORGE_KEY_Z, TRUE);
    forge_world_test_check(forge_world_get(&target, &after) && fabsf(after.rotation.pitch) < 0.001f &&
        forge.rotation_axis == 1, "undo tilt without changing rotation axis");
    forge_world_test_key(HALO_FORGE_KEY_Y, TRUE);
    forge_world_test_key(HALO_FORGE_KEY_X, FALSE);
    forge_world_test_key(HALO_FORGE_KEY_Q, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) && fabsf(after.rotation.pitch) > 0.1f &&
        fabsf(after.rotation.roll) > 0.1f, "redo pitch and rotate GLB roll/X");
    {
        real_point3d corners[8];
        real_vector3d forward, up, edge, normal;
        vectors3d_from_euler_angles3d(&forward, &up, &after.rotation);
        geometry->edit_corners(target.index, corners);
        for (short axis = 0; axis < 3; axis++) {
            edge.n[axis] = corners[1].n[axis] - corners[0].n[axis];
            normal.n[axis] = corners[4].n[axis] - corners[0].n[axis];
        }
        normalize3d(&edge); normalize3d(&normal);
        forge_world_test_check(dot_product3d(&edge, &forward) > 0.999f &&
            dot_product3d(&normal, &up) > 0.999f, "3D collision pose matches Halo Euler convention");
    }
    forge_world_test_key(HALO_FORGE_KEY_V, FALSE);
    forge_world_test_check(geometry->edit_count() == 2 && forge.selected.index == 1, "duplicate GLB");
    forge.camera.position.x += 3.0f;
    forge_world_test_key(NONE, FALSE);
    forge_world_test_key(HALO_FORGE_KEY_MOUSE_LEFT, FALSE);
    target.index = 1;
    forge_world_test_key(HALO_FORGE_KEY_DELETE, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) && after.deleted, "delete GLB");
    forge_world_test_key(HALO_FORGE_KEY_Z, TRUE);
    forge_world_test_check(forge_world_get(&target, &after) && !after.deleted, "undo GLB deletion");
    forge_world_test_key(HALO_FORGE_KEY_Y, TRUE);
    forge_world_test_check(forge_world_get(&target, &after) && after.deleted, "redo GLB deletion");
    forge_world_test_key(HALO_FORGE_KEY_Z, TRUE);
}

static void forge_world_test_palette(struct forge_target target, struct halo_world_geometry const *geometry)
{
    long entry;
    struct forge_transform after;
    for (entry = 0; entry < forge.palette_count; entry++)
        if (forge.palette[entry].type == NONE && forge.palette[entry].definition_index == target.type) break;
    if (!forge_world_test_check(entry < forge.palette_count, "GLB palette entry")) return;
    forge.palette_index = entry;
    forge_world_test_key(HALO_FORGE_KEY_P, FALSE);
    target.index = 2;
    forge_world_test_check(geometry->edit_count() == 3 && forge.selected.kind == _forge_target_world &&
        forge.selected.index == 2, "place GLB from palette");
    forge_world_test_key(HALO_FORGE_KEY_DELETE, FALSE);
    forge_world_test_check(forge_world_get(&target, &after) && after.deleted, "delete placed GLB");
    forge_world_test_check(forge_save(), "save external poses");
}

static void forge_world_test_wall(void)
{
    static real_vector3d const rays[4] = {{{20,0,0}},{{-20,0,0}},{{0,20,0}},{{0,-20,0}}};
    real_point3d center = forge_avatar.original_origin, inside;
    struct collision_result hit;
    short direction;
    center.z += forge_avatar.radius + forge_avatar.height * 0.5f;
    for (direction = 0; direction < 4; direction++) {
        if (!collision_test_vector(FLAG(_collision_test_structure_bit) |
            FLAG(_collision_test_front_facing_surfaces_bit), &center, &rays[direction],
            forge_avatar.unit_index, &hit) || hit.type != _collision_result_structure ||
            fabsf(hit.plane.n.k) > 0.25f) continue;
        point_from_line3d(&hit.point, &hit.plane.n, -forge_avatar.radius - 0.05f, &inside);
        forge_world_test_check(!forge_avatar_origin_clear(&inside), "reject capsule inside native wall");
        return;
    }
    forge_world_test_check(FALSE, "find native wall for collision fixture");
}

static void forge_world_test_native_bounds(struct forge_target target, real_point3d const *origin)
{
    struct forge_transform saved[3], hidden;
    real_point3d floor = *origin;
    short index;
    /* Tilted boards may correctly occlude the wall-search rays. Isolate the
     * native queries, then restore every pose before checking GLB obstruction. */
    for (index = 0; index < 3; index++) {
        target.index = index;
        if (!forge_world_get(&target, &saved[index])) return;
    }
    for (index = 0; index < 3; index++) {
        target.index = index;
        hidden = saved[index]; hidden.deleted = TRUE;
        forge_world_set(&target, &hidden);
    }
    forge_world_test_wall();
    floor.z -= 2.0f;
    forge_world_test_check(!forge_avatar_origin_clear(&floor), "reject capsule below native floor");
    for (index = 0; index < 3; index++) {
        target.index = index;
        forge_world_set(&target, &saved[index]);
    }
}

static void forge_world_test_avatar(struct forge_target target)
{
    real_point3d expected = forge_avatar.original_origin, actual;
    struct forge_transform after;
    long unit = forge_avatar.unit_index;
    short attempt;
    forge_world_test_native_bounds(target, &expected);
    for (attempt = 0; attempt < 8; attempt++) {
        expected.z += 0.5f;
        if (forge_avatar_origin_clear(&expected)) break;
    }
    forge_world_test_check(attempt < 8, "find clear avatar destination");
    point_from_line3d(&expected, &forge_avatar.eye_offset, 1.0f, &forge.camera.position);
    forge_close();
    object_get_origin(unit, &actual);
    forge_world_test_check(!forge.active && !game_time_get_paused() &&
        distance3d(&expected, &actual) < 0.001f, "exit places avatar at camera");
    forge_world_test_check(!TEST_FLAG(object_get(unit)->object.flags, _object_invisible_bit), "avatar visible again");
    forge_open();
    target.index = 1;
    forge_world_get(&target, &after);
    point_from_line3d(&after.position, &forge_avatar.eye_offset, 1.0f, &forge.camera.position);
    forge_world_test_check(!forge_avatar_origin_clear(&after.position), "reject capsule inside GLB board");
    forge_close();
    if (!forge.active) {
        object_get_origin(unit, &actual);
        /* restore clears ownership, not pill dimensions. */
        forge_avatar.unit_index = unit;
        forge_world_test_check(forge_avatar_origin_clear(&actual), "blocked-camera exit uses clear position");
        forge_avatar.unit_index = NONE;
    } else forge_world_test_check(forge.active && !game_time_get_paused(), "blocked exit stays in live Forge safely");
}

static void forge_world_test_verify(struct forge_target target, struct halo_world_geometry const *geometry)
{
    struct forge_transform after, copy;
    forge_world_test_check(geometry->edit_count() == 3, "reload count");
    forge_world_test_check(forge_world_get(&target, &after) && fabsf(after.rotation.yaw) > 0.1f &&
        fabsf(after.rotation.pitch) > 0.1f && fabsf(after.rotation.roll) > 0.1f,
        "reload full 3D rotation");
    target.index = 1;
    forge_world_test_check(forge_world_get(&target, &copy) && !copy.deleted &&
        distance3d(&copy.position, &after.position) > 1.0f, "reload independent duplicate");
    target.index = 2;
    forge_world_test_check(forge_world_get(&target, &copy) && copy.deleted, "reload deletion");
}

static void forge_world_test_alpha(real seconds, char const *view)
{
    static boolean done;
    static real elapsed;
    struct forge_target target = {_forge_target_world, 0, 0};
    struct forge_transform state;
    long provider;
    boolean below = !strcmp(view, "below");
    boolean grazing = !strcmp(view, "grazing");
    if (done || !game_in_progress()) return;
    for (provider = 0; provider < halo_extensions_count(); provider++) {
        target.type = (short)provider;
        if (forge_world_get(&target, &state)) break;
    }
    if (provider == halo_extensions_count()) return;
    elapsed += seconds;
    if (elapsed < 2.0f) return;
    forge_open();
    if (!forge.active) return;
    done = TRUE;
    /* Isolate a single grid in clear air so the underside cannot be confused
     * with native floors, copied grids or the former visible collision shell. */
    state.position.z += 12.0f;
    forge_world_set(&target, &state);
    forge.camera.position = state.position;
    forge.camera.position.x -= 4.0f;
    forge.camera.position.z += below ? -3.0f : grazing ? 0.2f : 3.0f;
    forge.camera.facing.yaw = 0.0f;
    forge.camera.facing.pitch = below ? 0.6435011f : grazing ? -0.0499584f : -0.6435011f;
    forge.help = FALSE;
    if (!strcmp(view, "selected")) forge_select(&target);
    platform_log("forge alpha test: %s ready", view);
}

static void forge_world_test_update(real seconds)
{
    static boolean done;
    static real elapsed;
    char const *mode = config_string("debug.forge_test");
    struct forge_target target = {_forge_target_world, 0, 0};
    struct halo_world_geometry const *geometry = NULL;
    long provider;
    boolean visual = !strcmp(mode, "world-view");
    if (!strncmp(mode, "world-alpha-", 12)) {
        forge_world_test_alpha(seconds, mode + 12);
        return;
    }
    if (done || (!visual && strcmp(mode, "world") && strcmp(mode, "world-verify")) || !game_in_progress()) return;
    for (provider = 0; provider < halo_extensions_count(); provider++) {
        geometry = halo_extensions_get(provider)->world_geometry;
        if (geometry && geometry->edit_count && geometry->edit_count() > 0) {
            target.type = (short)provider;
            break;
        }
    }
    if (provider == halo_extensions_count()) return;
    elapsed += seconds;
    if (elapsed < 2.0f) return;
    done = TRUE;
    forge_world_test_ok = TRUE;
    if (!strcmp(mode, "world-verify")) forge_world_test_verify(target, geometry);
    else {
        forge_open();
        if (!forge_world_test_check(forge.active && !game_time_get_paused(), "open without pausing")) return;
        forge_world_test_check(forge_avatar.hidden_count > 0 &&
            TEST_FLAG(object_get(forge_avatar.unit_index)->object.flags, _object_invisible_bit), "avatar hidden");
        forge_world_test_edit(target, geometry);
        forge_world_test_palette(target, geometry);
        if (visual) {
            struct forge_transform state;
            forge_world_get(&target, &state);
            forge_select(&target);
            forge.camera.position = state.position;
            forge.camera.position.x -= 4.0f;
            forge.camera.position.z += 4.0f;
            forge.camera.facing.yaw = 0.0f;
            forge.camera.facing.pitch = -_pi * 0.25f;
            forge.help = TRUE;
        } else forge_world_test_avatar(target);
    }
    platform_log("forge world test: %s %s", mode, forge_world_test_ok ? "PASS" : "FAIL");
}
