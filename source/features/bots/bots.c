/*
BOTS.C

port: computer-controlled multiplayer players ("bots").

A bot is a real player: a player datum, with a team, a spawn, kills, deaths
and a score, whose unit the game engine spawns and respawns as it does
everyone's. It plays through the same pipeline as a person: each tick it
decides a player_action, which the host puts in its next update for the
player (update_server_set_player_action), exactly where a local player's
input goes, and players_update_before_game turns into unit control. Bots have
no extra health, damage or knowledge: skill changes reaction time, aim error,
turn speed, sight and tactics only, and a bot fights only enemies it has seen
or that have just hurt it.

Decisions use the game-independent AI primitives (source/engine_ai):
  - behavior.h: a priority tree, VEHICLE > RETREAT > SCAVENGE > FIGHT > ROAM,
    reevaluation so that a roaming bot turns to fight as soon as it sees an
    enemy, and a fighting one falls back when its shields are down;
  - behavior_intent.h / intent.h: the leaves emit intents (look, move, fire,
    jump, grenade, interact, exit vehicle), which a Halo-owned executor validates against the
    live player and unit before they become the player's action;
  - navigation.h / traversal.h: an incremental route search over a graph
    built from the map's player starting locations, linked where a biped can
    walk straight between them, and per-bot route following.

Scope (a first, small slice):
  - only the host decides for its bots, and only in a game nobody else is in:
    a custom game you host by yourself (or a local one). Bots never join a
    game another machine is in, and leave if one joins. They are not
    replicated to other machines yet;
  - a game a bot has played in is never reported to the game list or Delta
    Stats (bots_game_had_bots);
  - free for all and team Slayer; reachable weapon upgrades and empty ground
    vehicle seats, with preference for a gunner when a friendly driver is in;
  - basic ground driving only; no aircraft, objectives or advanced vehicle paths.
    debug.bot_sandbox is an opt-in test teleport, never normal spawn policy.

Configuration (port/linux/src/port_config.c): bots.count (0..31), bots.skill.
*/

/* ---------- headers */

#ifdef HALO_TRACE_ENABLED
#include "../../../port/linux/src/halo_trace.h"
#endif

#include "cseries.h"
#include "game.h"
#include "game_engine.h"
#include "players.h"
#include "players_runtime.h"
#include "player_queues_new.h"
#include "bots.h"
#include "navigation_probe.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "real_math.h"
#include "scenario/scenario_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicles.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "engine_ai/fire_control.h"
#include "engine_ai/behavior.h"
#include "engine_ai/behavior_intent.h"
#include "engine_ai/intent.h"
#include "engine_ai/navigation.h"
#include "engine_ai/traversal.h"
#include "extensions/extension_api.h"
#include "navigation_world.h"

#ifdef HALO_FEATURE_BOTS

/* network_game_globals.c's */
boolean network_game_distributed_client(void);
/* port/linux/game/network_distributed.c's */
short distributed_client_machines(long *machine_indices, short maximum);
/* port/linux/src/port_config.c's */
long config_integer(char const *name);
int config_boolean(char const *name);
char const *config_string(char const *name);
/* port/linux/src/platform.h's */
void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	/* (a 32-player lobby: the host and 31 bots) */
	BOTS_MAXIMUM = 31,
	/* the machines bots belong to: no real machine's, so that nothing that
	works through the machines (their connections, their players' input) takes
	them for anyone's. A machine's player list has room for
	MAXIMUM_LOCAL_PLAYERS, so bots fill machines down from the last: bot n
	is on BOT_MACHINE_INDEX - n / MAXIMUM_LOCAL_PLAYERS. Bots never join a
	game another machine is in, so these never meet a real machine. */
	BOT_MACHINE_INDEX = HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1,
	BOT_MACHINE_COUNT = (BOTS_MAXIMUM + MAXIMUM_LOCAL_PLAYERS - 1) / MAXIMUM_LOCAL_PLAYERS,
	BOT_PROGRESS_CHECK_TICKS = TICKS_PER_SECOND,
	BOT_NAV_NEIGHBORS = 6,
	BOT_NAV_EXPANSIONS_PER_TICK = 64,
	BOT_BEHAVIOR_WORK_PER_TICK = 32,
	BOT_INTENT_SCHEMA = 1,
};

/* the behavior tree's leaves */
enum
{
	_bot_leaf_retreat = 1,
	_bot_leaf_fight,
	_bot_leaf_roam,
	_bot_leaf_scavenge,
	_bot_leaf_vehicle,
};

/* the intents the leaves emit, and the executor takes */
enum
{
	_bot_intent_look = 1,
	_bot_intent_move,
	_bot_intent_fire,
	_bot_intent_jump,
	_bot_intent_grenade,
	_bot_intent_melee,
	_bot_intent_interact,
	_bot_intent_exit_vehicle,
};

/* why the executor turned an intent down */
enum
{
	_bot_reject_none = 0,
	_bot_reject_no_player,
	_bot_reject_no_unit,
	_bot_reject_bad_payload,
	_bot_reject_no_grenades,
	_bot_reject_unknown,
};

#define BOT_NAV_LINK_RANGE 30.f
#define BOT_NAV_NODE_MERGE_DISTANCE 1.f
#define BOT_NAV_EYE_HEIGHT 0.4f
#define BOT_WAYPOINT_REACHED_DISTANCE 1.2f
#define BOT_MINIMUM_ROAM_DISTANCE 12.f
#define BOT_TARGET_MEMORY_SECONDS 4.f
#define BOT_ATTACKER_AWARENESS_TICKS 15
#define BOT_GRENADE_COOLDOWN_SECONDS 8.f

/* ---------- structures */

struct bot_skill
{
	char const *name;
	real reaction_seconds;
	real aim_error_degrees;
	real turn_degrees_per_second;
	real sight_range;
	real field_of_view_degrees; /* half angle */
	real fire_tolerance_degrees;
	real burst_seconds; /* 0: no pauses */
	real burst_pause_seconds;
	boolean strafes;
	boolean throws_grenades;
	boolean retreats;
};

struct bot_look_payload
{
	real yaw;
	real pitch;
};

struct bot_move_payload
{
	real yaw; /* world direction */
	real speed; /* 0..1 */
};

struct bot_fire_payload
{
	real trigger;
};

/* what this tick's accepted intents add up to */
struct bot_pending_action
{
	boolean has_look;
	real_euler_angles2d look;
	boolean has_move;
	real move_yaw;
	real move_speed;
	real trigger;
	unsigned long control_flags;
};

struct bot
{
	boolean active;
	short slot;
	long player_index;
	long unit_index; /* the unit this life's state is for */

	struct engine_ai_rng32_state rng;
	engine_ai_behavior_state behavior;
	struct engine_ai_behavior_intent_state bridge;
	struct engine_ai_intent_batch batch;
	struct engine_ai_intent_report report;

	/* perception */
	long target_player_index;
	boolean target_visible;
	long target_first_seen_time;
	long target_last_seen_time;
	real_point3d target_last_position;

	/* combat */
	real_euler_angles2d facing;
	real_euler_angles2d aim_error;
	long aim_error_time;
	real strafe_sign;
	long strafe_change_time;
	long burst_change_time;
	boolean burst_firing;
	long grenade_time;
	long melee_time;
	long fire_weapon_index;
	struct engine_ai_fire_control fire_control;
	boolean charging;
	long charge_target_index;
	long next_charge_time;
	long retreat_start_time;
	long retreat_cooldown_time;

	/* navigation */
	boolean searching;
	boolean following;
	uint32_t goal_node;
	struct engine_ai_nav_search search;
	struct engine_ai_traversal_state traversal;
	real_point3d progress_position;
	long progress_time;
	short stuck_count;
	boolean jump_requested;

	/* Bounded opportunity search; handles are revalidated before use. */
	long opportunity_index;
	boolean opportunity_vehicle;
	short opportunity_seat;
	long opportunity_scan_time;
	long opportunity_end_time;
	long interaction_time;
	long observed_vehicle_index;
	long observed_weapons[MAXIMUM_WEAPONS_PER_UNIT];
	long vehicle_progress_time;
	real_point3d vehicle_progress_position;

	struct bot_pending_action pending;
};

struct bot_navigation
{
	boolean built;
	uint32_t node_count;
	real_point3d nodes[ENGINE_AI_NAV_NODES_MAX];
	struct engine_ai_nav_link links[ENGINE_AI_NAV_LINKS_MAX];
	struct engine_ai_nav_graph graph;
};

/* ---------- globals */

static struct bot_skill const bot_skills[] =
{
	/* name, reaction, aim error, turn speed, sight, fov, tolerance, burst, pause, strafe, grenades, retreat */
	{ "recruit", 0.9f, 7.f, 200.f, 20.f, 55.f, 10.f, 0.6f, 0.8f, FALSE, FALSE, FALSE },
	{ "marine", 0.5f, 4.f, 320.f, 28.f, 65.f, 8.f, 1.0f, 0.5f, TRUE, TRUE, FALSE },
	{ "odst", 0.3f, 2.5f, 450.f, 35.f, 75.f, 6.f, 1.5f, 0.3f, TRUE, TRUE, TRUE },
	{ "spartan", 0.18f, 1.5f, 720.f, 45.f, 85.f, 5.f, 1.2f, 0.45f, TRUE, TRUE, TRUE },
};

/* VEHICLE > RETREAT > SCAVENGE (safe/nearby) > FIGHT > ROAM: reevaluation
lets an earlier one interrupt a later one that is running */
static engine_ai_behavior_node const bot_behavior_nodes[] =
{
	{ ENGINE_AI_BEHAVIOR_PRIORITY, 1, 5, 0 },
	{ ENGINE_AI_BEHAVIOR_LEAF, 0, 0, _bot_leaf_vehicle },
	{ ENGINE_AI_BEHAVIOR_LEAF, 0, 0, _bot_leaf_retreat },
	{ ENGINE_AI_BEHAVIOR_LEAF, 0, 0, _bot_leaf_scavenge },
	{ ENGINE_AI_BEHAVIOR_LEAF, 0, 0, _bot_leaf_fight },
	{ ENGINE_AI_BEHAVIOR_LEAF, 0, 0, _bot_leaf_roam },
};

static engine_ai_behavior_definition const bot_behavior =
{
	bot_behavior_nodes,
	NUMBEROF(bot_behavior_nodes),
	0
};

static struct
{
	boolean behavior_valid;
	boolean had_bots;
	boolean sandbox_done;
	short desired_count;
	struct bot_skill const *skill;
	uint64_t epoch;
	struct bot bots[BOTS_MAXIMUM];
	struct bot_navigation navigation;
} bots_globals;

/* ---------- private prototypes */

static boolean bots_host_may_have_bots(void);
static void bots_refresh(void);
static void bots_join(void);
static boolean bot_join(struct bot *bot, short bot_index);
static void bot_leave(struct bot *bot);
static void bot_think(struct bot *bot);

/* ---------- public code */

void bots_initialize_for_new_map(
	void)
{
	char const *skill_name = config_string("bots.skill");
	short skill_index;
	long count = config_integer("bots.count");

	csmemset(&bots_globals.bots, 0, sizeof(bots_globals.bots));
	csmemset(&bots_globals.navigation, 0, sizeof(bots_globals.navigation));
	bots_globals.had_bots = FALSE;
	bots_globals.sandbox_done = FALSE;
	bots_globals.epoch++;
	bots_globals.desired_count = (short)PIN(count, 0, BOTS_MAXIMUM);
	bots_globals.skill = &bot_skills[1];
	for (skill_index = 0; skill_index < NUMBEROF(bot_skills); skill_index++)
	{
		if (skill_name && !csstrcasecmp(skill_name, bot_skills[skill_index].name))
			bots_globals.skill = &bot_skills[skill_index];
	}
	bots_globals.behavior_valid =
		engine_ai_behavior_validate(&bot_behavior) == ENGINE_AI_BEHAVIOR_VALID;
}

void bots_update(
	void)
{
	short bot_index;

	if (bots_globals.desired_count == 0 && !bots_globals.had_bots)
		return;
	if (!bots_globals.behavior_valid)
		return;
	bots_refresh();
	if (!bots_host_may_have_bots())
	{
		/* The mode, host, remote-machine or postgame gate no longer permits
		bots. They are not replicated to other machines. */
		for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
		{
			if (bots_globals.bots[bot_index].active)
				bot_leave(&bots_globals.bots[bot_index]);
		}
		return;
	}
	bots_join();
	for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
	{
		if (bots_globals.bots[bot_index].active)
			bot_think(&bots_globals.bots[bot_index]);
	}
}

boolean bots_game_had_bots(
	void)
{
	return bots_globals.had_bots;
}

boolean bots_player_is_bot(
	long player_index)
{
	short bot_index;

	for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
	{
		if (bots_globals.bots[bot_index].active && bots_globals.bots[bot_index].player_index == player_index)
			return TRUE;
	}
	return FALSE;
}

/* ---------- private code: math */

static real bot_angle_normalize(
	real angle)
{
	while (angle > (real)M_PI)
		angle -= 2.f * (real)M_PI;
	while (angle < -(real)M_PI)
		angle += 2.f * (real)M_PI;
	return angle;
}

static real bot_horizontal_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	real i = b->x - a->x;
	real j = b->y - a->y;

	return square_root(i * i + j * j);
}

static real bot_yaw_to(
	real_point3d const *from,
	real_point3d const *to)
{
	return arctangent(to->y - from->y, to->x - from->x);
}

static real_euler_angles2d bot_angles_to(
	real_point3d const *from,
	real_point3d const *to)
{
	real_vector3d vector;
	real_euler_angles2d angles;

	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	if (vector.i == 0.f && vector.j == 0.f && vector.k == 0.f)
		vector.i = 1.f;
	euler_angles2d_from_vector3d(&angles, &vector);
	return angles;
}

/* [0, 1) from the bot's own generator: bots never draw from the game's
random numbers, which every machine's simulation shares */
static real bot_random(
	struct bot *bot)
{
	uint32_t value = 0;

	engine_ai_rng32_next(&bot->rng, &value);
	return (real)((double)value / 4294967296.0);
}

static real bot_random_range(
	struct bot *bot,
	real minimum,
	real maximum)
{
	return minimum + (maximum - minimum) * bot_random(bot);
}

static long bot_seconds_to_ticks(
	real seconds)
{
	return (long)(seconds * TICKS_PER_SECOND + 0.5f);
}

/* ---------- private code: the world as a bot may know it */

static struct unit_datum *bot_living_unit(
	long unit_index)
{
	struct unit_datum *unit;

	if (unit_index == NONE)
		return NULL;
	unit = unit_try_and_get(unit_index);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return NULL;
	return unit;
}

/* the machine of the bot in a slot */
static long bot_machine_index(
	short slot)
{
	return BOT_MACHINE_INDEX - slot / MAXIMUM_LOCAL_PLAYERS;
}

static struct player_datum *bot_player(
	struct bot const *bot)
{
	struct player_datum *player = player_try_and_get(bot->player_index);

	if (!player || player->network_player_data.machine_index != bot_machine_index(bot->slot) ||
		player->quit_out_of_game)
		return NULL;
	return player;
}

static boolean bot_line_clear(
	unsigned long flags,
	real_point3d const *from,
	real_point3d const *to,
	long ignore_object_index)
{
	struct collision_result collision;
	real_vector3d vector;

	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	return !collision_test_vector(flags, from, &vector, ignore_object_index, &collision);
}

static void bot_aim_point(
	long unit_index,
	real_point3d *point)
{
	unit_get_head_position(unit_index, point);
	/* (the chest, below the head) */
	point->z -= 0.12f;
}

static boolean bot_players_are_enemies(
	struct player_datum const *player,
	long player_index,
	struct player_datum const *other,
	long other_index)
{
	if (player_index == other_index)
		return FALSE;
	if (game_engine_has_teams())
		return player->team_index != other->team_index;
	return TRUE;
}

/* ---------- private code: navigation */

#define BOT_NAV_FLAGS (FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit))
#define BOT_NAV_PROBE_STEP 0.75f
#define BOT_NAV_MAXIMUM_CLIMB 0.6f
#define BOT_NAV_MAXIMUM_DROP 3.f

/* the ground's height below the point, within depth */
static boolean bot_navigation_ground(
	real_point3d const *above,
	real depth,
	real *ground_z)
{
	struct collision_result collision;
	real_vector3d down;

	down.i = 0.f;
	down.j = 0.f;
	down.k = -depth;
	if (!collision_test_vector(BOT_NAV_FLAGS, above, &down, NONE, &collision))
		return FALSE;
	*ground_z = collision.point.z;
	return TRUE;
}

/* whether a biped can walk from a to b: stepping along the ground, nothing
in the way at its hips, no step up higher than it can climb, no gap and no
drop it would not survive. Directional: a drop is walkable one way only */
static boolean bot_navigation_walkable(
	real_point3d const *a,
	real_point3d const *b)
{
	real horizontal = bot_horizontal_distance(a, b);
	short steps = (short)PIN((long)(horizontal / BOT_NAV_PROBE_STEP) + 1, 1, 64);
	real_point3d probe;
	real ground_z;
	real end_z;
	short step;

	probe = *a;
	probe.z += 0.5f;
	if (!bot_navigation_ground(&probe, 2.5f, &ground_z))
		return FALSE;
	probe = *b;
	probe.z += 0.5f;
	if (!bot_navigation_ground(&probe, 2.5f, &end_z))
		return FALSE;
	for (step = 1; step <= steps; step++)
	{
		real t = (real)step / (real)steps;
		real_point3d from;
		real_point3d to;
		real next_z;

		from.x = a->x + (b->x - a->x) * (real)(step - 1) / (real)steps;
		from.y = a->y + (b->y - a->y) * (real)(step - 1) / (real)steps;
		from.z = ground_z + BOT_NAV_EYE_HEIGHT;
		to.x = a->x + (b->x - a->x) * t;
		to.y = a->y + (b->y - a->y) * t;
		to.z = from.z;
		/* (a wall, or a slope too steep) */
		if (!bot_line_clear(BOT_NAV_FLAGS, &from, &to, NONE))
			return FALSE;
		to.z = ground_z + BOT_NAV_MAXIMUM_CLIMB;
		if (!bot_navigation_ground(&to, BOT_NAV_MAXIMUM_CLIMB + BOT_NAV_MAXIMUM_DROP, &next_z))
			return FALSE;
		ground_z = next_z;
	}
	return (real)fabs(ground_z - end_z) < 1.f;
}

static boolean bot_navigation_linked(
	struct bot_navigation const *navigation,
	uint32_t link_count,
	uint32_t source,
	uint32_t destination)
{
	uint32_t index;

	for (index = 0; index < link_count; index++)
	{
		if (navigation->links[index].source_node == source &&
			navigation->links[index].destination_node == destination)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void bot_navigation_add_link(
	struct bot_navigation *navigation,
	uint32_t *link_count,
	uint32_t source,
	uint32_t destination,
	real distance)
{
	struct engine_ai_nav_link *link;

	if (*link_count >= ENGINE_AI_NAV_LINKS_MAX)
		return;
	link = &navigation->links[(*link_count)++];
	link->source_node = source;
	link->destination_node = destination;
	link->cost = (uint32_t)(distance * 100.f) + 1;
	link->required_capabilities = 0;
	link->traversal_id = 0;
	link->enabled = true;
}

/* the map's graph: its player starting locations (of every game type), each
linked both ways to its nearest few that a biped can walk to in a straight
line. Built once a map, when its first bot needs it */
static void bot_navigation_build(
	void)
{
	struct bot_navigation *navigation = &bots_globals.navigation;
	short location_count = player_get_starting_location_count();
	short location_index;
	uint32_t node;
	uint32_t link_count = 0;

	navigation->built = TRUE;
	navigation->node_count = 0;
	for (location_index = 0; location_index < location_count &&
		navigation->node_count < ENGINE_AI_NAV_NODES_MAX; location_index++)
	{
		struct player_starting_location const *location = player_get_starting_location(location_index);
		boolean duplicate = FALSE;

		if (!location)
			continue;
		for (node = 0; node < navigation->node_count; node++)
		{
			if (distance3d(&navigation->nodes[node], &location->position) < BOT_NAV_NODE_MERGE_DISTANCE)
				duplicate = TRUE;
		}
		if (!duplicate)
			navigation->nodes[navigation->node_count++] = location->position;
	}

	for (node = 0; node < navigation->node_count; node++)
	{
		/* (some starting locations are a little above the ground) */
		real_point3d probe = navigation->nodes[node];
		real ground_z;

		probe.z += 0.5f;
		if (bot_navigation_ground(&probe, 3.f, &ground_z))
			navigation->nodes[node].z = ground_z + 0.05f;
	}

	for (node = 0; node < navigation->node_count; node++)
	{
		uint32_t nearest[BOT_NAV_NEIGHBORS];
		real nearest_distance[BOT_NAV_NEIGHBORS];
		short nearest_count = 0;
		uint32_t other;
		short index;

		/* (the nearest in range, nearest first) */
		for (other = 0; other < navigation->node_count; other++)
		{
			real distance;
			short position;

			if (other == node)
				continue;
			distance = distance3d(&navigation->nodes[node], &navigation->nodes[other]);
			if (distance > BOT_NAV_LINK_RANGE)
				continue;
			for (position = nearest_count; position > 0 && nearest_distance[position - 1] > distance; position--)
			{
				if (position < BOT_NAV_NEIGHBORS)
				{
					nearest[position] = nearest[position - 1];
					nearest_distance[position] = nearest_distance[position - 1];
				}
			}
			if (position < BOT_NAV_NEIGHBORS)
			{
				nearest[position] = other;
				nearest_distance[position] = distance;
				if (nearest_count < BOT_NAV_NEIGHBORS)
					nearest_count++;
			}
		}
		for (index = 0; index < nearest_count; index++)
		{
			other = nearest[index];
			if (!bot_navigation_linked(navigation, link_count, node, other) &&
				bot_navigation_walkable(&navigation->nodes[node], &navigation->nodes[other]))
			{
				bot_navigation_add_link(navigation, &link_count, node, other, nearest_distance[index]);
			}
			if (!bot_navigation_linked(navigation, link_count, other, node) &&
				bot_navigation_walkable(&navigation->nodes[other], &navigation->nodes[node]))
			{
				bot_navigation_add_link(navigation, &link_count, other, node, nearest_distance[index]);
			}
		}
	}

	navigation->graph.links = navigation->links;
	navigation->graph.node_count = navigation->node_count;
	navigation->graph.link_count = link_count;
	navigation->graph.revision = bots_globals.epoch;
	platform_log("bots: navigation graph of %lu nodes and %lu links",
		(unsigned long)navigation->node_count, (unsigned long)link_count);
}

static struct engine_ai_nav_graph const *bot_navigation_graph(
	void)
{
	if (!bots_globals.navigation.built)
		bot_navigation_build();
	return bots_globals.navigation.node_count > 0 ? &bots_globals.navigation.graph : NULL;
}

/* the nearest node the point can walk to (of the few nearest), else the
nearest; NONE for none */
static long bot_navigation_nearest_node(
	real_point3d const *point)
{
	struct bot_navigation const *navigation = &bots_globals.navigation;
	uint32_t nearest[BOT_NAV_NEIGHBORS];
	real nearest_distance[BOT_NAV_NEIGHBORS];
	short nearest_count = 0;
	uint32_t node;
	short index;

	for (node = 0; node < navigation->node_count; node++)
	{
		real distance = distance3d(point, &navigation->nodes[node]);
		short position;

		for (position = nearest_count; position > 0 && nearest_distance[position - 1] > distance; position--)
		{
			if (position < BOT_NAV_NEIGHBORS)
			{
				nearest[position] = nearest[position - 1];
				nearest_distance[position] = nearest_distance[position - 1];
			}
		}
		if (position < BOT_NAV_NEIGHBORS)
		{
			nearest[position] = node;
			nearest_distance[position] = distance;
			if (nearest_count < BOT_NAV_NEIGHBORS)
				nearest_count++;
		}
	}
	for (index = 0; index < nearest_count; index++)
	{
		if (bot_navigation_walkable(point, &navigation->nodes[nearest[index]]))
			return (long)nearest[index];
	}
	return nearest_count > 0 ? (long)nearest[0] : NONE;
}

/* a link a bot could not get along: off, in a new revision of the graph
(every route through the old one is replanned: engine_ai_traversal_step) */
static void bot_navigation_disable_link(
	uint32_t link_index)
{
	struct bot_navigation *navigation = &bots_globals.navigation;

	if (link_index >= navigation->graph.link_count || !navigation->links[link_index].enabled)
		return;
	navigation->links[link_index].enabled = false;
	navigation->graph.revision++;
	platform_log("bots: link %lu (node %lu to %lu) is not walkable: off", (unsigned long)link_index,
		(unsigned long)navigation->links[link_index].source_node,
		(unsigned long)navigation->links[link_index].destination_node);
}

static void bot_navigation_reset(
	struct bot *bot)
{
	bot->searching = FALSE;
	bot->following = FALSE;
	bot->stuck_count = 0;
	bot->progress_time = NONE;
}

/* ---------- private code: perception */

static boolean bot_can_see(
	struct bot const *bot,
	long unit_index,
	real_point3d const *eye,
	real_point3d const *point)
{
	struct bot_skill const *skill = bots_globals.skill;
	real distance = distance3d(eye, point);
	real_euler_angles2d angles;
	real yaw_offset;

	if (distance > skill->sight_range)
		return FALSE;
	angles = bot_angles_to(eye, point);
	yaw_offset = (real)fabs(bot_angle_normalize(angles.yaw - bot->facing.yaw));
	/* (out of view, unless close enough to hear) */
	if (yaw_offset > DEGREES_TO_RADIANS(skill->field_of_view_degrees) && distance > 3.f)
		return FALSE;
	return bot_line_clear(_collision_test_for_line_of_sight_flags, eye, point, unit_index);
}

/* what the bot knows of its enemies: the nearest it sees (the one it already
fights first, while it sees it), or one that has just hurt it */
static void bot_perceive(
	struct bot *bot,
	struct player_datum const *player,
	struct unit_datum const *unit)
{
	long now = game_time_get();
	struct data_iterator iterator;
	struct player_datum *other;
	real_point3d eye;
	long best_index = NONE;
	real best_distance = 0.f;
	real_point3d best_position;
	short attacker_index;

	unit_get_camera_position(bot->unit_index, &eye);
	data_iterator_new(&iterator, player_data);
	while ((other = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		real_point3d point;
		real distance;

		if (!bot_players_are_enemies(player, bot->player_index, other, iterator.datum_index) ||
			!bot_living_unit(other->unit_index))
		{
			continue;
		}
		bot_aim_point(other->unit_index, &point);
		if (!bot_can_see(bot, bot->unit_index, &eye, &point))
			continue;
		distance = distance3d(&eye, &point);
		/* (the current target is kept while it is seen) */
		if (iterator.datum_index == bot->target_player_index)
			distance *= 0.5f;
		if (best_index == NONE || distance < best_distance)
		{
			best_index = iterator.datum_index;
			best_distance = distance;
			best_position = point;
		}
	}

	/* (an enemy that has just hurt it, seen or not: it turns to it) */
	if (best_index == NONE)
	{
		for (attacker_index = 0; attacker_index < MAXIMUM_ATTACKERS_PER_UNIT; attacker_index++)
		{
			struct unit_attacker const *attacker = &unit->unit.attackers[attacker_index];
			struct player_datum *attacking_player;

			if (attacker->player_index == NONE ||
				now - (long)attacker->game_time_stamp > BOT_ATTACKER_AWARENESS_TICKS)
			{
				continue;
			}
			attacking_player = player_try_and_get(attacker->player_index);
			if (!attacking_player ||
				!bot_players_are_enemies(player, bot->player_index, attacking_player, attacker->player_index) ||
				!bot_living_unit(attacking_player->unit_index))
			{
				continue;
			}
			bot_aim_point(attacking_player->unit_index, &bot->target_last_position);
			if (bot->target_player_index != attacker->player_index)
				bot->target_first_seen_time = now;
			bot->target_player_index = attacker->player_index;
			bot->target_last_seen_time = now;
			bot->target_visible = FALSE;
			break;
		}
	}

	if (best_index != NONE)
	{
		if (best_index != bot->target_player_index || !bot->target_visible)
			bot->target_first_seen_time = now;
		bot->target_player_index = best_index;
		bot->target_visible = TRUE;
		bot->target_last_seen_time = now;
		bot->target_last_position = best_position;
	}
	else
	{
		struct player_datum *target = bot->target_player_index != NONE ?
			player_try_and_get(bot->target_player_index) : NULL;

		bot->target_visible = FALSE;
		/* (forgotten after a while, or once it is dead) */
		if (!target || !bot_living_unit(target->unit_index) ||
			now - bot->target_last_seen_time > bot_seconds_to_ticks(BOT_TARGET_MEMORY_SECONDS))
		{
			bot->target_player_index = NONE;
		}
	}
}

/* ---------- private code: behavior leaves */

static void bot_emit_look(
	struct engine_ai_behavior_intent_emitter *emitter,
	real_euler_angles2d const *angles)
{
	struct bot_look_payload payload;

	payload.yaw = angles->yaw;
	payload.pitch = angles->pitch;
	engine_ai_behavior_intent_emit(emitter, _bot_intent_look, BOT_INTENT_SCHEMA, &payload, sizeof(payload));
}

static void bot_emit_move(
	struct engine_ai_behavior_intent_emitter *emitter,
	real yaw,
	real speed)
{
	struct bot_move_payload payload;

	payload.yaw = yaw;
	payload.speed = speed;
	engine_ai_behavior_intent_emit(emitter, _bot_intent_move, BOT_INTENT_SCHEMA, &payload, sizeof(payload));
}

static void bot_emit_fire(
	struct engine_ai_behavior_intent_emitter *emitter,
	real trigger)
{
	struct bot_fire_payload payload;

	payload.trigger = trigger;
	engine_ai_behavior_intent_emit(emitter, _bot_intent_fire, BOT_INTENT_SCHEMA, &payload, sizeof(payload));
}

/* aim at the target with this skill's error, and shoot when the aim is near
enough, after its reaction time, in bursts */
static void bot_engage(
	struct bot *bot,
	real_point3d const *eye,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct bot_skill const *skill = bots_globals.skill;
	long now = game_time_get();
	real_euler_angles2d aim = bot_angles_to(eye, &bot->target_last_position);
	real distance = distance3d(eye, &bot->target_last_position);
	real offset;

	if (now >= bot->aim_error_time)
	{
		real error = DEGREES_TO_RADIANS(skill->aim_error_degrees);

		bot->aim_error.yaw = bot_random_range(bot, -error, error);
		bot->aim_error.pitch = bot_random_range(bot, -error, error) * 0.5f;
		bot->aim_error_time = now + bot_seconds_to_ticks(bot_random_range(bot, 0.3f, 0.7f));
	}
	aim.yaw = bot_angle_normalize(aim.yaw + bot->aim_error.yaw);
	aim.pitch += bot->aim_error.pitch;
	bot_emit_look(emitter, &aim);

	if (!bot->target_visible ||
		now - bot->target_first_seen_time < bot_seconds_to_ticks(skill->reaction_seconds) ||
		distance > skill->sight_range)
	{
		return;
	}
	offset = (real)fabs(bot_angle_normalize(aim.yaw - bot->facing.yaw)) + (real)fabs(aim.pitch - bot->facing.pitch);
	if (offset > DEGREES_TO_RADIANS(skill->fire_tolerance_degrees))
		return;
	if (skill->burst_seconds > 0.f && now >= bot->burst_change_time)
	{
		bot->burst_firing = !bot->burst_firing;
		bot->burst_change_time = now + bot_seconds_to_ticks(bot->burst_firing ?
			bot_random_range(bot, 0.5f, 1.f) * skill->burst_seconds :
			bot_random_range(bot, 0.5f, 1.f) * skill->burst_pause_seconds);
	}
	if (bot_living_unit(bot->unit_index)->object.parent_object_index == NONE &&
		distance < 1.5f && now >= bot->melee_time)
	{
		bot->melee_time = now + bot_seconds_to_ticks(1.f);
		engine_ai_behavior_intent_emit(emitter, _bot_intent_melee, BOT_INTENT_SCHEMA, NULL, 0);
	}
	else if (skill->burst_seconds <= 0.f || bot->burst_firing)
		bot_emit_fire(emitter, 1.f);
	if (bot_living_unit(bot->unit_index)->object.parent_object_index == NONE &&
		skill->throws_grenades && now >= bot->grenade_time && distance > 8.f && distance < 20.f)
	{
		bot->grenade_time = now + bot_seconds_to_ticks(BOT_GRENADE_COOLDOWN_SECONDS * bot_random_range(bot, 1.f, 2.f));
		engine_ai_behavior_intent_emit(emitter, _bot_intent_grenade, BOT_INTENT_SCHEMA, NULL, 0);
	}
}

static engine_ai_behavior_result bot_leaf_fight(
	struct bot *bot,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct bot_skill const *skill = bots_globals.skill;
	long now = game_time_get();
	real_point3d eye;
	real_point3d origin;
	real distance;
	real toward;

	if (bot->target_player_index == NONE)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	unit_get_camera_position(bot->unit_index, &eye);
	object_get_origin(bot->unit_index, &origin);
	bot_engage(bot, &eye, emitter);

	distance = bot_horizontal_distance(&origin, &bot->target_last_position);
	toward = bot_yaw_to(&origin, &bot->target_last_position);
	if (!bot->target_visible)
	{
		/* (where it was last seen) */
		bot_emit_move(emitter, toward, 1.f);
		return ENGINE_AI_BEHAVIOR_RUNNING;
	}
	if (skill->strafes && now >= bot->strafe_change_time)
	{
		bot->strafe_sign = bot_random(bot) < 0.5f ? -1.f : 1.f;
		bot->strafe_change_time = now + bot_seconds_to_ticks(bot_random_range(bot, 0.5f, 1.4f));
	}
	if (distance > 10.f)
		bot_emit_move(emitter, toward + (skill->strafes ? bot->strafe_sign * 0.6f : 0.f), 1.f);
	else if (distance < 3.f)
		bot_emit_move(emitter, toward + (real)M_PI, 1.f);
	else if (skill->strafes)
		bot_emit_move(emitter, toward + bot->strafe_sign * (real)M_PI_2, 1.f);
	/* (a jump now and then, as players do) */
	if (skill->retreats && bot_random(bot) < 0.01f)
		engine_ai_behavior_intent_emit(emitter, _bot_intent_jump, BOT_INTENT_SCHEMA, NULL, 0);
	return ENGINE_AI_BEHAVIOR_RUNNING;
}

static boolean bot_should_retreat(
	struct bot const *bot)
{
	struct unit_datum *unit = bot_living_unit(bot->unit_index);

	return bots_globals.skill->retreats && unit && bot->target_player_index != NONE &&
		game_time_get() >= bot->retreat_cooldown_time &&
		unit->object.shield_vitality < 0.25f && unit->object.body_vitality < 0.75f;
}

/* back away from the threat, still shooting, until the shields come back */
static engine_ai_behavior_result bot_leaf_retreat(
	struct bot *bot,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct unit_datum *unit = bot_living_unit(bot->unit_index);
	long now = game_time_get();
	real_point3d eye;
	real_point3d origin;
	real away;

	if (!unit || bot->target_player_index == NONE)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	/* (the root tries it first every time: not unless it is needed) */
	if (bot->retreat_start_time == NONE && !bot_should_retreat(bot))
		return ENGINE_AI_BEHAVIOR_FAILURE;
	if (bot->retreat_start_time == NONE)
		bot->retreat_start_time = now;
	if (unit->object.shield_vitality > 0.9f || now - bot->retreat_start_time > bot_seconds_to_ticks(5.f))
	{
		bot->retreat_start_time = NONE;
		bot->retreat_cooldown_time = now + bot_seconds_to_ticks(6.f);
		return ENGINE_AI_BEHAVIOR_SUCCESS;
	}
	unit_get_camera_position(bot->unit_index, &eye);
	object_get_origin(bot->unit_index, &origin);
	bot_engage(bot, &eye, emitter);
	away = bot_yaw_to(&bot->target_last_position, &origin);
	if (now >= bot->strafe_change_time)
	{
		bot->strafe_sign = bot_random(bot) < 0.5f ? -1.f : 1.f;
		bot->strafe_change_time = now + bot_seconds_to_ticks(bot_random_range(bot, 0.6f, 1.2f));
	}
	bot_emit_move(emitter, away + bot->strafe_sign * 0.5f, 1.f);
	return ENGINE_AI_BEHAVIOR_RUNNING;
}

/* A coarse utility estimate, only for usable weapons. Empty battery or
   ammunition weapons must not lure a bot away from a usable loadout. */
static real bot_weapon_utility(long weapon_index)
{
	struct weapon_datum *weapon = weapon_try_and_get(weapon_index);
	struct weapon_definition *definition;
	struct weapon_trigger_definition *trigger;
	real rate;
	real damage;

	if (!weapon || TEST_FLAG(weapon->object.damage_flags, _object_dead_bit))
		return 0.f;
	definition = weapon_definition_get(weapon->definition_index);
	if (definition->weapon.triggers.count == 0 || weapon->weapon.age >= 0.99f)
		return 0.f;
	trigger = TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition);
	if (trigger->rounds_per_shot > 0)
	{
		short magazine_index = trigger->magazine_index;
		if (magazine_index < 0 || magazine_index >= NUMBEROF(weapon->weapon.magazines))
			return 0.f;
		if (weapon->weapon.magazines[magazine_index].rounds_loaded <= 0 &&
			weapon->weapon.magazines[magazine_index].rounds_total <= 0)
			return 0.f;
	}
	damage = weapon_definition_get_damage_potential(weapon->definition_index, &rate);
	if (!valid_real(damage) || !valid_real(rate))
		return 0.f;
	return MAX(1.f, PIN(damage, 0.f, 1000.f) * PIN(rate, 1.f, 10.f));
}

/* An empty slot is useful; otherwise only a clear upgrade over the selected
   weapon warrants a swap. Duplicate definitions are handled as ammo by Halo. */
static boolean bot_wants_weapon(struct bot *bot, long weapon_index)
{
	struct unit_datum *unit = bot_living_unit(bot->unit_index);
	struct weapon_datum *candidate = weapon_try_and_get(weapon_index);
	short slot;
	boolean empty = FALSE;

	if (!unit || !candidate || bot_weapon_utility(weapon_index) <= 0.f ||
		candidate->object.parent_object_index != NONE ||
		!unit_can_use_weapon(bot->unit_index, weapon_index))
		return FALSE;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long held_index = unit->unit.weapon_object_indices[slot];
		if (held_index == NONE)
			empty = TRUE;
		else if (weapon_get(held_index)->definition_index == candidate->definition_index)
			return FALSE;
	}
	if (empty)
		return TRUE;
	slot = unit->unit.current_weapon_index;
	return slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT &&
		bot_weapon_utility(weapon_index) >
		bot_weapon_utility(unit->unit.weapon_object_indices[slot]) * 1.35f;
}

/* Only empty compatible seats, no eviction. Prefer a gunner with a friendly
   driver, then a ground-vehicle driver. Aircraft need a separate controller. */
static short bot_vehicle_seat(struct bot *bot, long vehicle_index)
{
	struct unit_datum *vehicle = vehicle_try_and_get(vehicle_index);
	struct unit_definition *definition;
	short seat_index;
	short best = NONE;
	short best_priority = 0;

	if (!vehicle || TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) ||
		vehicle->object.up.k < 0.6f || !vehicle_supports_bot_driver(vehicle_index) ||
		magnitude_squared3d(&vehicle->object.translational_velocity) > 0.01f)
		return NONE;
	definition = unit_definition_get(vehicle->definition_index);
	for (seat_index = 0; seat_index < MIN(definition->unit.seats.count, 16); seat_index++)
	{
		struct unit_seat *seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat_index, struct unit_seat);
		long occupant = NONE;
		short priority = TEST_FLAG(seat->flags, _unit_seat_driver_bit) ? 2 :
			(TEST_FLAG(seat->flags, _unit_seat_gunner_bit) ? 3 : 1);
		if (!TEST_FLAG(seat->flags, _unit_seat_driver_bit) && vehicle->unit.driver_object_index == NONE)
			continue;
		if (priority > best_priority && unit_can_enter_seat(bot->unit_index, vehicle_index, seat_index, &occupant))
		{
			best = seat_index;
			best_priority = priority;
		}
	}
	return best;
}

static void bot_observe_inventory(struct bot *bot, struct unit_datum const *unit)
{
	short slot;
	long vehicle_index = unit->object.parent_object_index;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];
		if (weapon_index != bot->observed_weapons[slot])
		{
			if (weapon_index != NONE)
				platform_log("bots: bot %d inventory slot %d now weapon %lx", bot->slot + 1,
					slot, (unsigned long)weapon_get(weapon_index)->definition_index & 0xffff);
			bot->observed_weapons[slot] = weapon_index;
		}
	}
	if (vehicle_index != bot->observed_vehicle_index)
	{
		platform_log("bots: bot %d %s vehicle %ld seat %d", bot->slot + 1,
			vehicle_index == NONE ? "left" : "entered", (long)vehicle_index,
			(short)unit->unit.parent_seat_index);
		bot->observed_vehicle_index = vehicle_index;
		bot->vehicle_progress_time = NONE;
		bot->goal_node = ENGINE_AI_NAV_NODES_MAX;
		bot->opportunity_index = NONE;
	}
}

static boolean bot_friendly_driver(struct bot *bot, long vehicle_index)
{
	struct unit_datum *vehicle = vehicle_try_and_get(vehicle_index);
	struct unit_datum *driver;

	if (!game_engine_has_teams() || !vehicle || vehicle->unit.driver_object_index == NONE)
		return FALSE;
	driver = bot_living_unit(vehicle->unit.driver_object_index);
	return driver && driver->unit.player_index != NONE &&
		player_get(driver->unit.player_index)->team_index == bot_player(bot)->team_index;
}

/* Scan once a second, staggered by slot. Geometry work is limited to the
   best candidates inside 18 world units; no knowledge of hidden pickups. */
static void bot_find_opportunity(struct bot *bot)
{
	struct object_iterator iterator;
	real_point3d origin;
	real best_score = 0.f;
	short examined = 0;

	if (bot->opportunity_index != NONE || game_time_get() < bot->opportunity_scan_time ||
		bot_living_unit(bot->unit_index)->object.parent_object_index != NONE)
		return;
	bot->opportunity_scan_time = game_time_get() + TICKS_PER_SECOND + bot->slot * 3;
	bot->opportunity_index = NONE;
	object_get_origin(bot->unit_index, &origin);
	object_iterator_new(&iterator, _object_mask_weapon | _object_mask_vehicle, 0);
	while (examined++ < 256 && object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		real_point3d point;
		real distance;
		real score;
		short seat = NONE;
		boolean is_vehicle = object->object.type == _object_type_vehicle;

		object_get_origin(iterator.index, &point);
		distance = distance3d(&origin, &point);
		if (distance > 18.f || object->object.parent_object_index != NONE)
			continue;
		if (bot->target_visible && !(is_vehicle && distance < 8.f &&
			bot_friendly_driver(bot, iterator.index)) && (is_vehicle || distance > 3.f))
			continue;
		if (is_vehicle)
		{
			seat = bot_vehicle_seat(bot, iterator.index);
			if (seat == NONE || !unit_get_seat_entrance_point(bot->unit_index,
				iterator.index, seat, &point, NULL, NULL))
				continue;
		}
		else if (!bot_wants_weapon(bot, iterator.index))
			continue;
		score = (is_vehicle ? 3.f : 4.f) / (1.f + distance);
		if (score <= best_score || !bot_navigation_walkable(&origin, &point))
			continue;
		best_score = score;
		bot->opportunity_index = iterator.index;
		bot->opportunity_vehicle = is_vehicle;
		bot->opportunity_seat = seat;
		bot->opportunity_end_time = game_time_get() + bot_seconds_to_ticks(8.f);
	}
	if (bot->opportunity_index != NONE)
		platform_log("bots: bot %d seeking %s %ld", bot->slot + 1,
			bot->opportunity_vehicle ? "vehicle" : "weapon", bot->opportunity_index);
}

static boolean bot_opportunity_relevant(struct bot *bot)
{
	struct object_datum *object = object_try_and_get(bot->opportunity_index);
	real_point3d origin;

	if (!object)
		return FALSE;
	if (!bot->target_visible)
		return TRUE;
	object_get_origin(bot->unit_index, &origin);
	if (bot->opportunity_vehicle)
		return bot_friendly_driver(bot, bot->opportunity_index) &&
			distance3d(&origin, &object->object.position) < 8.f;
	return distance3d(&origin, &object->object.position) < 3.f;
}

static engine_ai_behavior_result bot_leaf_scavenge(struct bot *bot,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct player_datum *player = bot_player(bot);
	struct object_datum *object = object_try_and_get(bot->opportunity_index);
	real_point3d origin;
	real_point3d point;
	real_euler_angles2d look;
	long now = game_time_get();

	if (bot->opportunity_index == NONE)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	if (!object || now >= bot->opportunity_end_time || object->object.parent_object_index != NONE)
		goto abandon;
	if (!bot_opportunity_relevant(bot))
		return ENGINE_AI_BEHAVIOR_FAILURE;
	if (bot->opportunity_vehicle)
	{
		if (bot_vehicle_seat(bot, bot->opportunity_index) == NONE ||
			!unit_get_seat_entrance_point(bot->unit_index, bot->opportunity_index,
			bot->opportunity_seat, &point, NULL, NULL))
			goto abandon;
	}
	else
	{
		if (!bot_wants_weapon(bot, bot->opportunity_index))
			goto abandon;
		object_get_origin(bot->opportunity_index, &point);
	}
	object_get_origin(bot->unit_index, &origin);
	look = bot_angles_to(&origin, &point);
	look.pitch = 0.f;
	bot_emit_look(emitter, &look);
	/* Stop for seat entry: Halo requires a nearly stationary biped. */
	if (bot_horizontal_distance(&origin, &point) > 0.6f)
		bot_emit_move(emitter, look.yaw, bot_horizontal_distance(&origin, &point) < 2.f ? 0.35f : 1.f);
	if (player->action_object_index == bot->opportunity_index && now >= bot->interaction_time)
	{
		bot->interaction_time = now + TICKS_PER_SECOND;
		engine_ai_behavior_intent_emit(emitter, _bot_intent_interact, BOT_INTENT_SCHEMA, NULL, 0);
	}
	return ENGINE_AI_BEHAVIOR_RUNNING;
abandon:
	bot->opportunity_index = NONE;
	bot->opportunity_scan_time = now + bot_seconds_to_ticks(3.f);
	return ENGINE_AI_BEHAVIOR_FAILURE;
}

static engine_ai_behavior_result bot_leaf_vehicle(struct bot *bot,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct unit_datum *unit = bot_living_unit(bot->unit_index);
	struct unit_datum *vehicle;
	struct unit_definition *definition;
	struct unit_seat *seat;
	real_point3d eye;
	boolean drive_wanted = FALSE;
	long now = game_time_get();

	if (!unit || unit->object.parent_object_index == NONE)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	vehicle = vehicle_try_and_get(unit->object.parent_object_index);
	if (!vehicle)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	definition = unit_definition_get(vehicle->definition_index);
	if (unit->unit.parent_seat_index < 0 || unit->unit.parent_seat_index >= definition->unit.seats.count)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
	unit_get_camera_position(bot->unit_index, &eye);
	if (bot->target_player_index != NONE &&
		(!TEST_FLAG(seat->flags, _unit_seat_driver_bit) || vehicle->unit.weapon_object_indices[0] != NONE))
		bot_engage(bot, &eye, emitter);
	if (TEST_FLAG(seat->flags, _unit_seat_driver_bit))
	{
		real_point3d goal;
		real_euler_angles2d look;

		if (bot->target_player_index != NONE)
			goal = bot->target_last_position;
		else
		{
			struct engine_ai_nav_graph const *graph = bot_navigation_graph();
			if (!graph || graph->node_count == 0)
				return ENGINE_AI_BEHAVIOR_RUNNING;
			if (bot->goal_node >= graph->node_count ||
				distance3d(&vehicle->object.position, &bots_globals.navigation.nodes[bot->goal_node]) < 6.f)
				engine_ai_rng32_bounded(&bot->rng, graph->node_count, &bot->goal_node);
			goal = bots_globals.navigation.nodes[bot->goal_node];
		}
		look = bot_angles_to(&vehicle->object.position, &goal);
		look.pitch = 0.f;
		if (!bot->target_visible || vehicle->unit.weapon_object_indices[0] == NONE)
			bot_emit_look(emitter, &look);
		/* No on-foot strafing/jumping; steer toward the goal at half throttle. */
		drive_wanted = bot_horizontal_distance(&vehicle->object.position, &goal) > 8.f;
		if (drive_wanted)
			bot_emit_move(emitter, look.yaw, 0.5f);
	}
	if (bot->vehicle_progress_time == NONE)
	{
		bot->vehicle_progress_time = now;
		bot->vehicle_progress_position = vehicle->object.position;
	}
	else if (now - bot->vehicle_progress_time >= bot_seconds_to_ticks(8.f))
	{
		if (vehicle->object.up.k < 0.5f || (drive_wanted &&
			distance3d(&vehicle->object.position, &bot->vehicle_progress_position) < 1.f) ||
			(!TEST_FLAG(seat->flags, _unit_seat_driver_bit) && vehicle->unit.driver_object_index == NONE))
			engine_ai_behavior_intent_emit(emitter, _bot_intent_exit_vehicle, BOT_INTENT_SCHEMA, NULL, 0);
		bot->vehicle_progress_time = now;
		bot->vehicle_progress_position = vehicle->object.position;
	}
	return ENGINE_AI_BEHAVIOR_RUNNING;
}

static enum engine_ai_traversal_availability bot_traversal_availability(
	void *context,
	uint32_t traversal_id,
	uint32_t source_node,
	uint32_t destination_node)
{
	return ENGINE_AI_TRAVERSAL_AVAILABLE;
}

static enum engine_ai_traversal_reservation_result bot_traversal_reserve(
	void *context,
	uint32_t traversal_id,
	uint32_t source_node,
	uint32_t destination_node,
	uint64_t *reservation_id)
{
	*reservation_id = 1;
	return ENGINE_AI_TRAVERSAL_RESERVATION_GRANTED;
}

/* (the graph has ordinary links only so far; a special one is a jump) */
static enum engine_ai_traversal_execution_result bot_traversal_execute(
	void *context,
	uint32_t traversal_id,
	uint32_t source_node,
	uint32_t destination_node,
	uint64_t reservation_id)
{
	struct bot *bot = (struct bot *)context;

	bot->jump_requested = TRUE;
	return ENGINE_AI_TRAVERSAL_EXECUTION_COMPLETE;
}

static void bot_traversal_release(
	void *context,
	uint32_t traversal_id,
	uint64_t reservation_id,
	enum engine_ai_traversal_release_reason reason)
{
}

static struct engine_ai_traversal_callbacks const bot_traversal_callbacks =
{
	bot_traversal_availability,
	bot_traversal_reserve,
	bot_traversal_execute,
	bot_traversal_release,
};

/* walk the graph to a random far node; SUCCESS on arrival (the root then
picks another) */
static engine_ai_behavior_result bot_leaf_roam(
	struct bot *bot,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct engine_ai_nav_graph const *graph = bot_navigation_graph();
	long now = game_time_get();
	real_point3d origin;
	real_point3d const *target;
	uint32_t target_node;
	enum engine_ai_traversal_result result;
	real_euler_angles2d look;

	if (!graph)
		return ENGINE_AI_BEHAVIOR_FAILURE;
	object_get_origin(bot->unit_index, &origin);

	if (!bot->searching && !bot->following)
	{
		long start = bot_navigation_nearest_node(&origin);
		uint32_t attempt;

		if (start == NONE)
			return ENGINE_AI_BEHAVIOR_FAILURE;
		bot->goal_node = (uint32_t)start;
		for (attempt = 0; attempt < 8; attempt++)
		{
			uint32_t candidate = 0;

			engine_ai_rng32_bounded(&bot->rng, graph->node_count, &candidate);
			bot->goal_node = candidate;
			if (distance3d(&origin, &bots_globals.navigation.nodes[candidate]) > BOT_MINIMUM_ROAM_DISTANCE)
				break;
		}
		if (engine_ai_nav_begin(&bot->search, graph, (uint32_t)start, bot->goal_node, 0) != ENGINE_AI_NAV_RUNNING)
			return ENGINE_AI_BEHAVIOR_FAILURE;
		bot->searching = TRUE;
	}

	if (bot->searching)
	{
		enum engine_ai_nav_status status = engine_ai_nav_step(&bot->search, graph, BOT_NAV_EXPANSIONS_PER_TICK);

		if (status == ENGINE_AI_NAV_RUNNING)
			return ENGINE_AI_BEHAVIOR_RUNNING;
		bot->searching = FALSE;
		if (status == ENGINE_AI_NAV_FOUND)
		{
			struct engine_ai_nav_route route;

			if (engine_ai_nav_result(&bot->search, graph, &route) == ENGINE_AI_NAV_FOUND &&
				engine_ai_traversal_begin(&bot->traversal, graph, &route, 0))
			{
				bot->following = TRUE;
				bot->progress_time = NONE;
				bot->stuck_count = 0;
			}
		}
		if (!bot->following)
		{
			/* (no way there: another goal next time) */
			return ENGINE_AI_BEHAVIOR_FAILURE;
		}
	}

	if (!engine_ai_traversal_target_node(&bot->traversal, graph, &target_node))
	{
		/* (an empty route: there already) */
		bot_navigation_reset(bot);
		return ENGINE_AI_BEHAVIOR_SUCCESS;
	}
	target = &bots_globals.navigation.nodes[target_node];
	result = engine_ai_traversal_step(&bot->traversal, graph, graph->revision, 0,
		bot_horizontal_distance(&origin, target) < BOT_WAYPOINT_REACHED_DISTANCE,
		&bot_traversal_callbacks, bot);
	if (result == ENGINE_AI_TRAVERSAL_COMPLETED)
	{
		bot_navigation_reset(bot);
		return ENGINE_AI_BEHAVIOR_SUCCESS;
	}
	if (result != ENGINE_AI_TRAVERSAL_APPROACHING && result != ENGINE_AI_TRAVERSAL_EXECUTING &&
		result != ENGINE_AI_TRAVERSAL_WAITING)
	{
		bot_navigation_reset(bot);
		return ENGINE_AI_BEHAVIOR_FAILURE;
	}
	/* (the next waypoint, which may have changed) */
	if (engine_ai_traversal_target_node(&bot->traversal, graph, &target_node))
		target = &bots_globals.navigation.nodes[target_node];

	/* stuck: a jump, then another way */
	if (bot->progress_time == NONE)
	{
		bot->progress_time = now;
		bot->progress_position = origin;
	}
	else if (now - bot->progress_time >= BOT_PROGRESS_CHECK_TICKS)
	{
		if (distance3d(&origin, &bot->progress_position) < 0.5f)
		{
			bot->stuck_count++;
			bot->jump_requested = TRUE;
		}
		else
		{
			bot->stuck_count = 0;
		}
		bot->progress_time = now;
		bot->progress_position = origin;
		if (bot->stuck_count >= 3)
		{
			if (bot->traversal.next_link < bot->traversal.route.link_count)
				bot_navigation_disable_link(bot->traversal.route.link_indices[bot->traversal.next_link]);
			engine_ai_traversal_cancel(&bot->traversal, &bot_traversal_callbacks, bot);
			bot_navigation_reset(bot);
			return ENGINE_AI_BEHAVIOR_FAILURE;
		}
	}

	look.yaw = bot_yaw_to(&origin, target);
	look.pitch = 0.f;
	bot_emit_look(emitter, &look);
	bot_emit_move(emitter, look.yaw, 1.f);
	if (bot->jump_requested)
	{
		bot->jump_requested = FALSE;
		engine_ai_behavior_intent_emit(emitter, _bot_intent_jump, BOT_INTENT_SCHEMA, NULL, 0);
	}
	return ENGINE_AI_BEHAVIOR_RUNNING;
}

static engine_ai_behavior_result bot_behavior_tick(
	void *context,
	uint32_t node,
	uint32_t leaf_id,
	struct engine_ai_behavior_intent_emitter *emitter)
{
	struct bot *bot = (struct bot *)context;

	switch (leaf_id)
	{
	case _bot_leaf_retreat: return bot_leaf_retreat(bot, emitter);
	case _bot_leaf_fight: return bot_leaf_fight(bot, emitter);
	case _bot_leaf_roam: return bot_leaf_roam(bot, emitter);
	case _bot_leaf_scavenge: return bot_leaf_scavenge(bot, emitter);
	case _bot_leaf_vehicle: return bot_leaf_vehicle(bot, emitter);
	}
	return ENGINE_AI_BEHAVIOR_ERROR;
}

static void bot_behavior_cancel(
	void *context,
	uint32_t node,
	uint32_t leaf_id)
{
	struct bot *bot = (struct bot *)context;

	switch (leaf_id)
	{
	case _bot_leaf_retreat:
		bot->retreat_start_time = NONE;
		break;
	case _bot_leaf_roam:
		if (bot->following)
			engine_ai_traversal_cancel(&bot->traversal, &bot_traversal_callbacks, bot);
		bot_navigation_reset(bot);
		break;
	}
}

/* (side-effect free: whether the subtree could run now) */
static bool bot_behavior_relevant(
	void *context,
	uint32_t node)
{
	struct bot *bot = (struct bot *)context;

	switch (bot_behavior_nodes[node].leaf_id)
	{
	case _bot_leaf_vehicle:
		return bot_living_unit(bot->unit_index) &&
			bot_living_unit(bot->unit_index)->object.parent_object_index != NONE;
	case _bot_leaf_scavenge: return bot_opportunity_relevant(bot);
	case _bot_leaf_retreat: return bot_should_retreat(bot);
	case _bot_leaf_fight: return bot->target_player_index != NONE;
	case _bot_leaf_roam: return true;
	}
	return false;
}

static engine_ai_behavior_result bot_behavior_tick_plain(
	void *context,
	uint32_t node,
	uint32_t leaf_id)
{
	return ENGINE_AI_BEHAVIOR_FAILURE;
}

/* ---------- private code: the executor */

/* an intent, checked against the live player and unit as a person's input
would be, into this tick's action */
static struct engine_ai_intent_result bot_execute_intent(
	void *context,
	struct engine_ai_intent const *intent)
{
	struct bot *bot = (struct bot *)context;
	struct engine_ai_intent_result result;
	struct player_datum *player = bot_player(bot);
	struct unit_datum *unit;

	result.outcome = ENGINE_AI_INTENT_REJECTED;
	result.reason = _bot_reject_none;
	if (!player || intent->id.actor.index != (uint32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index) ||
		intent->id.actor.generation != (uint32_t)(unsigned short)(bot->player_index >> 16))
	{
		result.reason = _bot_reject_no_player;
		return result;
	}
	unit = bot_living_unit(player->unit_index);
	if (!unit || player->unit_index != bot->unit_index || !unit_controllable(player->unit_index))
	{
		result.reason = _bot_reject_no_unit;
		return result;
	}
	if (intent->schema_version != BOT_INTENT_SCHEMA)
	{
		result.reason = _bot_reject_bad_payload;
		return result;
	}

	switch (intent->action_kind)
	{
	case _bot_intent_look:
	{
		struct bot_look_payload payload;

		if (intent->payload_size != sizeof(payload))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		csmemcpy(&payload, intent->payload, sizeof(payload));
		if (!valid_real(payload.yaw) || !valid_real(payload.pitch))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		bot->pending.has_look = TRUE;
		bot->pending.look.yaw = bot_angle_normalize(payload.yaw);
		bot->pending.look.pitch = PIN(payload.pitch, -1.4f, 1.4f);
		break;
	}
	case _bot_intent_move:
	{
		struct bot_move_payload payload;

		if (intent->payload_size != sizeof(payload))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		csmemcpy(&payload, intent->payload, sizeof(payload));
		if (!valid_real(payload.yaw) || !valid_real(payload.speed))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		bot->pending.has_move = TRUE;
		bot->pending.move_yaw = bot_angle_normalize(payload.yaw);
		bot->pending.move_speed = PIN(payload.speed, 0.f, 1.f);
		break;
	}
	case _bot_intent_fire:
	{
		struct bot_fire_payload payload;

		if (intent->payload_size != sizeof(payload))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		csmemcpy(&payload, intent->payload, sizeof(payload));
		if (!valid_real(payload.trigger))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		bot->pending.trigger = PIN(payload.trigger, 0.f, 1.f);
		break;
	}
	case _bot_intent_interact:
		/* Never turn an unrelated prompt into a pickup or vehicle action. */
		if (unit->object.parent_object_index != NONE ||
			player->action_object_index != bot->opportunity_index)
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		if (!bot->opportunity_vehicle &&
			(player->action_result == _player_action_result_swap_for_weapon ||
			player->action_result == _player_action_result_add_weapon_to_inventory))
			bot->pending.control_flags |= FLAG(_unit_control_swap_weapons_bit);
		else if (bot->opportunity_vehicle &&
			player->action_result == _player_action_result_enter_vehicle &&
			player->action_seat_index == bot->opportunity_seat)
			bot->pending.control_flags |= FLAG(_unit_control_action_bit) | FLAG(UNIT_CONTROL_PORT_ACTION_ONLY_BIT);
		else
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		break;
	case _bot_intent_exit_vehicle:
		if (unit->object.parent_object_index == NONE || !unit_try_and_exit_seat(bot->unit_index))
		{
			result.reason = _bot_reject_bad_payload;
			return result;
		}
		bot->opportunity_scan_time = game_time_get() + bot_seconds_to_ticks(8.f);
		break;
	case _bot_intent_melee:
		bot->pending.control_flags |= FLAG(_unit_control_use_equipment_bit);
		break;
	case _bot_intent_jump:
		bot->pending.control_flags |= FLAG(_unit_control_jump_bit);
		break;
	case _bot_intent_grenade:
		if (unit->unit.current_grenade_index < 0 ||
			unit->unit.current_grenade_index >= NUMBER_OF_UNIT_GRENADE_TYPES ||
			unit->unit.grenade_counts[(short)unit->unit.current_grenade_index] <= 0)
		{
			result.reason = _bot_reject_no_grenades;
			return result;
		}
		bot->pending.control_flags |= FLAG(_unit_control_throw_grenade_bit);
		break;
	default:
		result.reason = _bot_reject_unknown;
		return result;
	}

	result.outcome = ENGINE_AI_INTENT_ACCEPTED;
	result.reason = _bot_reject_none;
	return result;
}

/* Charged shots are deliberate: a visible, shielded opponent at medium
   range, no more than once every few seconds. Continue aiming through burst
   pauses, but release if the target changes or disappears. */
static boolean bot_weapon_fire(struct bot *bot, struct weapon_trigger_definition const *trigger)
{
	long now = game_time_get();
	long period = bot_seconds_to_ticks(bot_random_range(bot, 0.35f, 0.55f));
	long hold_ticks = 1;
	boolean wants_fire = bot->pending.trigger > 0.f;
	boolean tap = trigger->charging_time > 0.f || TEST_FLAG(trigger->flags, _weapon_trigger_latched_bit);
	boolean pressed;

	if (trigger->initial_rate_of_fire > 0.f && valid_real(trigger->initial_rate_of_fire))
		period = MAX(period, (long)ceil((real)TICKS_PER_SECOND /
			PIN(trigger->initial_rate_of_fire, 0.1f, 30.f)));
	if (bot->charging)
	{
		wants_fire = bot->target_visible && bot->target_player_index == bot->charge_target_index;
	}
	else if (wants_fire && trigger->charging_time > 0.f && valid_real(trigger->charging_time) &&
		trigger->charging_time <= 5.f && !bot->fire_control.pressed &&
		now >= bot->fire_control.next_press_tick && now >= bot->next_charge_time)
	{
		struct player_datum *target = bot->target_player_index != NONE ?
			player_get(bot->target_player_index) : NULL;
		struct unit_datum *target_unit = target ? bot_living_unit(target->unit_index) : NULL;
		real_point3d origin;
		real distance;

		object_get_origin(bot->unit_index, &origin);
		distance = distance3d(&origin, &bot->target_last_position);
		if (target_unit && bot->target_visible && target_unit->object.shield_vitality > 0.5f &&
			distance > 6.f && distance < 25.f)
		{
			hold_ticks = (long)ceil(trigger->charging_time * TICKS_PER_SECOND) + 2;
			bot->charging = TRUE;
			bot->charge_target_index = bot->target_player_index;
			bot->next_charge_time = now + bot_seconds_to_ticks(bot_random_range(bot, 5.f, 9.f));
		}
	}
	pressed = engine_ai_fire_control_step(&bot->fire_control, now, wants_fire,
		tap, (int32_t)period, (int32_t)hold_ticks);
	if (bot->charging && !pressed)
	{
		bot->charging = FALSE;
		/* A beat after releasing the overcharge, not an immediate tap. */
		bot->fire_control.next_press_tick = now + bot_seconds_to_ticks(0.6f);
	}
	return pressed;
}

/* Resolve the equipped weapon each tick; don't carry a held trigger across
   weapon swaps or respawns. Charging and latched triggers need press/release. */
static void bot_control_weapon(struct bot *bot, struct player_action *action)
{
	struct unit_datum *unit = bot_living_unit(bot->unit_index);
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	struct weapon_trigger_definition *trigger;
	long weapon_index;
	short slot;

	if (!unit)
		return;
	if (unit->object.parent_object_index != NONE)
	{
		struct unit_datum *parent = vehicle_try_and_get(unit->object.parent_object_index);
		struct unit_definition *definition;
		struct unit_seat *seat;

		if (!parent)
			return;
		definition = unit_definition_get(parent->definition_index);
		if (unit->unit.parent_seat_index < 0 || unit->unit.parent_seat_index >= definition->unit.seats.count)
			return;
		seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
		if (TEST_FLAG(seat->flags, _unit_seat_driver_bit) || TEST_FLAG(seat->flags, _unit_seat_gunner_bit))
			unit = parent; /* Use the vehicle's weapon, not the carried pistol. */
	}
	slot = unit->unit.current_weapon_index;
	if (slot < 0 || slot >= MAXIMUM_WEAPONS_PER_UNIT)
		return;
	weapon_index = unit->unit.weapon_object_indices[slot];
	if (weapon_index == NONE)
		return;
	if (unit->object.type != _object_type_vehicle && !bot->charging)
	{
		short other = 1 - slot;
		long other_weapon = unit->unit.weapon_object_indices[other];

		if (other_weapon != NONE && bot_weapon_utility(other_weapon) > bot_weapon_utility(weapon_index) * 1.35f)
		{
			action->desired_weapon_index = other;
			return;
		}
	}
	if (weapon_index != bot->fire_weapon_index)
	{
		bot->fire_weapon_index = weapon_index;
		csmemset(&bot->fire_control, 0, sizeof(bot->fire_control));
		bot->charging = FALSE;
		return; /* Release before using the new weapon. */
	}
	weapon = weapon_get(weapon_index);
	definition = weapon_definition_get(weapon->definition_index);
	if (definition->weapon.triggers.count <= 0)
		return;
	trigger = TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition);
	/* Battery weapons may reference an unused magazine with zero rounds. */
	if (trigger->rounds_per_shot > 0 && trigger->magazine_index >= 0 &&
		trigger->magazine_index < NUMBEROF(weapon->weapon.magazines))
	{
		struct weapon_magazine *magazine = &weapon->weapon.magazines[trigger->magazine_index];

		if (magazine->rounds_loaded <= 0)
		{
			if (magazine->rounds_total > 0)
				action->control_flags |= FLAG(_unit_control_weapon_reload_bit);
			else if (unit->unit.weapon_object_indices[1 - slot] != NONE)
				action->desired_weapon_index = 1 - slot;
			bot->pending.trigger = 0.f;
		}
	}
	if (bot_weapon_fire(bot, trigger))
	{
		action->primary_trigger = 1.f;
		action->control_flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
	}
}

/* the accepted intents as the player's action: the facing turned toward the
look at this skill's speed, and the movement relative to it */
static void bot_submit_action(
	struct bot *bot)
{
	struct player_action action;
	real turn = DEGREES_TO_RADIANS(bots_globals.skill->turn_degrees_per_second) / TICKS_PER_SECOND;

	if (bot->pending.has_look)
	{
		real yaw_delta = bot_angle_normalize(bot->pending.look.yaw - bot->facing.yaw);
		real pitch_delta = bot->pending.look.pitch - bot->facing.pitch;

		bot->facing.yaw = bot_angle_normalize(bot->facing.yaw + PIN(yaw_delta, -turn, turn));
		bot->facing.pitch += PIN(pitch_delta, -turn, turn);
	}
	csmemset(&action, 0, sizeof(action));
	action.control_flags = bot->pending.control_flags;
	action.desired_facing = bot->facing;
	if (bot->pending.has_move)
	{
		real relative = bot->pending.move_yaw - bot->facing.yaw;

		action.throttle.i = cosine(relative) * bot->pending.move_speed;
		action.throttle.j = sine(relative) * bot->pending.move_speed;
	}
	action.desired_weapon_index = NONE;
	action.desired_grenade_index = NONE;
	action.desired_zoom_level = NONE;
	bot_control_weapon(bot, &action);
	update_server_set_player_action(bot->player_index, &action);
}

/* ---------- private code: a bot's tick */

static void bot_reset_life(
	struct bot *bot,
	long unit_index)
{
	engine_ai_behavior_callbacks callbacks;
	struct unit_datum *unit = bot_living_unit(unit_index);

	/* (the last life's running leaf is let go first) */
	callbacks.context = bot;
	callbacks.tick = bot_behavior_tick_plain;
	callbacks.cancel = bot_behavior_cancel;
	callbacks.trace = NULL;
	if (bot->behavior.initialized)
		engine_ai_behavior_cancel(&bot->behavior, &bot_behavior, &callbacks);
	engine_ai_behavior_begin(&bot->behavior, &bot_behavior);

	bot->unit_index = unit_index;
	bot->target_player_index = NONE;
	bot->target_visible = FALSE;
	bot->aim_error_time = 0;
	bot->strafe_change_time = 0;
	bot->burst_change_time = 0;
	bot->burst_firing = FALSE;
	bot->fire_weapon_index = NONE;
	bot->charging = FALSE;
	bot->charge_target_index = NONE;
	bot->next_charge_time = game_time_get() + bot_seconds_to_ticks(bot_random_range(bot, 2.f, 4.f));
	csmemset(&bot->fire_control, 0, sizeof(bot->fire_control));
	bot->melee_time = game_time_get();
	bot->grenade_time = game_time_get() + bot_seconds_to_ticks(3.f);
	bot->retreat_start_time = NONE;
	bot->retreat_cooldown_time = 0;
	bot->jump_requested = FALSE;
	bot->opportunity_index = NONE;
	bot->opportunity_scan_time = game_time_get();
	bot->interaction_time = 0;
	bot->observed_vehicle_index = NONE;
	bot->observed_weapons[0] = bot->observed_weapons[1] = NONE;
	bot->vehicle_progress_time = NONE;
	bot_navigation_reset(bot);
	if (unit)
		euler_angles2d_from_vector3d(&bot->facing, &unit->unit.desired_aiming_vector);
}

/* Find a parked, empty ground vehicle with a driver and gunner seat. This
   fixture deliberately does not create vehicles or bypass the bot's entry. */
static long bot_sandbox_vehicle(struct bot *bot, short *driver_seat, short *gunner_seat)
{
	struct object_iterator iterator;
	real_point3d origin;
	real nearest = 100000.f;
	long best = NONE;
	short examined = 0;

	object_get_origin(bot->unit_index, &origin);
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (examined++ < 256 && object_iterator_next(&iterator))
	{
		struct unit_datum *vehicle = vehicle_get(iterator.index);
		struct unit_definition *definition = unit_definition_get(vehicle->definition_index);
		short driver = NONE;
		short gunner = NONE;
		short seat_index;
		real distance;

		if (!vehicle_supports_bot_driver(iterator.index) || vehicle->unit.driver_object_index != NONE ||
			TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) || vehicle->object.up.k < 0.6f)
			continue;
		for (seat_index = 0; seat_index < MIN(definition->unit.seats.count, 16); seat_index++)
		{
			if (unit_seat_is_driver(iterator.index, seat_index))
				driver = seat_index;
			if (unit_seat_is_gunner(iterator.index, seat_index))
				gunner = seat_index;
		}
		distance = distance3d(&origin, &vehicle->object.position);
		if (driver != NONE && gunner != NONE && distance < nearest)
		{
			best = iterator.index;
			nearest = distance;
			*driver_seat = driver;
			*gunner_seat = gunner;
		}
	}
	return best;
}

static void bot_debug_sandbox(struct bot *bot)
{
	struct data_iterator iterator;
	struct player_datum *host = NULL;
	struct player_datum *player;
	short driver = NONE;
	short gunner = NONE;
	long vehicle_index;
	struct unit_datum *vehicle;
	real_point3d point;
	real_point3d host_point;
	real ground_z;

	if (bots_globals.sandbox_done || bot->slot != 1 || !game_engine_has_teams() ||
		!config_boolean("debug.bot_sandbox"))
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (player->local_player_index != NONE && bot_living_unit(player->unit_index) &&
			player->team_index == bot_player(bot)->team_index)
		{
			host = player;
			break;
		}
	}
	if (!host || unit_get(host->unit_index)->object.parent_object_index != NONE)
		return;
	vehicle_index = bot_sandbox_vehicle(bot, &driver, &gunner);
	if (vehicle_index == NONE)
		return;
	vehicle = vehicle_get(vehicle_index);
	if (!unit_get_seat_entrance_point(bot->unit_index, vehicle_index, gunner, &point, NULL, NULL) ||
		!unit_get_seat_entrance_point(host->unit_index, vehicle_index, driver, &host_point, NULL, NULL))
		return;
	point.x += vehicle->object.forward.i * 2.f;
	point.y += vehicle->object.forward.j * 2.f;
	point.z += 1.f;
	if (!bot_navigation_ground(&point, 4.f, &ground_z))
		return;
	point.z = ground_z + 0.05f;
	/* A parked vehicle can be tilted. Preserve the biped's orthonormal
	   orientation rather than pairing vehicle forward with world up. */
	object_set_position(host->unit_index, &host_point, NULL, NULL);
	unit_get(host->unit_index)->object.translational_velocity = *global_zero_vector3d;
	if (!unit_enter_seat(host->unit_index, vehicle_index, driver))
		return;
	object_set_position(bot->unit_index, &point, NULL, NULL);
	unit_get(bot->unit_index)->object.translational_velocity = *global_zero_vector3d;
	bot->opportunity_index = NONE;
	bot->opportunity_scan_time = game_time_get() + TICKS_PER_SECOND;
	bots_globals.sandbox_done = TRUE;
	platform_log("bots: sandbox host drives vehicle %ld; Bot 2 approaches gunner seat %d normally",
		vehicle_index, gunner);
}

static void bot_think(
	struct bot *bot)
{
	struct player_datum *player = bot_player(bot);
	struct unit_datum *unit;
	struct engine_ai_actor_id actor;
	struct engine_ai_tick_context tick;
	struct engine_ai_behavior_intent_callbacks callbacks;
	engine_ai_behavior_result result;

	if (!player)
		return;
	unit = bot_living_unit(player->unit_index);
	if (!unit || !unit_controllable(player->unit_index))
	{
		/* (dead, or waiting to respawn) */
		if (bot->unit_index != NONE)
			bot_reset_life(bot, NONE);
		return;
	}
	if (player->unit_index != bot->unit_index)
		bot_reset_life(bot, player->unit_index);

	/* Geometry acceptance probe takes only Bot 1, after the normal authority,
	   lifecycle and unit-control gates. Normal tactics/spawns stay unchanged. */
	if (bot->slot == 0)
	{
		struct player_action probe_action;
		if (navigation_probe_action(player->unit_index, &probe_action))
		{
			update_server_set_player_action(bot->player_index, &probe_action);
			return;
		}
	}
	bot_debug_sandbox(bot);
	bot_perceive(bot, player, unit);
	bot_observe_inventory(bot, unit);
	bot_find_opportunity(bot);

	actor.index = (uint32_t)DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index);
	actor.generation = (uint32_t)(unsigned short)(bot->player_index >> 16);
	tick.id.epoch = bots_globals.epoch;
	tick.id.tick = (uint64_t)game_time_get();
	tick.revision_id = 0;
	if (engine_ai_intent_begin(&bot->batch, actor, &tick, ENGINE_AI_INTENTS_MAX) != ENGINE_AI_INTENT_OK)
		return;
	engine_ai_behavior_intent_state_init(&bot->bridge);
	callbacks.context = bot;
	callbacks.tick = bot_behavior_tick;
	callbacks.cancel = bot_behavior_cancel;
	callbacks.relevant = bot_behavior_relevant;
	callbacks.trace = NULL;

	/* an earlier leaf that has become relevant (an enemy seen, the shields
	down) takes over from the running one */
	if (bot->behavior.running_node != ENGINE_AI_BEHAVIOR_NODE_MAX && !bot->behavior.faulted)
	{
		engine_ai_behavior_intent_reevaluate(&bot->bridge, &bot->behavior, &bot_behavior,
			ENGINE_AI_BEHAVIOR_NODE_MAX, &callbacks, &bot->batch, &result);
	}
	if (engine_ai_behavior_intent_step(&bot->bridge, &bot->behavior, &bot_behavior,
		BOT_BEHAVIOR_WORK_PER_TICK, &callbacks, &bot->batch, &result) != ENGINE_AI_BEHAVIOR_INTENT_OK)
	{
		/* (a faulted tree: start again next tick) */
		bot_reset_life(bot, bot->unit_index);
	}

	csmemset(&bot->pending, 0, sizeof(bot->pending));
	engine_ai_intent_execute(&bot->batch, &tick, &bot->report, bot, bot_execute_intent);
	bot_submit_action(bot);

	/* (what it is doing, every ten seconds, to the log) */
	if (game_time_get() % (10 * TICKS_PER_SECOND) == bot->slot * 7)
	{
		static char const *const leaves[] = { "none", "retreat", "fight", "roam", "scavenge", "vehicle" };
		real_point3d origin;
		uint32_t leaf = bot->behavior.running_node < NUMBEROF(bot_behavior_nodes) ?
			bot_behavior_nodes[bot->behavior.running_node].leaf_id : 0;

		object_get_origin(bot->unit_index, &origin);
		platform_log("bots: bot %d at (%.1f %.1f %.1f) yaw %.0f %s, target %ld%s, shield %.2f, kills %d deaths %d",
			bot->slot + 1, origin.x, origin.y, origin.z, RADIANS_TO_DEGREES(bot->facing.yaw), leaves[leaf < NUMBEROF(leaves) ? leaf : 0],
			(long)(bot->target_player_index == NONE ? -1 : DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->target_player_index)),
			bot->target_visible ? " (seen)" : "", unit->object.shield_vitality,
			player->statistics.kills[0], player->statistics.deaths);
	}
}

/* ---------- private code: joining and leaving */

/* the host of a game with a game engine that nobody else is in */
static boolean bots_host_may_have_bots(
	void)
{
	long machine_indices[1];

	/* Objective variants need game-owned goal policy, not Slayer roaming. */
	if (!game_engine_get_variant() ||
		game_engine_get_variant()->game_engine_index != game_engine_slayer)
	{
		return FALSE;
	}
	if (!game_time_initialized() || !game_engine_running() || game_engine_showing_postgame() ||
		network_game_distributed_client())
	{
		return FALSE;
	}
	switch (game_connection())
	{
	case _game_connection_local:
		return TRUE;
	case _game_connection_network_server:
		return distributed_client_machines(machine_indices, 1) == 0;
	}
	return FALSE;
}

/* bots whose player is gone (a player who joined took its slot) */
static void bots_refresh(
	void)
{
	short bot_index;

	for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
	{
		struct bot *bot = &bots_globals.bots[bot_index];

		if (bot->active && !bot_player(bot))
		{
			struct unit_datum *unit = bot_living_unit(bot->unit_index);

			/* (its unit, no one's now, goes with it) */
			if (unit && unit->unit.player_index == NONE)
				unit_kill_no_statistics(bot->unit_index);
			bot->active = FALSE;
		}
	}
}

static short bots_free_machine_slots(
	long machine_index)
{
	long *player_list = machine_get_player_list(machine_index);
	short index;
	short count = 0;

	for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
	{
		if (player_list[index] == NONE)
			count++;
	}
	return count;
}

/* the team with fewer players */
static char bots_choose_team(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long counts[2] = { 0, 0 };

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!player->quit_out_of_game && player->team_index >= 0 && player->team_index < 2)
			counts[player->team_index]++;
	}
	return counts[1] < counts[0] ? 1 : 0;
}

/* every bot missing, at once, until there are as many as configured: a new
game's bots join in its first tick and spawn with its players */
static void bots_join(
	void)
{
	short active_count = 0;
	short bot_index;

	for (bot_index = 0; bot_index < BOTS_MAXIMUM; bot_index++)
	{
		if (bots_globals.bots[bot_index].active)
			active_count++;
	}
	for (bot_index = 0; bot_index < BOTS_MAXIMUM && active_count < bots_globals.desired_count; bot_index++)
	{
		if (!bots_globals.bots[bot_index].active && bot_join(&bots_globals.bots[bot_index], bot_index))
			active_count++;
	}
}

/* the bot in the slot joins as a player; FALSE if there is no room for it */
static boolean bot_join(
	struct bot *bot,
	short bot_index)
{
	long now = game_time_get();
	struct network_player network_player;
	long player_index;
	char name[12];
	short character;
	long machine_index = bot_machine_index(bot_index);

	if (bots_free_machine_slots(machine_index) == 0)
		return FALSE;

	csmemset(&network_player, 0, sizeof(network_player));
	csmemset(name, 0, sizeof(name));
	name[0] = 'B'; name[1] = 'o'; name[2] = 't'; name[3] = ' ';
	if (bot_index + 1 >= 10)
	{
		name[4] = (char)('0' + (bot_index + 1) / 10);
		name[5] = (char)('0' + (bot_index + 1) % 10);
	}
	else
		name[4] = (char)('1' + bot_index);
	for (character = 0; character < 11 && name[character]; character++)
		network_player.name[character] = (wchar_t)name[character];
	network_player.primary_color_index = (short)((3 + bot_index * 5) % 18);
	network_player.machine_index = (char)machine_index;
	network_player.controller_index = (char)(bot_index % MAXIMUM_LOCAL_PLAYERS);
	network_player.team_index = bots_choose_team();
	network_player.player_list_index = NONE;

	player_index = player_new(machine_index, NONE, NONE, &network_player);
	if (player_index == NONE)
		return FALSE;
	/* (as a player who joins a game in progress:
	network_game_client_add_player_to_game) */
	game_engine_player_added(player_index);
	update_client_add_player(player_index);
	update_server_add_player(player_index);

	csmemset(bot, 0, sizeof(*bot));
	bot->active = TRUE;
	bot->slot = bot_index;
	bot->player_index = player_index;
	bot->unit_index = NONE;
	bot->target_player_index = NONE;
	bot->retreat_start_time = NONE;
	bot->progress_time = NONE;
	engine_ai_rng32_seed(&bot->rng, bots_globals.epoch * 7919u + (uint64_t)now, (uint64_t)bot_index);
	engine_ai_behavior_begin(&bot->behavior, &bot_behavior);
	bots_globals.had_bots = TRUE;
	platform_log("bots: %s joined (%s) at tick %ld", name, bots_globals.skill->name, now);
	return TRUE;
}

/* a bot leaves as a player who quits does (game_update_quit_players) */
static void bot_leave(
	struct bot *bot)
{
	struct player_datum *player = bot_player(bot);

	if (player && player->quit_out_of_game_time == NONE)
		player->quit_out_of_game_time = game_time_get();
	machine_remove_player(bot->player_index);
	bot->active = FALSE;
	platform_log("bots: a bot left (solo Slayer host gate no longer permits bots)");
}

/* ---------- extension registration (source/extensions/extension_api.h) */

/* each tick, before the players' actions: the navigation world first, then
the host's bots join or leave and decide their actions for its next update,
as its local players' input goes there */
static void bots_controller_update(
	void)
{
#ifdef HALO_TRACE_ENABLED
	halo_trace_zone_begin(HALO_TRACE_ZONE_BOT_AI);
#endif
	navigation_world_update_for_players();
	bots_update();
#ifdef HALO_TRACE_ENABLED
	halo_trace_zone_end(HALO_TRACE_ZONE_BOT_AI);
#endif
}

static struct halo_player_controller const bots_controller =
{
	.update = bots_controller_update,
	.controls_player = bots_player_is_bot,
};

struct halo_extension const bots_extension =
{
	.name = "bots",
	/* no bots yet; they join once the game is under way */
	.objects_placed = bots_initialize_for_new_map,
	/* derived, host-local navigation never outlives its map, a restored
	game or the tags it was built from */
	.dispose_from_old_map = navigation_world_reset,
	.invalidate_derived_state = navigation_world_reset,
	/* a game bots played in is not reported to the game list, Delta Stats
	or the event log */
	.suppress_game_report = bots_game_had_bots,
	.player_controller = &bots_controller,
};

#endif /* HALO_FEATURE_BOTS */
