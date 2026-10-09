/* INFECTION_RULES.H
 * Independent authoritative rules core. No engine datums, assets or packets.
 * The engine adapter must call these mutators ONLY on the host, submit all
 * deaths/disconnects for a simulation tick, then call infection_rules_tick.
 * This header is not a wire format; never transmit these structs.
 */
#ifndef INFECTION_RULES_H
#define INFECTION_RULES_H

#include <stdint.h>

enum
{
	INFECTION_MAXIMUM_PARTICIPANTS = 128,
	INFECTION_NO_SLOT = 128,
	INFECTION_MAXIMUM_ROUNDS = 100,
	INFECTION_MAXIMUM_SECONDS = 3600
};

enum infection_phase
{
	_infection_waiting = 0,
	_infection_preparation,
	_infection_select_alphas,
	_infection_countdown,
	_infection_active,
	_infection_round_results,
	_infection_match_results
};

enum infection_role
{
	_infection_role_none = 0,
	_infection_survivor,
	_infection_alpha,
	_infection_beta
};

enum infection_winner
{
	_infection_no_winner = 0,
	_infection_survivors_win,
	_infection_infected_win
};

struct infection_rules_config
{
	uint32_t ticks_per_second;
	uint32_t minimum_participants;
	uint32_t rounds;
	uint32_t round_seconds;
	uint32_t countdown_seconds;
	uint32_t results_seconds;
	uint32_t infected_respawn_seconds;
	/* 0: one for 2-7, two for 8+; always leaves at least one Survivor. */
	uint32_t alpha_count;
	uint8_t last_spartan_enabled;
	int32_t infection_score;
	int32_t kill_infected_score;
	int32_t survival_score;
	int32_t last_spartan_score;
	int32_t win_score;
};

struct infection_participant
{
	/* Opaque host identity, NOT a team or a slot. 0 means no identity.
	 * Host allocates monotonically increasing admission handles. This is
	 * session-local, independent of a player's permanent identity. */
	uint32_t handle;
	/* Never reset between rounds. A death refers to this exact spawned life. */
	uint32_t life_id;
	int32_t score;
	uint32_t respawn_ticks;
	uint8_t connected;
	uint8_t alive;
	uint8_t role;
};

struct infection_death
{
	uint32_t round_id;
	uint32_t victim_handle;
	uint32_t life_id;
	uint32_t killer_handle;
	uint8_t killer_role;
	uint8_t pending;
};

struct infection_rules
{
	struct infection_rules_config config;
	struct infection_participant participants[INFECTION_MAXIMUM_PARTICIPANTS];
	/* At most one accepted death per participant/life in each tick. */
	struct infection_death deaths[INFECTION_MAXIMUM_PARTICIPANTS];
	uint32_t random_state;
	uint32_t last_admission_handle;
	uint32_t round_id;
	uint32_t completed_rounds;
	uint32_t phase_ticks;
	uint32_t last_spartan_handle;
	uint8_t phase;
	uint8_t winner;
};

void infection_rules_config_default(struct infection_rules_config *config);
int infection_rules_config_valid(struct infection_rules_config const *config);
/* Returns 0 without modifying state for invalid config/NULL/zero seed. */
int infection_rules_initialize(struct infection_rules *rules,
	struct infection_rules_config const *config, uint32_t seed);
/* Admissions during countdown/active are Beta Infected. A new round
 * restores eligibility. Reconnects are new admissions, not resurrection.
 * handle must exceed every previously admitted handle in this session. */
int infection_rules_join(struct infection_rules *rules, uint32_t slot, uint32_t handle);
int infection_rules_leave(struct infection_rules *rules, uint32_t slot, uint32_t handle);
int infection_rules_can_spawn(struct infection_rules const *rules, uint32_t slot, uint32_t handle);
int infection_rules_spawn(struct infection_rules *rules, uint32_t slot, uint32_t handle);
/* killer_handle 0 means environment. Suicide passes victim_handle as killer.
 * Rejected events change nothing. The role/score conversion is committed by
 * tick, not by arrival order. No remote client may call this API. */
int infection_rules_death(struct infection_rules *rules, uint32_t round_id,
	uint32_t slot, uint32_t handle, uint32_t life_id, uint32_t killer_handle);
/* Exactly one simulation tick; process queued deaths before timeout/results.
 * One phase transition maximum per call, including when a duration is zero. */
void infection_rules_tick(struct infection_rules *rules);
uint32_t infection_rules_survivors(struct infection_rules const *rules);
/* Connected participants, descending score, slot breaks ties deterministically.
 * Returns count written, never more than capacity. */
uint32_t infection_rules_rankings(struct infection_rules const *rules,
	uint32_t *slots, uint32_t capacity);
char const *infection_rules_phase_name(uint8_t phase);
char const *infection_rules_role_name(uint8_t role);

#endif
