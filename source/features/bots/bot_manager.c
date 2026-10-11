/*
BOT_MANAGER.C

port: the bots' blackboard, after Halo Infinite's BotManager (GDC 2022,
"Thinking Like Players"; docs/bot_decisions.md). It holds:

  - ambitions: the objects and areas that score in the game mode, each with
    the teams that may use it. The game engine stays the authority: an
    adapter for each mode reads its state and writes the ambitions, and a bot
    acts on them only as a player can (walking, picking up, standing in);
  - sightings: each team's last sighting of each enemy, written by the bots
    whose skill shares what they see, read by their teammates. port: local
    humans also publish sustained camera/LOS contacts through human_callouts.c;
    incoming-damage cues are separate bearings, never visual sightings.

Everything here is host-local and derived: it is rebuilt from the game's
state and never sent to another machine.
*/

/* ---------- headers */

#include "cseries.h"
#include "game.h"
#include "game_engine.h"
#include "players.h"
#include "units/units.h"
#include "engine_ai/claims.h"
#include "bot_manager.h"
#include "human_callouts.h" /* port: implicit local-human team awareness */

/* port/linux/src/platform.h's */
void platform_log(char const *format, ...);

#ifdef HALO_FEATURE_BOTS

/* ---------- structures */

struct bot_sighting
{
	long player_index; /* NONE: no sighting */
	long time;
	long last_callout_time;
	real_point3d position;
};

/* ---------- globals */

static struct
{
	short ambition_count;
	struct bot_ambition ambitions[BOT_AMBITIONS_MAXIMUM];
	struct bot_sighting sightings[BOT_MANAGER_TEAMS][HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	struct engine_ai_claims item_claims;
} bot_manager_globals;

/* ---------- private prototypes */

static void bot_manager_publish_ambitions(void);

/* ---------- public code */

void bot_manager_reset(
	void)
{
	short team_index;
	short player_index;

	csmemset(&bot_manager_globals, 0, sizeof(bot_manager_globals));
	engine_ai_claims_reset(&bot_manager_globals.item_claims);
	human_callouts_reset(); /* port: no contacts survive map/game resets */
	for (team_index = 0; team_index < BOT_MANAGER_TEAMS; team_index++)
	{
		for (player_index = 0; player_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; player_index++)
		{
			bot_manager_globals.sightings[team_index][player_index].player_index = NONE;
			bot_manager_globals.sightings[team_index][player_index].last_callout_time = NONE;
		}
	}
}

void bot_manager_update(
	void)
{
	bot_manager_publish_ambitions();
	human_callouts_update(); /* port: human POV contacts before teammates think */
}

short bot_manager_ambition_count(
	void)
{
	return bot_manager_globals.ambition_count;
}

struct bot_ambition const *bot_manager_ambition_get(
	short ambition_index)
{
	if (ambition_index < 0 || ambition_index >= bot_manager_globals.ambition_count)
		return NULL;
	return &bot_manager_globals.ambitions[ambition_index];
}

boolean bot_manager_ambition_allowed(
	struct bot_ambition const *ambition,
	long team_index)
{
	if (!ambition || ambition->kind == _bot_ambition_none)
		return FALSE;
	if (!game_engine_has_teams())
		return TRUE;
	if (team_index < 0 || team_index >= 32)
		return FALSE;
	return (ambition->team_mask & (1UL << team_index)) != 0;
}

void bot_manager_report_sighting(
	long team_index,
	long enemy_player_index,
	short reporter_bot_number,
	real_point3d const *position)
{
	struct bot_sighting *sighting;
	long absolute_index;
	long now;
	boolean callout;

	if (!game_engine_has_teams() || team_index < 0 || team_index >= BOT_MANAGER_TEAMS ||
		enemy_player_index == NONE || !position)
	{
		return;
	}
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(enemy_player_index);
	if (absolute_index < 0 || absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		return;
	sighting = &bot_manager_globals.sightings[team_index][absolute_index];
	now = game_time_get();
	callout = sighting->player_index != enemy_player_index ||
		sighting->last_callout_time == NONE || now < sighting->last_callout_time ||
		now - sighting->last_callout_time >= 4 * TICKS_PER_SECOND ||
		distance3d(position, &sighting->position) >= 8.f;
	if (callout)
	{
		/* port: zero denotes an implicit human callout, not Bot 0. */
		if (reporter_bot_number == 0)
			platform_log("bots: human called out enemy %ld last seen at (%.1f %.1f %.1f)",
				absolute_index, position->x, position->y, position->z);
		else
			platform_log("bots: bot %d called out enemy %ld last seen at (%.1f %.1f %.1f)",
				reporter_bot_number, absolute_index, position->x, position->y, position->z);
		sighting->last_callout_time = now;
	}
	sighting->player_index = enemy_player_index;
	sighting->time = now;
	sighting->position = *position;
}

boolean bot_manager_nearest_sighting(
	long team_index,
	real_point3d const *from,
	long maximum_age_ticks,
	long *enemy_player_index,
	real_point3d *position)
{
	long now = game_time_get();
	long best_index = NONE;
	real best_distance = 0.f;
	short absolute_index;

	if (!game_engine_has_teams() || team_index < 0 || team_index >= BOT_MANAGER_TEAMS || !from)
		return FALSE;
	for (absolute_index = 0; absolute_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; absolute_index++)
	{
		struct bot_sighting *sighting = &bot_manager_globals.sightings[team_index][absolute_index];
		struct player_datum *player;
		struct unit_datum *unit;
		real distance;

		if (sighting->player_index == NONE)
			continue;
		player = player_try_and_get(sighting->player_index);
		unit = player && player->unit_index != NONE ? unit_try_and_get(player->unit_index) : NULL;
		/* (stale, gone, dead, or now a teammate) */
		if (now - sighting->time > maximum_age_ticks || now < sighting->time || !unit ||
			TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || player->team_index == team_index)
		{
			sighting->player_index = NONE;
			continue;
		}
		distance = distance3d(from, &sighting->position);
		if (best_index == NONE || distance < best_distance)
		{
			best_index = absolute_index;
			best_distance = distance;
		}
	}
	if (best_index == NONE)
		return FALSE;
	if (enemy_player_index)
		*enemy_player_index = bot_manager_globals.sightings[team_index][best_index].player_index;
	if (position)
		*position = bot_manager_globals.sightings[team_index][best_index].position;
	return TRUE;
}

static boolean bot_manager_claim_scope(
	long team_index,
	uint32_t *claim_scope)
{
	assert(claim_scope);
	if (!game_engine_has_teams())
	{
		*claim_scope = 0;
		return TRUE;
	}
	if (team_index < 0 || team_index >= BOT_MANAGER_TEAMS)
		return FALSE;
	*claim_scope = (uint32_t)team_index;
	return TRUE;
}

/* Claims are scoped to a team and keyed by the object's salted datum index. */
boolean bot_manager_claim_item(
	long team_index,
	long object_index,
	short bot_number,
	long duration_ticks)
{
	long now = game_time_get();
	uint32_t claim_scope;

	if (object_index == NONE || bot_number <= 0 || duration_ticks <= 0 || now < 0 ||
		!bot_manager_claim_scope(team_index, &claim_scope))
		return FALSE;
	return engine_ai_claims_acquire(&bot_manager_globals.item_claims, claim_scope,
		(uint32_t)object_index, (uint32_t)bot_number, (uint64_t)now, (uint64_t)duration_ticks);
}

boolean bot_manager_item_claimed_by_other(
	long team_index,
	long object_index,
	short bot_number)
{
	long now = game_time_get();
	uint32_t claim_scope;

	if (object_index == NONE || bot_number <= 0 || now < 0 ||
		!bot_manager_claim_scope(team_index, &claim_scope))
		return FALSE;
	return engine_ai_claims_held_by_other(&bot_manager_globals.item_claims, claim_scope,
		(uint32_t)object_index, (uint32_t)bot_number, (uint64_t)now);
}

void bot_manager_release_item(
	long team_index,
	long object_index,
	short bot_number)
{
	uint32_t claim_scope;

	if (object_index == NONE || bot_number <= 0 ||
		!bot_manager_claim_scope(team_index, &claim_scope))
		return;
	engine_ai_claims_release(&bot_manager_globals.item_claims, claim_scope,
		(uint32_t)object_index, (uint32_t)bot_number);
}

void bot_manager_release_bot(
	long team_index,
	short bot_number)
{
	uint32_t claim_scope;

	if (bot_number <= 0 || !bot_manager_claim_scope(team_index, &claim_scope))
		return;
	engine_ai_claims_release_owner(&bot_manager_globals.item_claims, claim_scope,
		(uint32_t)bot_number);
}

/* ---------- private code: the game modes' adapters */

/* Slayer scores by kills alone: no ambitions. The objective modes' adapters
(capture the flag, oddball, king of the hill, race) come with read-only
accessors for their game engines' state; until then those modes keep bots out
(bots_host_may_have_bots). */
static void bot_manager_publish_ambitions(
	void)
{
	bot_manager_globals.ambition_count = 0;
	if (!game_engine_get_variant())
		return;
	switch (game_engine_get_variant()->game_engine_index)
	{
	case game_engine_slayer:
	default:
		break;
	}
}

#endif /* HALO_FEATURE_BOTS */
