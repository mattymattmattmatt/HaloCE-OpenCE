/*
HUD_TOGGLE.H
The HUD/reticle visibility cycle (port/linux/game/hud_toggle.c): the
gamepad Back (Select) button steps through HUD and reticle shown, HUD
hidden with the reticle shown, and both hidden.
*/
#ifndef __HUD_TOGGLE_H
#define __HUD_TOGGLE_H

/* game_tick (game.c), after gore_update: watch Back on the first local
player's gamepad (the keyboard's scoreboard key, Tab by default, feeds the
same Back button in the game) */
void hud_toggle_update(void);
/* game_initialize_for_new_map (game.c): back to display.hud_toggle_start */
void hud_toggle_reset(void);
/* whether the HUD proper (unit and weapon panels, grenades, nav points,
damage direction) is drawn; true when display.hud_toggle is false */
boolean hud_toggle_hud_visible(void);
/* whether the reticle is drawn for this local player: in the third state
only while zoomed (a zoomed weapon's reticle and scope are never hidden) */
boolean hud_toggle_reticle_visible(short local_player_index);

#endif
