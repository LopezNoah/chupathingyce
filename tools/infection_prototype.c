/* Controlled local participants, not Spartans or network clients.
 * Compile with source/game/infection_rules.c; see docs/infection.md.
 */
#include "../source/game/infection_rules.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(char const *text, uint32_t *value)
{
	char *end;
	unsigned long parsed;
	if (!text || text[0] < '0' || text[0] > '9') return 0;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *end || parsed > UINT32_MAX) return 0;
	*value = (uint32_t)parsed;
	return 1;
}

static void show(struct infection_rules const *rules)
{
	uint32_t slot, rankings[INFECTION_MAXIMUM_PARTICIPANTS];
	uint32_t count = infection_rules_rankings(rules, rankings, INFECTION_MAXIMUM_PARTICIPANTS);
	printf("Infection: %s | round %u/%u (id %u) | phase time %u ticks | Survivors %u\n",
		infection_rules_phase_name(rules->phase),
		rules->completed_rounds + (rules->phase < _infection_round_results ? 1U : 0U),
		rules->config.rounds, rules->round_id, rules->phase_ticks, infection_rules_survivors(rules));
	if (rules->last_spartan_handle) printf("Last Spartan Standing: handle %u\n", rules->last_spartan_handle);
	if (rules->phase >= _infection_round_results)
		printf("Round winner: %s\n", rules->winner == _infection_survivors_win ? "Survivors" :
			rules->winner == _infection_infected_win ? "Infected" : "None (empty round)");
	for (slot = 0; slot < count; slot++)
	{
		uint32_t index = rankings[slot];
		struct infection_participant const *participant = &rules->participants[index];
		printf("  slot %u handle %u: %s | %s | score %d | life %u | respawn %u ticks\n",
			index, participant->handle, infection_rules_role_name(participant->role),
			participant->alive ? "alive" : "not spawned", participant->score,
			participant->life_id, participant->respawn_ticks);
	}
}

static void step(struct infection_rules *rules)
{
	uint32_t slot;
	infection_rules_tick(rules);
	/* This is the harness's spawn adapter: no Halo units/loadouts are made. */
	for (slot = 0; slot < INFECTION_MAXIMUM_PARTICIPANTS; slot++)
	{
		uint32_t handle = rules->participants[slot].handle;
		if (infection_rules_can_spawn(rules, slot, handle))
			if (!infection_rules_spawn(rules, slot, handle)) abort();
	}
}

static int configure(int argc, char **argv, struct infection_rules_config *config,
	uint32_t *players, uint32_t *seed)
{
	int index;
	for (index = 1; index < argc; index++)
	{
		char *equal = strchr(argv[index], '=');
		uint32_t value;
		if (!equal || !number(equal + 1, &value)) return 0;
		*equal = '\0';
		if (!strcmp(argv[index], "players")) *players = value;
		else if (!strcmp(argv[index], "seed")) *seed = value;
		else if (!strcmp(argv[index], "seconds")) config->round_seconds = value;
		else if (!strcmp(argv[index], "rounds")) config->rounds = value;
		else if (!strcmp(argv[index], "alphas")) config->alpha_count = value;
		else if (!strcmp(argv[index], "minimum")) config->minimum_participants = value;
		else if (!strcmp(argv[index], "respawn")) config->infected_respawn_seconds = value;
		else if (!strcmp(argv[index], "countdown")) config->countdown_seconds = value;
		else if (!strcmp(argv[index], "results")) config->results_seconds = value;
		else return 0;
	}
	return *players <= INFECTION_MAXIMUM_PARTICIPANTS && *seed != 0 && infection_rules_config_valid(config);
}

/* Own cursor, no global tokenizer state. Only called on the bounded input
 * buffer, which is intentionally consumed by this command. */
static char *token(char **cursor)
{
	char *begin, *end;
	begin = *cursor + strspn(*cursor, " \t\r\n");
	if (!*begin) { *cursor = begin; return NULL; }
	end = begin + strcspn(begin, " \t\r\n");
	*cursor = end;
	if (*end) { *end = '\0'; *cursor = end + 1; }
	return begin;
}

static int command(struct infection_rules *rules, char *line)
{
	char *cursor = line;
	char *name = token(&cursor);
	char *first = token(&cursor);
	char *second = token(&cursor);
	char *extra = token(&cursor);
	uint32_t slot, argument;
	if (!name) return 1;
	if (!strcmp(name, "quit")) return 0;
	if (!strcmp(name, "show") && !first) { show(rules); return 1; }
	if (!strcmp(name, "tick") && !second && number(first, &argument) && argument <= 108000)
	{
		uint32_t tick;
		for (tick = 0; tick < argument; tick++) step(rules);
		show(rules);
		return 1;
	}
	if (!number(first, &slot) || slot >= INFECTION_MAXIMUM_PARTICIPANTS || extra) goto invalid;
	if (!strcmp(name, "join") && !second && rules->last_admission_handle != UINT32_MAX)
	{
		if (!infection_rules_join(rules, slot, rules->last_admission_handle + 1)) goto invalid;
	}
	else if (!strcmp(name, "leave") && !second)
	{
		if (!infection_rules_leave(rules, slot, rules->participants[slot].handle)) goto invalid;
	}
	else if (!strcmp(name, "death"))
	{
		uint32_t killer = 0;
		if (second && strcmp(second, "environment"))
		{
			if (!number(second, &argument) || argument >= INFECTION_MAXIMUM_PARTICIPANTS ||
				!rules->participants[argument].connected) goto invalid;
			killer = rules->participants[argument].handle;
		}
		if (!infection_rules_death(rules, rules->round_id, slot, rules->participants[slot].handle,
			rules->participants[slot].life_id, killer)) goto invalid;
		puts("Death queued. Use tick 1 to commit this tick's batch.");
		return 1;
	}
	else goto invalid;
	show(rules);
	return 1;
invalid:
	puts("Rejected. Commands: show | tick N | death SLOT [KILLER_SLOT|environment] | join SLOT | leave SLOT | quit");
	return 1;
}

int main(int argc, char **argv)
{
	struct infection_rules rules;
	struct infection_rules_config config;
	uint32_t players = 2, seed = 42, slot;
	char line[256];
	infection_rules_config_default(&config);
	if (!configure(argc, argv, &config, &players, &seed) || !infection_rules_initialize(&rules, &config, seed))
	{
		fputs("Usage: infection-prototype [players=N seconds=N rounds=N alphas=N minimum=N respawn=N countdown=N results=N seed=N]\n", stderr);
		return 1;
	}
	for (slot = 0; slot < players; slot++)
		if (!infection_rules_join(&rules, slot, slot + 1)) return 1;
	puts("Host-only Infection rules prototype. No Halo objects, weapon simulation or network.");
	puts("Commands: show | tick N | death SLOT [KILLER_SLOT|environment] | join SLOT | leave SLOT | quit");
	show(&rules);
	while (fgets(line, sizeof(line), stdin))
	{
		if (!strchr(line, '\n') && !feof(stdin))
		{
			int character;
			do { character = getchar(); } while (character != '\n' && character != EOF);
			puts("Rejected: command too long.");
			continue;
		}
		if (!command(&rules, line)) break;
	}
	if (ferror(stdin)) return 1;
	return 0;
}
