/* Experimental, host-local geometry; read once when the platform is placed. */
    { "platform.asset", _config_string, "\"\"", "HALO_PLATFORM_ASSET", _environment_value, _platform_desktop,
        "Optional GLB visual fitted to the board: absolute path, or relative to paths.data.\n"
        "Static first mesh/primitive, embedded PNG/JPEG textures. Collision remains solid.\n"
        "Loaded once on first render; restart to change assets. Empty uses the procedural board." },
    { "debug.platform_test", _config_boolean, "false", "HALO_PLATFORM_TEST", _environment_value, _platform_desktop,
        "Automated fixture only: drop the first local biped onto the board, log\n"
        "its standing height after three seconds, reset the map once and repeat.\n"
        "Teleports the player and restarts the game; leave off for normal play." },
    { "platform.enabled", _config_boolean, "false", "HALO_PLATFORM_ENABLED", _environment_value, _platform_desktop,
        "One generated solid board in offline Blood Gulch, placed ahead of the first\n"
        "local player. Forge can select, move, rotate, copy and delete it (64 slots).\n"
        "No external assets required; no networking or AI pathfinding." },
    { "platform.width", _config_real, "4.0", "HALO_PLATFORM_WIDTH", _environment_value, _platform_desktop,
        "Board width along world X, in Halo units (about 3 metres each); 0.5 to 20." },
    { "platform.depth", _config_real, "4.0", "HALO_PLATFORM_DEPTH", _environment_value, _platform_desktop,
        "Board depth along world Y, in Halo units; 0.5 to 20." },
    { "platform.elevation", _config_real, "0.6", "HALO_PLATFORM_ELEVATION", _environment_value, _platform_desktop,
        "Top surface above the ground, in Halo units; 0.2 to 10. Default is jumpable." },
    { "platform.thickness", _config_real, "0.15", "HALO_PLATFORM_THICKNESS", _environment_value, _platform_desktop,
        "Board thickness in Halo units; 0.05 to 1, and less than elevation." },
    { "platform.forward_offset", _config_real, "3.0", "HALO_PLATFORM_FORWARD_OFFSET", _environment_value, _platform_desktop,
        "Placement distance ahead of the first local player, in Halo units; 0 to 20.\n"
        "Initial placement is fixed, not attached to the player; Forge can edit it." },
