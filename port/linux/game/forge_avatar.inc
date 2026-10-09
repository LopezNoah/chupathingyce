/* Forge camera/character handoff. Flight stays noclip while physics is paused.
 * Never unhide a biped at an unchecked origin: validate its full physics pill,
 * including map, native objects and extension geometry. */
#include "physics/collision_features.h"
#include "units/units.h"

enum { FORGE_AVATAR_ATTACHMENTS_MAX = 64 };
static struct {
    long unit_index;
    long hidden[FORGE_AVATAR_ATTACHMENTS_MAX];
    boolean visible[FORGE_AVATAR_ATTACHMENTS_MAX];
    short hidden_count;
    real_point3d original_origin;
    real_vector3d pill_offset;
    real_vector3d eye_offset;
    real height, radius;
} forge_avatar;

static void forge_avatar_restore(void)
{
    short index;
    for (index = 0; index < forge_avatar.hidden_count; index++) {
        if (forge_object_alive(forge_avatar.hidden[index]))
            object_set_visibility(forge_avatar.hidden[index], forge_avatar.visible[index]);
    }
    forge_avatar.hidden_count = 0;
    forge_avatar.unit_index = NONE;
}

static boolean forge_avatar_begin(void)
{
    long player_index = local_player_get_player_index(forge.local_player_index);
    long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
    struct object_iterator iterator;
    struct object_datum *object;
    real_point3d base, eye;
    forge_avatar.unit_index = NONE;
    forge_avatar.hidden_count = 0;
    if (!forge_object_alive(unit_index)) {
        forge_message("Wait until your character spawns before entering Forge");
        return FALSE;
    }
    object = object_get(unit_index);
    if (TEST_FLAG(object->object.damage_flags, _object_dead_bit)) {
        forge_message("Wait until you respawn before entering Forge");
        return FALSE;
    }
    if (object->object.type != _object_type_biped || object->object.parent_object_index != NONE) {
        forge_message("Exit your vehicle before entering Forge");
        return FALSE;
    }
    forge_avatar.unit_index = unit_index;
    object_get_origin(unit_index, &forge_avatar.original_origin);
    biped_get_physics_pill(unit_index, &base, &forge_avatar.height, &forge_avatar.radius);
    unit_get_camera_position(unit_index, &eye);
    vector_from_points3d(&forge_avatar.original_origin, &base, &forge_avatar.pill_offset);
    vector_from_points3d(&forge_avatar.original_origin, &eye, &forge_avatar.eye_offset);
    object_iterator_new(&iterator, _object_mask_all, 0);
    while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL) {
        short row;
        if (forge_root_object(iterator.index) != unit_index || !forge_object_alive(iterator.index)) continue;
        if (forge_avatar.hidden_count == FORGE_AVATAR_ATTACHMENTS_MAX) {
            forge_avatar_restore();
            forge_message("Too many character attachments to enter Forge safely");
            return FALSE;
        }
        row = forge_avatar.hidden_count++;
        forge_avatar.hidden[row] = iterator.index;
        forge_avatar.visible[row] = !TEST_FLAG(object->object.flags, _object_invisible_bit);
        object_set_visibility(iterator.index, FALSE);
    }
    return TRUE;
}

static unsigned long forge_avatar_collision_flags(void)
{
    return FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_bit) |
        _collision_test_objects_all_types_flags | FLAG(_collision_test_front_facing_surfaces_bit) |
        FLAG(_collision_test_back_facing_surfaces_bit);
}

static boolean forge_avatar_origin_clear(real_point3d const *origin)
{
    struct collision_feature_list features;
    struct collision_plane hit;
    real_point3d base, center, top;
    unsigned long flags = forge_avatar_collision_flags();
    short axis;
    for (axis = 0; axis < 3; axis++)
        if (!valid_real(origin->n[axis]) || fabsf(origin->n[axis]) >= FORGE_POSITION_LIMIT) return FALSE;
    point_from_line3d(origin, &forge_avatar.pill_offset, 1.0f, &base);
    center = top = base;
    center.z += forge_avatar.height * 0.5f;
    top.z += forge_avatar.height;
    collision_get_features_in_sphere(flags, &center, forge_avatar.height * 0.5f + forge_avatar.radius + 0.01f,
        forge_avatar.height, forge_avatar.radius, forge_avatar.unit_index, &features);
    /* A crowded query can exhaust the fixed engine feature list. Fail closed
     * rather than treating omitted world geometry as empty space. */
    if (features.count[_collision_feature_sphere] > MAXIMUM_COLLISION_FEATURES_PER_TEST - 16 ||
        features.count[_collision_feature_cylinder] > MAXIMUM_COLLISION_FEATURES_PER_TEST - 32 ||
        features.count[_collision_feature_prism] > MAXIMUM_COLLISION_FEATURES_PER_TEST - 30) return FALSE;
    return !collision_features_test_point(&features, &base, &hit) &&
        !collision_test_point(flags, &base, forge_avatar.unit_index) &&
        !collision_test_point(flags, &center, forge_avatar.unit_index) &&
        !collision_test_point(flags, &top, forge_avatar.unit_index);
}

/* Exact camera eye position first; a bounded local depenetration next, then
 * the still-clear entry position. If every candidate is blocked, stay in Forge. */
static boolean forge_avatar_destination(real_point3d *origin)
{
    real_point3d base, fixed;
    point_from_line3d(&forge.camera.position, &forge_avatar.eye_offset, -1.0f, origin);
    if (forge_avatar_origin_clear(origin)) return TRUE;
    point_from_line3d(origin, &forge_avatar.pill_offset, 1.0f, &base);
    if (collision_fix_pill(forge_avatar_collision_flags(), &base,
        MAX(0.5f, forge_avatar.radius * 2.0f), forge_avatar.height, forge_avatar.radius,
        forge_avatar.unit_index, &fixed)) {
        point_from_line3d(&fixed, &forge_avatar.pill_offset, -1.0f, origin);
        if (forge_avatar_origin_clear(origin)) {
            forge_message("Forge exit moved slightly to clear the character's collision volume");
            return TRUE;
        }
    }
    *origin = forge_avatar.original_origin;
    if (forge_avatar_origin_clear(origin)) {
        forge_message("Camera is inside solid geometry: returned to the Forge entry position");
        return TRUE;
    }
    forge_message("Cannot exit inside solid geometry: fly into clear space and try again");
    return FALSE;
}

static boolean forge_avatar_end(void)
{
    real_point3d origin;
    real_vector3d forward, right, level, up = {{0,0,1}};
    struct unit_datum *unit;
    long player_index = local_player_get_player_index(forge.local_player_index);
    if (player_index == NONE || player_get(player_index)->unit_index != forge_avatar.unit_index ||
        !forge_object_alive(forge_avatar.unit_index)) { forge_avatar_restore(); return TRUE; }
    if (!forge_avatar_destination(&origin)) return FALSE;
    unit = unit_get(forge_avatar.unit_index);
    forge_camera_vectors(&forward, &right);
    set_real_vector3d(&level, cosf(forge.camera.facing.yaw), sinf(forge.camera.facing.yaw), 0.0f);
    object_set_position(forge_avatar.unit_index, &origin, &level, &up);
    set_real_vector3d(&unit->object.translational_velocity, 0.0f, 0.0f, 0.0f);
    set_real_vector3d(&unit->object.angular_velocity, 0.0f, 0.0f, 0.0f);
    unit->unit.desired_facing_vector = level;
    unit->unit.desired_aiming_vector = unit->unit.aiming_vector = forward;
    unit->unit.desired_looking_vector = unit->unit.looking_vector = forward;
    player_control_set_facing(forge.local_player_index, &forward);
    biped_disconnect_from_structure_bsp(forge_avatar.unit_index);
    platform_log("forge: avatar returned at %.4f %.4f %.4f", origin.x, origin.y, origin.z);
    forge_avatar_restore();
    return TRUE;
}
