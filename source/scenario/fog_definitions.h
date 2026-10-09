/*
FOG_DEFINITIONS.H

header included in hcex build.
*/

#ifndef __FOG_DEFINITIONS_H
#define __FOG_DEFINITIONS_H
#pragma once

/* ---------- headers */

#include <stdint.h>

#include "cseries/cseries.h"
#include "tag_files/tag_groups.h"

/* ---------- constants */

enum
{
	FOG_TAG = 'fog ',
};

/* ---------- macros */

#define fog_definition_get(index) ((struct fog_definition *)tag_get(FOG_TAG, (index)))

/* ---------- structures */

struct fog_definition
{
	byte flags;
	byte unused1[0x73];
	real plane_distance;
	byte unused78[0x88];
	int32_t background_sound_index;
	struct tag_reference sound_environment;
};

#ifdef HALO_GAME_ABI_LAYOUT
typedef char fog_definition_size_assert[
	sizeof(struct fog_definition) == 0x114 ? 1 : -1];
typedef char fog_definition_plane_distance_offset_assert[
	offsetof(struct fog_definition, plane_distance) == 0x74 ? 1 : -1];
typedef char fog_definition_background_sound_offset_assert[
	offsetof(struct fog_definition, background_sound_index) == 0x100 ? 1 : -1];
typedef char fog_definition_sound_environment_offset_assert[
	offsetof(struct fog_definition, sound_environment) == 0x104 ? 1 : -1];
#endif /* HALO_GAME_ABI_LAYOUT */

/* ---------- prototypes/EXAMPLE.C */

/* ---------- globals */

/* ---------- public code */

#endif // __FOG_DEFINITIONS_H
