#include "platform_shape.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* Counterclockwise viewed from outside. Top first, bottom last. */
const uint8_t platform_faces[PLATFORM_FACE_COUNT][4] = {
    {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5},
    {0, 1, 5, 4}, {3, 7, 6, 2}, {3, 2, 1, 0}
};
const uint8_t platform_edges[PLATFORM_EDGE_COUNT][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}
};
const float platform_normals[PLATFORM_FACE_COUNT][3] = {
    {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}
};

bool platform_shape_init(struct platform_shape *shape, const float minimum[3],
    const float maximum[3])
{
    static const uint8_t corners[PLATFORM_VERTEX_COUNT][3] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
    };
    uint32_t axis, vertex;
    assert(shape != NULL);
    assert(minimum != NULL && maximum != NULL);
    for (axis = 0; axis < 3; axis++) {
        if (!isfinite(minimum[axis]) || !isfinite(maximum[axis]) ||
            minimum[axis] >= maximum[axis] ||
            minimum[axis] < -4900.0f || maximum[axis] > 4900.0f) {
            return false;
        }
    }
    memcpy(shape->minimum, minimum, sizeof(shape->minimum));
    memcpy(shape->maximum, maximum, sizeof(shape->maximum));
    for (vertex = 0; vertex < PLATFORM_VERTEX_COUNT; vertex++) {
        for (axis = 0; axis < 3; axis++) {
            shape->vertices[vertex][axis] =
                corners[vertex][axis] ? maximum[axis] : minimum[axis];
        }
    }
    return true;
}

bool platform_shape_contains(const struct platform_shape *shape, const float point[3])
{
    assert(shape != NULL);
    assert(point != NULL);
    for (uint32_t axis = 0; axis < 3; axis++) {
        if (!isfinite(point[axis]) || point[axis] < shape->minimum[axis] ||
            point[axis] > shape->maximum[axis]) {
            return false;
        }
    }
    return true;
}

bool platform_shape_near(const struct platform_shape *shape, const float center[3], float radius)
{
    float distance_squared = 0.0f;
    assert(shape != NULL);
    assert(center != NULL);
    if (!isfinite(radius) || radius < 0.0f) return false;
    for (uint32_t axis = 0; axis < 3; axis++) {
        float delta = 0.0f;
        if (!isfinite(center[axis])) return false;
        if (center[axis] < shape->minimum[axis]) delta = shape->minimum[axis] - center[axis];
        if (center[axis] > shape->maximum[axis]) delta = center[axis] - shape->maximum[axis];
        distance_squared += delta * delta;
    }
    return distance_squared <= radius * radius;
}

bool platform_shape_ray(const struct platform_shape *shape, const float point[3],
    const float vector[3], bool front, bool back, float *t, uint32_t *face)
{
    bool hit = false;
    uint32_t axis, index;
    assert(shape != NULL);
    assert(point != NULL && vector != NULL);
    assert(t != NULL && face != NULL);
    if (!isfinite(*t) || *t < 0.0f) return false;
    for (axis = 0; axis < 3; axis++) {
        if (!isfinite(point[axis]) || !isfinite(vector[axis])) return false;
    }
    for (index = 0; index < PLATFORM_FACE_COUNT; index++) {
        const float *normal = platform_normals[index];
        const float *vertex = shape->vertices[platform_faces[index][0]];
        float dot = 0.0f;
        float distance = 0.0f;
        float intersection[3];
        for (axis = 0; axis < 3; axis++) {
            dot += vector[axis] * normal[axis];
            distance += (vertex[axis] - point[axis]) * normal[axis];
        }
        if (dot == 0.0f || (dot < 0.0f ? !front : !back)) continue;
        float candidate_t = distance / dot;
        if (candidate_t < 0.0f || candidate_t > 1.0f || candidate_t >= *t) continue;
        bool within_face = true;
        for (axis = 0; axis < 3; axis++) {
            intersection[axis] = point[axis] + vector[axis] * candidate_t;
            if (normal[axis] == 0.0f &&
                (intersection[axis] < shape->minimum[axis] ||
                 intersection[axis] > shape->maximum[axis])) within_face = false;
        }
        if (within_face) {
            *t = candidate_t;
            *face = index;
            hit = true;
        }
    }
    return hit;
}

void platform_shape_rotation(const float rotation[3], float rows[3][3])
{
    float cy, sy, cp, sp, cr, sr;
    assert(rotation != NULL);
    assert(rows != NULL);
    cy = cosf(rotation[0]); sy = sinf(rotation[0]);
    cp = cosf(rotation[1]); sp = sinf(rotation[1]);
    cr = cosf(rotation[2]); sr = sinf(rotation[2]);
    /* Halo applies yaw, then pitch, then roll: Rx(roll) Ry(-pitch) Rz(yaw).
     * Its matrix stores columns; these rows can also be uploaded to GLSL. */
    rows[0][0] = cy * cp;
    rows[0][1] = -sy * cp;
    rows[0][2] = -sp;
    rows[1][0] = sy * cr - cy * sp * sr;
    rows[1][1] = sy * sp * sr + cy * cr;
    rows[1][2] = -cp * sr;
    rows[2][0] = cy * sp * cr + sy * sr;
    rows[2][1] = cy * sr - sy * sp * cr;
    rows[2][2] = cp * cr;
}

static void platform_shape_rotate_impl(const float rotation[3], const float vector[3],
    bool inverse, float result[3])
{
    float rows[3][3], copy[3];
    uint32_t row, column;
    assert(vector != NULL);
    assert(result != NULL);
    memcpy(copy, vector, sizeof(copy));
    platform_shape_rotation(rotation, rows);
    for (row = 0; row < 3; row++) {
        result[row] = 0.0f;
        for (column = 0; column < 3; column++)
            result[row] += (inverse ? rows[column][row] : rows[row][column]) * copy[column];
    }
}

void platform_shape_rotate(const float rotation[3], const float vector[3], float result[3])
{
    platform_shape_rotate_impl(rotation, vector, false, result);
}

void platform_shape_unrotate(const float rotation[3], const float vector[3], float result[3])
{
    platform_shape_rotate_impl(rotation, vector, true, result);
}

void platform_shape_to_world(const struct platform_shape *shape, const float position[3],
    const float rotation[3], const float point[3], float result[3])
{
    float relative[3];
    uint32_t axis;
    assert(shape != NULL);
    assert(position != NULL && point != NULL);
    for (axis = 0; axis < 3; axis++)
        relative[axis] = point[axis] - (shape->minimum[axis] + shape->maximum[axis]) * 0.5f;
    platform_shape_rotate(rotation, relative, result);
    for (axis = 0; axis < 3; axis++) result[axis] += position[axis];
}

void platform_shape_to_local(const struct platform_shape *shape, const float position[3],
    const float rotation[3], const float point[3], float result[3])
{
    float relative[3];
    uint32_t axis;
    assert(shape != NULL);
    assert(position != NULL && point != NULL);
    for (axis = 0; axis < 3; axis++) relative[axis] = point[axis] - position[axis];
    platform_shape_unrotate(rotation, relative, result);
    for (axis = 0; axis < 3; axis++)
        result[axis] += (shape->minimum[axis] + shape->maximum[axis]) * 0.5f;
}
