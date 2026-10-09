/* GAME_ENGINE_INFECTION.H
 * Milestone 2 experimental LOCAL-ONLY adapter. Never a network variant.
 * cseries and game_engine.h must be included by engine callers.
 */
#ifndef GAME_ENGINE_INFECTION_H
#define GAME_ENGINE_INFECTION_H

#include "cseries.h"

struct game_variant;
struct player_action;
struct game_engine;
union real_rgb_color;

/* Returns the original engine unless explicitly enabled for local Slayer.
 * No enum, saved variant or protocol extension is used by this local adapter. */
struct game_engine *infection_game_select(struct game_engine *original, struct game_variant *variant);
boolean infection_game_active(void);
boolean infection_game_can_collect_items(long player_index);
boolean infection_game_should_spawn(long player_index);
boolean infection_game_should_end(void);
void infection_game_end_tick(void);
void infection_game_player_killed(long killer_index, long dead_index);
void infection_game_filter_action(long player_index, struct player_action *action);
real infection_game_damage_multiplier(long attacker_index, long victim_index);
void infection_game_color(long player_index, union real_rgb_color *color);
boolean infection_game_message(long player_index, wchar_t *message, long count);
/* Optional test launcher: local split screen only, never creates a server. */
void infection_game_test_update(boolean main_menu_loaded, real seconds);

#endif
