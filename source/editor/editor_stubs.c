/*
EDITOR_STUBS.C
*/

/* ---------- headers */

#include "cseries.h"
#include "editor_stubs.h"
#include "extensions/extension_api.h"

/* ---------- public code */

void editor_render(
	void)
{
	/* port: editors' (Forge's) selection and text over the view */
	halo_extensions_editor_render();
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
	/* port: editors' (Forge's) saved edits, before the objects are placed */
	halo_extensions_editor_initialize_for_new_map();
	return;
}

void editor_dispose_from_old_map(
	void)
{
	halo_extensions_editor_dispose_from_old_map();
	return;
}
