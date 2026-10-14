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
	/* port: render extension editor overlays */
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
	/* port: notify host-local editors before scenario objects are placed */
	halo_extensions_editor_initialize_for_new_map();
	return;
}

void editor_dispose_from_old_map(
	void)
{
	/* port: dispose host-local editor state with the map */
	halo_extensions_editor_dispose_from_old_map();
	return;
}
