#include "../source/game/infection_rules.h"
#include <assert.h>
#include <stdio.h>

static void two_player_round(void)
{
	struct infection_rules rules;
	struct infection_rules_config config;
	uint32_t slot;
	uint32_t alphas = 0;
	infection_rules_config_default(&config);
	assert(config.round_seconds == 180);
	assert(config.rounds == 3);
	config.countdown_seconds = 0;
	assert(infection_rules_initialize(&rules, &config, 42));
	assert(infection_rules_join(&rules, 0, 100));
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_waiting);
	assert(infection_rules_join(&rules, 1, 101));
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_preparation);
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_select_alphas);
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_countdown);
	for (slot = 0; slot < 2; slot++)
	{
		if (rules.participants[slot].role == _infection_alpha) alphas++;
		assert(infection_rules_spawn(&rules, slot, 100 + slot));
	}
	assert(alphas == 1);
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_active);
	assert(infection_rules_survivors(&rules) == 1);
}

static void start_config(struct infection_rules *rules, uint32_t count,
	struct infection_rules_config const *config, uint32_t seed)
{
	uint32_t slot;
	assert(infection_rules_initialize(rules, config, seed));
	for (slot = 0; slot < count; slot++) assert(infection_rules_join(rules, slot, 100 + slot));
	infection_rules_tick(rules);
	infection_rules_tick(rules);
	infection_rules_tick(rules);
	assert(rules->phase == _infection_countdown);
	for (slot = 0; slot < count; slot++) assert(infection_rules_spawn(rules, slot, 100 + slot));
	infection_rules_tick(rules);
	assert(rules->phase == _infection_active);
}

static void test_config(struct infection_rules_config *config)
{
	infection_rules_config_default(config);
	config->ticks_per_second = 1;
	config->countdown_seconds = 0;
	config->results_seconds = 0;
	config->round_seconds = 4;
}

static void start(struct infection_rules *rules, uint32_t count)
{
	struct infection_rules_config config;
	test_config(&config);
	start_config(rules, count, &config, 42);
}

static uint32_t role_slot(struct infection_rules const *rules, uint8_t role)
{
	uint32_t slot;
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
		if (rules->participants[slot].connected && rules->participants[slot].role == role) return slot;
	assert(0);
	return INFECTION_NO_SLOT;
}

static void conversion_and_respawn(void)
{
	struct infection_rules rules;
	uint32_t survivor, alpha, life;
	start(&rules, 4);
	survivor = role_slot(&rules, _infection_survivor);
	alpha = role_slot(&rules, _infection_alpha);
	life = rules.participants[survivor].life_id;
	assert(infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, life, 100 + alpha));
	assert(!infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, life, 100 + alpha));
	infection_rules_tick(&rules);
	assert(rules.participants[survivor].role == _infection_beta);
	assert(!rules.participants[survivor].alive);
	assert(rules.participants[survivor].respawn_ticks == 3);
	assert(rules.participants[alpha].score == 1);
	assert(!infection_rules_spawn(&rules, survivor, 100 + survivor));
	infection_rules_tick(&rules);
	infection_rules_tick(&rules);
	infection_rules_tick(&rules);
	/* Timer expires here, so test respawn with a longer round below. */
	assert(!infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, life, 100 + alpha));
}

static void victory_and_match(void)
{
	struct infection_rules rules;
	uint32_t survivor, alpha, round, old_round, old_life, rankings[2];
	start(&rules, 2);
	survivor = role_slot(&rules, _infection_survivor);
	alpha = role_slot(&rules, _infection_alpha);
	infection_rules_tick(&rules);
	assert(rules.last_spartan_handle == 100 + survivor);
	assert(rules.participants[survivor].score == 2);
	old_round = rules.round_id;
	old_life = rules.participants[survivor].life_id;
	assert(infection_rules_death(&rules, old_round, survivor, 100 + survivor, old_life, 100 + alpha));
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_round_results);
	assert(rules.winner == _infection_infected_win);
	assert(rules.completed_rounds == 1);
	assert(rules.participants[alpha].score == 2); /* infection + win */
	assert(rules.participants[survivor].score == 3); /* LSS + faction win after conversion */
	assert(!rules.last_spartan_handle);
	for (round = 1; round < 3; round++)
	{
		uint32_t slot;
		infection_rules_tick(&rules);
		assert(rules.phase == _infection_waiting);
		infection_rules_tick(&rules);
		infection_rules_tick(&rules);
		infection_rules_tick(&rules);
		assert(rules.phase == _infection_countdown);
		for (slot = 0; slot < 2; slot++) assert(infection_rules_spawn(&rules, slot, 100 + slot));
		infection_rules_tick(&rules);
		assert(!infection_rules_death(&rules, old_round, survivor, 100 + survivor, old_life, 100 + alpha));
		survivor = role_slot(&rules, _infection_survivor);
		for (slot = 0; slot < 4; slot++) infection_rules_tick(&rules);
		assert(rules.phase == _infection_round_results);
		assert(rules.winner == _infection_survivors_win);
		assert(rules.participants[survivor].role == _infection_survivor);
	}
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_match_results);
	assert(rules.completed_rounds == 3);
	assert(infection_rules_rankings(&rules, rankings, 2) == 2);
	assert(rules.participants[rankings[0]].score >= rules.participants[rankings[1]].score);
	old_life = (uint32_t)rules.participants[0].score;
	infection_rules_tick(&rules);
	assert((uint32_t)rules.participants[0].score == old_life);
}

static void joins_and_disconnects(void)
{
	struct infection_rules rules;
	uint32_t survivor, alpha, life;
	start(&rules, 3);
	assert(infection_rules_join(&rules, 3, 200));
	assert(rules.participants[3].role == _infection_beta);
	assert(infection_rules_spawn(&rules, 3, 200));
	life = rules.participants[3].life_id;
	assert(infection_rules_death(&rules, rules.round_id, 3, 200, life, 0));
	assert(!infection_rules_leave(&rules, 3, 201));
	assert(infection_rules_leave(&rules, 3, 200));
	assert(infection_rules_join(&rules, 3, 201));
	assert(rules.participants[3].score == 0);
	assert(infection_rules_spawn(&rules, 3, 201));
	assert(!infection_rules_death(&rules, rules.round_id, 3, 200, life, 0));
	infection_rules_tick(&rules);
	assert(rules.participants[3].alive);
	start(&rules, 2);
	survivor = role_slot(&rules, _infection_survivor);
	assert(infection_rules_leave(&rules, survivor, 100 + survivor));
	infection_rules_tick(&rules);
	assert(rules.winner == _infection_infected_win);
	start(&rules, 2);
	alpha = role_slot(&rules, _infection_alpha);
	assert(infection_rules_leave(&rules, alpha, 100 + alpha));
	infection_rules_tick(&rules);
	assert(rules.winner == _infection_survivors_win);
	start(&rules, 2);
	assert(infection_rules_leave(&rules, 0, 100));
	assert(infection_rules_leave(&rules, 1, 101));
	infection_rules_tick(&rules);
	assert(rules.winner == _infection_no_winner);
}

static void environment_suicide_and_life_tokens(void)
{
	struct infection_rules rules;
	struct infection_rules_config config;
	uint32_t survivor, alpha, life, tick;
	test_config(&config);
	config.round_seconds = 30;
	start_config(&rules, 4, &config, 42);
	survivor = role_slot(&rules, _infection_survivor);
	assert(infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, 1, 0));
	infection_rules_tick(&rules);
	assert(rules.participants[survivor].role == _infection_beta);
	survivor = role_slot(&rules, _infection_survivor);
	assert(infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, 1, 100 + survivor));
	infection_rules_tick(&rules);
	assert(rules.participants[survivor].role == _infection_beta);
	alpha = role_slot(&rules, _infection_alpha);
	survivor = role_slot(&rules, _infection_survivor);
	assert(infection_rules_death(&rules, rules.round_id, alpha, 100 + alpha, 1, 100 + survivor));
	infection_rules_tick(&rules);
	assert(rules.participants[alpha].role == _infection_alpha);
	assert(rules.participants[survivor].score == 3); /* LSS + kill */
	life = rules.participants[alpha].life_id;
	for (tick = 0; tick < 2; tick++)
	{
		infection_rules_tick(&rules);
		assert(!infection_rules_can_spawn(&rules, alpha, 100 + alpha));
	}
	infection_rules_tick(&rules);
	assert(infection_rules_spawn(&rules, alpha, 100 + alpha));
	assert(rules.participants[alpha].life_id == life + 1);
	assert(!infection_rules_death(&rules, rules.round_id, alpha, 100 + alpha, life, 100 + survivor));
	assert(!infection_rules_spawn(&rules, alpha, 100 + alpha));
	assert(rules.participants[alpha].alive);
	assert(infection_rules_spawn(&rules, role_slot(&rules, _infection_beta),
		100 + role_slot(&rules, _infection_beta)));
}

static void simultaneous_deaths(void)
{
	struct infection_rules first, second;
	uint32_t slot, alpha;
	start(&first, 4);
	start(&second, 4);
	alpha = role_slot(&first, _infection_alpha);
	for (slot = 0; slot < 4; slot++)
		if (first.participants[slot].role == _infection_survivor)
			assert(infection_rules_death(&first, first.round_id, slot, 100 + slot, 1, 100 + alpha));
	for (slot = 4; slot > 0; slot--)
		if (second.participants[slot - 1].role == _infection_survivor)
			assert(infection_rules_death(&second, second.round_id, slot - 1, 99 + slot, 1, 100 + alpha));
	infection_rules_tick(&first);
	infection_rules_tick(&second);
	assert(first.winner == _infection_infected_win);
	assert(second.winner == _infection_infected_win);
	assert(!first.last_spartan_handle); /* no transient LSS while draining batch */
	assert(first.participants[alpha].score == 4); /* 3 conversions + win */
	for (slot = 0; slot < 4; slot++)
	{
		assert(first.participants[slot].role == second.participants[slot].role);
		assert(first.participants[slot].score == second.participants[slot].score);
	}
	/* A Survivor kills an Alpha in the same tick they are converted. Both
	 * awards use roles before the batch, regardless of event arrival order. */
	start(&first, 3);
	alpha = role_slot(&first, _infection_alpha);
	slot = role_slot(&first, _infection_survivor);
	assert(infection_rules_death(&first, first.round_id, slot, 100 + slot, 1, 100 + alpha));
	assert(infection_rules_death(&first, first.round_id, alpha, 100 + alpha, 1, 100 + slot));
	infection_rules_tick(&first);
	assert(first.participants[slot].score == 1);
	assert(first.participants[alpha].score == 1);
}

static void alpha_selection_and_bounds(void)
{
	struct infection_rules rules;
	struct infection_rules_config config;
	uint32_t count, seed, slot, alphas, seen;
	test_config(&config);
	for (count = 2; count <= 16; count++)
	{
		seen = 0;
		for (seed = 1; seed <= 32; seed++)
		{
			start_config(&rules, count, &config, seed * UINT32_C(0x9e3779b9));
			alphas = 0;
			for (slot = 0; slot < count; slot++)
				if (rules.participants[slot].role == _infection_alpha)
				{
					alphas++;
					seen |= UINT32_C(1) << slot;
				}
			assert(alphas == (count >= 8 ? 2U : 1U));
			assert(infection_rules_survivors(&rules) == count - alphas);
		}
		assert(seen != 0 && (seen & (seen - 1)) != 0); /* selection varies */
	}
	config.alpha_count = 3;
	start_config(&rules, 8, &config, 42);
	assert(infection_rules_survivors(&rules) == 5);
	start_config(&rules, 2, &config, 42);
	assert(infection_rules_survivors(&rules) == 1); /* clamp, leave a Survivor */
	start_config(&rules, INFECTION_MAXIMUM_PARTICIPANTS, &config, 42);
	assert(infection_rules_survivors(&rules) == 125);
	assert(!infection_rules_join(&rules, INFECTION_NO_SLOT, 1000));
	assert(!infection_rules_death(&rules, rules.round_id, INFECTION_NO_SLOT, 100, 1, 0));
	assert(!infection_rules_join(&rules, 0, 0));
	assert(!infection_rules_initialize(&rules, &config, 0));
	config.round_seconds = UINT32_MAX;
	assert(!infection_rules_initialize(&rules, &config, 42));
	assert(rules.phase == _infection_active); /* invalid initialization is atomic */
}

static void deadline_disconnect_and_scoring_edges(void)
{
	struct infection_rules rules;
	struct infection_rules_config config;
	uint32_t survivor, alpha, tick, top[1];
	start(&rules, 2);
	survivor = role_slot(&rules, _infection_survivor);
	for (tick = 0; tick < 3; tick++) infection_rules_tick(&rules);
	assert(infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, 1, 0));
	infection_rules_tick(&rules);
	assert(rules.winner == _infection_infected_win); /* final death beats timeout */
	test_config(&config);
	config.countdown_seconds = 2;
	assert(infection_rules_initialize(&rules, &config, 42));
	assert(infection_rules_join(&rules, 0, 100));
	assert(infection_rules_join(&rules, 1, 101));
	for (tick = 0; tick < 3; tick++) infection_rules_tick(&rules);
	assert(rules.phase == _infection_countdown);
	alpha = role_slot(&rules, _infection_alpha);
	assert(infection_rules_leave(&rules, alpha, 100 + alpha));
	assert(!infection_rules_join(&rules, alpha, 100 + alpha)); /* retired handle */
	infection_rules_tick(&rules);
	assert(rules.phase == _infection_waiting);
	assert(rules.completed_rounds == 0);
	assert(infection_rules_join(&rules, alpha, 200));
	for (tick = 0; tick < 3; tick++) infection_rules_tick(&rules);
	assert(rules.phase == _infection_countdown);
	assert(rules.round_id == 2); /* abandoned round id is never reused */
	test_config(&config);
	config.last_spartan_enabled = 0;
	config.infection_score = INT32_MAX;
	config.win_score = INT32_MAX;
	start_config(&rules, 2, &config, 42);
	survivor = role_slot(&rules, _infection_survivor);
	alpha = role_slot(&rules, _infection_alpha);
	assert(infection_rules_death(&rules, rules.round_id, survivor, 100 + survivor, 1, 100 + alpha));
	infection_rules_tick(&rules);
	assert(rules.participants[alpha].score == INT32_MAX); /* saturates, no overflow */
	assert(infection_rules_rankings(&rules, top, 1) == 1);
	assert(top[0] == 0); /* equal scores, ascending slot tie break */
	assert(infection_rules_rankings(&rules, NULL, 1) == 0);
	assert(!rules.last_spartan_handle);
}

int main(void)
{
	two_player_round();
	conversion_and_respawn();
	victory_and_match();
	joins_and_disconnects();
	environment_suicide_and_life_tokens();
	simultaneous_deaths();
	alpha_selection_and_bounds();
	deadline_disconnect_and_scoring_edges();
	puts("Infection rules: all scenarios passed (including 480 seeded selection trials)");
	return 0;
}
