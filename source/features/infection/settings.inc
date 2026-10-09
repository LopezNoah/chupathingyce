/* Infection's config.toml settings: rows of port_config.c's config_settings
table, included there when the feature is built (tools/features.py,
HALO_FEATURE_SETTINGS_<n>). A setting may be in any section; the file is
written grouped by section. */

	/* the experimental Infection mode (configure.py --infection) */
	{ "infection.local_enabled", _config_boolean, "false", "HALO_INFECTION_LOCAL", _environment_value, _platform_all,
		"Experimental Infection rules for LOCAL Slayer only (never LAN/online).\n"
		"Off by default; use the local test launcher below, not network_test." },
	{ "infection.rounds", _config_integer, "3", "HALO_INFECTION_ROUNDS", _environment_value, _platform_all,
		"Local Infection rounds per match (1-100)." },
	{ "infection.round_seconds", _config_integer, "180", "HALO_INFECTION_SECONDS", _environment_value, _platform_all,
		"Local Infection round duration in seconds (1-3600)." },
	{ "infection.alpha_count", _config_integer, "0", "HALO_INFECTION_ALPHAS", _environment_value, _platform_all,
		"Local Infection Alpha count: 0 automatic, else 1-127 (leaves a Survivor)." },
	{ "infection.respawn_seconds", _config_integer, "3", "HALO_INFECTION_RESPAWN", _environment_value, _platform_all,
		"Local Infection respawn delay in seconds (0-60)." },
	{ "infection.shotgun_rounds", _config_integer, "18", "HALO_INFECTION_SHOTGUN_ROUNDS", _environment_value, _platform_all,
		"Survivor starting shotgun ammo, including loaded rounds (1-32767, limited by weapon)." },
	{ "infection.pistol_rounds", _config_integer, "36", "HALO_INFECTION_PISTOL_ROUNDS", _environment_value, _platform_all,
		"Survivor starting pistol ammo, including loaded rounds (1-32767, limited by weapon)." },
	{ "infection.seed", _config_integer, "0", "HALO_INFECTION_SEED", _environment_value, _platform_all,
		"Local Alpha selection seed: 0 automatic, nonzero for reproducible tests." },
	{ "debug.infection_test_map", _config_string, "\"\"", "HALO_INFECTION_TEST_MAP", _environment_value, _platform_all,
		"Opt-in local split-screen Infection launcher: Xbox map name (bloodgulch).\n"
		"Requires infection.local_enabled. No network server is created." },
	{ "debug.infection_test_players", _config_integer, "2", "HALO_INFECTION_TEST_PLAYERS", _environment_value, _platform_all,
		"Local test split-screen players (2-4). Normal real player datums, not bots." },
	{ "debug.infection_test_scenario", _config_string, "\"\"", "HALO_INFECTION_TEST_SCENARIO", _environment_value, _platform_all,
		"Local test fixture only: empty for manual play; lifecycle scripts real\n"
		"kills/respawns/conversions on three players across three rounds; melee\n"
		"tests input restrictions and unarmed impact on a wounded target;\n"
		"melee-full measures strikes on a full-health target; slayer-control\n"
		"(infection.local_enabled false) is an ordinary Slayer regression." },
