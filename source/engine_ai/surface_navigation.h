/* Geometry navigation: cooked convex collision surfaces, not the small region graph. */
#ifndef ENGINE_AI_SURFACE_NAVIGATION_H
#define ENGINE_AI_SURFACE_NAVIGATION_H
#include <stdbool.h>
#include <stdint.h>

#define SN_NONE UINT32_MAX
#define SN_FORMAT_VERSION 1u
enum { SN_SOURCE_MAX = 131072, SN_POLYGONS_MAX = 16384, SN_VERTICES_MAX = 8,
       SN_ROUTE_POLYGONS_MAX = 512, SN_WAYPOINTS_MAX = 1025 };

struct sn_point { float x, y, z; };
struct sn_agent { float radius, height, minimum_normal_z, projection_height; };
struct sn_key {
    uint64_t map_identity, overlay_revision;
    uint32_t checksum, bsp_index, agent_identity, format_version;
};
enum sn_status { SN_INVALID = 0, SN_RUNNING, SN_READY, SN_FOUND, SN_NO_PATH,
    SN_BAD_START, SN_BAD_GOAL, SN_CAPACITY, SN_STALE };

/* Source indices are local to the current BSP. Width/height are validated clearance
   hints; both zero means unknown and REQUIRES a query-time collision callback.
   A source reader returns -1 malformed, 0 excluded, 1 present. */
struct sn_polygon {
    struct sn_point vertices[SN_VERTICES_MAX];
    uint32_t neighbors[SN_VERTICES_MAX];
    struct sn_point normal, center;
    float width, height;
    uint32_t source_index;
    uint8_t vertex_count;
};
typedef int (*sn_read_polygon)(void *context, uint32_t source_index, struct sn_polygon *out);
struct sn_resource {
    struct sn_key key;
    struct sn_agent agent;
    uint64_t revision;
    uint32_t polygon_count;
    bool ready, requires_clearance;
    struct sn_polygon polygons[SN_POLYGONS_MAX];
};
struct sn_build {
    uint32_t mapping[SN_SOURCE_MAX];
    uint32_t source_count, cursor, phase;
    enum sn_status status;
};

/* Build only into unpublished storage. Each step processes <=budget source
   polygons or adjacency rows (1..256). Caller publishes only SN_READY. */
enum sn_status sn_build_begin(struct sn_build *build, struct sn_resource *out,
    uint32_t source_count, struct sn_key key, uint64_t revision, struct sn_agent agent);
enum sn_status sn_build_step(struct sn_build *build, struct sn_resource *out,
    uint32_t budget, sn_read_polygon read, void *context);

/* Height-aware vertical projection inside an existing polygon, never snaps an
   arbitrary out-of-bounds point onto a distant island. No cross-floor links. */
enum sn_status sn_project(const struct sn_resource *resource, struct sn_point point,
    uint32_t *polygon, struct sn_point *projected);

/* Query-time geometry/obstacle check (mandatory when clearance is unknown). Own its revision separately from
   immutable topology; changing it invalidates queries/routes. It must be bounded.
   Segments are center->portal and portal->center, with valid polygon indices. */
typedef bool (*sn_segment_clear)(void *context, struct sn_point a, uint32_t a_polygon,
    struct sn_point b, uint32_t b_polygon);
struct sn_query {
    struct sn_key key;
    uint32_t queue[SN_POLYGONS_MAX], previous[SN_POLYGONS_MAX];
    uint8_t previous_edge[SN_POLYGONS_MAX];
    uint32_t head, tail, start, goal;
    uint64_t revision, obstacle_revision;
    struct sn_point start_point, goal_point;
    enum sn_status status;
};
struct sn_route {
    struct sn_key key;
    uint64_t revision, obstacle_revision;
    uint32_t count;
    struct sn_point points[SN_WAYPOINTS_MAX];
    uint32_t polygons[SN_WAYPOINTS_MAX];
};
/* Stable BFS storage order, O(P+E), no quadratic scans, heap, or recursion.
   Optimizes polygon hops, not metric distance. begin/projection is O(P*8).
   step expands <=budget polygons (1..256), each with <=8 edges. */
enum sn_status sn_query_begin(struct sn_query *query, const struct sn_resource *resource,
    struct sn_point start, struct sn_point goal, uint64_t obstacle_revision);
enum sn_status sn_query_step(struct sn_query *query, const struct sn_resource *resource,
    uint64_t obstacle_revision, uint32_t budget, sn_segment_clear clear, void *context);
enum sn_status sn_query_result(const struct sn_query *query, const struct sn_resource *resource,
    uint64_t obstacle_revision, struct sn_route *route);
bool sn_route_current(const struct sn_route *route, const struct sn_resource *resource,
    uint64_t obstacle_revision);
const char *sn_status_name(enum sn_status status);
#endif
