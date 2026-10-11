/*
BOT_NAVIGATION.C

port: normal bots' BSP navigation and lane policy. The old start-location graph
is too sparse to represent walls, passages or the spaces between starts. This
adapter routes over CE's validated walkable collision surfaces, checking each
route edge with the spawned biped's live collision capsule before moving.

Each active bot owns one bounded search and route. Search expands at most four
polygons per tick; a blocked route waits for a bounded interval, marks one
polygon temporarily unavailable and replans. Team bots patrol three lanes:
pairs travel together in a lane, with opposite lateral offsets, and pairs in a
lane occupy near/far positions. No map tags are changed and no movement bypasses
Halo physics.
*/

/* ---------- headers */

#include "cseries.h"
#include "game.h"
#include "game_engine.h"
#include "players_runtime.h"
#include "scenario/scenario_definitions.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/biped_definitions.h"
#include "real_math.h"
#include "navigation_world.h"
#include "bot_navigation.h"

/* port/linux/src/platform.h's */
void platform_log(char const *format, ...);

#ifdef HALO_FEATURE_BOTS

/* ---------- constants */

enum
{
	BOT_NAVIGATION_EXPANSIONS_PER_TICK = 4,
	BOT_NAVIGATION_PROGRESS_TICKS = TICKS_PER_SECOND,
	BOT_NAVIGATION_REPLAN_DISTANCE = 12,
	BOT_NAVIGATION_GOAL_ATTEMPTS = 16,
};

#define BOT_NAVIGATION_MINIMUM_GOAL_DISTANCE 12.f
#define BOT_NAVIGATION_LANE_SIDE_OFFSET 2.5f
#define BOT_NAVIGATION_NEAR_PROGRESS 0.48f
#define BOT_NAVIGATION_FAR_PROGRESS 0.82f

/* ---------- private prototypes */

static real bot_nav_abs(real value);
static real bot_nav_horizontal_distance(struct sn_point a, struct sn_point b);
static real bot_nav_distance(struct sn_point a, struct sn_point b);
static struct sn_point bot_nav_position(long unit_index);
static boolean bot_nav_lane_axis(long team_index, real_point3d *home,
	real *forward_x, real *forward_y, real *length);
static boolean bot_nav_lane_goal(struct bot_navigation_agent *agent,
	struct sn_resource const *resource, struct sn_point start, struct sn_point *goal);
static boolean bot_nav_random_goal(struct sn_resource const *resource,
	struct sn_point start, struct engine_ai_rng32_state *rng, struct sn_point *goal);
static enum bot_navigation_result bot_nav_route_step(struct bot_navigation_agent *agent,
	long unit_index, struct sn_point goal, real speed, real arrival_distance,
	boolean patrol, struct bot_navigation_command *command);
static enum bot_navigation_result bot_nav_search(struct bot_navigation_agent *agent,
	long unit_index, struct sn_resource const *resource, struct sn_point start);
static enum bot_navigation_result bot_nav_follow(struct bot_navigation_agent *agent,
	long unit_index, struct sn_resource const *resource, struct sn_point origin,
	real speed, boolean patrol, struct bot_navigation_command *command);

/* ---------- public code */

void bot_navigation_agent_initialize(
	struct bot_navigation_agent *agent,
	short bot_number,
	long team_index,
	long team_rank)
{
	long squad;

	if (!agent)
		return;
	csmemset(agent, 0, sizeof(*agent));
	agent->bot_number = bot_number;
	agent->team_index = team_index;
	agent->team_rank = team_rank;
	agent->lane_index = NONE;
	agent->progress_tick = NONE;
	if (team_index == NONE || team_rank < 0)
		return;

	squad = team_rank / BOT_NAVIGATION_SQUAD_SIZE;
	agent->lane_index = (short)(squad % BOT_NAVIGATION_LANES);
	agent->squad_index = squad / BOT_NAVIGATION_LANES;
	agent->lane_phase = (short)(agent->squad_index % 2);
	agent->lane_side = team_rank % BOT_NAVIGATION_SQUAD_SIZE == 0 ? -1 : 1;
	agent->lane_assigned = TRUE;
}

void bot_navigation_agent_reset_path(
	struct bot_navigation_agent *agent)
{
	if (!agent)
		return;
	agent->searching = FALSE;
	agent->following = FALSE;
	agent->goal_valid = FALSE;
	agent->waypoint = 0;
	agent->route.count = 0;
	agent->progress_tick = NONE;
	agent->stuck_count = 0;
}

enum bot_navigation_result bot_navigation_agent_move_to(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct sn_point goal,
	real speed,
	real arrival_distance,
	struct bot_navigation_command *command)
{
	if (!agent || !command)
		return BOT_NAVIGATION_FAILED;
	csmemset(command, 0, sizeof(*command));
	return bot_nav_route_step(agent, unit_index, goal, speed, arrival_distance,
		FALSE, command);
}

enum bot_navigation_result bot_navigation_agent_roam(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct engine_ai_rng32_state *rng,
	struct bot_navigation_command *command)
{
	struct sn_resource const *resource;
	struct sn_point start;
	struct sn_point goal;
	boolean selected;
	enum bot_navigation_result result;

	if (!agent || !rng || !command)
		return BOT_NAVIGATION_FAILED;
	csmemset(command, 0, sizeof(*command));
	resource = navigation_world_resource();
	if (!resource)
		return BOT_NAVIGATION_UNAVAILABLE;

	if (agent->searching || agent->following)
		goal = agent->goal;
	else
	{
		start = bot_nav_position(unit_index);
		selected = agent->lane_assigned ?
			bot_nav_lane_goal(agent, resource, start, &goal) :
			bot_nav_random_goal(resource, start, rng, &goal);
		if (!selected)
			return BOT_NAVIGATION_FAILED;
		agent->goal = goal;
		agent->goal_valid = TRUE;
	}

	result = bot_nav_route_step(agent, unit_index, goal, 1.f, 0.6f, TRUE, command);
	if (result == BOT_NAVIGATION_FAILED && agent->lane_assigned)
		agent->lane_phase = (short)(1 - agent->lane_phase);
	return result;
}

/* ---------- private code: positions and lane targets */

static real bot_nav_abs(
	real value)
{
	return value < 0.f ? -value : value;
}

static real bot_nav_horizontal_distance(
	struct sn_point a,
	struct sn_point b)
{
	real x = a.x - b.x;
	real y = a.y - b.y;

	return square_root(x * x + y * y);
}

static real bot_nav_distance(
	struct sn_point a,
	struct sn_point b)
{
	real x = a.x - b.x;
	real y = a.y - b.y;
	real z = a.z - b.z;

	return square_root(x * x + y * y + z * z);
}

/* Surface-navigation points use the ground-foot coordinate, not the pill's
centered origin. This is the same adjustment as the verified Bot 1 probe. */
static struct sn_point bot_nav_position(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct biped_definition *definition = biped_definition_get(unit->definition_index);
	real_point3d origin;
	struct sn_point point;

	object_get_origin(unit_index, &origin);
	if (TEST_FLAG(definition->biped.flags, _biped_pill_centered_at_origin_bit))
		origin.z -= definition->biped.collision_radius;
	point.x = origin.x;
	point.y = origin.y;
	point.z = origin.z;
	return point;
}

/* Team start locations give the map-relative direction of advance. */
static boolean bot_nav_lane_axis(
	long team_index,
	real_point3d *home,
	real *forward_x,
	real *forward_y,
	real *length)
{
	short count = player_get_starting_location_count();
	short index;
	long enemy_team = team_index == 0 ? 1 : 0;
	long home_count = 0;
	long enemy_count = 0;
	real_point3d enemy;
	real dx;
	real dy;

	csmemset(home, 0, sizeof(*home));
	csmemset(&enemy, 0, sizeof(enemy));
	for (index = 0; index < count; index++)
	{
		struct player_starting_location const *location = player_get_starting_location(index);

		if (!location)
			continue;
		if (location->team_index == team_index)
		{
			home->x += location->position.x;
			home->y += location->position.y;
			home->z += location->position.z;
			home_count++;
		}
		else if (location->team_index == enemy_team)
		{
			enemy.x += location->position.x;
			enemy.y += location->position.y;
			enemy.z += location->position.z;
			enemy_count++;
		}
	}
	if (home_count == 0 || enemy_count == 0)
		return FALSE;
	home->x /= (real)home_count;
	home->y /= (real)home_count;
	home->z /= (real)home_count;
	enemy.x /= (real)enemy_count;
	enemy.y /= (real)enemy_count;
	enemy.z /= (real)enemy_count;
	dx = enemy.x - home->x;
	dy = enemy.y - home->y;
	*length = square_root(dx * dx + dy * dy);
	if (*length < 1.f)
		return FALSE;
	*forward_x = dx / *length;
	*forward_y = dy / *length;
	return TRUE;
}

/* Pick a walkable surface near this bot's lane and assigned depth. */
static boolean bot_nav_lane_goal(
	struct bot_navigation_agent *agent,
	struct sn_resource const *resource,
	struct sn_point start,
	struct sn_point *goal)
{
	real_point3d home;
	real forward_x;
	real forward_y;
	real length;
	real lateral_min = 0.f;
	real lateral_max = 0.f;
	real desired_progress;
	real desired_lateral;
	real best_cost = 100000000.f;
	real actual_progress;
	real actual_lateral;
	uint32_t best = SN_NONE;
	uint32_t index;
	boolean first = TRUE;

	if (!bot_nav_lane_axis(agent->team_index, &home, &forward_x, &forward_y, &length))
		return FALSE;
	for (index = 0; index < resource->polygon_count; index++)
	{
		struct sn_point center = resource->polygons[index].center;
		real lateral = -(center.x - home.x) * forward_y + (center.y - home.y) * forward_x;

		if (first || lateral < lateral_min)
			lateral_min = lateral;
		if (first || lateral > lateral_max)
			lateral_max = lateral;
		first = FALSE;
	}
	if (first)
		return FALSE;

	desired_progress = length * (agent->lane_phase ?
		BOT_NAVIGATION_FAR_PROGRESS : BOT_NAVIGATION_NEAR_PROGRESS);
	desired_lateral = lateral_min + (lateral_max - lateral_min) *
		((real)agent->lane_index + 0.5f) / BOT_NAVIGATION_LANES;
	desired_lateral += (real)agent->lane_side * BOT_NAVIGATION_LANE_SIDE_OFFSET;
	for (index = 0; index < resource->polygon_count; index++)
	{
		struct sn_point center = resource->polygons[index].center;
		real progress = (center.x - home.x) * forward_x + (center.y - home.y) * forward_y;
		real lateral = -(center.x - home.x) * forward_y + (center.y - home.y) * forward_x;
		real cost = bot_nav_abs(progress - desired_progress) +
			2.f * bot_nav_abs(lateral - desired_lateral) +
			0.25f * bot_nav_abs(center.z - home.z);

		if (progress < 0.f || progress > length * 1.2f)
			cost += length * 2.f;
		if (bot_nav_horizontal_distance(start, center) < BOT_NAVIGATION_MINIMUM_GOAL_DISTANCE)
			cost += length;
		if (cost < best_cost)
		{
			best_cost = cost;
			best = index;
		}
	}
	if (best == SN_NONE)
		return FALSE;
	*goal = resource->polygons[best].center;
	actual_progress = (goal->x - home.x) * forward_x + (goal->y - home.y) * forward_y;
	actual_lateral = -(goal->x - home.x) * forward_y + (goal->y - home.y) * forward_x;
	platform_log("bots: bot %d lane target team %ld lane %d phase %d home (%.1f %.1f) "
		"axis (%.2f %.2f) length %.1f cross %.1f..%.1f want %.1f/%.1f "
		"got %.1f/%.1f goal (%.1f %.1f %.1f) polygon %lu", agent->bot_number, agent->team_index,
		agent->lane_index, agent->lane_phase, home.x, home.y, forward_x, forward_y,
		length, lateral_min, lateral_max, desired_progress, desired_lateral,
		actual_progress, actual_lateral, goal->x, goal->y, goal->z, (unsigned long)best);
	return TRUE;
}

static boolean bot_nav_random_goal(
	struct sn_resource const *resource,
	struct sn_point start,
	struct engine_ai_rng32_state *rng,
	struct sn_point *goal)
{
	uint32_t attempt;

	if (resource->polygon_count == 0)
		return FALSE;
	for (attempt = 0; attempt < BOT_NAVIGATION_GOAL_ATTEMPTS; attempt++)
	{
		uint32_t candidate = 0;

		if (engine_ai_rng32_bounded(rng, resource->polygon_count, &candidate) != ENGINE_AI_RNG32_OK)
			return FALSE;
		if (bot_nav_horizontal_distance(start, resource->polygons[candidate].center) >=
			BOT_NAVIGATION_MINIMUM_GOAL_DISTANCE)
		{
			*goal = resource->polygons[candidate].center;
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------- private code: bounded route search and following */

static enum bot_navigation_result bot_nav_route_step(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct sn_point goal,
	real speed,
	real arrival_distance,
	boolean patrol,
	struct bot_navigation_command *command)
{
	struct sn_resource const *resource;
	struct sn_point origin = bot_nav_position(unit_index);
	enum bot_navigation_result result;

	if ((agent->searching || agent->following) && agent->patrol_goal != patrol)
		bot_navigation_agent_reset_path(agent);
	agent->patrol_goal = patrol;
	if (patrol)
		agent->goal = goal;
	if (!patrol && bot_nav_horizontal_distance(origin, goal) <= arrival_distance)
	{
		bot_navigation_agent_reset_path(agent);
		return BOT_NAVIGATION_ARRIVED;
	}

	resource = navigation_world_resource();
	if (!resource)
		return BOT_NAVIGATION_UNAVAILABLE;
	if ((agent->searching || agent->following) &&
		bot_nav_distance(agent->goal, goal) > BOT_NAVIGATION_REPLAN_DISTANCE)
	{
		bot_navigation_agent_reset_path(agent);
	}
	if (!agent->searching && !agent->following)
	{
		agent->goal = goal;
		agent->goal_valid = TRUE;
		return bot_nav_search(agent, unit_index, resource, origin);
	}
	if (agent->searching)
	{
		result = bot_nav_search(agent, unit_index, resource, origin);
		if (result != BOT_NAVIGATION_MOVING)
			return result;
	}
	return bot_nav_follow(agent, unit_index, resource, origin, speed, patrol, command);
}

static enum bot_navigation_result bot_nav_search(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct sn_resource const *resource,
	struct sn_point start)
{
	enum sn_status status;

	if (!agent->searching)
	{
		status = sn_query_begin(&agent->search, resource, start, agent->goal,
			navigation_world_obstacle_revision());
		if (status != SN_RUNNING)
		{
			platform_log("bots: bot %d surface %s search failed: %s", agent->bot_number,
				agent->patrol_goal ? "patrol" : "tactical", sn_status_name(status));
			bot_navigation_agent_reset_path(agent);
			return BOT_NAVIGATION_FAILED;
		}
		agent->searching = TRUE;
	}
	status = sn_query_step(&agent->search, resource, navigation_world_obstacle_revision(),
		BOT_NAVIGATION_EXPANSIONS_PER_TICK, navigation_world_segment_clear, &unit_index);
	if (status == SN_RUNNING)
		return BOT_NAVIGATION_WAITING;
	if (status == SN_FOUND)
		status = sn_query_result(&agent->search, resource,
			navigation_world_obstacle_revision(), &agent->route);
	if (status != SN_FOUND)
	{
		platform_log("bots: bot %d surface %s search failed: %s", agent->bot_number,
			agent->patrol_goal ? "patrol" : "tactical", sn_status_name(status));
		bot_navigation_agent_reset_path(agent);
		return BOT_NAVIGATION_FAILED;
	}

	agent->searching = FALSE;
	agent->following = TRUE;
	agent->waypoint = 0;
	agent->progress_tick = game_time_get();
	agent->progress_position = start;
	agent->stuck_count = 0;
	platform_log("bots: bot %d team %ld lane %d squad %ld surface %s route goal "
		"(%.1f %.1f %.1f) waypoints %lu", agent->bot_number, agent->team_index,
		agent->lane_index, agent->squad_index, agent->patrol_goal ? "patrol" : "tactical",
		agent->route.points[agent->route.count - 1].x,
		agent->route.points[agent->route.count - 1].y,
		agent->route.points[agent->route.count - 1].z, (unsigned long)agent->route.count);
	return BOT_NAVIGATION_MOVING;
}

static enum bot_navigation_result bot_nav_follow(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct sn_resource const *resource,
	struct sn_point origin,
	real speed,
	boolean patrol,
	struct bot_navigation_command *command)
{
	struct sn_point target;
	uint32_t polygon;
	real distance;
	long now = game_time_get();
	long ignored_unit = unit_index;

	if (!agent->following || !sn_route_current(&agent->route, resource,
		navigation_world_obstacle_revision()))
	{
		bot_navigation_agent_reset_path(agent);
		return BOT_NAVIGATION_FAILED;
	}
	while (agent->waypoint < agent->route.count)
	{
		struct sn_point next = agent->route.points[agent->waypoint];
		if (bot_nav_horizontal_distance(origin, next) >= 0.45f || bot_nav_abs(origin.z - next.z) >= 0.35f)
			break;
		agent->waypoint++;
	}
	if (agent->waypoint >= agent->route.count)
	{
		bot_navigation_agent_reset_path(agent);
		if (patrol && agent->lane_assigned)
			agent->lane_phase = (short)(1 - agent->lane_phase);
		return BOT_NAVIGATION_ARRIVED;
	}

	if (agent->progress_tick == NONE)
	{
		agent->progress_tick = now;
		agent->progress_position = origin;
	}
	else if (now - agent->progress_tick >= BOT_NAVIGATION_PROGRESS_TICKS)
	{
		if (bot_nav_distance(origin, agent->progress_position) < 0.5f)
			agent->stuck_count++;
		else
			agent->stuck_count = 0;
		agent->progress_tick = now;
		agent->progress_position = origin;
		if (agent->stuck_count >= 2)
		{
			polygon = agent->route.polygons[agent->waypoint];
			navigation_world_block(polygon);
			platform_log("bots: bot %d surface route blocked at polygon %lu; replanning",
				agent->bot_number, (unsigned long)polygon);
			bot_navigation_agent_reset_path(agent);
			if (patrol && agent->lane_assigned)
				agent->lane_phase = (short)(1 - agent->lane_phase);
			return BOT_NAVIGATION_FAILED;
		}
	}

	target = agent->route.points[agent->waypoint];
	polygon = agent->route.polygons[agent->waypoint];
	/* Never emit movement toward a waypoint unless the live biped capsule can
	 * sweep there. A dynamic occupant blocks briefly; a persistent block replans. */
	if (!navigation_world_segment_clear(&ignored_unit, origin, polygon, target, polygon))
		return BOT_NAVIGATION_WAITING;
	distance = bot_nav_horizontal_distance(origin, target);
	command->has_move = TRUE;
	command->yaw = arctangent(target.y - origin.y, target.x - origin.x);
	command->speed = MIN(speed, distance * 2.f);
	return BOT_NAVIGATION_MOVING;
}

#endif /* HALO_FEATURE_BOTS */
