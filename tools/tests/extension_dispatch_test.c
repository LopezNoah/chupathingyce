/* Dispatcher-only contract test; cseries types supplied by test_features.py. */
#include "extensions/extension_api.h"
#include <assert.h>

static boolean enabled;
static long notifications;
static boolean first_active(void) { return enabled; }
static boolean second_active(void) { return TRUE; }
static void notify_first(void) { notifications = notifications * 10 + 1; }
static void notify_second(void) { notifications = notifications * 10 + 2; }
static boolean end_false(void) { return FALSE; }
static boolean end_true(void) { return TRUE; }
static boolean report(void) { return enabled; }
static boolean controls(long player) { return player == 7; }
static real damage(long attacker, long victim) { (void)attacker; (void)victim; return 2.f; }
static boolean camera_first(short player, void **proc, boolean *reset)
{
    (void)player;
    *reset = !enabled;
    if (!enabled) return FALSE;
    *proc = &notifications;
    return TRUE;
}
static boolean camera_second(short player, void **proc, boolean *reset)
{
    (void)player; (void)proc;
    *reset = TRUE;
    return FALSE;
}
static uint64_t revision(void) { return 3; }
static const struct halo_ruleset first_rules = {
    .active = first_active, .local_only = TRUE,
    .should_end_game = end_false, .damage_multiplier = damage
};
static const struct halo_ruleset second_rules = {
    .active = second_active, .should_end_game = end_true
};
static const struct halo_player_controller controller = { .controls_player = controls };
static const struct halo_editor first_editor = {
    .active = first_active, .director_camera = camera_first, .world_edit_revision = revision
};
static const struct halo_editor second_editor = { .director_camera = camera_second };
const struct halo_extension first_extension = {
    .name = "first", .objects_placed = notify_first, .ruleset = &first_rules,
    .suppress_game_report = report, .player_controller = &controller, .editor = &first_editor
};
const struct halo_extension second_extension = {
    .name = "second", .objects_placed = notify_second, .ruleset = &second_rules, .editor = &second_editor
};
int main(void)
{
    boolean result = FALSE, reset = FALSE;
    void *proc = NULL;
    assert(halo_extensions_count() == 2);
    assert(halo_extensions_get(-1) == NULL);
    assert(halo_extensions_get(2) == NULL);
    halo_extensions_objects_placed();
    assert(notifications == 12);
    assert(halo_extensions_should_end_game(&result) && result);
    assert(!halo_extensions_suppress_network_state());
    assert(!halo_extensions_suppress_game_report());
    assert(halo_extensions_damage_multiplier(1, 2) == 1.f);
    assert(halo_extensions_can_collect_items(1));
    assert(!halo_extensions_player_killed(1, 2));
    assert(halo_extensions_player_is_computer_controlled(7));
    assert(!halo_extensions_player_is_computer_controlled(8));
    assert(!halo_extensions_input_captured());
    assert(!halo_extensions_editor_director_camera(0, &proc, &reset) && reset && !proc);
    enabled = TRUE;
    assert(halo_extensions_should_end_game(&result) && !result);
    assert(halo_extensions_suppress_network_state());
    assert(halo_extensions_suppress_game_report());
    assert(halo_extensions_damage_multiplier(1, 2) == 2.f);
    assert(halo_extensions_input_captured());
    assert(halo_extensions_editor_director_camera(0, &proc, &reset) && !reset && proc == &notifications);
    assert(halo_extensions_world_edit_revision() == 3);
    return 0;
}
