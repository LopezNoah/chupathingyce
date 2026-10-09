/* Bots' config.toml settings: rows of port_config.c's config_settings
table, included there when the feature is built (tools/features.py,
HALO_FEATURE_SETTINGS_<n>). A setting may be in any section; the file is
written grouped by section. */

	/* computer-controlled players (source/features/bots/bots.c; configure.py --bots) */
	{ "bots.count", _config_integer, "0", "HALO_BOTS", _environment_value, _platform_all,
		"Computer-controlled players (0 to 3) in FFA or Team Slayer games you\n"
		"host by yourself: a custom game nobody else is in. Bots never join a game\n"
		"another machine is in. They play as players do, with the same weapons,\n"
		"health and rules; experimental." },
	{ "bots.skill", _config_string, "\"marine\"", "HALO_BOT_SKILL", _environment_value, _platform_all,
		"How well the bots play: \"recruit\", \"marine\", \"odst\" or \"spartan\".\n"
		"Skill changes their reactions, aim and sight, never their health or damage." },
	{ "debug.nav_probe", _config_boolean, "false", "HALO_NAV_PROBE", _environment_set_is_true, _platform_all,
		"Opt-in BSP navigation walking probe controlling Bot 1 in a solo Slayer game.\n"
		"No teleport or physics bypass; off in normal play." },
	{ "debug.nav_dump", _config_string, "\"\"", "HALO_NAV_DUMP", _environment_value, _platform_all,
		"Diagnostic walkable BSP polygon dump path (derived local data, not a map edit)." },
	{ "debug.nav_goal", _config_string, "\"\"", "HALO_NAV_GOAL", _environment_value, _platform_all,
		"Optional navigation probe goal as x,y,z in CE world units; empty picks a nearby region." },
	{ "debug.bot_sandbox", _config_boolean, "false", "HALO_BOT_SANDBOX", _environment_set_is_true, _platform_all,
		"Solo Team Slayer bot test fixture (requires three bots): seat the local\n"
		"player in a parked Warthog and place teammate Bot 2 near the gunner entrance\n"
		"once per map. The bot must approach and enter normally; off in normal play." },
