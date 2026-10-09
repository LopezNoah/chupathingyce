/* Fixed-capacity box geometry, independent of engine tags and GPU resources. */
#ifndef PLATFORM_SHAPE_H
#define PLATFORM_SHAPE_H

#include <stdbool.h>
#include <stdint.h>

enum { PLATFORM_VERTEX_COUNT = 8, PLATFORM_FACE_COUNT = 6, PLATFORM_EDGE_COUNT = 12 };

struct platform_shape
{
    float minimum[3];
    float maximum[3];
    float vertices[PLATFORM_VERTEX_COUNT][3];
};

extern const uint8_t platform_faces[PLATFORM_FACE_COUNT][4];
extern const uint8_t platform_edges[PLATFORM_EDGE_COUNT][2];
extern const float platform_normals[PLATFORM_FACE_COUNT][3];

/* Rejects nonfinite, inverted or out-of-world bounds, leaving shape untouched. */
bool platform_shape_init(struct platform_shape *shape, const float minimum[3],
    const float maximum[3]);
bool platform_shape_contains(const struct platform_shape *shape, const float point[3]);
bool platform_shape_near(const struct platform_shape *shape, const float center[3], float radius);
/* Segment/box faces; miss leaves t and face untouched. Front/back are explicit. */
bool platform_shape_ray(const struct platform_shape *shape, const float point[3],
    const float vector[3], bool front, bool back, float *t, uint32_t *face);

/* Full rigid pose, with local origin at the box center. Angles are Halo's
 * yaw/pitch/roll (radians); row-major basis matches matrix4x3_rotation_from_angles.
 * Rotation and its transpose preserve vector lengths. Point/vector inputs may
 * alias their outputs (but not the pose or shape bounds). */
void platform_shape_rotation(const float rotation[3], float rows[3][3]);
void platform_shape_to_world(const struct platform_shape *shape, const float position[3],
    const float rotation[3], const float point[3], float result[3]);
void platform_shape_to_local(const struct platform_shape *shape, const float position[3],
    const float rotation[3], const float point[3], float result[3]);
void platform_shape_rotate(const float rotation[3], const float vector[3], float result[3]);
void platform_shape_unrotate(const float rotation[3], const float vector[3], float result[3]);

#endif
