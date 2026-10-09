/*
FORGE.C

Forge, the map editor in the game (forge.h). forge.toggle_key (F7 or B by
default) opens it in a game on this machine: a campaign level, or a
multiplayer game nobody else is in. Simulation and networking keep running;
the keyboard and mouse fly a free camera (halo_forge_input.h) and edit the
map's objects:

	mouse              look                 W A S D        fly
	Space / C          up / down            Shift / Alt    faster / slower
	left button        select (or drop)     right button/E grab or drop the selection
	wheel              nearer or further while carrying, else the item to place
	Q / R              turn left / right    arrows, PgUp/PgDn  nudge
	[ / ]              item to place        P              place it at the crosshair
	V                  duplicate            Delete         delete
	G                  snapping             H              help
	Ctrl+Z / Ctrl+Y    undo / redo          Ctrl+S         save
	Escape             put back what is carried, or deselect

What can be edited is what the map places: the scenario's placements of
scenery, vehicles, weapons, equipment, machines, controls, light fixtures and
sound scenery (moved, turned or deleted), and objects Forge adds from the
map's palettes. The players, their weapons and what the game engine or the
scripts make are not the map's, and are left as they are.

Edits never change the map's file: tag blocks cannot grow while a cache file
is loaded (cache_files.c), and the map stays the one other players have.
They are kept in d:\forge\<map>.forge.json (beside the maps), keyed by the
map's name and its cache file's checksum, and each time the map loads they
are written into its scenario's placements before its objects are placed
(forge_initialize_for_new_map), and the objects Forge added are made after
(forge_objects_placed). An edit to a placement is its new transform, or its
deletion (the placement made not automatic, so that the map never places
it: a script may still make it by name); an addition is a palette's tag by
name, with its transform and its structure BSP.

Not yet: editing in a game with other players (they would need the same
edits), the multiplayer spawn points and game type flags, the campaign's
encounters and trigger volumes, and the structure itself.
*/

#include "cseries.h"
#include "forge.h"
#include "extensions/extension_api.h"

/* port neutrality: the 32-bit builds, and those configured --no-forge,
compile an empty unit (the platform layer asks the extensions, not Forge,
whether the input is captured: halo_extensions_input_captured) */
#ifndef HALO_FORGE

#ifdef HALO_FEATURE_FORGE
/* The feature is selected but this target has no editor implementation. */
struct halo_extension const forge_extension = { .name = "forge" };
#else
typedef int forge_compiled_out;
#endif

#else

/* CreateDirectoryA's port declaration is needed by overlay saves. */
#include "cseries/cseries_windows.h" // IWYU pragma: keep
#include "camera/flying_camera.h"
#include "camera/observer.h"
#include "camera/static_camera.h"
#include "cache/cache_files.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "math/real_math.h"
#include "objects/object_types.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "units/bipeds.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_debug.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"

#include "halo_forge_input.h"

/* Used by included camera/character handoff helpers. */
#include <math.h> // IWYU pragma: keep
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
double config_real(char const *name);
/* Geometry edits invalidate host-local derived navigation, including undo/redo. */
static uint64_t forge_nav_revision;
uint64_t forge_navigation_revision(void) { return forge_nav_revision; }

/* ---------- constants */

enum
{
	FORGE_MAXIMUM_PLACEMENT_EDITS = 2048,
	FORGE_MAXIMUM_ADDITIONS = 1024,
	FORGE_MAXIMUM_COMMANDS = 1024,
	FORGE_MAXIMUM_PALETTE = 512,
	FORGE_TAG_NAME_LENGTH = 256,
	FORGE_MAP_NAME_LENGTH = 64,
	FORGE_MAXIMUM_FILE_SIZE = 4 * 1024 * 1024,
	FORGE_MESSAGE_LENGTH = 160,
	FORGE_FORMAT_VERSION = 1,
	FORGE_MAXIMUM_WORLD_EDITS = 128,
};

enum forge_target_kind
{
	_forge_target_none = 0,
	_forge_target_placement,
	_forge_target_addition,
	_forge_target_world,
};

/* world units (one is about 3 m) and radians */
#define FORGE_FLY_SPEED 4.0f
#define FORGE_FAST_SCALE 4.0f
#define FORGE_SLOW_SCALE 0.25f
#define FORGE_LOOK_SCALE 0.0022f
#define FORGE_REACH 300.0f
#define FORGE_SNAP_DISTANCE 0.1f
#define FORGE_SNAP_ANGLE (_pi / 12.0f)
#define FORGE_TURN_SPEED (_pi / 2.0f)
#define FORGE_NUDGE 0.05f
#define FORGE_NUDGE_FAST 0.5f
#define FORGE_CARRY_MINIMUM 0.5f
#define FORGE_CARRY_WHEEL 0.5f
#define FORGE_PLACE_DISTANCE 3.0f
#define FORGE_MESSAGE_SECONDS 4.0f
#define FORGE_PITCH_LIMIT 1.56765485f
#define FORGE_POSITION_LIMIT 4900.0f

#define FORGE_DIRECTORY "d:\\forge"

/* ---------- structures */

struct forge_transform
{
	real_point3d position;
	real_euler_angles3d rotation;
	boolean deleted;
};

struct forge_target
{
	short kind;
	short type;
	/* the placement's index in its type's block, or the addition's slot */
	long index;
};

/* a placement Forge changed: as the map has it, and as it is now */
struct forge_placement_edit
{
	short type;
	short index;
	word original_placement_flags;
	word original_on_bsp_flags;
	struct forge_transform original;
	struct forge_transform current;
};

/* an object Forge added (deleted ones are kept, for undo) */
struct forge_addition
{
	long id;
	short type;
	short bsp_index;
	long definition_index;
	char tag_name[FORGE_TAG_NAME_LENGTH];
	struct forge_transform current;
	long object_index;
};

struct forge_command
{
	struct forge_target target;
	struct forge_transform before;
	struct forge_transform after;
};

/* an object a scenario placement made (forge_object_placed_from_scenario),
by the object's absolute index */
struct forge_world_loaded
{
	struct forge_target target;
	struct forge_transform state;
	boolean applied;
};

struct forge_source
{
	long object_index;
	short type;
	short index;
};

struct forge_palette_entry
{
	short type;
	long definition_index;
};

/* ---------- globals */

static struct
{
	/* the map the edits are for */
	boolean map_loaded;
	char map_name[FORGE_MAP_NAME_LENGTH];
	unsigned long checksum;

	struct forge_placement_edit placements[FORGE_MAXIMUM_PLACEMENT_EDITS];
	long placement_count;
	struct forge_addition additions[FORGE_MAXIMUM_ADDITIONS];
	long addition_count;
	long next_addition_id;
	struct forge_world_loaded world_loaded[FORGE_MAXIMUM_WORLD_EDITS];
	long world_loaded_count;

	struct forge_command commands[FORGE_MAXIMUM_COMMANDS];
	long command_count;
	long command_position;
	/* the command position the file has, or NONE if no position does */
	long saved_position;

	struct forge_palette_entry palette[FORGE_MAXIMUM_PALETTE];
	long palette_count;
	long palette_index;

	/* open */
	boolean active;
	short local_player_index;
	boolean camera_reset[MAXIMUM_LOCAL_PLAYERS];
	struct flying_camera camera;
	struct halo_forge_input input;
	struct halo_forge_input previous_input;

	/* what the crosshair is on */
	boolean hover_hit;
	real_point3d hover_point;
	long hover_object_index;
	struct forge_target hover_world;

	struct forge_target selected;
	boolean carrying;
	real carry_distance;
	struct forge_transform carry_before;
	boolean turning;
	short rotation_axis; /* Euler slot: 0 yaw/Z, 1 pitch/Y, 2 roll/X */

	boolean snap;
	boolean help;
	char message[FORGE_MESSAGE_LENGTH];
	real message_seconds;
} forge;

static struct forge_source forge_sources[MAXIMUM_OBJECTS_PER_MAP];

/* ---------- private code: objects */

static boolean forge_object_alive(
	long object_index)
{
	struct object_header_datum *header;

	if (object_index == NONE || !object_try_and_get(object_index))
		return FALSE;
	header = object_header_get(object_index);
	return !TEST_FLAG(header->flags, _object_header_being_deleted_bit);
}

static boolean forge_type_has_placements(
	short type)
{
	struct object_type_definition *definition;

	if (type < 0 || type >= NUMBER_OF_OBJECT_TYPES)
		return FALSE;
	definition = object_type_definition_get(type);
	return definition->placement_tag_block_offset != NONE && definition->palette_tag_block_offset != NONE;
}

static struct scenario_object_datum *forge_placement_datum(
	short type,
	long index,
	struct tag_block **palette)
{
	struct scenario *scenario = global_scenario_get();
	struct tag_block *datums;
	long element_size;

	if (!scenario || !forge_type_has_placements(type))
		return NULL;
	datums = scenario_get_object_type_scenario_datums(scenario, type, &element_size);
	if (index < 0 || index >= datums->count)
		return NULL;
	if (palette)
		*palette = scenario_get_object_type_scenario_palette(scenario, type);
	return (struct scenario_object_datum *)tag_block_get_element_with_size(datums, index, element_size);
}

static long forge_placement_object(
	short type,
	long index)
{
	long absolute_index;

	for (absolute_index = 0; absolute_index < MAXIMUM_OBJECTS_PER_MAP; absolute_index++)
	{
		struct forge_source const *source = &forge_sources[absolute_index];

		if (source->object_index != NONE && source->type == type && source->index == index &&
			forge_object_alive(source->object_index))
		{
			return source->object_index;
		}
	}
	return NONE;
}

static boolean forge_placement_of_object(
	long object_index,
	short *type,
	long *index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	struct forge_source const *source;

	if (object_index == NONE || absolute_index >= MAXIMUM_OBJECTS_PER_MAP)
		return FALSE;
	source = &forge_sources[absolute_index];
	if (source->object_index != object_index)
		return FALSE;
	*type = source->type;
	*index = source->index;
	return TRUE;
}

static void forge_transform_vectors(
	struct forge_transform const *transform,
	real_vector3d *forward,
	real_vector3d *up)
{
	vectors3d_from_euler_angles3d(forward, up, &transform->rotation);
}

static void forge_object_move(
	long object_index,
	struct forge_transform const *transform)
{
	struct object_datum *object = object_get(object_index);
	real_vector3d forward, up;

	forge_transform_vectors(transform, &forward, &up);
	object->object.translational_velocity = *global_zero_vector3d;
	object->object.angular_velocity = *global_zero_vector3d;
	object_set_position(object_index, &transform->position, &forward, &up);
	object_compute_node_matrices_recursive(object_index);
}

static long forge_object_new(
	long definition_index,
	struct forge_transform const *transform)
{
	struct object_placement_data data;

	if (definition_index == NONE)
		return NONE;
	object_placement_data_new(&data, definition_index, NONE);
	data.position = transform->position;
	forge_transform_vectors(transform, &data.forward, &data.up);
	forge_nav_revision++;
	return object_new(&data);
}

/* ---------- private code: the edits */

static struct forge_placement_edit *forge_placement_edit_find(
	short type,
	long index)
{
	long edit_index;

	for (edit_index = 0; edit_index < forge.placement_count; edit_index++)
	{
		struct forge_placement_edit *edit = &forge.placements[edit_index];

		if (edit->type == type && edit->index == index)
			return edit;
	}
	return NULL;
}

/* the edit of a placement, begun from what the map has if there is none */
static struct forge_placement_edit *forge_placement_edit_get(
	short type,
	long index)
{
	struct forge_placement_edit *edit = forge_placement_edit_find(type, index);
	struct scenario_object_datum *datum;

	if (edit)
		return edit;
	datum = forge_placement_datum(type, index, NULL);
	if (!datum || forge.placement_count >= FORGE_MAXIMUM_PLACEMENT_EDITS)
		return NULL;
	edit = &forge.placements[forge.placement_count++];
	csmemset(edit, 0, sizeof(*edit));
	edit->type = type;
	edit->index = (short)index;
	edit->original_placement_flags = datum->placement_flags;
	edit->original_on_bsp_flags = datum->on_bsp_flags;
	edit->original.position = datum->position;
	edit->original.rotation = datum->rotation;
	edit->original.deleted = FALSE;
	edit->current = edit->original;
	return edit;
}

/* the placement as the edit has it, in the scenario */
static void forge_placement_write(
	struct forge_placement_edit const *edit)
{
	struct scenario_object_datum *datum = forge_placement_datum(edit->type, edit->index, NULL);

	if (!datum)
		return;
	datum->position = edit->current.position;
	datum->rotation = edit->current.rotation;
	datum->placement_flags = edit->original_placement_flags;
	datum->on_bsp_flags = edit->original_on_bsp_flags;
	if (edit->current.deleted)
	{
		SET_FLAG(datum->placement_flags, _scenario_object_placement_not_automatic_bit, TRUE);
	}
	else if (global_structure_bsp_index != NONE &&
		(edit->current.position.x != edit->original.position.x ||
		edit->current.position.y != edit->original.position.y ||
		edit->current.position.z != edit->original.position.z))
	{
		/* (moved: placed again with the structure BSP it was moved in) */
		datum->on_bsp_flags |= FLAG(global_structure_bsp_index);
	}
}

/* the placements as the map has them (leaving a map, or reading its edits
again) */
static void forge_placements_restore(
	void)
{
	long edit_index;

	for (edit_index = 0; edit_index < forge.placement_count; edit_index++)
	{
		struct forge_placement_edit edit = forge.placements[edit_index];

		edit.current = edit.original;
		forge_placement_write(&edit);
	}
}

static void forge_message(
	char const *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(forge.message, sizeof(forge.message), format, arguments);
	va_end(arguments);
	forge.message_seconds = FORGE_MESSAGE_SECONDS;
	platform_log("forge: %s", forge.message);
}

static struct forge_addition *forge_addition_get(
	long slot)
{
	return slot >= 0 && slot < forge.addition_count ? &forge.additions[slot] : NULL;
}

static boolean forge_addition_spawns_here(
	struct forge_addition const *addition)
{
	/* (the structure BSP's own objects are made again for each; the rest
	stay through a switch) */
	return !TEST_FLAG(_object_mask_remove_on_bsp_switch, addition->type) ||
		addition->bsp_index == NONE || addition->bsp_index == global_structure_bsp_index;
}

static void forge_addition_spawn(
	struct forge_addition *addition)
{
	if (addition->current.deleted || forge_object_alive(addition->object_index) ||
		!forge_addition_spawns_here(addition))
	{
		return;
	}
	addition->object_index = forge_object_new(addition->definition_index, &addition->current);
	if (addition->object_index == NONE)
		platform_log("forge: could not make %s", addition->tag_name);
}

static long forge_target_object(
	struct forge_target const *target)
{
	switch (target->kind)
	{
	case _forge_target_placement:
		return forge_placement_object(target->type, target->index);
	case _forge_target_addition:
	{
		struct forge_addition *addition = forge_addition_get(target->index);

		return addition && forge_object_alive(addition->object_index) ? addition->object_index : NONE;
	}
	}
	return NONE;
}

static boolean forge_target_of_object(long object_index, struct forge_target *target);
#include "forge_world.inc"

static boolean forge_target_state(
	struct forge_target const *target,
	struct forge_transform *state)
{
	switch (target->kind)
	{
	case _forge_target_world:
		return forge_world_get(target, state);
	case _forge_target_placement:
	{
		struct forge_placement_edit *edit = forge_placement_edit_find(target->type, (short)target->index);
		struct scenario_object_datum *datum;

		if (edit)
		{
			*state = edit->current;
			return TRUE;
		}
		datum = forge_placement_datum(target->type, target->index, NULL);
		if (!datum)
			return FALSE;
		state->position = datum->position;
		state->rotation = datum->rotation;
		state->deleted = FALSE;
		return TRUE;
	}
	case _forge_target_addition:
	{
		struct forge_addition *addition = forge_addition_get(target->index);

		if (!addition)
			return FALSE;
		*state = addition->current;
		return TRUE;
	}
	}
	return FALSE;
}

/* the target made as the state has it: in the edits, in the scenario, and
in the world */
static void forge_target_apply(
	struct forge_target const *target,
	struct forge_transform const *state)
{
	long object_index = forge_target_object(target);

	forge_nav_revision++;
	switch (target->kind)
	{
	case _forge_target_world:
		if (!forge_world_set(target, state)) forge_message("GLB edit rejected: invalid bounds or full palette");
		break;
	case _forge_target_placement:
	{
		struct forge_placement_edit *edit = forge_placement_edit_get(target->type, target->index);
		struct tag_block *palette;
		struct scenario_object_datum *datum;

		if (!edit)
		{
			forge_message("Too many placements edited");
			return;
		}
		edit->current = *state;
		forge_placement_write(edit);
		if (state->deleted)
		{
			if (object_index != NONE)
				object_delete(object_index);
		}
		else if (object_index != NONE)
		{
			forge_object_move(object_index, state);
		}
		else if ((datum = forge_placement_datum(target->type, target->index, &palette)) != NULL)
		{
			object_new_from_scenario(datum, palette);
		}
		break;
	}
	case _forge_target_addition:
	{
		struct forge_addition *addition = forge_addition_get(target->index);

		if (!addition)
			return;
		addition->current = *state;
		if (state->deleted)
		{
			if (object_index != NONE)
				object_delete(object_index);
			addition->object_index = NONE;
		}
		else if (object_index != NONE)
		{
			forge_object_move(object_index, state);
		}
		else
		{
			forge_addition_spawn(addition);
		}
		break;
	}
	}
}

static boolean forge_transforms_equal(
	struct forge_transform const *a,
	struct forge_transform const *b)
{
	return a->deleted == b->deleted &&
		a->position.x == b->position.x && a->position.y == b->position.y && a->position.z == b->position.z &&
		a->rotation.yaw == b->rotation.yaw && a->rotation.pitch == b->rotation.pitch &&
		a->rotation.roll == b->rotation.roll;
}

/* an edit done (already applied): kept for undo, the ones undone dropped */
static void forge_command_push(
	struct forge_target const *target,
	struct forge_transform const *before,
	struct forge_transform const *after)
{
	struct forge_command *command;

	if (forge_transforms_equal(before, after))
		return;
	if (forge.saved_position > forge.command_position)
		forge.saved_position = NONE;
	forge.command_count = forge.command_position;
	if (forge.command_count >= FORGE_MAXIMUM_COMMANDS)
	{
		/* (the oldest forgotten) */
		memmove(&forge.commands[0], &forge.commands[1], (FORGE_MAXIMUM_COMMANDS - 1) * sizeof(forge.commands[0]));
		forge.command_count--;
		if (forge.saved_position != NONE)
			forge.saved_position = forge.saved_position > 0 ? forge.saved_position - 1 : NONE;
	}
	command = &forge.commands[forge.command_count++];
	command->target = *target;
	command->before = *before;
	command->after = *after;
	forge.command_position = forge.command_count;
}

static boolean forge_unsaved(
	void)
{
	return forge.saved_position != forge.command_position;
}

/* ---------- private code: the palette */

static void forge_palette_build(
	void)
{
	struct scenario *scenario = global_scenario_get();
	short type;

	forge.palette_count = 0;
	if (!scenario)
		return;
	for (type = 0; type < NUMBER_OF_OBJECT_TYPES; type++)
	{
		struct tag_block *palette;
		long entry_index;

		if (type == _object_type_biped || !forge_type_has_placements(type))
			continue;
		palette = scenario_get_object_type_scenario_palette(scenario, type);
		for (entry_index = 0; entry_index < palette->count; entry_index++)
		{
			long definition_index = TAG_BLOCK_GET_ELEMENT(palette, entry_index,
				struct scenario_object_palette_entry)->reference.index;
			long existing;

			if (definition_index == NONE)
				continue;
			for (existing = 0; existing < forge.palette_count; existing++)
			{
				if (forge.palette[existing].definition_index == definition_index)
					break;
			}
			if (existing < forge.palette_count || forge.palette_count >= FORGE_MAXIMUM_PALETTE)
				continue;
			forge.palette[forge.palette_count].type = type;
			forge.palette[forge.palette_count].definition_index = definition_index;
			forge.palette_count++;
		}
	}
	{
		long provider;
		for (provider = 0; provider < halo_extensions_count() && forge.palette_count < FORGE_MAXIMUM_PALETTE; provider++) {
			struct halo_world_geometry const *geometry = halo_extensions_get(provider)->world_geometry;
			if (!geometry || !geometry->edit_count || !geometry->edit_set || geometry->edit_count() == 0) continue;
			forge.palette[forge.palette_count].type = NONE;
			forge.palette[forge.palette_count++].definition_index = provider;
		}
	}
	if (forge.palette_index >= forge.palette_count)
		forge.palette_index = 0;
}

static char const *forge_tag_short_name(
	long definition_index)
{
	char const *name = definition_index != NONE ? tag_get_name(definition_index) : NULL;

	return name ? tag_name_strip_path(name) : "?";
}

static long forge_addition_new(
	short type,
	long definition_index,
	struct forge_transform const *transform)
{
	struct forge_addition *addition;
	char const *name = tag_get_name(definition_index);

	if (forge.addition_count >= FORGE_MAXIMUM_ADDITIONS)
	{
		forge_message("Too many objects added");
		return NONE;
	}
	addition = &forge.additions[forge.addition_count];
	csmemset(addition, 0, sizeof(*addition));
	addition->id = ++forge.next_addition_id;
	addition->type = type;
	addition->bsp_index = global_structure_bsp_index;
	addition->definition_index = definition_index;
	csstrncpy(addition->tag_name, name ? name : "", sizeof(addition->tag_name) - 1);
	addition->current = *transform;
	addition->current.deleted = TRUE;
	addition->object_index = NONE;
	return forge.addition_count++;
}

/* ---------- private code: the file */

static void forge_file_path(
	char *path,
	long size)
{
	char name[FORGE_MAP_NAME_LENGTH];
	long index;

	csstrncpy(name, forge.map_name, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	for (index = 0; name[index]; index++)
	{
		char c = name[index];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '@' || c == '.'))
		{
			name[index] = '_';
		}
	}
	snprintf(path, (size_t)size, "%s\\%s.forge.json", FORGE_DIRECTORY, name);
}

static short forge_type_from_name(
	char const *name)
{
	short type;

	for (type = 0; type < NUMBER_OF_OBJECT_TYPES; type++)
	{
		if (!strcmp(object_type_get_name(type), name))
			return type;
	}
	return NONE;
}

static void forge_write_string(
	FILE *file,
	char const *string)
{
	fputc('"', file);
	for (; *string; string++)
	{
		unsigned char c = (unsigned char)*string;

		if (c == '"' || c == '\\')
			fprintf(file, "\\%c", c);
		else if (c < 0x20)
			fprintf(file, "\\u%04x", c);
		else
			fputc(c, file);
	}
	fputc('"', file);
}

static void forge_write_transform(
	FILE *file,
	struct forge_transform const *transform)
{
	fprintf(file, "\"position\": [%.6f, %.6f, %.6f], \"rotation\": [%.6f, %.6f, %.6f]",
		transform->position.x, transform->position.y, transform->position.z,
		transform->rotation.yaw, transform->rotation.pitch, transform->rotation.roll);
}

static void forge_world_save(FILE *file);
static void forge_world_apply_loaded(void);

static boolean forge_save(
	void)
{
	char path[256];
	char temporary_path[260];
	struct scenario *scenario = global_scenario_get();
	FILE *file;
	long index;
	boolean first;
	char const *scenario_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;

	forge_file_path(path, sizeof(path));
	snprintf(temporary_path, sizeof(temporary_path), "%s.new", path);
	CreateDirectoryA(FORGE_DIRECTORY, NULL);
	file = fopen(temporary_path, "w");
	if (!file)
	{
		forge_message("Could not write %s", path);
		return FALSE;
	}
	fprintf(file, "{\n  \"format\": \"chupathingyce-forge\",\n  \"version\": %d,\n  \"map\": ", FORGE_FORMAT_VERSION);
	forge_write_string(file, forge.map_name);
	fprintf(file, ",\n  \"scenario\": ");
	forge_write_string(file, scenario_name ? scenario_name : "");
	fprintf(file, ",\n  \"scenario_type\": \"%s\",\n  \"checksum\": %lu,\n  \"placements\": [",
		scenario && scenario->type == _scenario_type_solo ? "solo" :
			scenario && scenario->type == _scenario_type_multiplayer ? "multiplayer" : "main_menu",
		forge.checksum);
	first = TRUE;
	for (index = 0; index < forge.placement_count; index++)
	{
		struct forge_placement_edit const *edit = &forge.placements[index];

		if (forge_transforms_equal(&edit->current, &edit->original))
			continue;
		fprintf(file, "%s\n    {\"type\": \"%s\", \"index\": %d, \"deleted\": %s, ", first ? "" : ",",
			object_type_get_name(edit->type), edit->index, edit->current.deleted ? "true" : "false");
		forge_write_transform(file, &edit->current);
		fprintf(file, "}");
		first = FALSE;
	}
	fprintf(file, "%s],\n  \"additions\": [", first ? "" : "\n  ");
	first = TRUE;
	for (index = 0; index < forge.addition_count; index++)
	{
		struct forge_addition const *addition = &forge.additions[index];

		if (addition->current.deleted)
			continue;
		fprintf(file, "%s\n    {\"id\": %ld, \"type\": \"%s\", \"tag\": ", first ? "" : ",", addition->id,
			object_type_get_name(addition->type));
		forge_write_string(file, addition->tag_name);
		fprintf(file, ", \"bsp\": %d, ", addition->bsp_index);
		forge_write_transform(file, &addition->current);
		fprintf(file, "}");
		first = FALSE;
	}
	fprintf(file, "%s],\n  \"external\": [", first ? "" : "\n  ");
	forge_world_save(file);
	fprintf(file, "\n  ]\n}\n");
	if (fclose(file) != 0)
	{
		forge_message("Could not write %s", path);
		return FALSE;
	}
	remove(path);
	if (rename(temporary_path, path) != 0)
	{
		forge_message("Could not write %s", path);
		return FALSE;
	}
	forge.saved_position = forge.command_position;
	forge_message("Saved %s", path);
	return TRUE;
}

/* a JSON reader for the file Forge writes: values it does not know are
skipped */
struct forge_json
{
	char const *at;
	boolean error;
};

static void forge_json_space(
	struct forge_json *json)
{
	while (*json->at == ' ' || *json->at == '\t' || *json->at == '\n' || *json->at == '\r')
		json->at++;
}

static boolean forge_json_take(
	struct forge_json *json,
	char c)
{
	forge_json_space(json);
	if (*json->at != c)
		return FALSE;
	json->at++;
	return TRUE;
}

static void forge_json_expect(
	struct forge_json *json,
	char c)
{
	if (!forge_json_take(json, c))
		json->error = TRUE;
}

static void forge_json_string(
	struct forge_json *json,
	char *string,
	long size)
{
	long length = 0;

	forge_json_expect(json, '"');
	while (!json->error && *json->at && *json->at != '"')
	{
		char c = *json->at++;

		if (c == '\\')
		{
			c = *json->at++;
			switch (c)
			{
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
			{
				long digits;

				/* (only the ASCII ones Forge writes) */
				c = (char)strtol(json->at, NULL, 16);
				for (digits = 0; digits < 4 && *json->at; digits++)
					json->at++;
				break;
			}
			case 0:
				json->error = TRUE;
				return;
			}
		}
		if (length < size - 1)
			string[length++] = c;
	}
	if (size > 0)
		string[length] = 0;
	forge_json_expect(json, '"');
}

static double forge_json_number(
	struct forge_json *json)
{
	char *end;
	double value;

	forge_json_space(json);
	value = strtod(json->at, &end);
	if (end == json->at)
		json->error = TRUE;
	json->at = end;
	return value;
}

static boolean forge_json_boolean(
	struct forge_json *json)
{
	forge_json_space(json);
	if (!strncmp(json->at, "true", 4))
	{
		json->at += 4;
		return TRUE;
	}
	if (!strncmp(json->at, "false", 5))
	{
		json->at += 5;
		return FALSE;
	}
	json->error = TRUE;
	return FALSE;
}

static void forge_json_skip(
	struct forge_json *json)
{
	forge_json_space(json);
	switch (*json->at)
	{
	case '"':
	{
		char ignored[1];

		forge_json_string(json, ignored, 0);
		break;
	}
	case '{':
		json->at++;
		if (forge_json_take(json, '}'))
			break;
		do
		{
			char ignored[1];

			forge_json_string(json, ignored, 0);
			forge_json_expect(json, ':');
			forge_json_skip(json);
		} while (!json->error && forge_json_take(json, ','));
		forge_json_expect(json, '}');
		break;
	case '[':
		json->at++;
		if (forge_json_take(json, ']'))
			break;
		do
		{
			forge_json_skip(json);
		} while (!json->error && forge_json_take(json, ','));
		forge_json_expect(json, ']');
		break;
	case 't':
	case 'f':
		forge_json_boolean(json);
		break;
	case 'n':
		if (!strncmp(json->at, "null", 4))
			json->at += 4;
		else
			json->error = TRUE;
		break;
	default:
		forge_json_number(json);
		break;
	}
}

static void forge_json_triple(
	struct forge_json *json,
	real *values)
{
	long index;

	forge_json_expect(json, '[');
	for (index = 0; index < 3 && !json->error; index++)
	{
		if (index)
			forge_json_expect(json, ',');
		values[index] = (real)forge_json_number(json);
	}
	forge_json_expect(json, ']');
}

/* one placement or addition: its fields */
struct forge_json_entry
{
	char type[32];
	long index;
	long id;
	long bsp;
	char tag[FORGE_TAG_NAME_LENGTH];
	struct forge_transform transform;
};

static void forge_json_entry(
	struct forge_json *json,
	struct forge_json_entry *entry)
{
	csmemset(entry, 0, sizeof(*entry));
	entry->index = NONE;
	entry->bsp = NONE;
	forge_json_expect(json, '{');
	if (forge_json_take(json, '}'))
		return;
	do
	{
		char key[32];

		forge_json_string(json, key, sizeof(key));
		forge_json_expect(json, ':');
		if (json->error)
			return;
		if (!strcmp(key, "type"))
			forge_json_string(json, entry->type, sizeof(entry->type));
		else if (!strcmp(key, "index"))
			entry->index = (long)forge_json_number(json);
		else if (!strcmp(key, "id"))
			entry->id = (long)forge_json_number(json);
		else if (!strcmp(key, "bsp"))
			entry->bsp = (long)forge_json_number(json);
		else if (!strcmp(key, "tag"))
			forge_json_string(json, entry->tag, sizeof(entry->tag));
		else if (!strcmp(key, "deleted"))
			entry->transform.deleted = forge_json_boolean(json);
		else if (!strcmp(key, "position"))
			forge_json_triple(json, entry->transform.position.n);
		else if (!strcmp(key, "rotation"))
			forge_json_triple(json, entry->transform.rotation.n);
		else
			forge_json_skip(json);
	} while (!json->error && forge_json_take(json, ','));
	forge_json_expect(json, '}');
}

static boolean forge_transform_valid(
	struct forge_transform const *transform)
{
	long index;

	for (index = 0; index < 3; index++)
	{
		if (!valid_real(transform->position.n[index]) || fabsf(transform->position.n[index]) > FORGE_POSITION_LIMIT ||
			!valid_real(transform->rotation.n[index]))
		{
			return FALSE;
		}
	}
	return TRUE;
}

static void forge_json_placement(
	struct forge_json_entry const *entry)
{
	short type = forge_type_from_name(entry->type);
	struct forge_placement_edit *edit;

	if (type == NONE || !forge_placement_datum(type, entry->index, NULL) || !forge_transform_valid(&entry->transform))
	{
		platform_log("forge: no %s placement %ld on this map: left out", entry->type, entry->index);
		return;
	}
	edit = forge_placement_edit_get(type, entry->index);
	if (!edit)
		return;
	edit->current = entry->transform;
	forge_placement_write(edit);
}

static void forge_json_addition(
	struct forge_json_entry const *entry)
{
	short type = forge_type_from_name(entry->type);
	long definition_index;
	long slot;

	if (type == NONE || !forge_type_has_placements(type) || !forge_transform_valid(&entry->transform))
	{
		platform_log("forge: an addition of %s left out", entry->type);
		return;
	}
	definition_index = tag_loaded(object_type_definition_get(type)->group_tag, entry->tag);
	if (definition_index == NONE)
	{
		platform_log("forge: no %s %s on this map: left out", entry->type, entry->tag);
		return;
	}
	slot = forge_addition_new(type, definition_index, &entry->transform);
	if (slot == NONE)
		return;
	forge.additions[slot].current.deleted = FALSE;
	forge.additions[slot].bsp_index = (short)entry->bsp;
	if (entry->id > 0)
	{
		forge.additions[slot].id = entry->id;
		if (entry->id > forge.next_addition_id)
			forge.next_addition_id = entry->id;
	}
}

#include "forge_world_io.inc"

static void forge_load(
	void)
{
	char path[256];
	FILE *file;
	char *text;
	long size;
	struct forge_json json;
	unsigned long checksum = 0;
	boolean has_checksum = FALSE;
	char map_name[FORGE_MAP_NAME_LENGTH] = "";
	char const *placements_at = NULL;
	char const *additions_at = NULL;
	char const *external_at = NULL;

	forge_file_path(path, sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0 || size > FORGE_MAXIMUM_FILE_SIZE || !(text = malloc((size_t)size + 1)))
	{
		fclose(file);
		platform_log("forge: %s is empty or too big", path);
		return;
	}
	size = (long)fread(text, 1, (size_t)size, file);
	fclose(file);
	text[size] = 0;

	/* the header first, then the edits once the map is known to be theirs */
	json.at = text;
	json.error = FALSE;
	forge_json_expect(&json, '{');
	if (!forge_json_take(&json, '}'))
	{
		do
		{
			char key[32];

			forge_json_string(&json, key, sizeof(key));
			forge_json_expect(&json, ':');
			if (json.error)
				break;
			if (!strcmp(key, "map"))
				forge_json_string(&json, map_name, sizeof(map_name));
			else if (!strcmp(key, "checksum"))
			{
				checksum = (unsigned long)forge_json_number(&json);
				has_checksum = TRUE;
			}
			else
			{
				if (!strcmp(key, "placements"))
					placements_at = json.at;
				else if (!strcmp(key, "additions"))
					additions_at = json.at;
				else if (!strcmp(key, "external"))
					external_at = json.at;
				forge_json_skip(&json);
			}
		} while (!json.error && forge_json_take(&json, ','));
	}
	if (json.error)
	{
		platform_log("forge: %s is not a Forge file: left out", path);
		free(text);
		return;
	}
	if (strcmp(map_name, forge.map_name) || (has_checksum && checksum != forge.checksum))
	{
		forge_message("%s is for another version of %s: left out", path, forge.map_name);
		free(text);
		return;
	}
	if (placements_at)
	{
		json.at = placements_at;
		forge_json_expect(&json, '[');
		if (!forge_json_take(&json, ']'))
		{
			do
			{
				struct forge_json_entry entry;

				forge_json_entry(&json, &entry);
				if (!json.error)
					forge_json_placement(&entry);
			} while (!json.error && forge_json_take(&json, ','));
		}
	}
	if (additions_at && !json.error)
	{
		json.at = additions_at;
		forge_json_expect(&json, '[');
		if (!forge_json_take(&json, ']'))
		{
			do
			{
				struct forge_json_entry entry;

				forge_json_entry(&json, &entry);
				if (!json.error)
					forge_json_addition(&entry);
			} while (!json.error && forge_json_take(&json, ','));
		}
	}
	if (external_at && !json.error) {
		json.at = external_at;
		forge_world_load(&json);
	}
	if (json.error)
		platform_log("forge: %s is damaged: read as far as it could be", path);
	free(text);
	platform_log("forge: %s: %ld placements edited, %ld objects added", path, forge.placement_count,
		forge.addition_count);
}

/* ---------- private code: the camera */

static void forge_camera_vectors(
	real_vector3d *forward,
	real_vector3d *right)
{
	vector3d_from_euler_angles2d(forward, &forge.camera.facing);
	right->i = forward->j;
	right->j = -forward->i;
	right->k = 0.0f;
	if (normalize3d(right) == 0.0f)
	{
		right->i = 1.0f;
		right->j = 0.0f;
	}
}

/* the director's camera proc while Forge is open (forge_director_camera) */
static void forge_camera_update(
	void *data,
	void *controls,
	void *result)
{
	struct flying_camera_action action;
	struct camera_command *command = (struct camera_command *)result;

	(void)data;
	(void)controls;
	csmemset(&action, 0, sizeof(action));
	flying_camera_update(&forge.camera, &action, command);
	/* (straight to where it is: no easing behind the mouse) */
	command->timer = 0.0f;
}

/* ---------- private code: picking */

static real forge_ray_sphere(
	real_point3d const *origin,
	real_vector3d const *direction,
	real_point3d const *center,
	real radius)
{
	real_vector3d to_center;
	real along, distance_squared;

	vector_from_points3d(origin, center, &to_center);
	along = dot_product3d(&to_center, direction);
	distance_squared = dot_product3d(&to_center, &to_center) - along * along;
	if (along <= 0.0f || distance_squared > radius * radius)
		return -1.0f;
	/* (inside it: big scenery all round the camera is not what it looks at) */
	if (dot_product3d(&to_center, &to_center) < radius * radius)
		return -1.0f;
	return along - (real)sqrt(radius * radius - distance_squared);
}

static long forge_root_object(
	long object_index)
{
	while (object_index != NONE)
	{
		long parent_index = object_get(object_index)->object.parent_object_index;

		if (parent_index == NONE)
			break;
		object_index = parent_index;
	}
	return object_index;
}

static long forge_ignored_object(
	void)
{
	long player_index = local_player_get_player_index(forge.local_player_index);

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

static void forge_pick(
	void)
{
	real_vector3d forward, right, ray;
	struct collision_result collision;
	unsigned long flags =
		FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) |
		(_collision_test_objects_all_types_flags & ~FLAG(_collision_test_objects_projectiles_bit));
	long carried = forge.carrying ? forge_target_object(&forge.selected) : NONE;
	long ignored = carried != NONE ? carried : forge_ignored_object();
	real hit_distance = FORGE_REACH;
	struct object_iterator iterator;
	struct object_datum *object;

	forge_camera_vectors(&forward, &right);
	ray.i = forward.i * FORGE_REACH;
	ray.j = forward.j * FORGE_REACH;
	ray.k = forward.k * FORGE_REACH;
	forge.hover_hit = FALSE;
	forge.hover_object_index = NONE;
	csmemset(&forge.hover_world, 0, sizeof(forge.hover_world));
	if (carried != NONE)
	{
		/* (neither what is carried nor the player) */
		flags &= ~FLAG(_collision_test_objects_bit);
		flags &= ~_collision_test_objects_all_types_flags;
	}
	if (collision_test_vector(flags, &forge.camera.position, &ray, ignored, &collision))
	{
		forge.hover_hit = TRUE;
		forge.hover_point = collision.point;
		hit_distance = collision.t * FORGE_REACH;
		if (collision.type == _collision_result_object && collision.object_index != NONE)
			forge.hover_object_index = forge_root_object(collision.object_index);
	}
	forge_world_pick(&ray, hit_distance / FORGE_REACH);
	if (forge.hover_world.kind != _forge_target_none || forge.hover_object_index != NONE || carried != NONE)
		return;

	/* an object without collision (plants, sound scenery, small props): the
	nearest whose bounding sphere the line goes through, short of what the
	line hits */
	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real distance;

		if (iterator.index == ignored || object->object.parent_object_index != NONE ||
			object->object.type == _object_type_projectile ||
			!forge_object_alive(iterator.index))
		{
			continue;
		}
		distance = forge_ray_sphere(&forge.camera.position, &forward, &object->object.bounding_sphere_center,
			MAX(object->object.bounding_sphere_radius, 0.1f));
		if (distance >= 0.0f && distance < hit_distance)
		{
			hit_distance = distance;
			forge.hover_object_index = iterator.index;
		}
	}
}

static boolean forge_target_of_object(
	long object_index,
	struct forge_target *target)
{
	short type;
	long index;
	long slot;

	csmemset(target, 0, sizeof(*target));
	if (object_index == NONE)
		return FALSE;
	if (forge_placement_of_object(object_index, &type, &index))
	{
		target->kind = _forge_target_placement;
		target->type = type;
		target->index = index;
		return TRUE;
	}
	for (slot = 0; slot < forge.addition_count; slot++)
	{
		if (forge.additions[slot].object_index == object_index && !forge.additions[slot].current.deleted)
		{
			target->kind = _forge_target_addition;
			target->type = forge.additions[slot].type;
			target->index = slot;
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------- private code: editing */

static real forge_snap(
	real value,
	real step)
{
	return (real)floor(value / step + 0.5f) * step;
}

static void forge_snap_transform(
	struct forge_transform *transform)
{
	if (!forge.snap)
		return;
	transform->position.x = forge_snap(transform->position.x, FORGE_SNAP_DISTANCE);
	transform->position.y = forge_snap(transform->position.y, FORGE_SNAP_DISTANCE);
	transform->position.z = forge_snap(transform->position.z, FORGE_SNAP_DISTANCE);
	transform->rotation.yaw = forge_snap(transform->rotation.yaw, FORGE_SNAP_ANGLE);
	transform->rotation.pitch = forge_snap(transform->rotation.pitch, FORGE_SNAP_ANGLE);
	transform->rotation.roll = forge_snap(transform->rotation.roll, FORGE_SNAP_ANGLE);
}

static void forge_turn_end(void)
{
	struct forge_transform after;
	if (!forge.turning) return;
	forge.turning = FALSE;
	if (forge_target_state(&forge.selected, &after))
		forge_command_push(&forge.selected, &forge.carry_before, &after);
}

static void forge_select(
	struct forge_target const *target)
{
	forge_turn_end();
	forge.selected = *target;
	forge.carrying = FALSE;
}

static void forge_carry_begin(
	void)
{
	long object_index = forge_target_object(&forge.selected);
	real_vector3d forward, right;

	forge_turn_end();
	if ((object_index == NONE && forge.selected.kind != _forge_target_world) ||
		!forge_target_state(&forge.selected, &forge.carry_before))
		return;
	forge_camera_vectors(&forward, &right);
	forge.carry_distance = MAX(FORGE_CARRY_MINIMUM,
		distance3d(&forge.camera.position, &forge.carry_before.position));
	forge.carrying = TRUE;
}

static void forge_carry_end(
	boolean keep)
{
	struct forge_transform after;

	forge_turn_end();
	if (!forge.carrying)
		return;
	forge.carrying = FALSE;
	if (!forge_target_state(&forge.selected, &after))
		return;
	if (keep)
	{
		forge_command_push(&forge.selected, &forge.carry_before, &after);
	}
	else
	{
		forge_target_apply(&forge.selected, &forge.carry_before);
	}
}

/* a one-step change of the selection (a nudge, a turn, a deletion) */
static void forge_change(
	struct forge_transform const *after)
{
	struct forge_transform before, applied;

	forge_turn_end();
	if (!forge_target_state(&forge.selected, &before))
		return;
	forge_target_apply(&forge.selected, after);
	/* Providers may reject an out-of-world pose. Record only the actual
	 * state so undo never claims an edit that did not happen. */
	if (forge_target_state(&forge.selected, &applied))
		forge_command_push(&forge.selected, &before, &applied);
}

static void forge_place(
	short type,
	long definition_index,
	struct forge_transform const *transform,
	boolean carry)
{
	long slot = forge_addition_new(type, definition_index, transform);
	struct forge_transform before;
	struct forge_transform after = *transform;
	struct forge_target target;

	if (slot == NONE)
		return;
	before = forge.additions[slot].current;
	after.deleted = FALSE;
	target.kind = _forge_target_addition;
	target.type = type;
	target.index = slot;
	forge_target_apply(&target, &after);
	if (forge.additions[slot].object_index == NONE)
	{
		forge_message("Could not place %s", forge_tag_short_name(definition_index));
		return;
	}
	forge_command_push(&target, &before, &after);
	forge_select(&target);
	if (carry)
		forge_carry_begin();
	forge_message("Placed %s", forge_tag_short_name(definition_index));
}

static void forge_undo(
	boolean redo)
{
	struct forge_command const *command;

	forge_carry_end(TRUE);
	if (redo ? forge.command_position >= forge.command_count : forge.command_position <= 0)
	{
		forge_message(redo ? "Nothing to redo" : "Nothing to undo");
		return;
	}
	command = redo ? &forge.commands[forge.command_position++] : &forge.commands[--forge.command_position];
	forge_target_apply(&command->target, redo ? &command->after : &command->before);
	forge_select(&command->target);
	forge_message(redo ? "Redone" : "Undone");
}

/* ---------- private code: opening and closing */

static boolean forge_may_open(
	short local_player_index,
	char const **reason)
{
	struct scenario *scenario = global_scenario_get();
	short connection = game_connection();

	*reason = NULL;
	if (!game_in_progress() || !scenario || global_scenario_index == NONE)
		return FALSE;
	if (scenario->type == _scenario_type_main_menu)
		return FALSE;
	if (local_player_get_player_index(local_player_index) == NONE)
		return FALSE;
	if (cinematic_in_progress())
	{
		*reason = "Forge opens once the cutscene is over";
		return FALSE;
	}
	if (connection == _game_connection_network_client || connection == _game_connection_film_playback)
	{
		*reason = "Forge edits only games on this machine";
		return FALSE;
	}
	if (connection == _game_connection_network_server)
	{
		struct data_iterator iterator;
		long players = 0;

		/* (a player of another machine has no local player here; this
		machine's bots are no one else: source/features/bots/bots.c) */
		data_iterator_new(&iterator, player_data);
		while (data_iterator_next(&iterator))
		{
			if (player_get(iterator.datum_index)->local_player_index == NONE &&
				!halo_extensions_player_is_computer_controlled(iterator.datum_index))
			{
				players++;
			}
		}
		if (players > 0)
		{
			*reason = "Forge edits only games nobody else is in";
			return FALSE;
		}
	}
	return TRUE;
}

#include "forge_avatar.inc"

static void forge_open(
	void)
{
	short local_player_index = local_player_get_next(NONE);
	struct observer_result const *observer;
	char const *reason = NULL;

	if (local_player_index == NONE || !forge_may_open(local_player_index, &reason))
	{
		if (reason)
			forge_message("%s", reason);
		return;
	}
	forge.local_player_index = local_player_index;
	if (!forge_avatar_begin()) return;
	observer = observer_get_camera(local_player_index);
	flying_camera_new(&forge.camera);
	if (observer)
		flying_camera_new_from_point_and_vector(&forge.camera, &observer->position, &observer->forward);
	forge_palette_build();
	csmemset(&forge.selected, 0, sizeof(forge.selected));
	forge.carrying = FALSE;
	forge.active = TRUE;
	/* (the keys that opened it count once let go of) */
	halo_forge_input_read(&forge.previous_input, TRUE);
	forge_message("Forge: %s (H for help)", forge.map_name);
}

static void forge_close(
	void)
{
	short local_player_index;

	if (!forge.active)
		return;
	forge_carry_end(TRUE);
	if (game_in_progress()) {
		if (!forge_avatar_end()) return;
	} else forge_avatar_restore();
	forge.active = FALSE;
	csmemset(&forge.selected, 0, sizeof(forge.selected));
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		forge.camera_reset[local_player_index] = TRUE;
	if (forge_unsaved())
		forge_message("Forge closed: unsaved edits stay until the map is left (Ctrl+S in Forge saves)");
}

/* ---------- private code: a frame */

static boolean forge_pressed(
	short key)
{
	return forge.input.keys[key] && !forge.previous_input.keys[key];
}

static boolean forge_held(
	short key)
{
	return forge.input.keys[key] != 0;
}

static void forge_fly(
	real seconds)
{
	real_vector3d forward, right;
	real speed = FORGE_FLY_SPEED * seconds;
	real sensitivity = (real)config_real("input.mouse_sensitivity");
	real move_forward = (real)(forge_held(HALO_FORGE_KEY_W) - forge_held(HALO_FORGE_KEY_S));
	real move_right = (real)(forge_held(HALO_FORGE_KEY_D) - forge_held(HALO_FORGE_KEY_A));
	real move_up = (real)(forge_held(HALO_FORGE_KEY_SPACE) - forge_held(HALO_FORGE_KEY_C));
	real_point3d position;

	if (sensitivity <= 0.0f)
		sensitivity = 1.0f;
	forge.camera.facing.yaw -= forge.input.mouse_dx * FORGE_LOOK_SCALE * sensitivity;
	forge.camera.facing.pitch = PIN(forge.camera.facing.pitch - forge.input.mouse_dy * FORGE_LOOK_SCALE * sensitivity,
		-FORGE_PITCH_LIMIT, FORGE_PITCH_LIMIT);
	if (forge.camera.facing.yaw > _pi)
		forge.camera.facing.yaw -= 2.0f * _pi;
	else if (forge.camera.facing.yaw < -_pi)
		forge.camera.facing.yaw += 2.0f * _pi;
	/* (Ctrl is for the commands: Ctrl+S does not fly backward) */
	if (forge_held(HALO_FORGE_KEY_CTRL))
		return;
	if (forge_held(HALO_FORGE_KEY_SHIFT))
		speed *= FORGE_FAST_SCALE;
	if (forge_held(HALO_FORGE_KEY_ALT))
		speed *= FORGE_SLOW_SCALE;
	forge_camera_vectors(&forward, &right);
	position = forge.camera.position;
	position.x += (forward.i * move_forward + right.i * move_right) * speed;
	position.y += (forward.j * move_forward + right.j * move_right) * speed;
	position.z += (forward.k * move_forward + move_up) * speed;
	if (fabsf(position.x) < FORGE_POSITION_LIMIT && fabsf(position.y) < FORGE_POSITION_LIMIT &&
		fabsf(position.z) < FORGE_POSITION_LIMIT)
	{
		forge.camera.position = position;
	}
}

static void forge_edit(
	real seconds)
{
	struct forge_transform state;
	boolean have_selection = forge.selected.kind != _forge_target_none &&
		forge_target_state(&forge.selected, &state) && !state.deleted;
	boolean control = forge_held(HALO_FORGE_KEY_CTRL);
	real_vector3d forward, right;

	forge_camera_vectors(&forward, &right);

	/* the commands */
	if (control && forge_pressed(HALO_FORGE_KEY_S))
	{
		forge_carry_end(TRUE);
		forge_save();
		return;
	}
	if (control && (forge_pressed(HALO_FORGE_KEY_Y) ||
		(forge_pressed(HALO_FORGE_KEY_Z) && forge_held(HALO_FORGE_KEY_SHIFT))))
	{
		forge_undo(TRUE);
		return;
	}
	if (control && forge_pressed(HALO_FORGE_KEY_Z))
	{
		forge_undo(FALSE);
		return;
	}
	if (!control) {
		short axis = forge.rotation_axis;
		if (forge_pressed(HALO_FORGE_KEY_X)) axis = 2;
		else if (forge_pressed(HALO_FORGE_KEY_Y)) axis = 1;
		else if (forge_pressed(HALO_FORGE_KEY_Z)) axis = 0;
		if (axis != forge.rotation_axis) {
			forge_turn_end();
			forge.rotation_axis = axis;
			forge_message("Rotation axis %c: Q/R turns; grab and fly moves freely", "ZYX"[axis]);
		}
	}
	if (forge_pressed(HALO_FORGE_KEY_G))
	{
		forge_turn_end();
		forge.snap = !forge.snap;
		forge_message(forge.snap ? "Snapping on (0.1 units, 15 degrees)" : "Snapping off");
	}
	if (forge_pressed(HALO_FORGE_KEY_H))
		forge.help = !forge.help;

	/* the item to place */
	if (forge.palette_count > 0)
	{
		long step = forge_pressed(HALO_FORGE_KEY_RIGHT_BRACKET) - forge_pressed(HALO_FORGE_KEY_LEFT_BRACKET);

		if (!forge.carrying && forge.input.wheel != 0.0f)
			step += forge.input.wheel > 0.0f ? -1 : 1;
		if (step)
			forge.palette_index = (forge.palette_index + step + forge.palette_count) % forge.palette_count;
	}

	if (forge_pressed(HALO_FORGE_KEY_ESCAPE))
	{
		if (forge.carrying)
			forge_carry_end(FALSE);
		else {
			forge_turn_end();
			csmemset(&forge.selected, 0, sizeof(forge.selected));
		}
		return;
	}

	if (forge_pressed(HALO_FORGE_KEY_MOUSE_LEFT))
	{
		struct forge_target target;

		forge_turn_end();
		if (forge.carrying)
		{
			forge_carry_end(TRUE);
		}
		else if (forge_hover_target(&target))
		{
			forge_select(&target);
		}
		else
		{
			csmemset(&forge.selected, 0, sizeof(forge.selected));
			if (forge.hover_object_index != NONE)
			{
				forge_message("%s is not one of the map's placements: the game made it",
					forge_tag_short_name(object_get(forge.hover_object_index)->definition_index));
			}
		}
		return;
	}

	if (forge_pressed(HALO_FORGE_KEY_P) && forge.palette_count > 0)
	{
		struct forge_transform transform;
		struct forge_palette_entry const *entry = &forge.palette[forge.palette_index];

		forge_carry_end(TRUE);
		csmemset(&transform, 0, sizeof(transform));
		if (forge.hover_hit)
		{
			transform.position = forge.hover_point;
		}
		else
		{
			transform.position.x = forge.camera.position.x + forward.i * FORGE_PLACE_DISTANCE;
			transform.position.y = forge.camera.position.y + forward.j * FORGE_PLACE_DISTANCE;
			transform.position.z = forge.camera.position.z + forward.k * FORGE_PLACE_DISTANCE;
		}
		transform.rotation.yaw = forge.camera.facing.yaw;
		forge_snap_transform(&transform);
		if (entry->type == NONE) {
			struct forge_target target = { _forge_target_world, (short)entry->definition_index, 0 };
			struct halo_world_geometry const *geometry = forge_world_geometry(&target);
			struct forge_transform before = transform;
			if (!geometry || !geometry->edit_count) return;
			target.index = geometry->edit_count();
			before.deleted = TRUE;
			if (!forge_world_set(&target, &transform)) { forge_message("GLB instance limit reached or invalid position"); return; }
			forge_nav_revision++;
			forge_command_push(&target, &before, &transform);
			forge_select(&target);
		} else forge_place(entry->type, entry->definition_index, &transform, FALSE);
		return;
	}

	if (!have_selection)
		return;

	if (forge_pressed(HALO_FORGE_KEY_MOUSE_RIGHT) || forge_pressed(HALO_FORGE_KEY_E))
	{
		if (forge.carrying)
			forge_carry_end(TRUE);
		else
			forge_carry_begin();
		return;
	}

	if (forge_pressed(HALO_FORGE_KEY_DELETE))
	{
		struct forge_transform after = state;

		forge_carry_end(TRUE);
		forge_target_state(&forge.selected, &after);
		after.deleted = TRUE;
		forge_change(&after);
		csmemset(&forge.selected, 0, sizeof(forge.selected));
		forge_message("Deleted");
		return;
	}

	if (forge_pressed(HALO_FORGE_KEY_V))
	{
		long object_index = forge_target_object(&forge.selected);

		forge_carry_end(TRUE);
		if (forge.selected.kind == _forge_target_world) {
			struct halo_world_geometry const *geometry = forge_world_geometry(&forge.selected);
			struct forge_target copy = forge.selected;
			struct forge_transform before, after;
			if (!geometry || !geometry->edit_count || !forge_target_state(&forge.selected, &after)) return;
			copy.index = geometry->edit_count();
			before = after; before.deleted = TRUE; after.deleted = FALSE;
			if (!forge_world_set(&copy, &after)) { forge_message("GLB instance limit reached"); return; }
			forge_command_push(&copy, &before, &after);
			forge_nav_revision++;
			forge_select(&copy);
			forge_carry_begin();
		} else if (object_index != NONE)
		{
			struct forge_transform transform;

			forge_target_state(&forge.selected, &transform);
			transform.deleted = FALSE;
			forge_place(object_get(object_index)->object.type, object_get(object_index)->definition_index,
				&transform, TRUE);
		}
		return;
	}

	/* turning: a step a press when snapping, else smoothly */
	{
		real turn = 0.0f;
		long direction = forge_held(HALO_FORGE_KEY_Q) - forge_held(HALO_FORGE_KEY_R);

		if (forge.snap)
		{
			turn = (real)(forge_pressed(HALO_FORGE_KEY_Q) - forge_pressed(HALO_FORGE_KEY_R)) * FORGE_SNAP_ANGLE;
		}
		else if (direction)
		{
			turn = (real)direction * FORGE_TURN_SPEED * seconds *
				(forge_held(HALO_FORGE_KEY_SHIFT) ? FORGE_FAST_SCALE : 1.0f);
		}
		if (turn != 0.0f)
		{
			struct forge_transform after = state;

			real *angle = &after.rotation.n[forge.rotation_axis];
			*angle += turn;
			while (*angle > _pi) *angle -= 2.0f * _pi;
			while (*angle < -_pi) *angle += 2.0f * _pi;
			if (forge.snap) *angle = forge_snap(*angle, FORGE_SNAP_ANGLE);
			if (forge.carrying)
			{
				forge_target_apply(&forge.selected, &after);
			}
			else if (forge.snap)
			{
				forge_change(&after);
			}
			else
			{
				/* (held: one undo step for the whole turn, pushed once let go) */
				if (!forge.turning) forge.carry_before = state;
				forge.turning = TRUE;
				forge_target_apply(&forge.selected, &after);
			}
			forge_target_state(&forge.selected, &state);
		}
		else forge_turn_end();
	}

	/* nudging, along the camera's level directions and up */
	{
		real step = forge_held(HALO_FORGE_KEY_SHIFT) ? FORGE_NUDGE_FAST : (forge.snap ? FORGE_SNAP_DISTANCE : FORGE_NUDGE);
		real along = (real)(forge_pressed(HALO_FORGE_KEY_UP) - forge_pressed(HALO_FORGE_KEY_DOWN));
		real across = (real)(forge_pressed(HALO_FORGE_KEY_RIGHT) - forge_pressed(HALO_FORGE_KEY_LEFT));
		real up = (real)(forge_pressed(HALO_FORGE_KEY_PAGE_UP) - forge_pressed(HALO_FORGE_KEY_PAGE_DOWN));

		if (along != 0.0f || across != 0.0f || up != 0.0f)
		{
			struct forge_transform after = state;
			real_vector3d level = { forward.i, forward.j, 0.0f };

			if (normalize3d(&level) == 0.0f)
				level.i = 1.0f;
			after.position.x += (level.i * along + right.i * across) * step;
			after.position.y += (level.j * along + right.j * across) * step;
			after.position.z += up * step;
			if (forge.carrying)
				forge_target_apply(&forge.selected, &after);
			else
				forge_change(&after);
			forge_target_state(&forge.selected, &state);
		}
	}

	/* carrying: in front of the camera */
	if (forge.carrying)
	{
		struct forge_transform after = state;

		if (forge.input.wheel != 0.0f)
			forge.carry_distance = MAX(FORGE_CARRY_MINIMUM, forge.carry_distance + forge.input.wheel * FORGE_CARRY_WHEEL);
		after.position.x = forge.camera.position.x + forward.i * forge.carry_distance;
		after.position.y = forge.camera.position.y + forward.j * forge.carry_distance;
		after.position.z = forge.camera.position.z + forward.k * forge.carry_distance;
		forge_snap_transform(&after);
		if (!forge_transforms_equal(&after, &state))
			forge_target_apply(&forge.selected, &after);
	}
}

/* ---------- private code: the scripted test (debug.forge_test) */

const char *config_string(char const *name);
long config_integer(char const *name);
int platform_forge_selector_draw(char const *cross, char const *bar, int selected,
	int size, int view_width, int view_height);

static struct
{
	boolean read;
	boolean edit;
	boolean verify;
	real seconds;
	long step;
	struct forge_target moved;
	struct forge_transform moved_after;
	real_point3d camera_before;
} forge_test;

/* the scenery placement nearest the camera with an object of its own (not
the one given) */
static boolean forge_test_nearest_placement(
	struct forge_target const *other,
	struct forge_target *target)
{
	long absolute_index;
	real best = 0.0f;
	boolean found = FALSE;

	for (absolute_index = 0; absolute_index < MAXIMUM_OBJECTS_PER_MAP; absolute_index++)
	{
		struct forge_source const *source = &forge_sources[absolute_index];
		real distance;

		if (source->object_index == NONE || source->type != _object_type_scenery ||
			!forge_object_alive(source->object_index) ||
			(other && other->kind == _forge_target_placement && other->type == source->type &&
			other->index == source->index))
		{
			continue;
		}
		distance = distance3d(&forge.camera.position, &object_get(source->object_index)->object.position);
		if (!found || distance < best)
		{
			best = distance;
			found = TRUE;
			target->kind = _forge_target_placement;
			target->type = source->type;
			target->index = source->index;
		}
	}
	return found;
}

static void forge_test_log_target(
	char const *what,
	struct forge_target const *target)
{
	long object_index = forge_target_object(target);
	struct forge_transform state;

	forge_target_state(target, &state);
	if (object_index != NONE)
	{
		real_point3d const *position = &object_get(object_index)->object.position;

		platform_log("forge test: %s %s %ld: state (%.3f %.3f %.3f) yaw %.3f%s, object at (%.3f %.3f %.3f)",
			what, object_type_get_name(target->type), target->index, state.position.x, state.position.y,
			state.position.z, state.rotation.yaw, state.deleted ? " deleted" : "", position->x, position->y,
			position->z);
	}
	else
	{
		platform_log("forge test: %s %s %ld: state (%.3f %.3f %.3f) yaw %.3f%s, no object", what,
			object_type_get_name(target->type), target->index, state.position.x, state.position.y,
			state.position.z, state.rotation.yaw, state.deleted ? " deleted" : "");
	}
}

/* the keys the script holds, over the reader's */
static void forge_test_input(
	struct halo_forge_input *input)
{
	if (!forge_test.edit)
		return;
	/* (half a second of flying forward, a step in) */
	if (forge_test.step == 2)
	{
		input->available = TRUE;
		input->keys[HALO_FORGE_KEY_W] = 1;
	}
}

static void forge_test_update(
	real seconds)
{
	long player_index;

	if (!forge_test.read)
	{
		char const *setting = config_string("debug.forge_test");

		forge_test.read = TRUE;
		forge_test.edit = !strcmp(setting, "edit");
		forge_test.verify = !strcmp(setting, "verify");
	}
	if (!forge_test.edit && !forge_test.verify)
		return;
	player_index = game_in_progress() ? local_player_get_player_index(0) : NONE;
	if (player_index == NONE || player_get(player_index)->unit_index == NONE)
	{
		forge_test.seconds = 0.0f;
		return;
	}
	forge_test.seconds += seconds;
	if (forge_test.seconds < 2.0f + forge_test.step)
		return;

	if (forge_test.verify)
	{
		long index;
		long matching = 0;

		if (forge_test.step++)
			return;
		for (index = 0; index < forge.placement_count; index++)
		{
			struct forge_placement_edit const *edit = &forge.placements[index];
			long object_index = forge_placement_object(edit->type, edit->index);
			boolean ok;

			if (edit->current.deleted)
				ok = object_index == NONE;
			else
				ok = object_index != NONE &&
					distance3d(&object_get(object_index)->object.position, &edit->current.position) < 0.01f;
			platform_log("forge test: verify %s placement %d %s: %s", object_type_get_name(edit->type), edit->index,
				edit->current.deleted ? "deleted" : "moved", ok ? "on the map" : "NOT ON THE MAP");
			matching += ok;
		}
		for (index = 0; index < forge.addition_count; index++)
		{
			struct forge_addition const *addition = &forge.additions[index];
			boolean alive = forge_object_alive(addition->object_index);
			/* (the game has run since: a vehicle settles under its own weight) */
			boolean ok = alive &&
				distance3d(&object_get(addition->object_index)->object.position, &addition->current.position) < 2.0f;

			platform_log("forge test: verify addition %ld %s (object %s, %.3f from its place): %s", addition->id,
				addition->tag_name, alive ? "made" : "none",
				alive ? distance3d(&object_get(addition->object_index)->object.position, &addition->current.position) : 0.0f,
				ok ? "on the map" : "NOT ON THE MAP");
			matching += ok;
		}
		platform_log("forge test: verify %s: %ld of %ld edits on the map",
			matching == forge.placement_count + forge.addition_count && matching > 0 ? "PASSED" : "FAILED",
			matching, forge.placement_count + forge.addition_count);
		return;
	}

	switch (forge_test.step++)
	{
	case 0:
		forge_open();
		if (!forge.active)
		{
			/* (a cutscene: again in a second) */
			forge_test.step = 0;
			forge_test.seconds = 1.0f;
			break;
		}
		platform_log("forge test: open on %s (scenario type %d, structure BSP %d), game paused %d", forge.map_name,
			global_scenario_get()->type, global_structure_bsp_index, game_time_get_paused());
		forge.help = TRUE;
		break;
	case 1:
		forge_test.camera_before = forge.camera.position;
		break;
	case 2:
		/* (the keys held: forge_test_input) */
		break;
	case 3:
	{
		struct forge_transform after;

		platform_log("forge test: flew %.3f units", distance3d(&forge_test.camera_before, &forge.camera.position));
		if (!forge_test_nearest_placement(NULL, &forge_test.moved))
		{
			platform_log("forge test: FAILED: no scenery placement to move");
			forge_test.edit = FALSE;
			break;
		}
		forge_select(&forge_test.moved);
		forge_test_log_target("selected", &forge_test.moved);
		forge_target_state(&forge_test.moved, &after);
		after.position.x += 1.0f;
		after.position.z += 0.5f;
		after.rotation.yaw += _pi / 4.0f;
		forge_change(&after);
		forge_test.moved_after = after;
		forge_test_log_target("moved", &forge_test.moved);
		/* (where the camera can see it) */
		forge.camera.position.x = after.position.x - 4.0f;
		forge.camera.position.y = after.position.y;
		forge.camera.position.z = after.position.z + 2.0f;
		forge.camera.facing.yaw = 0.0f;
		forge.camera.facing.pitch = -0.4f;
		break;
	}
	case 4:
	{
		long entry_index;
		struct forge_transform transform;

		csmemset(&transform, 0, sizeof(transform));
		transform.position = forge_test.moved_after.position;
		transform.position.y += 3.0f;
		transform.position.z += 1.0f;
		for (entry_index = 0; entry_index < forge.palette_count; entry_index++)
		{
			if (forge.palette[entry_index].type == _object_type_vehicle)
				break;
		}
		if (entry_index == forge.palette_count)
			entry_index = 0;
		if (forge.palette_count > 0)
		{
			forge.palette_index = entry_index;
			forge_place(forge.palette[entry_index].type, forge.palette[entry_index].definition_index, &transform, FALSE);
			platform_log("forge test: placed %s: %s", forge_tag_short_name(forge.palette[entry_index].definition_index),
				forge.addition_count > 0 && forge_object_alive(forge.additions[0].object_index) ? "made" : "NOT MADE");
		}
		else
		{
			platform_log("forge test: FAILED: nothing in the palette");
		}
		break;
	}
	case 5:
	{
		struct forge_target deleted;
		struct forge_transform after;

		if (forge_test_nearest_placement(&forge_test.moved, &deleted))
		{
			forge_select(&deleted);
			forge_target_state(&deleted, &after);
			after.deleted = TRUE;
			forge_change(&after);
			forge_test_log_target("deleted", &deleted);
		}
		forge_select(&forge_test.moved);
		break;
	}
	case 6:
		platform_log("forge test: save %s", forge_save() ? "done" : "FAILED");
		break;
	case 7:
		forge_undo(FALSE);
		forge_undo(FALSE);
		forge_undo(FALSE);
		forge_test_log_target("after undoing everything", &forge_test.moved);
		platform_log("forge test: added object after undo: %s",
			forge.addition_count > 0 && !forge_object_alive(forge.additions[0].object_index) ? "gone" : "STILL THERE");
		forge_undo(TRUE);
		forge_undo(TRUE);
		forge_undo(TRUE);
		forge_test_log_target("after redoing everything", &forge_test.moved);
		platform_log("forge test: unsaved after redo: %s", forge_unsaved() ? "YES" : "no");
		break;
	case 8:
		forge_close();
		platform_log("forge test: closed, game paused %d", game_time_get_paused());
		break;
	case 9:
		forge_test_log_target("after closing", &forge_test.moved);
		platform_log("forge test: edit done");
		break;
	}
}

/* ---------- public code */

boolean forge_active(
	void)
{
	return forge.active;
}

#include "forge_world_test.inc"
#include "forge_clock_test.inc"

void forge_update(
	real seconds)
{
	struct halo_forge_input input;

	if (seconds < 0.0f || seconds > 0.25f)
		seconds = 0.0f;
	if (forge.message_seconds > 0.0f)
		forge.message_seconds -= seconds;

	if (forge.map_loaded && game_in_progress()) forge_world_apply_loaded();
	forge_world_test_update(seconds);
	forge_clock_test_update(seconds);
	forge_test_update(seconds);
	halo_forge_input_read(&input, forge.active);
	if (forge.active)
		forge_test_input(&input);
	if (!forge.active)
	{
		if (input.keys[HALO_FORGE_KEY_TOGGLE] && !forge.previous_input.keys[HALO_FORGE_KEY_TOGGLE] && input.available)
			forge_open();
		else
			forge.previous_input = input;
		return;
	}
	forge.input = input;
	if (!game_in_progress() || local_player_get_player_index(forge.local_player_index) == NONE)
	{
		forge_close();
		forge.previous_input = input;
		return;
	}
	if (forge_pressed(HALO_FORGE_KEY_TOGGLE))
	{
		forge_close();
		forge.previous_input = input;
		return;
	}
	/* Forge owns the camera/input, not the simulation clock. Normal pause
	controls remain independent; keep host loopback traffic and bots alive. */
	/* a selection whose object is gone (a script deleted it, a BSP switch) */
	if (forge.selected.kind != _forge_target_none && forge.selected.kind != _forge_target_world &&
		forge_target_object(&forge.selected) == NONE)
	{
		forge.carrying = FALSE;
		csmemset(&forge.selected, 0, sizeof(forge.selected));
	}
	forge_fly(seconds);
	forge_pick();
	forge_edit(seconds);
	forge.previous_input = input;
}

boolean forge_director_camera(
	short local_player_index,
	void **camera_proc,
	boolean *reset)
{
	*camera_proc = (void *)forge_camera_update;
	*reset = FALSE;
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return FALSE;
	if (forge.camera_reset[local_player_index])
	{
		forge.camera_reset[local_player_index] = FALSE;
		*reset = TRUE;
	}
	return forge.active && local_player_index == forge.local_player_index;
}

void forge_initialize_for_new_map(
	void)
{
	char const *map_name = cache_file_loaded_map_name();
	unsigned long checksum = cache_files_get_checksum();
	long absolute_index;
	short local_player_index;

	forge_avatar_restore();
	forge.active = FALSE;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		forge.camera_reset[local_player_index] = FALSE;
	/* (the same map again: its tags may be the ones edited, kept in memory) */
	if (forge.map_loaded && !strcmp(forge.map_name, map_name ? map_name : "") && forge.checksum == checksum)
		forge_placements_restore();
	forge.map_loaded = TRUE;
	csstrncpy(forge.map_name, map_name ? map_name : "", sizeof(forge.map_name) - 1);
	forge.map_name[sizeof(forge.map_name) - 1] = 0;
	forge.checksum = checksum;
	forge.placement_count = 0;
	forge.addition_count = 0;
	forge.world_loaded_count = 0;
	forge.next_addition_id = 0;
	forge.command_count = 0;
	forge.command_position = 0;
	forge.saved_position = 0;
	forge.palette_count = 0;
	forge.palette_index = 0;
	forge.carrying = FALSE;
	forge.turning = FALSE;
	forge.rotation_axis = 0;
	csmemset(&forge.selected, 0, sizeof(forge.selected));
	for (absolute_index = 0; absolute_index < MAXIMUM_OBJECTS_PER_MAP; absolute_index++)
		forge_sources[absolute_index].object_index = NONE;
	if (global_scenario_get() && global_scenario_get()->type != _scenario_type_main_menu)
		forge_load();
}

void forge_dispose_from_old_map(
	void)
{
	forge_avatar_restore();
	forge.active = FALSE;
	forge.carrying = FALSE;
	if (forge.map_loaded)
		forge_placements_restore();
	forge.map_loaded = FALSE;
	forge.placement_count = 0;
	forge.addition_count = 0;
}

void forge_objects_placed(
	void)
{
	long slot;

	for (slot = 0; slot < forge.addition_count; slot++)
		forge_addition_spawn(&forge.additions[slot]);
}

void forge_structure_bsp_reconnected(
	void)
{
	forge_objects_placed();
}

void forge_object_placed_from_scenario(
	long object_index,
	struct scenario_object_datum *scenario_object)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct tag_block *datums;
	long element_size;
	byte *base;
	long offset;
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	short type;

	if (!object || absolute_index >= MAXIMUM_OBJECTS_PER_MAP || !global_scenario_get())
		return;
	type = object->object.type;
	if (!forge_type_has_placements(type))
		return;
	datums = scenario_get_object_type_scenario_datums(global_scenario_get(), type, &element_size);
	if (datums->count <= 0 || element_size <= 0)
		return;
	base = (byte *)tag_block_get_element_with_size(datums, 0, element_size);
	offset = (long)((byte *)scenario_object - base);
	if (offset < 0 || offset >= datums->count * element_size || offset % element_size)
		return;
	forge_sources[absolute_index].object_index = object_index;
	forge_sources[absolute_index].type = type;
	forge_sources[absolute_index].index = (short)(offset / element_size);
}

/* ---------- drawing */

static void forge_draw_text(
	char const *text,
	short x,
	short y,
	short justification,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	wchar_t wide[1024];
	long index;
	rectangle2d bounds;

	if (font_index == NONE)
		return;
	for (index = 0; text[index] && index < NUMBEROF(wide) - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
	bounds = render.camera.window_bounds;
	bounds.x0 = (short)(bounds.x0 + x);
	bounds.y0 = (short)(bounds.y0 + y);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wide);
}

static void forge_selection_name(
	char *text,
	long size)
{
	long object_index = forge_target_object(&forge.selected);
	char const *name = object_index != NONE ? forge_tag_short_name(object_get(object_index)->definition_index) : "?";

	if (forge.selected.kind == _forge_target_world) {
		struct halo_world_geometry const *geometry = forge_world_geometry(&forge.selected);
		snprintf(text, (size_t)size, "%s (instance %ld)",
			geometry && geometry->edit_name ? geometry->edit_name(forge.selected.index) : "external geometry",
			forge.selected.index);
	} else if (forge.selected.kind == _forge_target_placement)
	{
		snprintf(text, (size_t)size, "%s %s (placement %ld)", object_type_get_name(forge.selected.type), name,
			forge.selected.index);
	}
	else
	{
		snprintf(text, (size_t)size, "%s %s (added)", object_type_get_name(forge.selected.type), name);
	}
}

static boolean forge_draw_selector(void)
{
	return platform_forge_selector_draw(config_string("forge.selector_asset"),
		config_string("forge.selected_selector_asset"), forge.selected.kind != _forge_target_none,
		config_integer("forge.selector_size"), render.camera.window_bounds.x1 - render.camera.window_bounds.x0,
		render.camera.window_bounds.y1 - render.camera.window_bounds.y0) != 0;
}

void forge_render(
	void)
{
	real_argb_color const white = { 1.0f, 1.0f, 1.0f, 1.0f };
	real_argb_color const yellow = { 1.0f, 1.0f, 0.85f, 0.2f };
	real_argb_color const cyan = { 1.0f, 0.3f, 0.9f, 1.0f };
	real_argb_color const grey = { 1.0f, 0.75f, 0.75f, 0.75f };
	long font_index;
	short line_height;
	char text[512];
	short y;

	if (!forge.active || render.local_player_index != forge.local_player_index)
	{
		/* (the message saying why it did not open, or that it closed) */
		if (forge.message_seconds > 0.0f && render.local_player_index == local_player_get_next(NONE))
			forge_draw_text(forge.message, 16, 16, 0, &white);
		return;
	}

	forge_world_outline(&forge.hover_world, &grey);
	forge_world_outline(&forge.selected, forge.carrying ? &cyan : &yellow);
	/* what the crosshair is on, and the selection */
	if (forge.hover_object_index != NONE && forge_object_alive(forge.hover_object_index))
	{
		struct object_datum *object = object_get(forge.hover_object_index);

		render_debug_sphere(TRUE, &object->object.bounding_sphere_center,
			MAX(object->object.bounding_sphere_radius, 0.1f), &grey);
	}
	if (forge.selected.kind != _forge_target_none)
	{
		long object_index = forge_target_object(&forge.selected);

		if (object_index != NONE)
		{
			struct object_datum *object = object_get(object_index);
			real_vector3d forward, up;
			real_point3d tip;
			struct forge_transform state;

			render_debug_sphere(TRUE, &object->object.bounding_sphere_center,
				MAX(object->object.bounding_sphere_radius, 0.1f), forge.carrying ? &cyan : &yellow);
			if (forge_target_state(&forge.selected, &state))
			{
				forge_transform_vectors(&state, &forward, &up);
				tip.x = state.position.x + forward.i * 0.5f;
				tip.y = state.position.y + forward.j * 0.5f;
				tip.z = state.position.z + forward.k * 0.5f;
				render_debug_line(TRUE, &state.position, &tip, &yellow);
			}
		}
	}
	if (forge.hover_hit)
		render_debug_point(TRUE, &forge.hover_point, 0.05f, &white);

	font_index = hud_get_font_index();
	if (font_index == NONE) {
		forge_draw_selector();
		return;
	}
	{
		struct font_header *font = font_definition_get(font_index);

		line_height = (short)(font->ascending_height + font->descending_height + 2);
	}

	y = 12;
	snprintf(text, sizeof(text), "FORGE  %s%s%s  rotate %c", forge.map_name, forge_unsaved() ? "  (unsaved)" : "",
		forge.snap ? "  snapping" : "", "ZYX"[forge.rotation_axis]);
	forge_draw_text(text, 16, y, 0, &yellow);
	y = (short)(y + line_height);
	if (forge.selected.kind != _forge_target_none)
	{
		char name[256];

		forge_selection_name(name, sizeof(name));
		snprintf(text, sizeof(text), "%s: %s", forge.carrying ? "Carrying" : "Selected", name);
		forge_draw_text(text, 16, y, 0, &white);
		y = (short)(y + line_height);
	}
	if (forge.palette_count > 0)
	{
		struct forge_palette_entry const *entry = &forge.palette[forge.palette_index];

		if (entry->type == NONE) {
			struct forge_target target = { _forge_target_world, (short)entry->definition_index, 0 };
			struct halo_world_geometry const *geometry = forge_world_geometry(&target);
			snprintf(text, sizeof(text), "Place (P): %s  [%ld/%ld]",
				geometry && geometry->edit_name ? geometry->edit_name(0) : "GLB geometry",
				forge.palette_index + 1, forge.palette_count);
		} else snprintf(text, sizeof(text), "Place (P): %s %s  [%ld/%ld]", object_type_get_name(entry->type),
			forge_tag_short_name(entry->definition_index), forge.palette_index + 1, forge.palette_count);
		forge_draw_text(text, 16, y, 0, &grey);
		y = (short)(y + line_height);
	}
	if (forge.message_seconds > 0.0f)
	{
		forge_draw_text(forge.message, 16, y, 0, &cyan);
		y = (short)(y + line_height);
	}
	if (forge.help)
	{
		static char const *const help[] =
		{
			"Mouse look   WASD fly   Space/C up/down",
			"Shift faster   Alt slower",
			"Click select/drop   Right click or E grab",
			"Wheel: distance/item   X/Y/Z axis   Q/R rotate",
			"Arrows, PgUp/PgDn nudge   G snapping",
			"[ ] item   P place   V copy   Del delete",
			"Ctrl+Z undo   Ctrl+Y redo   Ctrl+S save",
			"Esc put back/deselect   F7 or B close",
		};
		long index;

		for (index = 0; index < NUMBEROF(help); index++)
		{
			forge_draw_text(help[index], 16, y, 0, &grey);
			y = (short)(y + line_height);
		}
	}
	/* Draw last so the help panel cannot cover the selector. */
	if (!forge_draw_selector()) {
		short height = (short)(render.camera.window_bounds.y1 - render.camera.window_bounds.y0);
		forge_draw_text(forge.selected.kind == _forge_target_none ? "+" : "-", 0,
			(short)(height / 2 - line_height / 2), 2, &white);
	}
}

/* ---------- extension registration (source/extensions/extension_api.h) */

static struct halo_editor const forge_editor =
{
	.active = forge_active,
	.update = forge_update,
	.director_camera = forge_director_camera,
	/* the map's saved edits, before its objects are placed */
	.initialize_for_new_map = forge_initialize_for_new_map,
	.dispose_from_old_map = forge_dispose_from_old_map,
	.object_placed_from_scenario = forge_object_placed_from_scenario,
	/* the objects Forge added to the structure BSP switched to */
	.structure_bsp_reconnected = forge_structure_bsp_reconnected,
	.render = forge_render,
	/* geometry edits, undo and redo included, make derived navigation stale */
	.world_edit_revision = forge_navigation_revision,
};

struct halo_extension const forge_extension =
{
	.name = "forge",
	/* the map's objects placed: the objects Forge added are made */
	.objects_placed = forge_objects_placed,
	.editor = &forge_editor,
};

#endif
