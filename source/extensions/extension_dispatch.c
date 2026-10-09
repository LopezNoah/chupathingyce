/*
EXTENSION_DISPATCH.C

port: the extension registry and its hook dispatch (extension_api.h). The
engine calls halo_extensions_*; this file calls each registered feature.

The registry is the build's: configure.py (tools/features.py) defines
HALO_EXTENSION_1, HALO_EXTENSION_2, ... as the enabled features' names, in
their manifests' order, and each feature defines <name>_extension. Registry
order is call order.
*/

#include "cseries.h"
#include "extensions/extension_api.h"

/* ---------- registry */

#define HALO_EXTENSION_SYMBOL_(name) name##_extension
#define HALO_EXTENSION_SYMBOL(name) HALO_EXTENSION_SYMBOL_(name)

/* (tools/features.py MAXIMUM_EXTENSIONS: one block per slot) */
#ifdef HALO_EXTENSION_1
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_1);
#endif
#ifdef HALO_EXTENSION_2
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_2);
#endif
#ifdef HALO_EXTENSION_3
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_3);
#endif
#ifdef HALO_EXTENSION_4
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_4);
#endif
#ifdef HALO_EXTENSION_5
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_5);
#endif
#ifdef HALO_EXTENSION_6
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_6);
#endif
#ifdef HALO_EXTENSION_7
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_7);
#endif
#ifdef HALO_EXTENSION_8
extern struct halo_extension const HALO_EXTENSION_SYMBOL(HALO_EXTENSION_8);
#endif
#ifdef HALO_EXTENSION_9
#error "more extensions than extension_dispatch.c has slots (tools/features.py MAXIMUM_EXTENSIONS)"
#endif

static struct halo_extension const *const extensions[] =
{
#ifdef HALO_EXTENSION_1
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_1),
#endif
#ifdef HALO_EXTENSION_2
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_2),
#endif
#ifdef HALO_EXTENSION_3
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_3),
#endif
#ifdef HALO_EXTENSION_4
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_4),
#endif
#ifdef HALO_EXTENSION_5
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_5),
#endif
#ifdef HALO_EXTENSION_6
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_6),
#endif
#ifdef HALO_EXTENSION_7
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_7),
#endif
#ifdef HALO_EXTENSION_8
	&HALO_EXTENSION_SYMBOL(HALO_EXTENSION_8),
#endif
	NULL /* (an empty initializer list is not C89) */
};

#define EXTENSION_COUNT ((long)NUMBEROF(extensions) - 1)

/* each extension, as e; and each with the part, as e and part */
#define EACH_EXTENSION(e) \
	long extension_index; \
	struct halo_extension const *e; \
	for (extension_index = 0; extension_index<EXTENSION_COUNT && (e = extensions[extension_index], TRUE); extension_index++)
#define EACH_PART(e, part) \
	EACH_EXTENSION(e) \
		if (e->part)

long halo_extensions_count(
	void)
{
	return EXTENSION_COUNT;
}

struct halo_extension const *halo_extensions_get(
	long index)
{
	return index >= 0 && index < EXTENSION_COUNT ? extensions[index] : NULL;
}

/* ---------- general hooks */

void halo_extensions_objects_placed(void)
{
	EACH_EXTENSION(e) if (e->objects_placed) e->objects_placed();
}

void halo_extensions_dispose_from_old_map(void)
{
	EACH_EXTENSION(e) if (e->dispose_from_old_map) e->dispose_from_old_map();
}

void halo_extensions_invalidate_derived_state(void)
{
	EACH_EXTENSION(e) if (e->invalidate_derived_state) e->invalidate_derived_state();
}

void halo_extensions_main_frame_update(boolean main_menu_loaded, real seconds)
{
	EACH_EXTENSION(e) if (e->main_frame_update) e->main_frame_update(main_menu_loaded, seconds);
}

struct game_engine *halo_extensions_select_game_engine(struct game_engine *original, struct game_variant *variant)
{
	struct game_engine *engine = original;

	{
		EACH_EXTENSION(e) if (e->select_game_engine) engine = e->select_game_engine(engine, variant);
	}
	return engine;
}

boolean halo_extensions_suppress_game_report(void)
{
	EACH_EXTENSION(e) if (e->suppress_game_report && e->suppress_game_report()) return TRUE;
	return FALSE;
}

/* ---------- rulesets */

struct halo_ruleset const *halo_extensions_active_ruleset(void)
{
	EACH_PART(e, ruleset)
		if (e->ruleset->active && e->ruleset->active())
			return e->ruleset;
	return NULL;
}

void halo_extensions_filter_player_action(long player_index, struct player_action *action)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (ruleset && ruleset->filter_player_action)
		ruleset->filter_player_action(player_index, action);
}

void halo_extensions_end_tick(void)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (ruleset && ruleset->end_tick)
		ruleset->end_tick();
}

boolean halo_extensions_player_killed(long killing_player_index, long dead_player_index)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (!ruleset || !ruleset->player_killed)
		return FALSE;
	ruleset->player_killed(killing_player_index, dead_player_index);
	return TRUE;
}

boolean halo_extensions_should_end_game(boolean *result)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (!ruleset || !ruleset->should_end_game)
		return FALSE;
	*result = ruleset->should_end_game();
	return TRUE;
}

boolean halo_extensions_should_spawn_player(long player_index, boolean *result)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (!ruleset || !ruleset->should_spawn_player)
		return FALSE;
	*result = ruleset->should_spawn_player(player_index);
	return TRUE;
}

boolean halo_extensions_player_change_color(long player_index, union real_rgb_color *color)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (!ruleset || !ruleset->player_change_color)
		return FALSE;
	ruleset->player_change_color(player_index, color);
	return TRUE;
}

boolean halo_extensions_player_state_message(long player_index, wchar_t *message, long message_character_count,
	boolean *result)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	if (!ruleset || !ruleset->player_state_message)
		return FALSE;
	*result = ruleset->player_state_message(player_index, message, message_character_count);
	return TRUE;
}

real halo_extensions_damage_multiplier(long damaging_player_index, long damaged_player_index)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	return ruleset && ruleset->damage_multiplier ?
		ruleset->damage_multiplier(damaging_player_index, damaged_player_index) : 1.f;
}

boolean halo_extensions_can_collect_items(long player_index)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	return !ruleset || !ruleset->can_collect_items || ruleset->can_collect_items(player_index);
}

boolean halo_extensions_suppress_network_state(void)
{
	struct halo_ruleset const *ruleset = halo_extensions_active_ruleset();

	return ruleset && ruleset->local_only;
}

/* ---------- player controllers */

void halo_extensions_update_player_controllers(void)
{
	EACH_PART(e, player_controller)
		if (e->player_controller->update)
			e->player_controller->update();
}

boolean halo_extensions_player_is_computer_controlled(long player_index)
{
	EACH_PART(e, player_controller)
		if (e->player_controller->controls_player && e->player_controller->controls_player(player_index))
			return TRUE;
	return FALSE;
}

/* ---------- editors */

static boolean editor_is_active(struct halo_editor const *editor)
{
	return editor->active && editor->active();
}

boolean halo_extensions_editor_active(void)
{
	EACH_PART(e, editor)
		if (editor_is_active(e->editor))
			return TRUE;
	return FALSE;
}

int halo_extensions_input_captured(void)
{
	return halo_extensions_editor_active() ? 1 : 0;
}

void halo_extensions_editor_update(real seconds)
{
	EACH_PART(e, editor)
		if (e->editor->update)
			e->editor->update(seconds);
}

boolean halo_extensions_editor_director_camera(short local_player_index, void **camera_proc, boolean *reset)
{
	boolean any_reset = FALSE;

	*camera_proc = NULL;
	{
		/* every editor is asked (each has its one-shot reset to give), the
		first with the view keeps it */
		EACH_PART(e, editor)
		{
			void *proc = NULL;
			boolean editor_reset = FALSE;

			if (!e->editor->director_camera)
				continue;
			if (e->editor->director_camera(local_player_index, &proc, &editor_reset) && !*camera_proc)
				*camera_proc = proc;
			any_reset |= editor_reset;
		}
	}
	*reset = *camera_proc ? FALSE : any_reset;
	return *camera_proc != NULL;
}

void halo_extensions_editor_initialize_for_new_map(void)
{
	EACH_PART(e, editor)
		if (e->editor->initialize_for_new_map)
			e->editor->initialize_for_new_map();
}

void halo_extensions_editor_dispose_from_old_map(void)
{
	EACH_PART(e, editor)
		if (e->editor->dispose_from_old_map)
			e->editor->dispose_from_old_map();
}

void halo_extensions_object_placed_from_scenario(long object_index, struct scenario_object_datum *scenario_object)
{
	EACH_PART(e, editor)
		if (e->editor->object_placed_from_scenario)
			e->editor->object_placed_from_scenario(object_index, scenario_object);
}

void halo_extensions_structure_bsp_reconnected(void)
{
	EACH_PART(e, editor)
		if (e->editor->structure_bsp_reconnected)
			e->editor->structure_bsp_reconnected();
}

void halo_extensions_editor_render(void)
{
	EACH_PART(e, editor)
		if (e->editor->render)
			e->editor->render();
}

uint64_t halo_extensions_world_edit_revision(void)
{
	uint64_t revision = 0;

	{
		EACH_PART(e, editor)
			if (e->editor->world_edit_revision)
				revision += e->editor->world_edit_revision();
	}
	return revision;
}
