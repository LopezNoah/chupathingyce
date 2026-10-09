#ifndef HALO_NAVIGATION_WORLD_H
#define HALO_NAVIGATION_WORLD_H
#include "engine_ai/surface_navigation.h"
/* Derived, host-local data. Reset before BSP/map/game-state storage changes. */
void navigation_world_reset(void);
void navigation_world_update_for_players(void);
const struct sn_resource *navigation_world_resource(void);
uint64_t navigation_world_obstacle_revision(void);
/* context points to the unit datum index to ignore, not to an ECS entity. */
bool navigation_world_segment_clear(void *context, struct sn_point a, uint32_t ap,
    struct sn_point b, uint32_t bp);
void navigation_world_block(uint32_t polygon);
#endif
