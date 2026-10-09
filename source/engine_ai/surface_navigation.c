#include "surface_navigation.h"
#include <assert.h>
#include <math.h>
#include <string.h>

typedef char sn_route_capacity_check[SN_WAYPOINTS_MAX >= 2*SN_ROUTE_POLYGONS_MAX+1 ? 1 : -1];

static bool finite_point(struct sn_point p)
{
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z) &&
        fabsf(p.x) < 1000000.f && fabsf(p.y) < 1000000.f && fabsf(p.z) < 1000000.f;
}
static float distance_squared(struct sn_point a, struct sn_point b)
{
    float x = a.x-b.x, y = a.y-b.y, z = a.z-b.z;
    return x*x+y*y+z*z;
}
static struct sn_point midpoint(struct sn_point a, struct sn_point b)
{
    struct sn_point p = {(a.x+b.x)*0.5f, (a.y+b.y)*0.5f, (a.z+b.z)*0.5f};
    return p;
}
static float polygon_area(const struct sn_polygon *p)
{
    float area = 0.f;
    uint32_t i;
    for (i=0; i<p->vertex_count; i++) {
        struct sn_point a=p->vertices[i], b=p->vertices[(i+1)%p->vertex_count];
        area += a.x*b.y-b.x*a.y;
    }
    return area;
}
static float edge_distance(const struct sn_polygon *p, uint32_t edge, struct sn_point q)
{
    struct sn_point a=p->vertices[edge], b=p->vertices[(edge+1)%p->vertex_count];
    float dx=b.x-a.x, dy=b.y-a.y, length=sqrtf(dx*dx+dy*dy);
    float cross=dx*(q.y-a.y)-dy*(q.x-a.x);
    return (polygon_area(p)>0.f ? cross : -cross)/length;
}
static bool valid_agent(struct sn_agent a)
{
    return isfinite(a.radius) && a.radius>0.f && a.radius<10.f &&
        isfinite(a.height) && a.height>=2.f*a.radius && a.height<20.f &&
        isfinite(a.minimum_normal_z) && a.minimum_normal_z>0.f && a.minimum_normal_z<=1.f &&
        isfinite(a.projection_height) && a.projection_height>0.f && a.projection_height<10.f;
}
/* -1 malformed, 0 valid but unsuitable, 1 usable. Validate before filtering. */
static int prepare_polygon(struct sn_polygon *p, struct sn_agent agent, uint32_t sources)
{
    uint32_t i,j;
    struct sn_point center={0.f,0.f,0.f};
    float norm=p->normal.x*p->normal.x+p->normal.y*p->normal.y+p->normal.z*p->normal.z;
    if (p->vertex_count<3 || p->vertex_count>SN_VERTICES_MAX || !finite_point(p->normal) ||
        fabsf(norm-1.f)>0.01f || !isfinite(p->width) || !isfinite(p->height) ||
        p->width<0.f || p->height<0.f || ((p->width==0.f)!=(p->height==0.f))) return -1;
    for (i=0; i<p->vertex_count; i++) {
        struct sn_point a=p->vertices[i], b=p->vertices[(i+1)%p->vertex_count];
        float dx=b.x-a.x,dy=b.y-a.y;
        struct sn_point delta={a.x-p->vertices[0].x,a.y-p->vertices[0].y,a.z-p->vertices[0].z};
        if (!finite_point(a) || dx*dx+dy*dy<0.00000001f ||
            fabsf(delta.x*p->normal.x+delta.y*p->normal.y+delta.z*p->normal.z)>0.03f ||
            (p->neighbors[i]!=SN_NONE && p->neighbors[i]>=sources)) return -1;
        center.x+=a.x; center.y+=a.y; center.z+=a.z;
    }
    if (fabsf(polygon_area(p))<0.000001f) return -1;
    for (i=0; i<p->vertex_count; i++)
        for (j=0; j<p->vertex_count; j++)
            if (edge_distance(p,i,p->vertices[j]) < -0.003f) return -1;
    p->center.x=center.x/p->vertex_count;
    p->center.y=center.y/p->vertex_count;
    p->center.z=center.z/p->vertex_count;
    return p->normal.z>=agent.minimum_normal_z && (p->width==0.f ||
        (p->width>=2.f*agent.radius && p->height>=agent.height));
}
enum sn_status sn_build_begin(struct sn_build *b, struct sn_resource *out,
    uint32_t count, struct sn_key key, uint64_t revision, struct sn_agent agent)
{
    assert(b && out);
    out->ready=false; out->requires_clearance=false; out->polygon_count=0;
    b->status=SN_INVALID;
    if (!valid_agent(agent) || key.format_version!=SN_FORMAT_VERSION || !revision || !count) return b->status;
    if (count>SN_SOURCE_MAX) return b->status=SN_CAPACITY;
    memset(b->mapping,0xff,count*sizeof(b->mapping[0]));
    b->source_count=count; b->cursor=0; b->phase=0;
    out->key=key; out->agent=agent; out->revision=revision;
    return b->status=SN_RUNNING;
}
static enum sn_status read_row(struct sn_build *b, struct sn_resource *out,
    sn_read_polygon read, void *context)
{
    struct sn_polygon p;
    int result;
    memset(&p,0,sizeof(p));
    result=read(context,b->cursor,&p);
    if (result<0 || result>1) return SN_INVALID;
    if (!result) return SN_RUNNING;
    result=prepare_polygon(&p,out->agent,b->source_count);
    if (result<0) return SN_INVALID;
    if (!result) return SN_RUNNING;
    if (out->polygon_count==SN_POLYGONS_MAX) return SN_CAPACITY;
    if (p.width==0.f) out->requires_clearance=true;
    p.source_index=b->cursor;
    b->mapping[b->cursor]=out->polygon_count;
    out->polygons[out->polygon_count++]=p;
    return SN_RUNNING;
}
static void map_row(const struct sn_build *b, struct sn_resource *out, uint32_t row)
{
    struct sn_polygon *p=&out->polygons[row];
    uint32_t e;
    for (e=0; e<p->vertex_count; e++) {
        uint32_t next=p->neighbors[e];
        struct sn_point a=p->vertices[e],z=p->vertices[(e+1)%p->vertex_count];
        float dx=a.x-z.x,dy=a.y-z.y;
        p->neighbors[e]=(next!=SN_NONE && dx*dx+dy*dy>4.f*out->agent.radius*out->agent.radius) ?
            b->mapping[next] : SN_NONE;
    }
}
static bool valid_adjacency(const struct sn_resource *r, uint32_t row)
{
    const struct sn_polygon *p=&r->polygons[row];
    uint32_t e,j;
    for (e=0; e<p->vertex_count; e++) {
        uint32_t n=p->neighbors[e];
        bool found=false;
        if (n==SN_NONE) continue;
        if (n>=r->polygon_count || n==row) return false;
        for (j=0; j<r->polygons[n].vertex_count; j++) {
            const struct sn_polygon *q=&r->polygons[n];
            struct sn_point a=p->vertices[e],b=p->vertices[(e+1)%p->vertex_count];
            struct sn_point c=q->vertices[j],d=q->vertices[(j+1)%q->vertex_count];
            if (q->neighbors[j]==row && ((distance_squared(a,c)<0.0001f && distance_squared(b,d)<0.0001f) ||
                (distance_squared(a,d)<0.0001f && distance_squared(b,c)<0.0001f))) found=true;
        }
        if (!found) return false;
    }
    return true;
}
enum sn_status sn_build_step(struct sn_build *b, struct sn_resource *out,
    uint32_t budget, sn_read_polygon read, void *context)
{
    uint32_t work;
    assert(b && out);
    if (b->status!=SN_RUNNING) return b->status;
    if (!read || !budget || budget>256) return b->status=SN_INVALID;
    for (work=0; work<budget; work++) {
        uint32_t count=b->phase==0 ? b->source_count : out->polygon_count;
        if (b->cursor==count) {
            b->cursor=0;
            if (++b->phase==3) {
                out->ready=true;
                return b->status=SN_READY;
            }
            continue;
        }
        if (b->phase==0) b->status=read_row(b,out,read,context);
        else if (b->phase==1) map_row(b,out,b->cursor);
        else if (!valid_adjacency(out,b->cursor)) b->status=SN_INVALID;
        if (b->status!=SN_RUNNING) return b->status;
        b->cursor++;
    }
    return b->status;
}
static bool contains(const struct sn_resource *r, uint32_t index, struct sn_point point)
{
    const struct sn_polygon *p=&r->polygons[index];
    uint32_t edge;
    for (edge=0; edge<p->vertex_count; edge++) {
        float clearance=p->neighbors[edge]==SN_NONE ? r->agent.radius : 0.f;
        if (edge_distance(p,edge,point)<clearance-0.0001f) return false;
    }
    return true;
}
enum sn_status sn_project(const struct sn_resource *r, struct sn_point point,
    uint32_t *polygon, struct sn_point *projected)
{
    uint32_t i;
    float best;
    assert(r && polygon && projected);
    *polygon=SN_NONE;
    if (!r->ready || !finite_point(point)) return SN_INVALID;
    best=r->agent.projection_height+0.00001f;
    for (i=0; i<r->polygon_count; i++) {
        const struct sn_polygon *p=&r->polygons[i];
        struct sn_point q=point;
        float dz;
        if (!contains(r,i,p->center) || !contains(r,i,point)) continue;
        q.z=p->vertices[0].z-(p->normal.x*(point.x-p->vertices[0].x)+
            p->normal.y*(point.y-p->vertices[0].y))/p->normal.z;
        dz=fabsf(q.z-point.z);
        if (dz<best) { best=dz; *polygon=i; *projected=q; }
    }
    return *polygon==SN_NONE ? SN_NO_PATH : SN_FOUND;
}
enum sn_status sn_query_begin(struct sn_query *q, const struct sn_resource *r,
    struct sn_point start, struct sn_point goal, uint64_t obstacles)
{
    enum sn_status status;
    assert(q && r);
    q->status=SN_INVALID;
    if (!r->ready) return q->status;
    q->key=r->key; q->revision=r->revision; q->obstacle_revision=obstacles;
    status=sn_project(r,start,&q->start,&q->start_point);
    if (status!=SN_FOUND) return q->status=status==SN_INVALID ? SN_INVALID : SN_BAD_START;
    status=sn_project(r,goal,&q->goal,&q->goal_point);
    if (status!=SN_FOUND) return q->status=status==SN_INVALID ? SN_INVALID : SN_BAD_GOAL;
    memset(q->previous,0xff,r->polygon_count*sizeof(q->previous[0]));
    q->previous[q->start]=q->start;
    q->head=0; q->tail=1; q->queue[0]=q->start;
    return q->status=SN_RUNNING;
}
static bool same_key(struct sn_key a, struct sn_key b)
{
    return a.map_identity==b.map_identity && a.overlay_revision==b.overlay_revision &&
        a.checksum==b.checksum && a.bsp_index==b.bsp_index &&
        a.agent_identity==b.agent_identity && a.format_version==b.format_version;
}
static bool current(const struct sn_query *q, const struct sn_resource *r, uint64_t obstacles)
{
    return r->ready && same_key(q->key,r->key) &&
        r->revision==q->revision && q->obstacle_revision==obstacles;
}
static bool expand(struct sn_query *q, const struct sn_resource *r, uint32_t row,
    sn_segment_clear clear, void *context)
{
    const struct sn_polygon *p=&r->polygons[row];
    uint32_t e;
    for (e=0; e<p->vertex_count; e++) {
        uint32_t n=p->neighbors[e];
        struct sn_point portal;
        if (n==SN_NONE || q->previous[n]!=SN_NONE ||
            !contains(r,n,r->polygons[n].center)) continue;
        portal=midpoint(p->vertices[e],p->vertices[(e+1)%p->vertex_count]);
        if (clear && (!clear(context,p->center,row,portal,row) ||
            !clear(context,portal,n,r->polygons[n].center,n))) continue;
        assert(q->tail<SN_POLYGONS_MAX);
        q->previous[n]=row; q->previous_edge[n]=(uint8_t)e;
        q->queue[q->tail++]=n;
    }
    return true;
}
enum sn_status sn_query_step(struct sn_query *q, const struct sn_resource *r,
    uint64_t obstacles, uint32_t budget, sn_segment_clear clear, void *context)
{
    uint32_t work;
    assert(q && r);
    if (!current(q,r,obstacles)) return q->status=SN_STALE;
    if (q->status!=SN_RUNNING) return q->status;
    if (!budget || budget>256 || (r->requires_clearance && !clear)) return q->status=SN_INVALID;
    if (clear && (!clear(context,q->start_point,q->start,r->polygons[q->start].center,q->start) ||
        !clear(context,r->polygons[q->goal].center,q->goal,q->goal_point,q->goal))) return q->status=SN_NO_PATH;
    for (work=0; work<budget && q->head<q->tail; work++) {
        uint32_t row=q->queue[q->head++];
        if (row==q->goal) return q->status=SN_FOUND;
        expand(q,r,row,clear,context);
    }
    return q->status=q->head==q->tail ? SN_NO_PATH : SN_RUNNING;
}
static void append(struct sn_route *route, struct sn_point point, uint32_t polygon)
{
    assert(route->count<SN_WAYPOINTS_MAX);
    route->points[route->count]=point; route->polygons[route->count++]=polygon;
}
enum sn_status sn_query_result(const struct sn_query *q, const struct sn_resource *r,
    uint64_t obstacles, struct sn_route *route)
{
    uint32_t chain[SN_ROUTE_POLYGONS_MAX],count=0,row,i;
    assert(q && r && route);
    route->count=0;
    if (!current(q,r,obstacles)) return SN_STALE;
    if (q->status!=SN_FOUND) return q->status;
    row=q->goal;
    for (i=0; i<SN_ROUTE_POLYGONS_MAX; i++) {
        chain[count++]=row;
        if (row==q->start) break;
        row=q->previous[row];
        if (row>=r->polygon_count) return SN_INVALID;
    }
    if (chain[count-1]!=q->start) return SN_CAPACITY;
    route->key=q->key; route->revision=q->revision; route->obstacle_revision=q->obstacle_revision;
    append(route,q->start_point,q->start);
    for (i=count; i>0; i--) {
        uint32_t n=chain[i-1];
        const struct sn_polygon *p=&r->polygons[n];
        append(route,p->center,n);
        if (i>1) {
            uint32_t next=chain[i-2],e=q->previous_edge[next];
            append(route,midpoint(p->vertices[e],p->vertices[(e+1)%p->vertex_count]),n);
        }
    }
    append(route,q->goal_point,q->goal);
    assert(route->count<=SN_WAYPOINTS_MAX);
    return SN_FOUND;
}
bool sn_route_current(const struct sn_route *route, const struct sn_resource *r, uint64_t obstacles)
{
    assert(route && r);
    return route->count>0 && r->ready && same_key(route->key,r->key) &&
        route->revision==r->revision && route->obstacle_revision==obstacles;
}
const char *sn_status_name(enum sn_status status)
{
    static const char *const names[]={"invalid","running","ready","found","no-path",
        "bad-start","bad-goal","capacity","stale"};
    return (unsigned)status<sizeof(names)/sizeof(names[0]) ? names[status] : "invalid";
}
