/*
 * One opt-in board, anchored once per offline Blood Gulch session. Collision
 * uses the engine's rounded polygon/edge/vertex features, not a position clamp.
 * No tag memory, gameplay datums or network messages are created or changed.
 */
#include "cseries.h"
#include "extensions/extension_api.h"

#ifndef HALO_EXTENSION_WORLD_GEOMETRY
struct halo_extension const platform_extension = { .name = "platform" };
#else

#include "platform_shape.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "physics/collision_features.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "render/render.h"
#include "math/real_math.h"
#include "scenario/scenario.h"
#include "units/bipeds.h"
#include "main/main.h"

#include <math.h>
#include <string.h>

int config_boolean(char const *name);
double config_real(char const *name);
char const *config_string(char const *name);
int platform_mesh_draw(char const *path, float const minimum[3], float const maximum[3],
    float const position[3], float const rotation[3][3]);
void platform_log(char const *format, ...);

static struct platform_shape board;
static boolean active;
static boolean placement_attempted;
static real test_elapsed;
static short test_stage;
/* Process-scoped fixture guard, intentionally not reset with the map. */
static boolean test_reloaded;

static real platform_setting(char const *name, real minimum, real maximum)
{
    double value = config_real(name);
    if (!isfinite(value) || value < minimum || value > maximum) {
        platform_log("platform: invalid %s; using minimum %.2f", name, minimum);
        return minimum;
    }
    return (real)value;
}

#include "platform_edit.inc"

static boolean platform_test_point(real_point3d const *point)
{
    long slot;
    float local[3];
    if (!active) return FALSE;
    for (slot = 0; slot < platform_instance_count; slot++) {
        struct halo_world_edit_transform const *state = &platform_instances[slot];
        if (state->deleted) continue;
        platform_shape_to_local(&board, state->position, state->rotation, point->n, local);
        if (platform_shape_contains(&board, local)) return TRUE;
    }
    return FALSE;
}

/* All geometry fits in 8 spheres, 12 edges and 6 quads, even for an unbounded
 * query. Capacity is checked before appending; do not evict engine features.
 * NONE surface identity matches object-model features and prevents indexing
 * the map's BSP with an external surface. The top still supports bipeds. */
static void platform_instance_features(long slot, real_point3d const *center, real radius,
    real height, real width, struct collision_feature_list *features)
{
    uint32_t index;
    real_point3d corners[8];
    float local[3];
    struct halo_world_edit_transform const *state = &platform_instances[slot];
    if (!platform_edit_corners(slot, corners)) return;
    platform_shape_to_local(&board, state->position, state->rotation, center->n, local);
    if (!platform_shape_near(&board, local, radius)) return;
    if (features->count[_collision_feature_sphere] + 16 > MAXIMUM_COLLISION_FEATURES_PER_TEST ||
        features->count[_collision_feature_cylinder] + 32 > MAXIMUM_COLLISION_FEATURES_PER_TEST ||
        features->count[_collision_feature_prism] + 30 > MAXIMUM_COLLISION_FEATURES_PER_TEST) {
        return;
    }
    for (index = 0; index < PLATFORM_VERTEX_COUNT; index++) {
        real_point3d point;
        point = corners[index];
        collision_features_from_point(&point, height, width,
            NONE, NONE, 0, (byte)NONE, NONE, features);
    }
    for (index = 0; index < PLATFORM_EDGE_COUNT; index++) {
        real_point3d point, end;
        real_vector3d vector;
        point = corners[platform_edges[index][0]];
        end = corners[platform_edges[index][1]];
        vector_from_points3d(&point, &end, &vector);
        collision_features_from_line(&point, &vector, height, width,
            NONE, NONE, 0, (byte)NONE, NONE, features);
    }
    for (index = 0; index < PLATFORM_FACE_COUNT; index++) {
        real_point3d points[4];
        real_plane3d plane;
        uint32_t vertex;
        platform_shape_rotate(state->rotation, platform_normals[index], plane.n.n);
        for (vertex = 0; vertex < 4; vertex++) {
            points[vertex] = corners[platform_faces[index][vertex]];
        }
        plane.d = dot_product3d(&plane.n, (real_vector3d const *)&points[0]);
        collision_features_from_polygon(4, points, &plane, height, width,
            NONE, NONE, 0, (byte)NONE, NONE, features);
    }
}

static void platform_get_features(real_point3d const *center, real radius,
    real height, real width, struct collision_feature_list *features)
{
    long slot;
    if (!active) return;
    for (slot = 0; slot < platform_instance_count; slot++)
        platform_instance_features(slot, center, radius, height, width, features);
}

static void platform_collision_result(real_point3d const *point, real_vector3d const *vector,
    real t, real_plane3d const *plane, struct collision_result *result)
{
    /* Mesh hits have no object or BSP surface to dereference. */
    memset(result, 0, sizeof(*result));
    result->type = _collision_result_mesh;
    result->t = t;
    result->plane = *plane;
    point_from_line3d(point, vector, t, &result->point);
    result->object_index = NONE;
    result->region_index = NONE;
    result->node_index = NONE;
    result->bsp_index = NONE;
    result->surface_index = NONE;
    result->plane_designator = NONE;
    result->material_type = NONE;
    result->material_index = NONE;
    result->breakable_surface_index = (byte)NONE;
    scenario_location_from_point(&result->start_location, point);
    scenario_location_from_point(&result->location, &result->point);
}

static boolean platform_test_vector(unsigned long flags, real_point3d const *point,
    real_vector3d const *vector, real radius, struct collision_result *result)
{
    real t = result->t;
    real_plane3d plane;
    if (!active) return FALSE;
    if (radius > 0.0f) {
        /* Use the same rounded features as movement, not an expanded AABB
         * (which would have incorrect square corners for a swept sphere). */
        struct collision_feature_list features;
        struct collision_plane hit;
        real_point3d center;
        point_from_line3d(point, vector, 0.5f, &center);
        collision_features_new(&features);
        platform_get_features(&center, magnitude3d(vector) * 0.5f + radius,
            0.0f, radius, &features);
        if (!collision_features_test_vector(&features, point, vector, &hit) || hit.t >= t)
            return FALSE;
        t = hit.t;
        plane = hit.plane;
    } else {
        uint32_t face = 0;
        boolean front = TEST_FLAG(flags, _collision_test_front_facing_surfaces_bit);
        boolean back = TEST_FLAG(flags, _collision_test_back_facing_surfaces_bit);
        if (!front && !back) front = back = TRUE;
        long slot, hit_slot = NONE;
        real_point3d corners[8];
        for (slot = 0; slot < platform_instance_count; slot++)
            if (platform_instance_ray(slot, point, vector, front, back, &t, &face)) hit_slot = slot;
        if (hit_slot == NONE) return FALSE;
        platform_shape_rotate(platform_instances[hit_slot].rotation, platform_normals[face], plane.n.n);
        platform_edit_corners(hit_slot, corners);
        plane.d = dot_product3d(&plane.n,
            (real_vector3d const *)&corners[platform_faces[face][0]]);
        if (dot_product3d(&plane.n, vector) > 0.0f) {
            scale_vector3d(&plane.n, -1.0f, &plane.n);
            plane.d = -plane.d;
        }
    }
    platform_collision_result(point, vector, t, &plane, result);
    return TRUE;
}

static void platform_draw_quad(real_point3d const points[4], real_rgb_color const *color)
{
    _rasterizer_debug_immediate_triangle(&points[0], &points[1], &points[2], color, NULL, NULL);
    _rasterizer_debug_immediate_triangle(&points[0], &points[2], &points[3], color, NULL, NULL);
}

static void platform_instance_render(long slot)
{
    real_rgb_color colors[2] = { { {0.65f, 0.68f, 0.72f} }, { {0.25f, 0.28f, 0.32f} } };
    uint32_t face;
    long saved_zbias;
    real_point3d corners[8];
    float rotation[3][3];
    if (!platform_edit_corners(slot, corners)) return;
    platform_shape_rotation(platform_instances[slot].rotation, rotation);
    /* Collision remains a solid box, but a loaded BLEND visual must not have
     * an opaque top/underside beneath it. That shell made the grid's opacity
     * depend on which side the camera viewed it from. Keep the shell only as
     * the no-asset / failed-load fallback. */
    if (platform_mesh_draw(config_string("platform.asset"), board.minimum, board.maximum,
        platform_instances[slot].position, rotation)) return;
    /* Existing immediate colored shader: scene camera, depth test/write,
     * engine-managed D3D/GL state, before transparency and screen overlays. */
    /* Debug primitives normally use a large overlay bias. This is real world
     * geometry; that bias would hide the textured surface immediately above. */
    saved_zbias = rasterizer_debug_options.zbias;
    rasterizer_debug_options.zbias = 0;
    _rasterizer_debug_immediate_begin();
    rasterizer_debug_options.zbias = saved_zbias;
    for (face = 0; face < PLATFORM_FACE_COUNT; face++) {
        real_point3d points[4];
        uint32_t vertex;
        for (vertex = 0; vertex < 4; vertex++) {
            points[vertex] = corners[platform_faces[face][vertex]];
        }
        platform_draw_quad(points, &colors[face == 0 ? 0 : 1]);
    }
    _rasterizer_debug_immediate_end();
}

static void platform_render(void)
{
    long slot, count = 0, index;
    long order[PLATFORM_INSTANCE_MAX];
    real depths[PLATFORM_INSTANCE_MAX];
    if (!active) return;
    /* Alpha copies share a mesh but can overlap at different heights. Draw
     * back-to-front along this split-screen view's camera, not creation order. */
    for (slot = 0; slot < platform_instance_count; slot++) {
        real_vector3d offset;
        real depth;
        if (platform_instances[slot].deleted) continue;
        for (index = 0; index < 3; index++)
            offset.n[index] = platform_instances[slot].position[index] - render.camera.position.n[index];
        depth = dot_product3d(&offset, &render.camera.forward);
        index = count;
        while (index > 0 && depths[index - 1] < depth) {
            depths[index] = depths[index - 1];
            order[index] = order[index - 1];
            index--;
        }
        depths[index] = depth;
        order[index] = slot;
        count++;
    }
    for (index = 0; index < count; index++) platform_instance_render(order[index]);
}

static void platform_reset(void)
{
    active = FALSE;
    platform_instance_count = 0;
    memset(platform_instances, 0, sizeof(platform_instances));
    placement_attempted = FALSE;
    test_elapsed = 0.0f;
    test_stage = 0;
    memset(&board, 0, sizeof(board));
}

static void platform_place(long object_index)
{
    struct object_datum const *object = object_get(object_index);
    real_point3d origin;
    real_vector3d down;
    struct collision_result ground;
    float minimum[3], maximum[3];
    real width = platform_setting("platform.width", 0.5f, 20.0f);
    real depth = platform_setting("platform.depth", 0.5f, 20.0f);
    real elevation = platform_setting("platform.elevation", 0.2f, 10.0f);
    real thickness = platform_setting("platform.thickness", 0.05f, 1.0f);
    real offset = platform_setting("platform.forward_offset", 0.0f, 20.0f);
    placement_attempted = TRUE;
    if (thickness >= elevation) {
        platform_log("platform: thickness must be less than elevation; not placed");
        return;
    }
    object_get_origin(object_index, &origin);
    origin.x += object->object.forward.i * offset;
    origin.y += object->object.forward.j * offset;
    origin.z += 2.0f;
    set_real_vector3d(&down, 0.0f, 0.0f, -20.0f);
    if (!collision_test_vector(FLAG(_collision_test_structure_bit) |
        FLAG(_collision_test_front_facing_surfaces_bit), &origin, &down, object_index, &ground) ||
        ground.plane.n.k < 0.7f) {
        platform_log("platform: no flat ground ahead of spawn; not placed");
        return;
    }
    minimum[0] = ground.point.x - width * 0.5f;
    maximum[0] = ground.point.x + width * 0.5f;
    minimum[1] = ground.point.y - depth * 0.5f;
    maximum[1] = ground.point.y + depth * 0.5f;
    maximum[2] = ground.point.z + elevation;
    minimum[2] = maximum[2] - thickness;
    active = platform_shape_init(&board, minimum, maximum);
    if (active) {
        uint32_t axis;
        platform_instance_count = 1;
        for (axis = 0; axis < 3; axis++)
            platform_instances[0].position[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
        platform_log("platform: board at %.3f %.3f %.3f, %.2f x %.2f, thickness %.2f",
            ground.point.x, ground.point.y, maximum[2], width, depth, thickness);
    } else {
        platform_log("platform: bounds outside world; not placed");
    }
}

/* Opt-in automated fixture, never run for normal platform sessions. */
static void platform_test_update(real seconds)
{
    long player_index, unit_index;
    real_point3d position;
    struct biped_datum *biped;
    if (!active || !config_boolean("debug.platform_test") || test_stage >= 2) return;
    player_index = local_player_get_player_index(0);
    if (player_index == NONE) return;
    unit_index = player_get(player_index)->unit_index;
    if (unit_index == NONE) return;
    biped = biped_get(unit_index);
    if (test_stage == 0) {
        set_real_point3d(&position, (board.minimum[0] + board.maximum[0]) * 0.5f,
            (board.minimum[1] + board.maximum[1]) * 0.5f, board.maximum[2] + 0.2f);
        object_set_position(unit_index, &position, NULL, NULL);
        set_real_vector3d(&biped->object.translational_velocity, 0.0f, 0.0f, 0.0f);
        test_stage = 1;
        return;
    }
    test_elapsed += seconds;
    if (test_elapsed >= 3.0f) {
        boolean airborne = TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
        object_get_origin(unit_index, &position);
        platform_log("platform test: standing %s z=%.4f top=%.4f airborne=%d",
            !airborne && fabs(position.z - board.maximum[2]) < 0.05f ? "PASS" : "FAIL",
            position.z, board.maximum[2], airborne);
        test_stage = 2;
        if (!test_reloaded) {
            test_reloaded = TRUE;
            platform_log("platform test: resetting map");
            main_reset_map();
        }
    }
}

static void platform_update(boolean main_menu_loaded, real seconds)
{
    long player_index;
    char const *map_name;
    if (!main_menu_loaded && game_in_progress()) platform_test_update(seconds);
    if (main_menu_loaded || placement_attempted || !game_in_progress() ||
        !config_boolean("platform.enabled")) return;
    map_name = cache_file_loaded_map_name();
    if (!map_name || strcmp(map_name, "bloodgulch")) return;
    /* Fail closed: never give a network host/client unmatched solid geometry. */
    if (game_connection() != _game_connection_local) {
        placement_attempted = TRUE;
        platform_log("platform: offline/local games only; not placed");
        return;
    }
    player_index = local_player_get_player_index(0);
    if (player_index != NONE && player_get(player_index)->unit_index != NONE) {
        platform_place(player_get(player_index)->unit_index);
    }
}

static boolean platform_suppress_report(void) { return active; }
static char const *platform_edit_resource(void) { return config_string("platform.asset"); }
static struct halo_world_geometry const platform_geometry = {
    .render = platform_render,
    .test_point = platform_test_point,
    .test_vector = platform_test_vector,
    .get_features = platform_get_features,
    .edit_count = platform_edit_count,
    .edit_get = platform_edit_get,
    .edit_set = platform_edit_set,
    .edit_pick = platform_edit_pick,
    .edit_corners = platform_edit_corners,
    .edit_name = platform_edit_name,
    .edit_resource = platform_edit_resource,
};
struct halo_extension const platform_extension = {
    .name = "platform",
    .main_frame_update = platform_update,
    .dispose_from_old_map = platform_reset,
    .invalidate_derived_state = platform_reset,
    .suppress_game_report = platform_suppress_report,
    .world_geometry = &platform_geometry,
};
#endif
