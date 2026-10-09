/* Forge's config.toml settings: rows of port_config.c's config_settings
table, included there when the feature is built (tools/features.py,
HALO_FEATURE_SETTINGS_<n>). A setting may be in any section; the file is
written grouped by section. */

	/* Forge, the map editor in the game (port/linux/game/forge.c) */
	{ "forge.toggle_key", _config_string, "\"F7, B\"", "HALO_FORGE_TOGGLE_KEY", _environment_value, _platform_desktop,
		"Opens and closes Forge, the map editor, in a game on this machine (a\n"
		"campaign level, or a multiplayer game nobody else is in): up to two keys,\n"
		"separated by a comma, named as in [controls]; empty for none. The game\n"
		"pauses while it is open. Its edits are kept beside the maps, in\n"
		"forge\\<map>.forge.json, and the map's own file is never changed." },
	{ "debug.forge_test", _config_string, "\"\"", "HALO_FORGE_TEST", _environment_value, _platform_desktop,
		"Scripted Forge edits once a game is under way, for automated tests\n"
		"(port/linux/game/forge.c): \"edit\" opens Forge, flies, moves, turns,\n"
		"places and deletes objects, saves, undoes and redoes, and closes it;\n"
		"\"verify\" logs whether the saved edits are on the map; empty none." },
