/*
EDITOR_STUBS.C
*/

/* ---------- headers */

#include "cseries.h"
#include "editor_stubs.h"
#ifdef HALO_64BIT
#include "forge.h" /* port: port/linux/game/forge.c */
#endif

/* ---------- public code */

void editor_render(
	void)
{
#ifdef HALO_FORGE
	/* port: Forge's selection and text over the view */
	forge_render();
#endif
	return;
}

boolean game_in_editor(
	void)
{
	return FALSE;
}

boolean editor_preprocess_rendered_object(
	long object_index,
	struct render_lighting *lighting)
{
	return TRUE;
}

boolean editor_should_exit(
	void)
{
	return FALSE;
}

void editor_initialize(
	void)
{
	return;
}

void editor_dispose(
	void)
{
	return;
}

void editor_update(
	void)
{
	return;
}

void editor_initialize_for_new_map(
	void)
{
#ifdef HALO_FORGE
	/* port: the map's Forge edits, before its objects are placed */
	forge_initialize_for_new_map();
#endif
	return;
}

void editor_dispose_from_old_map(
	void)
{
#ifdef HALO_FORGE
	forge_dispose_from_old_map();
#endif
	return;
}
