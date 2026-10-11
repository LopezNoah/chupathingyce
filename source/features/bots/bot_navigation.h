/*
BOT_NAVIGATION.H

port: per-bot BSP navigation and lane assignment (bot_navigation.c). Each
agent owns one bounded query and route. The interface hides BSP projections,
route searches, collision checks, blockage recovery and team lane targets.
*/

#ifndef HALO_BOT_NAVIGATION_H
#define HALO_BOT_NAVIGATION_H
#pragma once

#include "cseries/cseries.h"
#include "engine_ai/behavior_intent.h"
#include "engine_ai/surface_navigation.h"

/* ---------- constants */

enum
{
	BOT_NAVIGATION_LANES = 3,
	BOT_NAVIGATION_SQUAD_SIZE = 2,
};

/* ---------- structures */

struct bot_navigation_agent
{
	/* Two bots form a squad in one of three lanes. Pairs begin at different
	depths, and alternate between those points as they patrol. */
	short bot_number;
	long team_index;
	long team_rank;
	long squad_index;
	short lane_index;
	short lane_phase;
	short lane_side;
	boolean lane_assigned;

	struct sn_query search;
	struct sn_route route;
	struct sn_point goal;
	struct sn_point progress_position;
	uint32_t waypoint;
	long progress_tick;
	short stuck_count;
	boolean searching;
	boolean following;
	boolean goal_valid;
	boolean patrol_goal;
};

struct bot_navigation_command
{
	boolean has_move;
	real yaw;
	real speed;
};

enum bot_navigation_result
{
	BOT_NAVIGATION_UNAVAILABLE = 0, /* BSP resource is still building or absent */
	BOT_NAVIGATION_WAITING,
	BOT_NAVIGATION_MOVING,
	BOT_NAVIGATION_ARRIVED,
	BOT_NAVIGATION_FAILED,
};

/* team_rank counts lower-slot bots on this team. NONE means no team lanes. */
void bot_navigation_agent_initialize(
	struct bot_navigation_agent *agent,
	short bot_number,
	long team_index,
	long team_rank);

/* Cancel one route without changing the agent's assigned lane or patrol phase. */
void bot_navigation_agent_reset_path(
	struct bot_navigation_agent *agent);

/* Route to a tactical goal using the map's BSP walkable surfaces and live biped
 * capsule checks. ARRIVED means within arrival_distance; a moving target more
 * than 12 metres from its prior goal causes a bounded replan. */
enum bot_navigation_result bot_navigation_agent_move_to(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct sn_point goal,
	real speed,
	real arrival_distance,
	struct bot_navigation_command *command);

/* Patrol the assigned team lane, or choose a separated random BSP goal when
 * lanes are not available. ARRIVED advances the lane patrol phase. */
enum bot_navigation_result bot_navigation_agent_roam(
	struct bot_navigation_agent *agent,
	long unit_index,
	struct engine_ai_rng32_state *rng,
	struct bot_navigation_command *command);

#endif /* HALO_BOT_NAVIGATION_H */
