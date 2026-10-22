/*
MANUAL_RESPAWN.H

Respawning when the player chooses to, in the campaign (game.manual_respawn,
manual_respawn.c). main.c asks before it reverts or respawns; hud.c draws the
prompt; touch_input.c turns a tap into the button.
*/

#ifndef __MANUAL_RESPAWN_H
#define __MANUAL_RESPAWN_H

/* TRUE while the stock death delay has run out and the game waits for a
dead local player to press the button: main.c then holds the revert (or the
co-op respawn) back. FALSE, always, when the setting is off, in multiplayer
and in network co-op: those keep their stock timing. */
boolean manual_respawn_hold(
	void);

/* whether game.manual_respawn is on and this game is one it applies to
(the campaign on this machine): main.c then drops checkpoints while all the
players are dead, which would revert to them dead */
boolean manual_respawn_applies_now(
	void);

/* the small prompt, for a dead local player, while main.c waits */
void manual_respawn_draw(
	short local_player_index);

/* whether a tap on the touchscreen should press A now (touch_game.c) */
boolean manual_respawn_waiting(
	void);

/* debug.manual_respawn_test, each tick (game.c) */
void manual_respawn_test_update(
	void);

#endif
