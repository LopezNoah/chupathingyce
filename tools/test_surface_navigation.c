#include "../source/engine_ai/surface_navigation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct sn_resource first,second;
static struct sn_build build;
static struct sn_query query;
static struct sn_route route;
static struct sn_polygon source[4];
static struct sn_agent agent={0.2f,1.f,0.7f,0.6f};
static struct sn_key key={123,0,77,0,12,SN_FORMAT_VERSION};
static int read_polygon(void *context,uint32_t index,struct sn_polygon *out)
{
    (void)context;
    *out=source[index];
    return 1;
}
static void rectangle(uint32_t index,float x,float y,float z,float width,float length)
{
    uint32_t e;
    struct sn_polygon *p=&source[index];
    memset(p,0,sizeof(*p));
    p->vertices[0]=(struct sn_point){x,y,z};
    p->vertices[1]=(struct sn_point){x+width,y,z};
    p->vertices[2]=(struct sn_point){x+width,y+length,z};
    p->vertices[3]=(struct sn_point){x,y+length,z};
    p->vertex_count=4; p->normal.z=1.f; p->width=4.f; p->height=4.f;
    for(e=0;e<8;e++)p->neighbors[e]=SN_NONE;
}
static enum sn_status build_resource(struct sn_resource *r,uint32_t count,uint64_t revision)
{
    enum sn_status status=sn_build_begin(&build,r,count,key,revision,agent);
    uint32_t calls=0;
    while(status==SN_RUNNING && calls++<2000)
        status=sn_build_step(&build,r,64,read_polygon,NULL);
    assert(calls<2000);
    return status;
}
static enum sn_status find(struct sn_point start,struct sn_point goal,sn_segment_clear clear,void *context)
{
    uint32_t calls=0;
    enum sn_status status=sn_query_begin(&query,&first,start,goal,1);
    while(status==SN_RUNNING && calls++<SN_POLYGONS_MAX)
        status=sn_query_step(&query,&first,1,1,clear,context);
    assert(calls<SN_POLYGONS_MAX);
    if(status==SN_FOUND)status=sn_query_result(&query,&first,1,&route);
    return status;
}
static void test_floor_and_ramp(void)
{
    struct sn_point start={1.f,1.f,0.1f},goal={3.f,1.f,0.6f};
    rectangle(0,0,0,0,2,2); rectangle(1,2,0,0,2,2);
    source[0].neighbors[1]=1; source[1].neighbors[3]=0;
    source[1].vertices[1].z=source[1].vertices[2].z=1.f;
    source[1].normal.x=-1.f/sqrtf(5.f); source[1].normal.z=2.f/sqrtf(5.f);
    assert(build_resource(&first,2,1)==SN_READY);
    assert(find(start,goal,NULL,NULL)==SN_FOUND);
    assert(route.count==5);
    assert(fabsf(route.points[route.count-1].z-0.5f)<0.001f);
    assert(build_resource(&second,2,1)==SN_READY);
    assert(first.polygon_count==second.polygon_count);
    assert(memcmp(first.polygons,second.polygons,2*sizeof(first.polygons[0]))==0);
    assert(find((struct sn_point){99,99,0},goal,NULL,NULL)==SN_BAD_START);
    assert(find(start,(struct sn_point){99,99,0},NULL,NULL)==SN_BAD_GOAL);
}
static void test_stacked_and_disconnected(void)
{
    uint32_t polygon;
    struct sn_point projected;
    rectangle(0,0,0,0,2,2); rectangle(1,0,0,3,2,2);
    assert(build_resource(&first,2,2)==SN_READY);
    assert(sn_project(&first,(struct sn_point){1,1,3.1f},&polygon,&projected)==SN_FOUND);
    assert(polygon==1 && projected.z==3.f);
    assert(find((struct sn_point){1,1,0.1f},(struct sn_point){1,1,3.1f},NULL,NULL)==SN_NO_PATH);
    rectangle(1,4,0,0,2,2);
    assert(build_resource(&first,2,3)==SN_READY);
    assert(find((struct sn_point){1,1,0.1f},(struct sn_point){5,1,0.1f},NULL,NULL)==SN_NO_PATH);
}
static void test_clearance(void)
{
    rectangle(0,0,0,0,2,0.3f);
    /* Even optimistic cooked width cannot override physical boundary clearance. */
    assert(build_resource(&first,1,4)==SN_READY);
    assert(find((struct sn_point){1,0.15f,0},(struct sn_point){1.5f,0.15f,0},NULL,NULL)==SN_BAD_START);
    rectangle(0,0,0,0,2,2); source[0].height=0.5f;
    assert(build_resource(&first,1,4)==SN_READY && first.polygon_count==0);
}
struct blockage { bool blocked; };
static bool clear_segment(void *context,struct sn_point a,uint32_t ap,struct sn_point b,uint32_t bp)
{
    struct blockage *state=context;
    (void)a;(void)b;
    return !state->blocked || (ap!=1 && bp!=1);
}
static void test_obstacles_and_revisions(void)
{
    struct blockage obstacle={true};
    struct sn_point start={1,1,0},goal={3,1,0};
    rectangle(0,0,0,0,2,2); rectangle(1,2,0,0,2,2);
    source[0].neighbors[1]=1; source[1].neighbors[3]=0;
    assert(build_resource(&first,2,5)==SN_READY);
    assert(find(start,goal,clear_segment,&obstacle)==SN_NO_PATH);
    obstacle.blocked=false; /* Deleted/moved obstacle: collision adapter no longer blocks it. */
    assert(find(start,goal,clear_segment,&obstacle)==SN_FOUND);
    assert(sn_route_current(&route,&first,1));
    assert(!sn_route_current(&route,&first,2));
    assert(sn_query_step(&query,&first,2,1,NULL,NULL)==SN_STALE);
    first.revision++;
    assert(!sn_route_current(&route,&first,1));
    first.revision--;
    first.key.bsp_index=1; /* Campaign BSP change, even if caller reused a revision. */
    assert(!sn_route_current(&route,&first,1));
    first.key=key; first.key.overlay_revision=1;
    assert(!sn_route_current(&route,&first,1));
    first.key=key; first.key.agent_identity++;
    assert(!sn_route_current(&route,&first,1));
    first.key=key; first.key.checksum++;
    assert(!sn_route_current(&route,&first,1));
    first.key=key; first.ready=false;
    assert(!sn_route_current(&route,&first,1));
    first.ready=true;
}
static void test_unknown_clearance(void)
{
    struct blockage obstacle={false};
    rectangle(0,0,0,0,2,2);
    source[0].width=source[0].height=0.f;
    assert(build_resource(&first,1,6)==SN_READY && first.requires_clearance);
    assert(find((struct sn_point){1,1,0},(struct sn_point){1.5f,1,0},NULL,NULL)==SN_INVALID);
    assert(find((struct sn_point){1,1,0},(struct sn_point){1.5f,1,0},clear_segment,&obstacle)==SN_FOUND);
    assert(sn_query_begin(&query,&first,(struct sn_point){1,1,0},(struct sn_point){1.5f,1,0},1)==SN_RUNNING);
    assert(sn_query_step(&query,&first,1,0,clear_segment,&obstacle)==SN_INVALID);
}
static int chain_polygon(void *context,uint32_t index,struct sn_polygon *out)
{
    uint32_t count=*(uint32_t *)context;
    rectangle(0,2.f*(float)index,0,0,2,2);
    *out=source[0];
    out->neighbors[1]=index+1<count ? index+1 : SN_NONE;
    out->neighbors[3]=index>0 ? index-1 : SN_NONE;
    return 1;
}
static void test_route_capacity(void)
{
    uint32_t count;
    for (count=SN_ROUTE_POLYGONS_MAX;count<=SN_ROUTE_POLYGONS_MAX+1;count++) {
        enum sn_status status=sn_build_begin(&build,&first,count,key,7,agent);
        uint32_t calls=0;
        while(status==SN_RUNNING && calls++<100)
            status=sn_build_step(&build,&first,256,chain_polygon,&count);
        assert(status==SN_READY);
        status=find((struct sn_point){1,1,0},(struct sn_point){2.f*(float)count-1.f,1,0},NULL,NULL);
        if(count==SN_ROUTE_POLYGONS_MAX) {
            assert(status==SN_FOUND && route.count==SN_WAYPOINTS_MAX);
        } else {
            assert(status==SN_CAPACITY && route.count==0);
        }
    }
}
static int many_polygons(void *context,uint32_t index,struct sn_polygon *out)
{
    (void)context;(void)index;
    *out=source[0];
    return 1;
}
static void test_malformed_and_capacity(void)
{
    enum sn_status status;
    uint32_t calls=0;
    rectangle(0,0,0,0,2,2); source[0].neighbors[1]=99;
    assert(build_resource(&second,1,6)==SN_INVALID && !second.ready);
    rectangle(0,0,0,0,2,2); source[0].vertices[0].x=NAN;
    assert(build_resource(&second,1,6)==SN_INVALID);
    rectangle(0,0,0,0,2,2); source[0].vertex_count=9;
    assert(build_resource(&second,1,6)==SN_INVALID);
    rectangle(0,0,0,0,2,2);
    assert(sn_build_begin(&build,&second,SN_SOURCE_MAX+1,key,6,agent)==SN_CAPACITY);
    status=sn_build_begin(&build,&second,SN_POLYGONS_MAX+1,key,6,agent);
    while(status==SN_RUNNING && calls++<2000)
        status=sn_build_step(&build,&second,256,many_polygons,NULL);
    assert(status==SN_CAPACITY && !second.ready);
    /* A failed staging build never changes a separate previously published resource. */
    assert(first.ready);
}
int main(void)
{
    test_floor_and_ramp(); test_stacked_and_disconnected(); test_clearance();
    test_obstacles_and_revisions(); test_unknown_clearance();
    test_route_capacity(); test_malformed_and_capacity();
    printf("surface navigation passed; resource=%zu build=%zu query=%zu route=%zu bytes\n",
        sizeof(first),sizeof(build),sizeof(query),sizeof(route));
    return 0;
}
