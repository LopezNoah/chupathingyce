/*
BOTS.H

port: computer-controlled multiplayer players (bots.c).
*/

#ifndef HALO_GAME_BOTS_H
#define HALO_GAME_BOTS_H
#pragma once

/* ---------- prototypes/BOTS.C */

/* a new map: no bots, and the map's navigation graph to build again */
void bots_initialize_for_new_map(
	void);

/* each game tick, before the players' actions are taken (players.c): the
bots join or leave, then each decides the action the host sends for it in its
next update, as a local player's input is sent */
void bots_update(
	void);

/* whether a bot has played in this map's game: its results are not reported
to the game list or Delta Stats */
boolean bots_game_had_bots(
	void);

/* whether the player is one of this machine's bots */
boolean bots_player_is_bot(
	long player_index);

#endif /* HALO_GAME_BOTS_H */
