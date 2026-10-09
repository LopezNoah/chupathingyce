/*
FORGE.H

Forge, the map editor in the game (forge.c): a free camera over a campaign
level or a multiplayer map, objects picked, moved, turned, placed and
deleted, with undo, and the edits saved beside the maps (forge\<map>.forge.json)
and put back on the map each time it loads. The map's own file is never
changed.

The 64-bit desktop builds have it (HALO_FORGE); the 32-bit builds compile
none of the game's calls into it, so their game code is as it was
(tools/port_neutrality_check.py).
*/

#ifndef FORGE_H
#define FORGE_H

#include "cseries.h"
#include <stdint.h>

#if defined(HALO_64BIT) && !defined(HALO_SERVER) && !defined(HALO_ANDROID)
#define HALO_FORGE 1
#endif

struct scenario_object_datum;

/* every frame, before the director (main.c): the toggle key, and while
Forge is open its camera and editing */
void forge_update(real seconds);

/* whether Forge is open */
boolean forge_active(void);

/* Host-local derived-navigation invalidation, never serialized into map tags. */
uint64_t forge_navigation_revision(void);

/* the director's camera for the local player while Forge is open
(director.c): TRUE and the camera's proc if Forge has the view; FALSE
otherwise. *reset becomes TRUE once, the frame after Forge closes, for the
game's camera to be chosen again. */
boolean forge_director_camera(short local_player_index, void **camera_proc, boolean *reset);

/* a new map (editor_initialize_for_new_map, before its objects are
placed): its saved edits read and written into the scenario's placements */
void forge_initialize_for_new_map(void);
/* the map left (editor_dispose_from_old_map) */
void forge_dispose_from_old_map(void);
/* the map's objects placed (game.c): the objects Forge added are made */
void forge_objects_placed(void);
/* a structure BSP switched to (object_types.c): the objects Forge added in
it are made again */
void forge_structure_bsp_reconnected(void);
/* an object made from a scenario placement (objects.c): which it is, so
that it can be edited */
void forge_object_placed_from_scenario(long object_index, struct scenario_object_datum *scenario_object);

/* the selection, and what Forge shows over the view (editor_render) */
void forge_render(void);

#endif
