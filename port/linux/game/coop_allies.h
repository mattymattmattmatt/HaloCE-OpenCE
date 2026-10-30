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

#endif
