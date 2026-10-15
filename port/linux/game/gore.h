/*
GORE.H
The gore prototype (port/linux/game/gore.c): a unit killed by a strong
enough explosion or vehicle hit is replaced by flying pieces of its own model
(each copy shows only some of its regions) or the map's stock Flood bits,
with blood. Single player only.
*/
#ifndef __GORE_H
#define __GORE_H

struct damage_data;

/* object_damage_body (damage.c), when a unit's body is depleted: decides
whether it gibs. normalized_damage is the hit's body damage over the unit's
maximum vitality. */
void gore_unit_body_depleted(long unit_index, struct damage_data const *damage, real normalized_damage);
/* game_tick (game.c), after objects_update */
void gore_update(void);
/* game_initialize_for_new_map / game_dispose_from_old_map */
void gore_reset(void);

#endif
