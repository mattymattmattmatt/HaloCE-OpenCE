/* Campaign allies: AI marines who follow the players (coop_allies.c). */
#ifndef __COOP_ALLIES_H
#define __COOP_ALLIES_H

/* ai_update, each tick the AI runs, before the actors' update */
void coop_allies_update(void);
/* actor_update, after the actor's action chose where to go and before it moves */
void coop_allies_leash(long actor_index);

#endif
