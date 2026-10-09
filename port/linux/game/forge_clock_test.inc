/* Opt-in live regression: free flight must not stop simulation/network ticks. */
static boolean forge_clock_test_pause_ownership(void)
{
    boolean ok;
    /* A deliberate menu pause is not Forge-owned and must survive closing. */
    game_time_set_paused(TRUE);
    forge_open();
    ok = forge.active && game_time_get_paused();
    /* Ground contact can overlap the collision pill; test pause ownership
       using an airborne return, not an unrelated blocked-destination case. */
    forge.camera.position.z += 2.0f;
    forge_close();
    ok = ok && !forge.active && game_time_get_paused();
    game_time_set_paused(FALSE);
    platform_log("forge clock test: %s explicit pause preserved", ok ? "PASS" : "FAIL");
    return ok;
}
static void forge_clock_test_update(real seconds)
{
    static short phase;
    static real elapsed;
    static long started_tick;
    static boolean failed;
    long player_index;

    if (phase == 3 || strcmp(config_string("debug.forge_test"), "flight-clock")) return;
    if (!game_in_progress()) {
        if (phase) {
            platform_log("forge clock test: FAIL game ended during flight");
            phase = 3;
        }
        return;
    }
    elapsed += seconds;
    if (phase == 0) {
        player_index = local_player_get_player_index(0);
        /* Wait for live gameplay, not just the loading/pregame session. */
        if (game_time_get() < 4 * TICKS_PER_SECOND || player_index == NONE ||
            player_get(player_index)->unit_index == NONE) return;
        forge_open();
        if (!forge.active) return;
        started_tick = game_time_get();
        elapsed = 0.0f;
        phase = 1;
        failed = game_time_get_paused();
        /* A clear camera destination above the entry point, in a hidden test
           window only. Never inject mouse/keyboard events into the desktop. */
        forge.camera.position.z += 2.0f;
        platform_log("forge clock test: entered at tick %ld, paused %d", started_tick, game_time_get_paused());
        return;
    }
    if (phase == 1) {
        if (!forge.active) {
            platform_log("forge clock test: FAIL flight closed unexpectedly");
            phase = 3;
            return;
        }
        if (game_time_get_paused()) failed = TRUE;
        if (elapsed < 12.0f) return;
        long advanced = game_time_get() - started_tick;
        if (advanced < 6 * TICKS_PER_SECOND) failed = TRUE;
        platform_log("forge clock test: %s flight advanced %ld ticks in %.2f seconds, paused %d",
            failed ? "FAIL" : "PASS", advanced, elapsed, game_time_get_paused());
        forge_close();
        if (forge.active || game_time_get_paused()) failed = TRUE;
        started_tick = game_time_get();
        elapsed = 0.0f;
        phase = 2;
        return;
    }
    if (elapsed < 2.0f) return;
    if (forge.active || game_time_get_paused() || game_time_get() <= started_tick) failed = TRUE;
    if (!forge_clock_test_pause_ownership()) failed = TRUE;
    platform_log("forge clock test: %s flight-clock", failed ? "FAIL" : "PASS");
    phase = 3;
}
