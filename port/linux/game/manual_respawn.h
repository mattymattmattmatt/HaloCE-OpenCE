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

/* the look input of a dead local player while it waits (player_control.c
hands it over: the stick, the mouse, the touch controls' swipe and the
gyroscope, as they would turn a living one) */
void manual_respawn_look_add(
	short local_player_index,
	real yaw,
	real pitch);

/* the death camera (dead_camera.c) of that player, while it waits: turned
round the body by that look, its pitch kept above it. FALSE (untouched)
when it does not wait: the stock camera, as it was */
boolean manual_respawn_look_apply(
	long player_index,
	real_euler_angles2d *facing);

/* debug.manual_respawn_test, each tick (game.c) */
void manual_respawn_test_update(
	void);

#endif
