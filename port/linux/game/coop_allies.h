/* Campaign allies: AI marines who follow the players (coop_allies.c). */
#ifndef __COOP_ALLIES_H
#define __COOP_ALLIES_H

/* ai_update, each tick the AI runs, before the actors' update */
void coop_allies_update(void);
/* actor_update, after the actor's action chose where to go and before it moves */
void coop_allies_leash(long actor_index);

/* players.c, each tick of a player with a unit: whether the action button
is held, and whether nothing (a weapon, a vehicle) is near enough to take
it; held ALLY_ORDER_TICKS on foot, the allies hold the aimed point, or
follow again */
void coop_allies_order_input(long player_index, boolean pressed, boolean held, boolean nothing_near);
/* whether a campaign ally lives (a dead player respawns beside one) */
boolean coop_allies_alive(void);
/* a living ally on foot passing the test (NULL: any), else NONE */
long coop_allies_respawn_unit(boolean (*test)(long unit_index));
/* hud_unit.c: the allies' units and the HUDs that show their health (the
level's vehicles' passengers' HUDs, moved apart): how many */
struct unit_hud_interface_definition;
short coop_allies_hud_units(short local_player_index, boolean multiplayer, long *unit_indices, long *hud_indices,
	struct unit_hud_interface_definition **definitions, short maximum);

/* menu_functions.c: ALLIES chosen in a menu (kept for the next level's
start, which otherwise sets them back to none) */
void coop_allies_arm(void);

/* director.c, a dead player's camera (not network co-op's): the ally it
watches, the left and right of the D-pad (Y: right) going round the living
allies and the player's own body; NONE for the body */
long coop_allies_spectate_unit(short local_player_index);
/* whether the watched ally changed since the last call */
boolean coop_allies_spectate_changed(short local_player_index);
/* hud.c: the line telling a dead player whom it watches */
void coop_allies_spectate_draw(short local_player_index);

#endif
