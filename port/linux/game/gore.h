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

/* object_damage_body (damage.c), when a hit leaves a biped's body below
zero (it dies, or was a corpse): whether it gibs. normalized_damage is the
hit's body damage over the unit's maximum vitality. game.gore_log logs it. */
boolean gore_unit_hit(long unit_index, struct damage_data const *damage, real normalized_damage);
/* then, when it said so (after the unit's death, if this hit killed it) */
void gore_unit_gib_later(long unit_index, struct damage_data const *damage, real normalized_damage);
/* game_tick (game.c), after objects_update */
void gore_update(void);
/* game_initialize_for_new_map / game_dispose_from_old_map */
void gore_reset(void);

#endif
