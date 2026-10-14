/* INFECTION_RULES.C: bounded, allocation-free host simulation.
 * All time is in simulation ticks, never wall-clock/client supplied time.
 * Intentionally independent of cseries: standalone tests use the same code
 * the native game builds compile, without the Xbox/LP64 datum ABI.
 */
#include "infection_rules.h"
#include <assert.h>
#include <string.h>

void infection_rules_config_default(struct infection_rules_config *config)
{
	assert(config != NULL);
	memset(config, 0, sizeof(*config));
	config->ticks_per_second = 30;
	config->minimum_participants = 2;
	config->rounds = 3;
	config->round_seconds = 180;
	config->countdown_seconds = 3;
	config->results_seconds = 5;
	config->infected_respawn_seconds = 3;
	config->last_spartan_enabled = 1;
	config->infection_score = 1;
	config->kill_infected_score = 1;
	config->survival_score = 3;
	config->last_spartan_score = 2;
	config->win_score = 1;
}

int infection_rules_config_valid(struct infection_rules_config const *config)
{
	if (!config) return 0;
	if (config->ticks_per_second == 0 || config->ticks_per_second > 120) return 0;
	if (config->minimum_participants < 2 ||
		config->minimum_participants > INFECTION_MAXIMUM_PARTICIPANTS) return 0;
	if (config->rounds == 0 || config->rounds > INFECTION_MAXIMUM_ROUNDS) return 0;
	if (config->round_seconds == 0 || config->round_seconds > INFECTION_MAXIMUM_SECONDS) return 0;
	if (config->countdown_seconds > 60 || config->results_seconds > 60) return 0;
	if (config->infected_respawn_seconds > 60) return 0;
	if (config->alpha_count >= INFECTION_MAXIMUM_PARTICIPANTS) return 0;
	if (config->last_spartan_enabled > 1) return 0;
	if (config->infection_score < 0 || config->kill_infected_score < 0 ||
		config->survival_score < 0 || config->last_spartan_score < 0 || config->win_score < 0) return 0;
	return 1;
}

int infection_rules_initialize(struct infection_rules *rules,
	struct infection_rules_config const *config, uint32_t seed)
{
	/* Copy first: config may be rules->config during a match reset. */
	struct infection_rules_config copy;
	if (!rules || !infection_rules_config_valid(config) || seed == 0) return 0;
	copy = *config;
	memset(rules, 0, sizeof(*rules));
	rules->config = copy;
	rules->random_state = seed;
	return 1;
}

static uint32_t infection_find(struct infection_rules const *rules, uint32_t handle)
{
	uint32_t slot;
	if (handle == 0) return INFECTION_NO_SLOT;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		if (rules->participants[slot].connected && rules->participants[slot].handle == handle) return slot;
	return INFECTION_NO_SLOT;
}

static int infection_matches(struct infection_rules const *rules, uint32_t slot, uint32_t handle)
{
	return rules && slot < INFECTION_MAXIMUM_PARTICIPANTS && handle != 0 &&
		rules->participants[slot].connected && rules->participants[slot].handle == handle;
}

static uint32_t infection_connected(struct infection_rules const *rules)
{
	uint32_t slot, count = 0;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		if (rules->participants[slot].connected) count++;
	return count;
}

int infection_rules_join(struct infection_rules *rules, uint32_t slot, uint32_t handle)
{
	struct infection_participant *participant;
	if (!rules || slot >= INFECTION_MAXIMUM_PARTICIPANTS || handle == 0) return 0;
	if (rules->phase == _infection_match_results || rules->participants[slot].connected) return 0;
	if (handle <= rules->last_admission_handle) return 0;
	participant = &rules->participants[slot];
	memset(participant, 0, sizeof(*participant));
	memset(&rules->deaths[slot], 0, sizeof(rules->deaths[slot]));
	participant->handle = handle;
	participant->connected = 1;
	rules->last_admission_handle = handle;
	if (rules->phase == _infection_countdown || rules->phase == _infection_active)
		participant->role = _infection_beta;
	return 1;
}

int infection_rules_leave(struct infection_rules *rules, uint32_t slot, uint32_t handle)
{
	if (!infection_matches(rules, slot, handle)) return 0;
	rules->participants[slot].connected = 0;
	rules->participants[slot].alive = 0;
	rules->participants[slot].respawn_ticks = 0;
	rules->deaths[slot].pending = 0;
	/* Keep the once-per-round LSS latch even when that identity leaves. */
	return 1;
}

int infection_rules_can_spawn(struct infection_rules const *rules, uint32_t slot, uint32_t handle)
{
	if (!infection_matches(rules, slot, handle)) return 0;
	if (rules->phase != _infection_countdown && rules->phase != _infection_active) return 0;
	return rules->participants[slot].role != _infection_role_none &&
		!rules->participants[slot].alive && !rules->participants[slot].respawn_ticks &&
		!rules->deaths[slot].pending && rules->participants[slot].life_id != UINT32_MAX;
}

int infection_rules_spawn(struct infection_rules *rules, uint32_t slot, uint32_t handle)
{
	if (!infection_rules_can_spawn(rules, slot, handle)) return 0;
	rules->participants[slot].life_id++;
	rules->participants[slot].alive = 1;
	return 1;
}

uint32_t infection_rules_survivors(struct infection_rules const *rules)
{
	uint32_t slot, count = 0;
	assert(rules != NULL);
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		if (rules->participants[slot].connected && rules->participants[slot].role == _infection_survivor) count++;
	return count;
}

static void infection_prepare(struct infection_rules *rules)
{
	uint32_t slot;
	rules->last_spartan_handle = 0;
	rules->winner = _infection_no_winner;
	memset(rules->deaths, 0, sizeof(rules->deaths));
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		rules->participants[slot].role = _infection_role_none;
		rules->participants[slot].alive = 0;
		rules->participants[slot].respawn_ticks = 0;
	}
	rules->phase = _infection_preparation;
}

static uint32_t infection_random(struct infection_rules *rules)
{
	uint32_t value = rules->random_state;
	assert(value != 0);
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	rules->random_state = value;
	return value;
}

static void infection_select(struct infection_rules *rules)
{
	uint32_t candidates[INFECTION_MAXIMUM_PARTICIPANTS];
	uint32_t slot, count = 0, index, alphas;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		if (!rules->participants[slot].connected) continue;
		candidates[count++] = slot;
		rules->participants[slot].role = _infection_survivor;
	}
	assert(count >= 2);
	alphas = rules->config.alpha_count;
	if (!alphas) alphas = count >= 8 ? 2U : 1U;
	if (alphas >= count) alphas = count - 1;
	/* Partial Fisher-Yates. Bounded multiply-high range mapping, no retry
	 * loop/modulo bias; negligible PRNG range bias, not cryptographic. */
	for (index = 0; index < alphas; index++)
	{
		uint32_t choice = index + (uint32_t)(((uint64_t)infection_random(rules) * (count - index)) >> 32);
		uint32_t selected = candidates[choice];
		candidates[choice] = candidates[index];
		candidates[index] = selected;
		rules->participants[selected].role = _infection_alpha;
	}
	assert(rules->round_id != UINT32_MAX);
	rules->round_id++;
	rules->phase = _infection_countdown;
	rules->phase_ticks = rules->config.countdown_seconds * rules->config.ticks_per_second;
}

int infection_rules_death(struct infection_rules *rules, uint32_t round_id,
	uint32_t slot, uint32_t handle, uint32_t life_id, uint32_t killer_handle)
{
	struct infection_death *death;
	uint32_t killer_slot;
	if (!infection_matches(rules, slot, handle)) return 0;
	if (rules->phase != _infection_active || round_id != rules->round_id) return 0;
	if (!rules->participants[slot].alive || life_id != rules->participants[slot].life_id) return 0;
	if (rules->deaths[slot].pending) return 0;
	death = &rules->deaths[slot];
	death->round_id = round_id;
	death->victim_handle = handle;
	death->life_id = life_id;
	death->killer_handle = killer_handle;
	killer_slot = infection_find(rules, killer_handle);
	death->killer_role = killer_slot == INFECTION_NO_SLOT ? _infection_role_none :
		rules->participants[killer_slot].role;
	death->pending = 1;
	return 1;
}

static void infection_award(struct infection_participant *participant, int32_t points)
{
	assert(participant->score >= 0);
	assert(points >= 0);
	if (points > INT32_MAX - participant->score) participant->score = INT32_MAX;
	else participant->score += points;
}

static void infection_resolve_deaths(struct infection_rules *rules)
{
	uint32_t slot;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct infection_death *death = &rules->deaths[slot];
		struct infection_participant *victim = &rules->participants[slot];
		uint32_t killer_slot;
		uint8_t victim_role;
		if (!death->pending) continue;
		death->pending = 0;
		if (!victim->connected || !victim->alive || victim->handle != death->victim_handle ||
			victim->life_id != death->life_id || death->round_id != rules->round_id) continue;
		victim_role = victim->role;
		victim->alive = 0;
		victim->respawn_ticks = rules->config.infected_respawn_seconds * rules->config.ticks_per_second;
		if (victim_role == _infection_survivor) victim->role = _infection_beta;
		killer_slot = infection_find(rules, death->killer_handle);
		if (killer_slot == INFECTION_NO_SLOT || killer_slot == slot) continue;
		if (victim_role == _infection_survivor &&
			(death->killer_role == _infection_alpha || death->killer_role == _infection_beta))
			infection_award(&rules->participants[killer_slot], rules->config.infection_score);
		else if (victim_role != _infection_survivor && death->killer_role == _infection_survivor)
			infection_award(&rules->participants[killer_slot], rules->config.kill_infected_score);
	}
}

static void infection_finish(struct infection_rules *rules, uint8_t winner)
{
	uint32_t slot;
	assert(rules->phase == _infection_active);
	assert(rules->completed_rounds < rules->config.rounds);
	rules->winner = winner;
	rules->completed_rounds++;
	rules->phase = _infection_round_results;
	rules->phase_ticks = rules->config.results_seconds * rules->config.ticks_per_second;
	rules->last_spartan_handle = 0;
	memset(rules->deaths, 0, sizeof(rules->deaths));
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct infection_participant *participant = &rules->participants[slot];
		participant->respawn_ticks = 0;
		if (!participant->connected) continue;
		if (winner == _infection_survivors_win && participant->role == _infection_survivor)
		{
			infection_award(participant, rules->config.survival_score);
			infection_award(participant, rules->config.win_score);
		}
		else if (winner == _infection_infected_win &&
			(participant->role == _infection_alpha || participant->role == _infection_beta))
			infection_award(participant, rules->config.win_score);
	}
}

static void infection_last_spartan(struct infection_rules *rules)
{
	uint32_t slot;
	if (!rules->config.last_spartan_enabled || rules->last_spartan_handle) return;
	if (infection_rules_survivors(rules) != 1) return;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		struct infection_participant *participant = &rules->participants[slot];
		if (!participant->connected || !participant->alive || participant->role != _infection_survivor) continue;
		rules->last_spartan_handle = participant->handle;
		infection_award(participant, rules->config.last_spartan_score);
		return;
	}
}

static void infection_active_tick(struct infection_rules *rules)
{
	uint32_t slot;
	/* Age existing respawn delays before deaths, so a new death gets the
	 * full configured delay, independent of slot/order. */
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		if (rules->participants[slot].respawn_ticks) rules->participants[slot].respawn_ticks--;
	infection_resolve_deaths(rules);
	if (infection_connected(rules) == 0) infection_finish(rules, _infection_no_winner);
	else if (infection_rules_survivors(rules) == 0) infection_finish(rules, _infection_infected_win);
	else if (infection_rules_survivors(rules) == infection_connected(rules))
		infection_finish(rules, _infection_survivors_win); /* all Infected left */
	else
	{
		infection_last_spartan(rules);
		if (rules->phase_ticks) rules->phase_ticks--;
		if (!rules->phase_ticks) infection_finish(rules, _infection_survivors_win);
	}
}

void infection_rules_tick(struct infection_rules *rules)
{
	assert(rules != NULL);
	assert(infection_rules_config_valid(&rules->config));
	switch (rules->phase)
	{
	case _infection_waiting:
		if (infection_connected(rules) >= rules->config.minimum_participants) infection_prepare(rules);
		break;
	case _infection_preparation:
		rules->phase = _infection_select_alphas;
		break;
	case _infection_select_alphas:
		if (infection_connected(rules) < rules->config.minimum_participants) rules->phase = _infection_waiting;
		else infection_select(rules);
		break;
	case _infection_countdown:
		if (infection_connected(rules) < rules->config.minimum_participants ||
			infection_rules_survivors(rules) == 0 ||
			infection_rules_survivors(rules) == infection_connected(rules))
		{
			rules->phase = _infection_waiting;
			break;
		}
		if (rules->phase_ticks) rules->phase_ticks--;
		if (!rules->phase_ticks)
		{
			rules->phase = _infection_active;
			rules->phase_ticks = rules->config.round_seconds * rules->config.ticks_per_second;
		}
		break;
	case _infection_active:
		infection_active_tick(rules);
		break;
	case _infection_round_results:
		if (rules->phase_ticks) rules->phase_ticks--;
		if (!rules->phase_ticks)
			rules->phase = rules->completed_rounds == rules->config.rounds ?
				_infection_match_results : _infection_waiting;
		break;
	default: break;
	}
}

uint32_t infection_rules_rankings(struct infection_rules const *rules, uint32_t *slots, uint32_t capacity)
{
	uint32_t slot, count = 0, index;
	assert(rules != NULL);
	if (!slots || !capacity) return 0;
	if (capacity > INFECTION_MAXIMUM_PARTICIPANTS) capacity = INFECTION_MAXIMUM_PARTICIPANTS;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		if (!rules->participants[slot].connected) continue;
		for (index = 0; index < count; index++)
			if (rules->participants[slot].score > rules->participants[slots[index]].score) break;
		if (index == capacity) continue;
		if (count < capacity) count++;
		{
			uint32_t move;
			for (move = count - 1; move > index; move--) slots[move] = slots[move - 1];
		}
		slots[index] = slot;
	}
	return count;
}

char const *infection_rules_phase_name(uint8_t phase)
{
	static char const *const names[] = { "Waiting", "Preparation", "Select Alpha Infected",
		"Countdown", "Active", "Round results", "Match results" };
	return phase <= _infection_match_results ? names[phase] : "Invalid";
}

char const *infection_rules_role_name(uint8_t role)
{
	static char const *const names[] = { "Unassigned", "Survivor", "Alpha Infected", "Beta Infected" };
	return role <= _infection_beta ? names[role] : "Invalid";
}
