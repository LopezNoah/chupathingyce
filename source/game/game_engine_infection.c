/* GAME_ENGINE_INFECTION.C
 * Experimental local-only engine adapter. Existing Slayer ID/variant ABI,
 * separate runtime callbacks and rules; NO network state or admission path.
 * Death batches are committed after objects/players finish the simulation tick.
 */
#include "cseries.h"
#include "game_engine.h"
#include "game_engine_infection.h"
#include "infection_rules.h"
#include "game.h"
#include "players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "scenario/scenario_definitions.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "tag_files/tag_groups.h"
#include "text/unicode.h"
#include "main/main.h"
#include "bink/bink_playback.h"
#include "networking/network_game_globals.h"
#include <stdio.h>
#include <string.h>

#ifdef HALO_FEATURE_INFECTION

const char *config_string(const char *name);
long config_integer(const char *name);
int config_boolean(const char *name);
void platform_log(const char *format, ...);
unsigned long system_milliseconds(void);
void damage_kill_object_for_player(long object_index, long player_index);

/* Standalone rules capacity must cover the port's player data array. */
typedef char infection_player_capacity_check[
	INFECTION_MAXIMUM_PARTICIPANTS >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ? 1 : -1];

static struct
{
	boolean enabled;
	boolean faulted;
	struct infection_rules rules;
	long players[INFECTION_MAXIMUM_PARTICIPANTS];
	long units[INFECTION_MAXIMUM_PARTICIPANTS];
	short shotgun_rounds;
	short pistol_rounds;
	long granting_weapon;
	/* Debug fixture only, separate from gameplay rules. */
	long active_since;
	short test_stage;
	short test_strikes;
	long test_strike_time;
} infection;

/* Test fixture scenarios: "melee" (one strike on a wounded target) and
 * "melee-full" (repeated strikes on a full-health target). */
static boolean infection_melee_scenario(boolean *full)
{
	char const *scenario = config_string("debug.infection_test_scenario");
	if (!*config_string("debug.infection_test_map")) return FALSE;
	*full = !csstrcmp(scenario, "melee-full");
	return *full || !csstrcmp(scenario, "melee");
}
static struct game_engine infection_engine;

static long infection_slot(long player_index)
{
	long slot;
	if (!infection.enabled || player_index == NONE) return NONE;
	slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (!VALID_INDEX(slot, INFECTION_MAXIMUM_PARTICIPANTS)) return NONE;
	if (infection.players[slot] != player_index || !infection.rules.participants[slot].connected) return NONE;
	return slot;
}

boolean infection_game_active(void)
{
	/* If connection changes, retain the runtime adapter until cleanup rather
	 * than silently falling back to Slayer with custom roles. end_tick aborts. */
	return infection.enabled;
}

static void infection_dispose(void)
{
	csmemset(&infection, 0, sizeof(infection));
}

static boolean infection_initialize_map(void)
{
	platform_log("infection local: initialized (rounds=%lu seconds=%lu alphas=%lu)",
		(unsigned long)infection.rules.config.rounds, (unsigned long)infection.rules.config.round_seconds,
		(unsigned long)infection.rules.config.alpha_count);
	return TRUE;
}

static void infection_added(long player_index)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (!VALID_INDEX(slot, INFECTION_MAXIMUM_PARTICIPANTS)) { infection.faulted = TRUE; return; }
	if (infection.players[slot] == player_index && infection.rules.participants[slot].connected) return;
	if (infection.rules.participants[slot].connected)
		infection_rules_leave(&infection.rules, (uint32_t)slot, infection.rules.participants[slot].handle);
	if (infection.rules.last_admission_handle == UINT32_MAX ||
		!infection_rules_join(&infection.rules, (uint32_t)slot, infection.rules.last_admission_handle + 1))
	{
		infection.faulted = TRUE;
		return;
	}
	infection.players[slot] = player_index;
	infection.units[slot] = NONE;
	player_get(player_index)->multiplayer_special = NONE;
	player_get(player_index)->speed_multiplier = 1.0f;
	platform_log("infection local: admit slot=%ld datum=%lx handle=%lu", slot, player_index,
		(unsigned long)infection.rules.participants[slot].handle);
}

/* CE draws team 0 red and team 1 blue on the scoreboard. Survivors are blue,
 * matching their biped colour; the Infected horde is the red team. */
enum { INFECTION_TEAM_INFECTED = 0, INFECTION_TEAM_SURVIVORS = 1 };

static char const *infection_team_label(long team)
{
	return team == INFECTION_TEAM_SURVIVORS ? "Survivors" : "Infected";
}

static void infection_set_team(long slot)
{
	struct player_datum *player = player_try_and_get(infection.players[slot]);
	long team;
	if (!player) return;
	/* Conversion moves the player to the Infected team at once, the same tick
	 * the death batch commits (even while dead), not at their next spawn. */
	team = infection.rules.participants[slot].role == _infection_alpha ||
		infection.rules.participants[slot].role == _infection_beta ? INFECTION_TEAM_INFECTED : INFECTION_TEAM_SURVIVORS;
	if (player->team_index != team)
		platform_log("infection local: team slot=%ld datum=%lx %s -> %s role=%s alive=%d", slot,
			infection.players[slot], infection_team_label(player->team_index), infection_team_label(team),
			infection_rules_role_name(infection.rules.participants[slot].role),
			infection.rules.participants[slot].alive);
	player->team_index = team;
	player->network_player_data.team_index = (char)team;
	/* This mode has no network lobby/server record; both runtime fields are
	 * kept consistent without touching the participant's datum identifier. */
}

static void infection_prespawn(long player_index)
{
	long slot = infection_slot(player_index);
	if (slot != NONE) infection_set_team(slot);
}

static boolean infection_give_weapon(long unit_index, byte kind, short total_rounds, boolean first)
{
	long definition_index = game_engine_loadout_weapon_definition(kind);
	struct object_placement_data placement;
	long weapon_index;
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	struct weapon_magazine_definition *magazine;
	short rounds[1];
	if (definition_index == NONE) return FALSE;
	definition = weapon_definition_get(definition_index);
	/* Initial slice: supported ballistic pistol/shotgun, one magazine. */
	if (definition->weapon.magazines.count != 1) return FALSE;
	object_placement_data_new(&placement, definition_index, NONE);
	weapon_index = object_new(&placement);
	if (weapon_index == NONE) return FALSE;
	weapon = weapon_get(weapon_index);
	magazine = TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);
	weapon->weapon.magazines[0].rounds_loaded = MIN(total_rounds, magazine->rounds_loaded_maximum);
	/* CE rounds_total INCLUDES rounds_loaded; it is not reserve ammo. */
	rounds[0] = total_rounds;
	weapon_set_total_rounds(weapon_index, rounds);
	/* unit_add_weapon_to_inventory applies the game's pickup policy even
	 * for starting weapons. Permit precisely this engine-created grant. */
	infection.granting_weapon = weapon_index;
	{
		boolean added = unit_add_weapon_to_inventory(unit_index, weapon_index, first ? 2 : 0);
		infection.granting_weapon = NONE;
		if (!added)
		{
			object_delete(weapon_index);
			return FALSE;
		}
	}
	return TRUE;
}

static void infection_strip_weapons(long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long current = VALID_INDEX(unit->unit.current_weapon_index, MAXIMUM_WEAPONS_PER_UNIT) ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
	short slot;
	/* unit_delete_all_weapons intentionally preserves the currently held
	 * weapon. Infection must remove that weapon too, including at reset. */
	if (current != NONE)
	{
		unit_drop_current_weapon(unit_index, TRUE);
		if (object_try_and_get(current)) object_delete(current);
	}
	unit_delete_all_weapons(unit_index);
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++) unit->unit.weapon_object_indices[slot] = NONE;
	unit->unit.current_weapon_index = unit->unit.desired_weapon_index = NONE;
}

static long infection_weapon_ammo(long unit_index, short slot)
{
	long index;
	struct weapon_datum *weapon;
	if (unit_index == NONE) return 0;
	index = unit_get(unit_index)->unit.weapon_object_indices[slot];
	if (index == NONE) return 0;
	weapon = weapon_get(index);
	return weapon_definition_get(weapon->definition_index)->weapon.magazines.count > 0 ?
		weapon->weapon.magazines[0].rounds_total : 0;
}

static void infection_postspawn(long player_index)
{
	long slot = infection_slot(player_index);
	struct player_datum *player = player_try_and_get(player_index);
	struct unit_datum *unit;
	uint8_t role;
	if (slot == NONE || !player || player->unit_index == NONE) return;
	if (!infection_rules_spawn(&infection.rules, (uint32_t)slot, infection.rules.participants[slot].handle))
	{
		infection.faulted = TRUE;
		return;
	}
	infection.units[slot] = player->unit_index;
	unit = unit_get(player->unit_index);
	role = infection.rules.participants[slot].role;
	infection_strip_weapons(player->unit_index);
	unit->unit.grenade_counts[0] = unit->unit.grenade_counts[1] = 0;
	csmemset(player->powerup_durations, 0, sizeof(player->powerup_durations));
	player->speed_multiplier = 1.0f;
	player->respawn_timer = player->respawn_penalty = 0;
	if (role == _infection_survivor &&
		(!infection_give_weapon(player->unit_index, _loadout_weapon_shotgun, infection.shotgun_rounds, TRUE) ||
		!infection_give_weapon(player->unit_index, _loadout_weapon_pistol, infection.pistol_rounds, FALSE)))
	{
		platform_log("infection local: loadout failed; aborting instead of spawning an unarmed Survivor");
		infection.faulted = TRUE;
	}
	{
		/* Spawn safety evidence: distance to the nearest living enemy. */
		long other;
		real nearest = -1.f;
		for (other = 0; other < INFECTION_MAXIMUM_PARTICIPANTS; other++)
		{
			struct player_datum *enemy;
			real distance;
			if (other == slot || !infection.rules.participants[other].connected ||
				(infection.rules.participants[other].role == _infection_survivor) == (role == _infection_survivor)) continue;
			enemy = player_try_and_get(infection.players[other]);
			if (!enemy || enemy->unit_index == NONE) continue;
			distance = distance3d(&unit->object.position, &unit_get(enemy->unit_index)->object.position);
			if (nearest < 0.f || distance < nearest) nearest = distance;
		}
		platform_log("infection local: spawn distance slot=%ld role=%s nearest_enemy=%.2f", slot,
			infection_rules_role_name(role), nearest);
	}
	platform_log("infection local: spawn slot=%ld datum=%lx unit=%lx life=%lu role=%s team=%ld weapons=%d grenades=%d/%d ammo=%ld/%ld",
		slot, player_index, player->unit_index, (unsigned long)infection.rules.participants[slot].life_id,
		infection_rules_role_name(role), player->team_index, unit_get_weapon_count(player->unit_index),
		unit->unit.grenade_counts[0], unit->unit.grenade_counts[1],
		infection_weapon_ammo(player->unit_index, 0), infection_weapon_ammo(player->unit_index, 1));
}

static boolean infection_pickup(long unit_index, long weapon_index)
{
	struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit);
	long slot = unit ? infection_slot(unit->unit.player_index) : NONE;
	return slot != NONE && infection.rules.participants[slot].role == _infection_survivor &&
		(infection.rules.phase == _infection_active || weapon_index == infection.granting_weapon);
}

boolean infection_game_can_collect_items(long player_index)
{
	long slot = infection_slot(player_index);
	return slot != NONE && !infection.faulted && infection.rules.phase == _infection_active &&
		infection.rules.participants[slot].alive && infection.rules.participants[slot].role == _infection_survivor;
}

static long infection_score(long player_index, enum get_score_type type)
{
	long slot = infection_slot(player_index);
	(void)type;
	/* Personal score follows the identity across faction changes. No Slayer
	 * team-score accumulator is ever updated or consulted. */
	return slot == NONE ? 0 : infection.rules.participants[slot].score;
}

static wchar_t *infection_score_string(long player_index, wchar_t *string)
{
	usnprintf(string, 32, L"%ld", infection_score(player_index, _get_score_individual));
	return string;
}

static wchar_t *infection_score_name(wchar_t *string)
{
	ustrcpy(string, L"Infection");
	return string;
}

static wchar_t *infection_team_name(long team, wchar_t *string)
{
	ustrcpy(string, team == INFECTION_TEAM_SURVIVORS ? L"Survivors" : L"Infected");
	return string;
}

static long infection_did_win(long player_index)
{
	long slot = infection_slot(player_index);
	uint8_t role;
	if (slot == NONE) return FALSE;
	/* "Your team won" is the FACTION result of the last finished round, judged
	 * by the player's current faction (a converted Survivor won with the
	 * Infected). Personal ranking is shown separately by the score list. */
	role = infection.rules.participants[slot].role;
	if (infection.rules.winner == _infection_survivors_win) return role == _infection_survivor;
	if (infection.rules.winner == _infection_infected_win)
		return role == _infection_alpha || role == _infection_beta;
	return FALSE;
}

/* Faction-aware spawns: multiplies CE's own rating (which already keeps
 * players off occupied points and favours teammates). Points near a living
 * enemy are nearly excluded; further is better, saturating at 40 units.
 * Never zero, so a crowded map still spawns everyone. */
enum { INFECTION_SPAWN_UNSAFE = 8, INFECTION_SPAWN_SATURATE = 40 };

static real infection_starting_location_rating(long player_index,
	struct player_starting_location const *location)
{
	long slot = infection_slot(player_index);
	long other;
	real nearest = -1.f;
	boolean survivor;
	if (slot == NONE) return 1.f;
	survivor = infection.rules.participants[slot].role == _infection_survivor;
	for (other = 0; other < INFECTION_MAXIMUM_PARTICIPANTS; other++)
	{
		struct player_datum *enemy;
		real distance;
		if (other == slot || !infection.rules.participants[other].connected ||
			(infection.rules.participants[other].role == _infection_survivor) == survivor) continue;
		enemy = player_try_and_get(infection.players[other]);
		if (!enemy || enemy->unit_index == NONE) continue;
		distance = distance3d(&location->position, &unit_get(enemy->unit_index)->object.position);
		if (nearest < 0.f || distance < nearest) nearest = distance;
	}
	if (nearest < 0.f) return 1.f;
	if (nearest < (real)INFECTION_SPAWN_UNSAFE) return 0.001f;
	return 0.1f + MIN(nearest, (real)INFECTION_SPAWN_SATURATE) / (real)INFECTION_SPAWN_SATURATE;
}

static boolean infection_test_flag(long flag)
{
	return flag == 1; /* rasterize score; no fixed-team-only spawn restriction */
}

static boolean infection_read_integer(char const *name, uint32_t *value)
{
	long configured = config_integer(name);
	if (configured < 0 || (unsigned long)configured > UINT32_MAX) return FALSE;
	*value = (uint32_t)configured;
	return TRUE;
}

struct game_engine *infection_game_select(struct game_engine *original, struct game_variant *variant)
{
	struct infection_rules_config config;
	uint32_t seed;
	long slot, shotgun, pistol;
	if (!config_boolean("infection.local_enabled") || !original || original->type != game_engine_slayer) return original;
	if (game_connection() != _game_connection_local)
	{
		platform_log("infection local: refused non-local connection; ordinary Slayer unchanged");
		return original;
	}
	infection_rules_config_default(&config);
	config.ticks_per_second = TICKS_PER_SECOND;
	shotgun = config_integer("infection.shotgun_rounds");
	pistol = config_integer("infection.pistol_rounds");
	if (!infection_read_integer("infection.rounds", &config.rounds) ||
		!infection_read_integer("infection.round_seconds", &config.round_seconds) ||
		!infection_read_integer("infection.alpha_count", &config.alpha_count) ||
		!infection_read_integer("infection.respawn_seconds", &config.infected_respawn_seconds) ||
		!infection_read_integer("infection.seed", &seed) ||
		shotgun < 1 || shotgun > 32767 || pistol < 1 || pistol > 32767 || !infection_rules_config_valid(&config))
	{
		platform_log("infection local: invalid configuration; not activated");
		return original;
	}
	csmemset(&infection, 0, sizeof(infection));
	if (!seed) seed = (uint32_t)system_milliseconds() | 1U;
	if (!infection_rules_initialize(&infection.rules, &config, seed)) return original;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		infection.players[slot] = infection.units[slot] = NONE;
	infection.granting_weapon = NONE;
	infection.shotgun_rounds = (short)shotgun;
	infection.pistol_rounds = (short)pistol;
	infection.enabled = TRUE;
	/* Keep the fixed legacy ID, but none of Slayer's gameplay callbacks. */
	csmemset(&infection_engine, 0, sizeof(infection_engine));
	infection_engine.name = "infection (local)";
	infection_engine.type = game_engine_slayer;
	infection_engine.dispose = infection_engine.dispose_from_old_map = infection_dispose;
	infection_engine.initialize_for_new_map = infection_initialize_map;
	infection_engine.player_added = infection_added;
	infection_engine.player_update = infection_postspawn;
	infection_engine.prespawn_player_update = infection_prespawn;
	infection_engine.allow_pick_up = infection_pickup;
	infection_engine.get_player_score = infection_score;
	infection_engine.format_player_score = infection_score_string;
	infection_engine.format_score_name = infection_score_name;
	infection_engine.format_team_name = infection_team_name;
	infection_engine.did_player_win = infection_did_win;
	infection_engine.test_flag = infection_test_flag;
	infection_engine.starting_location_rating = infection_starting_location_rating;
	variant->universal_variant.teams = TRUE;
	variant->universal_variant.lives = 0;
	variant->universal_variant.odd_man_out = FALSE;
	variant->universal_variant.respawn_time_growth = variant->universal_variant.suicide_penalty = 0;
	variant->universal_variant.vehicle_set = 1; /* existing _game_engine_vehicles_none */
	variant->universal_variant.flags &= ~(FLAG(_game_variant_infinite_grenades_bit) |
		FLAG(_game_variant_always_invisible_bit) | FLAG(_game_variant_no_shields_bit));
	ustrncpy(variant->human_readable_game_description, L"Infection", 11);
	return &infection_engine;
}

boolean infection_game_should_spawn(long player_index)
{
	long slot = infection_slot(player_index);
	struct player_datum *player = player_try_and_get(player_index);
	return game_connection() == _game_connection_local && !infection.faulted && game_engine_can_score() &&
		slot != NONE && player && !player->quit_out_of_game &&
		infection_rules_can_spawn(&infection.rules, (uint32_t)slot, infection.rules.participants[slot].handle);
}

boolean infection_game_should_end(void)
{
	return infection.faulted || infection.rules.phase == _infection_match_results;
}

void infection_game_player_killed(long killer_index, long dead_index)
{
	long slot = infection_slot(dead_index);
	long killer = infection_slot(killer_index);
	struct player_datum *player = player_try_and_get(dead_index);
	if (slot == NONE || !player || game_connection() != _game_connection_local) return;
	if (player->unit_index != infection.units[slot]) return;
	if (infection_rules_death(&infection.rules, infection.rules.round_id, (uint32_t)slot,
		infection.rules.participants[slot].handle, infection.rules.participants[slot].life_id,
		killer == NONE ? 0 : infection.rules.participants[killer].handle))
	{
		player->death_time = game_time_get();
		player->respawn_penalty = 0;
		player->respawn_timer = (long)(infection.rules.config.infected_respawn_seconds * TICKS_PER_SECOND);
		platform_log("infection local: death round=%lu slot=%ld datum=%lx life=%lu killer=%ld role=%s",
			(unsigned long)infection.rules.round_id, slot, dead_index,
			(unsigned long)infection.rules.participants[slot].life_id, killer,
			infection_rules_role_name(infection.rules.participants[slot].role));
	}
}

void infection_game_filter_action(long player_index, struct player_action *action)
{
	long slot;
	boolean fixture_attack;
	unsigned long forbidden = FLAG(_unit_control_weapon_primary_trigger_bit) |
		FLAG(_unit_control_weapon_secondary_trigger_bit) | FLAG(_unit_control_throw_grenade_bit) |
		FLAG(_unit_control_action_bit) | FLAG(_unit_control_swap_weapons_bit);
	if (!infection.enabled) return;
	slot = infection_slot(player_index);
	if (slot == NONE || infection.faulted || infection.rules.phase != _infection_active)
	{
		action->control_flags = 0;
		action->primary_trigger = 0.f;
		action->throttle = *global_zero_vector2d;
		action->desired_weapon_index = action->desired_grenade_index = action->desired_zoom_level = NONE;
		return;
	}
	{
		boolean full;
		fixture_attack = infection.test_stage == 1 && infection.rules.participants[slot].role == _infection_alpha &&
			infection_melee_scenario(&full);
	}
	if (fixture_attack)
	{
		action->control_flags |= forbidden;
		action->primary_trigger = 1.f;
		action->desired_weapon_index = action->desired_grenade_index = action->desired_zoom_level = 0;
	}
	if (infection.rules.participants[slot].role != _infection_survivor)
	{
		action->control_flags &= ~forbidden;
		action->primary_trigger = 0.f;
		action->desired_weapon_index = action->desired_grenade_index = action->desired_zoom_level = NONE;
		/* Controlled test only: exercise the same melee input -> biped
		 * animation -> impact -> damage path as a real player's button. */
		if (fixture_attack)
		{
			platform_log("infection local fixture: filtered ranged input flags=%lu trigger=%.1f weapon=%d grenade=%d zoom=%d",
				action->control_flags & forbidden, action->primary_trigger, action->desired_weapon_index,
				action->desired_grenade_index, action->desired_zoom_level);
			SET_FLAG(action->control_flags, _unit_control_use_equipment_bit, TRUE);
			infection.test_stage = 2;
			infection.test_strikes++;
			infection.test_strike_time = game_time_get();
			platform_log("infection local fixture: unarmed melee input queued slot=%ld strike=%d", slot,
				infection.test_strikes);
		}
	}
}

real infection_game_damage_multiplier(long attacker_index, long victim_index)
{
	long victim = infection_slot(victim_index);
	long attacker = infection_slot(attacker_index);
	if (victim == NONE) return 1.f;
	if (infection.faulted || infection.rules.phase != _infection_active) return 0.f;
	if (attacker != NONE && attacker != victim)
	{
		boolean victim_survivor = infection.rules.participants[victim].role == _infection_survivor;
		boolean attacker_survivor = infection.rules.participants[attacker].role == _infection_survivor;
		if (victim_survivor == attacker_survivor) return 0.f;
	}
	return 1.f;
}

void infection_game_color(long player_index, real_rgb_color *color)
{
	long slot = infection_slot(player_index);
	uint8_t role = slot == NONE ? _infection_role_none : infection.rules.participants[slot].role;
	color->red = role == _infection_survivor ? 0.1f : 0.25f;
	color->green = role == _infection_survivor ? 0.45f : role == _infection_alpha ? 1.f : 0.65f;
	color->blue = role == _infection_survivor ? 1.f : 0.05f;
}

boolean infection_game_message(long player_index, wchar_t *message, long count)
{
	long slot = infection_slot(player_index);
	wchar_t const *role;
	wchar_t const *phase;
	uint32_t round = infection.rules.completed_rounds + 1;
	if (slot == NONE || !message || count < 1) return FALSE;
	role = infection.rules.participants[slot].role == _infection_survivor ? L"Survivor" :
		infection.rules.participants[slot].role == _infection_alpha ? L"Alpha Infected" :
		infection.rules.participants[slot].role == _infection_beta ? L"Beta Infected" : L"Waiting";
	phase = infection.rules.phase == _infection_active ?
		(infection.rules.last_spartan_handle == infection.rules.participants[slot].handle ? L"Last Spartan Standing!" : L"Hunt / survive") :
		infection.rules.phase == _infection_countdown ? L"Get ready" :
		infection.rules.phase >= _infection_round_results ?
		(infection.rules.winner == _infection_survivors_win ? L"Survivors win" : L"Infected win") : L"Preparing / waiting";
	if (infection.rules.phase >= _infection_round_results) round = infection.rules.completed_rounds;
	usnprintf(message, (unsigned long)count, L"Infection | %s | Round %lu/%lu\r\n%lu s | Survivors %lu | Score %ld | %s",
		role, (unsigned long)round, (unsigned long)infection.rules.config.rounds,
		(unsigned long)((infection.rules.phase_ticks + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND),
		(unsigned long)infection_rules_survivors(&infection.rules), infection_score(player_index, _get_score_individual), phase);
	message[count - 1] = 0;
	return TRUE;
}

static void infection_reset_units(void)
{
	long slot;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct player_datum *player = player_try_and_get(infection.players[slot]);
		if (!player) continue;
		/* Delete, not kill: a round reset must not score a suicide/death. */
		if (player->unit_index != NONE)
		{
			infection_strip_weapons(player->unit_index);
			object_delete(player->unit_index);
		}
		player->respawn_timer = player->respawn_penalty = 0;
		player->speed_multiplier = 1.f;
		csmemset(player->powerup_durations, 0, sizeof(player->powerup_durations));
		infection.units[slot] = NONE;
	}
}

static long infection_live_role(uint8_t role)
{
	long slot;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct player_datum *player;
		if (!infection.rules.participants[slot].connected || infection.rules.participants[slot].role != role ||
			!infection.rules.participants[slot].alive) continue;
		player = player_try_and_get(infection.players[slot]);
		if (player && player->unit_index != NONE) return slot;
	}
	return NONE;
}

static void infection_melee_fixture(void)
{
	long alpha, survivor;
	struct player_datum *attacker, *victim;
	real_point3d position;
	boolean full;
	if (infection.rules.phase != _infection_active || !infection_melee_scenario(&full)) return;
	alpha = infection_live_role(_infection_alpha);
	survivor = infection_live_role(_infection_survivor);
	/* Full-health: after each strike's animation and impact (16 ticks), log
	 * the result and press again, bounded, until the Survivor dies. */
	if (full && infection.test_stage == 2 && game_time_get() - infection.test_strike_time >= 24)
	{
		if (survivor == NONE) return;
		platform_log("infection local fixture: after strike=%d shield=%.3f body=%.3f", infection.test_strikes,
			unit_get(player_get(infection.players[survivor])->unit_index)->object.shield_vitality,
			unit_get(player_get(infection.players[survivor])->unit_index)->object.body_vitality);
		if (infection.test_strikes < 12) infection.test_stage = 1;
		return;
	}
	if (infection.test_stage != 0) return;
	if (game_time_get() - infection.active_since < TICKS_PER_SECOND) return;
	if (alpha == NONE || survivor == NONE) return;
	attacker = player_get(infection.players[alpha]);
	victim = player_get(infection.players[survivor]);
	if (infection_give_weapon(attacker->unit_index, _loadout_weapon_pistol, infection.pistol_rounds, TRUE))
	{
		platform_log("infection local fixture: aborting: Infected accepted a forbidden weapon pickup");
		infection.faulted = TRUE;
		return;
	}
	platform_log("infection local fixture: Infected pistol pickup blocked");
	position = unit_get(attacker->unit_index)->object.position;
	/* Place along the actual aim, not world X: local control preserves the
	 * spawn's yaw, and a single desired-facing update is turn-rate limited. */
	position.x += unit_get(attacker->unit_index)->unit.aiming_vector.i * 0.5f;
	position.y += unit_get(attacker->unit_index)->unit.aiming_vector.j * 0.5f;
	{
		/* Face the attacker: a CE melee to the back is an instant kill, which
		 * would hide face-to-face balance. */
		real_vector3d facing = unit_get(attacker->unit_index)->unit.aiming_vector;
		facing.i = -facing.i;
		facing.j = -facing.j;
		facing.k = 0.f;
		normalize3d(&facing);
		object_set_position(victim->unit_index, &position, &facing, global_up3d);
	}
	/* Fixture only: "melee" wounds the target to verify one impact
	 * deterministically. These are not normal Infection traits or spawns. */
	if (!full)
	{
		unit_get(victim->unit_index)->object.shield_vitality = 0.f;
		unit_get(victim->unit_index)->object.body_vitality = 0.05f;
	}
	infection.test_stage = 1;
	{
		long damage = unit_unarmed_melee_damage(attacker->unit_index);
		long own = unit_definition_get(unit_get(attacker->unit_index)->definition_index)->unit.melee_damage.index;
		platform_log("infection local fixture: unarmed melee damage tag=%s (biped's own: %s)",
			damage != NONE ? tag_get_name(damage) : "none", own != NONE ? "yes" : "no");
	}
	platform_log("infection local fixture: positioned %s Survivor for unarmed impact (weapons=%d) shield=%.3f body=%.3f facing_dot=%.3f",
		full ? "full-health" : "wounded", unit_get_weapon_count(attacker->unit_index),
		unit_get(victim->unit_index)->object.shield_vitality, unit_get(victim->unit_index)->object.body_vitality,
		dot_product3d(&unit_get(victim->unit_index)->object.forward, &unit_get(attacker->unit_index)->object.forward));
}

/* Opt-in scripted LOCAL fixture. Uses real damage/Spartans, never fabricates
 * conversion or score events. No effect in normal local Infection games. */
static void infection_lifecycle_fixture(void)
{
	long alpha, survivor, beta;
	if (infection.rules.phase != _infection_active || !*config_string("debug.infection_test_map") ||
		csstrcmp(config_string("debug.infection_test_scenario"), "lifecycle")) return;
	if (game_time_get() - infection.active_since < TICKS_PER_SECOND) return;
	alpha = infection_live_role(_infection_alpha);
	survivor = infection_live_role(_infection_survivor);
	beta = infection_live_role(_infection_beta);
	if (alpha == NONE || survivor == NONE) return;
	if (infection.rules.round_id == 1)
	{
		if (infection.test_stage == 0)
		{
			infection.test_stage = 1;
			platform_log("infection local fixture: Survivor kills Alpha through real damage");
			damage_kill_object_for_player(player_get(infection.players[alpha])->unit_index, infection.players[survivor]);
		}
		else if (infection.test_stage == 1 && infection.rules.participants[alpha].life_id >= 2)
		{
			infection.test_stage = 2;
			platform_log("infection local fixture: respawned Alpha infects Survivor through real damage");
			damage_kill_object_for_player(player_get(infection.players[survivor])->unit_index, infection.players[alpha]);
		}
		else if (infection.test_stage == 2 && beta != NONE)
		{
			infection.test_stage = 3;
			platform_log("infection local fixture: Beta respawn verified; environmental death of last Survivor");
			unit_kill(player_get(infection.players[survivor])->unit_index);
		}
	}
	else if (infection.rules.round_id == 2)
	{
		if (infection.test_stage == 0)
		{
			infection.test_stage = 1;
			platform_log("infection local fixture: Survivor suicide through real damage");
			damage_kill_object_for_player(player_get(infection.players[survivor])->unit_index, infection.players[survivor]);
		}
		else if (infection.test_stage == 1 && beta != NONE)
		{
			infection.test_stage = 2;
			platform_log("infection local fixture: Alpha infects last Survivor through real damage");
			damage_kill_object_for_player(player_get(infection.players[survivor])->unit_index, infection.players[alpha]);
		}
	}
	/* Round 3 deliberately times out with Survivors remaining. */
}

static void infection_log_players(void)
{
	long slot;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct player_datum *player;
		long ammo[2] = { 0, 0 };
		short weapon_slot;
		if (!infection.rules.participants[slot].connected) continue;
		player = player_try_and_get(infection.players[slot]);
		if (!player) continue;
		for (weapon_slot = 0; player->unit_index != NONE && weapon_slot < 2; weapon_slot++)
		{
			long weapon_index = unit_get(player->unit_index)->unit.weapon_object_indices[weapon_slot];
			if (weapon_index != NONE)
			{
				struct weapon_datum *weapon = weapon_get(weapon_index);
				ammo[weapon_slot] = weapon->weapon.magazines[0].rounds_total;
			}
		}
		platform_log("infection local: player slot=%ld datum=%lx role=%s score=%ld life=%lu alive=%d team=%ld ammo=%ld/%ld",
			slot, infection.players[slot], infection_rules_role_name(infection.rules.participants[slot].role),
			infection_score(infection.players[slot], _get_score_individual),
			(unsigned long)infection.rules.participants[slot].life_id, infection.rules.participants[slot].alive,
			player->team_index, ammo[0], ammo[1]);
	}
}

void infection_game_end_tick(void)
{
	long slot;
	uint8_t previous;
	if (!infection.enabled || !game_engine_can_score()) return;
	if (game_connection() != _game_connection_local)
	{
		infection.faulted = TRUE;
		return;
	}
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct player_datum *player;
		if (!infection.rules.participants[slot].connected) continue;
		player = player_try_and_get(infection.players[slot]);
		if (!player || player->quit_out_of_game)
			infection_rules_leave(&infection.rules, (uint32_t)slot, infection.rules.participants[slot].handle);
	}
	infection_lifecycle_fixture();
	infection_melee_fixture();
	previous = infection.rules.phase;
	infection_rules_tick(&infection.rules);
	if (infection.rules.phase == _infection_preparation && previous != _infection_preparation)
		infection_reset_units();
	if (infection.rules.phase == _infection_active && previous != _infection_active)
	{
		struct object_iterator iterator;
		long vehicles = 0;
		object_iterator_new(&iterator, _object_mask_vehicle, 0);
		while (object_iterator_next(&iterator)) vehicles++;
		/* The variant's vehicle set is none: Infected must not out-run by vehicle. */
		platform_log("infection local: round=%lu active vehicles=%ld",
			(unsigned long)infection.rules.round_id, vehicles);
		infection.active_since = game_time_get();
		infection.test_stage = 0;
	}
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct player_datum *player;
		if (!infection.rules.participants[slot].connected) continue;
		infection_set_team(slot);
		player = player_try_and_get(infection.players[slot]);
		if (!player) continue;
		player->respawn_timer = (long)infection.rules.participants[slot].respawn_ticks;
		if (player->unit_index != NONE && infection.rules.participants[slot].role != _infection_survivor)
		{
			struct unit_datum *unit = unit_get(player->unit_index);
			if (unit_get_weapon_count(player->unit_index)) infection_strip_weapons(player->unit_index);
			unit->unit.grenade_counts[0] = unit->unit.grenade_counts[1] = 0;
		}
	}
	if (previous != infection.rules.phase || game_time_get() % TICKS_PER_SECOND == 0)
	{
		platform_log("infection local: tick=%ld round=%lu phase=%s remaining=%lu survivors=%lu winner=%d",
			game_time_get(), (unsigned long)infection.rules.round_id, infection_rules_phase_name(infection.rules.phase),
			(unsigned long)infection.rules.phase_ticks, (unsigned long)infection_rules_survivors(&infection.rules), infection.rules.winner);
		infection_log_players();
	}
}

static long slayer_control_player(long index)
{
	struct data_iterator iterator;
	long found = 0;
	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
		if (found++ == index) return iterator.datum_index;
	return NONE;
}

/* Ordinary Slayer regression fixture (Infection disabled): a real kill, then
 * Slayer's own score, respawn and default loadout must be unchanged. */
static void infection_slayer_control_update(void)
{
	static short stage;
	static long killed_unit;
	long killer_index = slayer_control_player(0), victim_index = slayer_control_player(1);
	struct player_datum *killer, *victim;
	if (stage >= 2 || !game_in_progress() || !game_engine || game_time_get() < 3 * TICKS_PER_SECOND ||
		killer_index == NONE || victim_index == NONE) return;
	killer = player_get(killer_index);
	victim = player_get(victim_index);
	if (stage == 0 && killer->unit_index != NONE && victim->unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(victim->unit_index);
		platform_log("slayer control: spawn weapons=%d grenades=%d/%d infection_active=%d",
			unit_get_weapon_count(victim->unit_index), unit->unit.grenade_counts[0],
			unit->unit.grenade_counts[1], infection_game_active());
		killed_unit = victim->unit_index;
		damage_kill_object_for_player(victim->unit_index, killer_index);
		stage = 1;
	}
	else if (stage == 1 && victim->unit_index != NONE && victim->unit_index != killed_unit)
	{
		struct unit_datum *unit = unit_get(victim->unit_index);
		platform_log("slayer control: respawn weapons=%d grenades=%d/%d killer_score=%ld victim_score=%ld infection_active=%d",
			unit_get_weapon_count(victim->unit_index), unit->unit.grenade_counts[0], unit->unit.grenade_counts[1],
			game_engine->get_player_score ? game_engine->get_player_score(killer_index, _get_score_individual) : -1,
			game_engine->get_player_score ? game_engine->get_player_score(victim_index, _get_score_individual) : -1,
			infection_game_active());
		stage = 2;
	}
}

void infection_game_test_update(boolean main_menu_loaded, real seconds)
{
	static boolean launched;
	static real waiting;
	char const *map = config_string("debug.infection_test_map");
	char path[128];
	long players;
	boolean control = !csstrcmp(config_string("debug.infection_test_scenario"), "slayer-control");
	if (launched && control) { infection_slayer_control_update(); return; }
	/* The control requires Infection DISABLED: it proves ordinary Slayer
	 * through the same launcher is untouched by the adapter. */
	if (launched || !main_menu_loaded || !*map || config_boolean("infection.local_enabled") == control) return;
	/* A dedicated local launcher, not network_test's host/join modes. */
	if (game_connection() != _game_connection_local || global_network_game_server_get() ||
		global_network_game_client_get() || *config_string("debug.network_test")) return;
	if (bink_playback_in_progress()) { bink_playback_stop(); waiting = 0.f; return; }
	waiting += seconds;
	if (waiting < 2.f) return;
	launched = TRUE;
	players = config_integer("debug.infection_test_players");
	if (players < 2 || players > MAXIMUM_LOCAL_PLAYERS || strspn(map, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(map))
	{
		platform_log("infection local: invalid local test map or player count; not launched");
		return;
	}
	if (snprintf(path, sizeof(path), "levels\\test\\%s\\%s", map, map) >= (int)sizeof(path))
	{
		platform_log("infection local: test map name too long; not launched");
		return;
	}
	player_spawn_count = (short)players;
	game_set_game_variant_from_name("slayer");
	main_set_map_name(path);
	main_disallow_persistent_storage();
	platform_log("infection local: launching %s with %ld local players (no server)%s", map, players,
		control ? " as an ordinary Slayer control" : "");
}

#else /* !HALO_FEATURE_INFECTION: configure.py --infection builds the real code */

/* Infection is compiled out: every engine hook sees it inactive, selection
 * always keeps the original engine, and the launcher never starts. */
boolean infection_game_active(void) { return FALSE; }
boolean infection_game_can_collect_items(long player_index) { (void)player_index; return TRUE; }
struct game_engine *infection_game_select(struct game_engine *original, struct game_variant *variant)
{
	(void)variant;
	return original;
}
boolean infection_game_should_spawn(long player_index) { (void)player_index; return FALSE; }
boolean infection_game_should_end(void) { return FALSE; }
void infection_game_end_tick(void) {}
void infection_game_player_killed(long killer_index, long dead_index) { (void)killer_index; (void)dead_index; }
void infection_game_filter_action(long player_index, struct player_action *action) { (void)player_index; (void)action; }
real infection_game_damage_multiplier(long attacker_index, long victim_index)
{
	(void)attacker_index;
	(void)victim_index;
	return 1.f;
}
void infection_game_color(long player_index, real_rgb_color *color) { (void)player_index; (void)color; }
boolean infection_game_message(long player_index, wchar_t *message, long count)
{
	(void)player_index;
	(void)message;
	(void)count;
	return FALSE;
}
void infection_game_test_update(boolean main_menu_loaded, real seconds) { (void)main_menu_loaded; (void)seconds; }

#endif /* HALO_FEATURE_INFECTION */
