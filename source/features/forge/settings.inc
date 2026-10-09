/* Forge's config.toml settings: rows of port_config.c's config_settings
table, included there when the feature is built (tools/features.py,
HALO_FEATURE_SETTINGS_<n>). A setting may be in any section; the file is
written grouped by section. */

	/* Forge, the map editor in the game (port/linux/game/forge.c) */
	{ "forge.toggle_key", _config_string, "\"F7, B\"", "HALO_FORGE_TOGGLE_KEY", _environment_value, _platform_desktop,
		"Opens and closes Forge, the map editor, in a game on this machine (a\n"
		"campaign level, or a multiplayer game nobody else is in): up to two keys,\n"
		"separated by a comma, named as in [controls]; empty for none. Simulation\n"
		"and the game clock keep running. Its edits are kept beside the maps, in\n"
		"forge\\<map>.forge.json, and the map's own file is never changed." },
	{ "forge.selector_asset", _config_string, "\"\"", "HALO_FORGE_SELECTOR_ASSET", _environment_value, _platform_desktop,
		"PNG reticle while no object is selected: absolute path or relative to paths.data.\n"
		"Transparent, aspect-preserving, loaded once per process; empty uses a text cross." },
	{ "forge.selected_selector_asset", _config_string, "\"\"", "HALO_FORGE_SELECTED_SELECTOR_ASSET", _environment_value, _platform_desktop,
		"PNG reticle while an object is selected or carried. Same limits as selector_asset.\n"
		"Empty or failed decoding uses a text bar. Restart to change images." },
	{ "forge.selector_size", _config_integer, "32", "HALO_FORGE_SELECTOR_SIZE", _environment_value, _platform_desktop,
		"Reticle image's longest side in logical HUD pixels (8 to 64), scaled with the HUD.\n"
		"32 gives these ring assets approximately assault-rifle crosshair size." },
	{ "debug.forge_test", _config_string, "\"\"", "HALO_FORGE_TEST", _environment_value, _platform_desktop,
		"Scripted Forge edits once a game is under way, for automated tests\n"
		"(port/linux/game/forge.c): \"edit\" opens Forge, flies, moves, turns,\n"
		"places and deletes objects, saves, undoes and redoes, and closes it;\n"
		"\"verify\" logs whether the saved edits are on the map; world tests GLB edits\n"
		"and safe character return; world-verify checks saved GLB poses; world-view\n"
		"holds that test's Forge view for screenshots; flight-clock verifies that\n"
		"ticks and networking continue during flight; world-alpha-above/below/grazing\n"
		"isolate the transparent grid for render checks. Empty disables all fixtures." },
