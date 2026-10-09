#include "../source/features/platform/platform_shape.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct platform_shape box(void)
{
    struct platform_shape shape;
    const float minimum[3] = {-2, -2, 1};
    const float maximum[3] = {2, 2, 1.2f};
    assert(platform_shape_init(&shape, minimum, maximum));
    return shape;
}

static void test_geometry(void)
{
    struct platform_shape shape = box();
    for (uint32_t face = 0; face < PLATFORM_FACE_COUNT; face++) {
        const float *a = shape.vertices[platform_faces[face][0]];
        const float *b = shape.vertices[platform_faces[face][1]];
        const float *c = shape.vertices[platform_faces[face][2]];
        float u[3], v[3], cross[3];
        for (uint32_t axis = 0; axis < 3; axis++) {
            u[axis] = b[axis] - a[axis];
            v[axis] = c[axis] - b[axis];
        }
        cross[0] = u[1] * v[2] - u[2] * v[1];
        cross[1] = u[2] * v[0] - u[0] * v[2];
        cross[2] = u[0] * v[1] - u[1] * v[0];
        float dot = 0;
        for (uint32_t axis = 0; axis < 3; axis++) {
            dot += cross[axis] * platform_normals[face][axis];
        }
        assert(dot > 0); /* rendering/collision winding agrees with the normal */
    }
    for (uint32_t edge = 0; edge < PLATFORM_EDGE_COUNT; edge++) {
        uint32_t faces = 0;
        for (uint32_t face = 0; face < PLATFORM_FACE_COUNT; face++) {
            for (uint32_t vertex = 0; vertex < 4; vertex++) {
                uint8_t a = platform_faces[face][vertex];
                uint8_t b = platform_faces[face][(vertex+1) % 4];
                uint8_t x = platform_edges[edge][0], y = platform_edges[edge][1];
                if ((a == x && b == y) || (a == y && b == x)) faces++;
            }
        }
        assert(faces == 2); /* closed solid, no missing perimeter */
    }
}

static void test_validation(void)
{
    struct platform_shape shape = box(), original = shape;
    float minimum[3] = {-2, -2, 1}, maximum[3] = {2, 2, 1};
    assert(!platform_shape_init(&shape, minimum, maximum));
    assert(memcmp(&shape, &original, sizeof(shape)) == 0);
    maximum[2] = NAN;
    assert(!platform_shape_init(&shape, minimum, maximum));
    maximum[2] = INFINITY;
    assert(!platform_shape_init(&shape, minimum, maximum));
    maximum[2] = 5000;
    assert(!platform_shape_init(&shape, minimum, maximum));
    assert(platform_shape_contains(&shape, (float[3]){0, 0, 1.1f}));
    assert(!platform_shape_contains(&shape, (float[3]){0, 0, 0}));
    assert(!platform_shape_contains(&shape, (float[3]){NAN, 0, 1}));
    assert(platform_shape_near(&shape, (float[3]){0, 0, 1.5f}, 0.5f));
    assert(!platform_shape_near(&shape, (float[3]){3, 3, 1.1f}, 1));
    assert(!platform_shape_near(&shape, (float[3]){0, 0, 1}, -1));
}

static void test_rays(void)
{
    struct platform_shape shape = box();
    const float down[3] = {0, 0, -2}, up[3] = {0, 0, 2}, zero[3] = {0, 0, 0};
    float t = 1;
    uint32_t face = 99;
    assert(platform_shape_ray(&shape, (float[3]){0, 0, 2}, down, true, false, &t, &face));
    assert(fabsf(t - 0.4f) < 1e-6f && face == 0);
    t = 0.3f; face = 99;
    assert(!platform_shape_ray(&shape, (float[3]){0, 0, 2}, down, true, true, &t, &face));
    assert(t == 0.3f && face == 99); /* nearer map/object hit wins */
    t = 1;
    assert(!platform_shape_ray(&shape, (float[3]){3, 0, 2}, down, true, true, &t, &face));
    assert(!platform_shape_ray(&shape, (float[3]){0, 0, 2}, zero, true, true, &t, &face));
    assert(!platform_shape_ray(&shape, (float[3]){0, 0, 2}, down, false, false, &t, &face));
    assert(platform_shape_ray(&shape, (float[3]){0, 0, 2}, down, false, true, &t, &face));
    assert(t == 0.5f && face == 5); /* back-facing exit through underside */
    t = 1;
    assert(platform_shape_ray(&shape, (float[3]){0, 0, 0}, up, true, false, &t, &face));
    assert(t == 0.5f && face == 5); /* underside */
    t = 1;
    assert(platform_shape_ray(&shape, (float[3]){0, 0, 1.1f}, up, false, true, &t, &face));
    assert(face == 0); /* exit from inside */
    t = 1;
    assert(platform_shape_ray(&shape, (float[3]){3, 0, 1.1f},
        (float[3]){-2, 0, 0}, true, false, &t, &face));
    assert(t == 0.5f && face == 2); /* vertical side */
    t = 1;
    assert(platform_shape_ray(&shape, (float[3]){2, 2, 2}, down, true, false, &t, &face));
    assert(face == 0); /* exact corner */
    t = 1;
    assert(!platform_shape_ray(&shape, (float[3]){NAN, 0, 2}, down, true, true, &t, &face));
    assert(!platform_shape_ray(&shape, (float[3]){0, 0, 2},
        (float[3]){0, 0, INFINITY}, true, true, &t, &face));
}

static void test_rigid_pose(void)
{
    struct platform_shape shape;
    const float minimum[3] = {-2, -0.5f, 0}, maximum[3] = {2, 0.5f, 0.2f};
    const float position[3] = {10, 20, 5};
    const float rotation[3] = {1.57079632679f, 0, 0};
    float world[3], local[3], direction[3], t = 1.0f;
    uint32_t face = 99;
    assert(platform_shape_init(&shape, minimum, maximum));
    platform_shape_to_world(&shape, position, rotation, shape.vertices[6], world);
    assert(fabsf(world[0] - 9.5f) < 1e-5f);
    assert(fabsf(world[1] - 22.0f) < 1e-5f);
    assert(fabsf(world[2] - 5.1f) < 1e-5f);
    platform_shape_to_local(&shape, position, rotation, world, local);
    for (uint32_t axis = 0; axis < 3; axis++)
        assert(fabsf(local[axis] - shape.vertices[6][axis]) < 1e-5f);
    platform_shape_to_local(&shape, position, rotation, (float[3]){10, 23, 5}, local);
    platform_shape_unrotate(rotation, (float[3]){0, -2, 0}, direction);
    assert(platform_shape_ray(&shape, local, direction, true, false, &t, &face));
    assert(fabsf(t - 0.5f) < 1e-5f && face == 2);
    platform_shape_rotate(rotation, platform_normals[face], direction);
    assert(fabsf(direction[0]) < 1e-5f && fabsf(direction[1] - 1) < 1e-5f);
    platform_shape_unrotate(rotation, direction, direction); /* in-place is supported */
    assert(fabsf(direction[0] - 1) < 1e-5f && fabsf(direction[1]) < 1e-5f);
}

static void test_full_pose(void)
{
    struct platform_shape shape;
    const float minimum[3] = {1, 2, 3}, maximum[3] = {5, 8, 9};
    const float position[3] = {10, 20, 5};
    float world[3], local[3], direction[3], rows[3][3];
    assert(platform_shape_init(&shape, minimum, maximum));
    /* Halo pitch raises forward; roll raises left, unlike simply negating
     * all three Euler angles for the inverse of a composite rotation. */
    platform_shape_rotate((float[3]){0, 1.57079632679f, 0}, (float[3]){1, 0, 0}, world);
    assert(fabsf(world[2] - 1) < 1e-5f && fabsf(world[0]) < 1e-5f);
    platform_shape_rotate((float[3]){0, 0, 1.57079632679f}, (float[3]){0, 1, 0}, world);
    assert(fabsf(world[2] - 1) < 1e-5f && fabsf(world[1]) < 1e-5f);
    for (uint32_t sample = 0; sample < 128; sample++) {
        float rotation[3] = {sample * 0.319f - 20, sample * -0.253f + 16, sample * 0.277f - 17};
        float t = 1.0f;
        uint32_t face = 99;
        platform_shape_rotation(rotation, rows);
        for (uint32_t row = 0; row < 3; row++) {
            for (uint32_t column = 0; column < 3; column++) {
                float dot = 0;
                for (uint32_t axis = 0; axis < 3; axis++) dot += rows[row][axis] * rows[column][axis];
                assert(fabsf(dot - (row == column ? 1.0f : 0.0f)) < 1e-5f);
            }
        }
        for (uint32_t vertex = 0; vertex < PLATFORM_VERTEX_COUNT; vertex++) {
            platform_shape_to_world(&shape, position, rotation, shape.vertices[vertex], world);
            platform_shape_to_local(&shape, position, rotation, world, local);
            for (uint32_t axis = 0; axis < 3; axis++)
                assert(fabsf(local[axis] - shape.vertices[vertex][axis]) < 1e-4f);
        }
        platform_shape_to_world(&shape, position, rotation, (float[3]){3, 5, 10}, world);
        platform_shape_to_local(&shape, position, rotation, world, local);
        platform_shape_rotate(rotation, (float[3]){0, 0, -2}, direction);
        platform_shape_unrotate(rotation, direction, direction);
        assert(platform_shape_ray(&shape, local, direction, true, false, &t, &face));
        assert(fabsf(t - 0.5f) < 1e-5f && face == 0);
    }
}

int main(void)
{
    test_geometry();
    test_validation();
    test_rays();
    test_rigid_pose();
    test_full_pose();
    puts("platform shape: PASS");
    return 0;
}
