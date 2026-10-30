/*
COOP_ALLIES.C

CAMPAIGN ALLIES (bots.coop, 0 to 4): AI marines who fight beside the players
in the campaign: solo, split screen, and a network co-op game's host (the AI
runs nowhere else; its clients see the marines as they see any actor).

They are the campaign's own AI (not bots.c's computer players, who are
players: the levels' scripts count and test (players), and so would
trigger the level's trigger volumes, cutscenes and co-op scaling). An ally
is a free (encounterless) actor of the level's own marine variant, from the
scenario's actor palette (else its crewmen; a level with neither, Keyes, gets
none), placed by actor_place behind a player as a co-op player respawns
(players_coop_spawn_location) and fixed into room with a line of sight to
them (biped_fix_position, as player_teleport does). They are told apart from
the level's actors by their noncombat sequence id (only an encounter's move
positions read it; a free actor has none), which the game state saves, so
they survive checkpoints and reverts with nothing of their own to save.

THE LEASH (coop_allies_leash): the nearest living player on foot. An ally
whose action is fighting (fight, uncover, avoid, flee) stays within
ALLY_LEASH_COMBAT of them and walks back to within ALLY_LEASH_RETURN when
past it; any other time (alert, search, charge, guard...) it follows to
within ALLY_LEASH_FOLLOW, so it neither pushes ahead into an encounter nor
chases an enemy far. The walk overrides the action's destination for that
tick (actor_move_to_point, a raw location on the player's pathfinding
surface) after the action chose its own and before the actor moves; its
aiming and shooting are the action's. An ally left farther than
ALLY_TELEPORT_DISTANCE, or that walked back ALLY_STUCK_TICKS without getting
a metre nearer, is brought back behind the player as a co-op respawn is.

Nothing happens during a cutscene, while the players' input is off (a
scripted sequence), while the scripts have the AI off, or while the player
is in a vehicle (the allies regroup when they are on foot again). A killed
ally comes back ALLY_RESPAWN_TICKS later, behind a player. Fewer wanted
(the menu) takes away the extras; 0 takes them all.
*/
#include "cseries.h"
#include "ai/actor_definitions.h"
#include "ai/actor_placement.h"
#include "ai/actor_types.h"
#include "ai/actors.h"
#include "ai/ai_scenario_definitions.h"
#include "cache/cache_files.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/biped_definitions.h"
#include "units/bipeds.h"
#include "units/units.h"
#include "coop_allies.h"

#include <string.h>

long config_integer(char const *name);
char const *config_string(char const *name);
void platform_log(char const *format, ...);

enum
{
	ALLY_MAXIMUM = 4,
	/* the noncombat sequence id our allies have (an actor's is a signed char) */
	ALLY_SEQUENCE_ID = 113,
	/* actors.c's actor_default_state_alert (its enum is file-local) */
	ALLY_DEFAULT_STATE_ALERT = 2,
	ALLY_LEVEL_START_TICKS = 3 * TICKS_PER_SECOND,
	ALLY_RESPAWN_TICKS = 10 * TICKS_PER_SECOND,
	ALLY_RETRY_TICKS = 2 * TICKS_PER_SECOND,
	ALLY_STUCK_TICKS = 6 * TICKS_PER_SECOND,
};
#define ALLY_LEASH_FOLLOW 5.f
#define ALLY_LEASH_COMBAT 10.f
#define ALLY_LEASH_RETURN 4.f
#define ALLY_TELEPORT_DISTANCE 25.f
#define ALLY_SPAWN_BEHIND 1.5f
#define ALLY_RETARGET_DISTANCE 1.5f
/* (world units are 10 feet: metres to world units) */
#define ALLY_METRES(m) ((m) / 3.048f)

struct coop_ally
{
	long actor_index;
	boolean returning;
	real best_distance;
	long best_time;
	long teleport_time;
	long surface_index;
	real_point3d destination;
};

static struct
{
	long last_tick;
	long next_spawn_time;
	long next_log_time;
	short last_count;
	short last_wanted;
	boolean logged_variant;
	struct coop_ally allies[ALLY_MAXIMUM * 2];
} coop_allies;

static short allies_wanted(void)
{
	return (short)PIN(config_integer("bots.coop"), 0, ALLY_MAXIMUM);
}

static boolean allies_running(void)
{
	return !game_engine_running() && !network_game_distributed_client();
}

static boolean ally_actor(struct actor_datum const *actor)
{
	return actor->meta.encounter_index == NONE && !actor->meta.swarm &&
		actor->state.noncombat_sequence_id == ALLY_SEQUENCE_ID && actor->meta.unit_index != NONE;
}

/* the living unit of the player nearest the point, on foot or not (NONE if
nobody lives) */
static long allies_nearest_player(real_point3d const *point, real *distance)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long best = NONE;
	real best_distance = REAL_MAX;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit = unit_try_and_get(player->unit_index);
		struct object_datum *root;
		real d;

		if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		root = object_get(object_get_ultimate_parent(player->unit_index));
		d = point ? distance3d(point, &root->object.position) : 0.f;
		if (d < best_distance)
		{
			best_distance = d;
			best = player->unit_index;
		}
	}
	if (distance)
		*distance = best_distance;
	return best;
}

/* the player's unit if they are a biped on foot, else NONE */
static long allies_on_foot(long unit_index)
{
	return unit_index != NONE && object_get_ultimate_parent(unit_index) == unit_index &&
		object_try_and_get_and_verify_type(unit_index, _object_mask_biped) ? unit_index : NONE;
}

static boolean allies_quiet(void)
{
	return cinematic_in_progress() || !player_input_enabled();
}

/* the level's marines' variant, armed, on foot; else its crewmen's */
static long allies_variant(void)
{
	struct scenario *scenario = global_scenario_get();
	short pass, index;

	for (pass = 0; pass < 2; pass++)
	{
		for (index = 0; index < scenario->ai_actor_palette.count; index++)
		{
			struct tag_reference *entry = TAG_BLOCK_GET_ELEMENT(&scenario->ai_actor_palette, index, struct tag_reference);
			struct actor_variant_definition *variant;

			if (entry->index == NONE || tag_get_group_tag(entry->index) != ACTOR_VARIANT_DEFINITION_TAG)
				continue;
			variant = actor_variant_definition_get(entry->index);
			if (variant->actor_reference.index == NONE || variant->unit_reference.index == NONE ||
				variant->ranged_combat.reference.index == NONE ||
				tag_get_group_tag(variant->unit_reference.index) != BIPED_DEFINITION_TAG)
				continue;
			if (actor_definition_get(variant->actor_reference.index)->type == (pass ? _actor_crew : _actor_marine))
				return entry->index;
		}
	}
	return NONE;
}

/* behind the player, as a co-op respawn (players_coop_spawn_location) */
static void allies_behind(long player_unit_index, real_point3d *position, real *facing)
{
	struct object_datum *beside = object_get(player_unit_index);
	real behind = beside->object.bounding_sphere_radius + ALLY_METRES(ALLY_SPAWN_BEHIND);

	point_from_line3d(&beside->object.position, &beside->object.forward, -behind, position);
	if (player_position_switches_bsp(position))
		point_from_line3d(&beside->object.position, &beside->object.forward, behind, position);
	if (scenario_leaf_index_from_point(position) == NONE || player_position_switches_bsp(position))
		*position = beside->object.position;
	if (facing)
		*facing = arctangent(beside->object.forward.j, beside->object.forward.i);
}

/* moves the ally's biped beside the player, with room and a line of sight */
static boolean allies_bring_back(long unit_index, long player_unit_index)
{
	real_point3d position;

	allies_behind(player_unit_index, &position, NULL);
	return biped_fix_position(unit_index, player_unit_index, &position, NULL, 2.f, FALSE, FALSE, TRUE);
}

static struct coop_ally *allies_slot(long actor_index)
{
	short index, free_index = NONE;

	for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
	{
		struct coop_ally *ally = &coop_allies.allies[index];

		if (ally->actor_index == actor_index)
			return ally;
		if (free_index == NONE && (ally->actor_index == NONE || !datum_try_and_get(actor_data, ally->actor_index)))
			free_index = index;
	}
	if (free_index == NONE)
		return NULL;
	csmemset(&coop_allies.allies[free_index], 0, sizeof(coop_allies.allies[free_index]));
	coop_allies.allies[free_index].actor_index = actor_index;
	coop_allies.allies[free_index].surface_index = NONE;
	return &coop_allies.allies[free_index];
}

static void allies_reset(void)
{
	short index;

	csmemset(&coop_allies, 0, sizeof(coop_allies));
	for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
		coop_allies.allies[index].actor_index = NONE;
	coop_allies.next_spawn_time = ALLY_LEVEL_START_TICKS;
}

/* once a while, what holds the allies back (halo.log) */
static void allies_note(char const *why)
{
	long now = game_time_get();

	if (now < coop_allies.next_log_time)
		return;
	coop_allies.next_log_time = now + 10 * TICKS_PER_SECOND;
	platform_log("campaign allies: %s", why);
}

static void allies_spawn(long player_unit_index)
{
	long variant = allies_variant();
	struct actor_starting_location location;
	long actor_index;

	if (!coop_allies.logged_variant)
	{
		coop_allies.logged_variant = TRUE;
		platform_log("campaign allies: %s", variant == NONE ? "this level has no armed marine or crewman to be them" :
			tag_get_name(variant));
	}
	if (variant == NONE)
		return;
	csmemset(&location, 0, sizeof(location));
	allies_behind(player_unit_index, &location.position, &location.facing);
	location.cluster_index = NONE;
	location.noncombat_sequence_id = ALLY_SEQUENCE_ID;
	location.initial_state = ALLY_DEFAULT_STATE_ALERT;
	location.default_state = ALLY_DEFAULT_STATE_ALERT;
	location.actor_variant_index = NONE;
	location.command_list_index = NONE;
	actor_index = actor_place(variant, NONE, NONE, &location, FALSE, 0);
	if (actor_index == NONE)
	{
		allies_note("the game made no actor (the pool full?)");
		return;
	}
	{
		struct actor_datum *actor = actor_get(actor_index);

		if (!ally_actor(actor))
		{
			allies_note("the actor made is not one of ours");
			actor_erase(actor_index, TRUE);
			return;
		}
		/* (the place behind the player, else where it stands if it has room) */
		if (!allies_bring_back(actor->meta.unit_index, player_unit_index) &&
			!biped_fix_position(actor->meta.unit_index, NONE, &location.position, NULL, 2.f, FALSE, FALSE, TRUE))
		{
			allies_note("no room behind the player");
			actor_erase(actor_index, TRUE);
			return;
		}
		actor->meta.force_active = TRUE;
		platform_log("campaign allies: one joined behind the player (%d wanted)", allies_wanted());
	}
}

void coop_allies_update(void)
{
	struct data_iterator iterator;
	struct actor_datum *actor;
	long now = game_time_get();
	short wanted, count = 0;
	long player_unit_index;

	/* a new level or a revert: forget the walks (the allies are in the game state) */
	if (now < coop_allies.last_tick || coop_allies.last_tick == 0)
		allies_reset();
	coop_allies.last_tick = now;
	if (!allies_running())
		return;

	wanted = allies_wanted();
	/* (more wanted, as the pause menu's ALLIES asks: at once) */
	if (wanted > coop_allies.last_wanted)
		coop_allies.next_spawn_time = MIN(coop_allies.next_spawn_time, MAX(now, ALLY_LEVEL_START_TICKS));
	coop_allies.last_wanted = wanted;
	data_iterator_new(&iterator, actor_data);
	while ((actor = data_iterator_next(&iterator)) != NULL)
	{
		if (!ally_actor(actor) || actor->meta.erase_next_tick)
			continue;
		if (++count > wanted)
		{
			actor_erase(iterator.datum_index, FALSE);
			count--;
		}
	}
	if (count < coop_allies.last_count)
		coop_allies.next_spawn_time = MAX(coop_allies.next_spawn_time, now + ALLY_RESPAWN_TICKS);
	coop_allies.last_count = count;
	if (count >= wanted || now < coop_allies.next_spawn_time)
		return;
	if (allies_quiet())
	{
		allies_note(cinematic_in_progress() ? "waiting for the cutscene to end" : "waiting for the players' controls");
		return;
	}

	/* one at a time, behind the first player on foot */
	player_unit_index = allies_on_foot(allies_nearest_player(NULL, NULL));
	if (player_unit_index == NONE)
	{
		allies_note("waiting for a player on foot");
		return;
	}
	allies_spawn(player_unit_index);
	coop_allies.next_spawn_time = now + ALLY_RETRY_TICKS / 4;
	coop_allies.last_count = count;
}

void coop_allies_leash(long actor_index)
{
	struct actor_datum *actor = actor_get(actor_index);
	struct coop_ally *ally;
	real distance, leash;
	long player_unit_index;
	long now;
	boolean fighting;

	if (!ally_actor(actor) || !allies_running() || allies_quiet())
		return;
	if (object_get_ultimate_parent(actor->meta.unit_index) != actor->meta.unit_index)
		return;
	player_unit_index = allies_nearest_player(&actor->input.position.body_position, &distance);
	if (allies_on_foot(player_unit_index) == NONE)
		return;
	ally = allies_slot(actor_index);
	if (!ally)
		return;
	now = game_time_get();

	/* (a place beside them tried at most once a second) */
	if (now >= ally->teleport_time && (distance > ALLY_METRES(ALLY_TELEPORT_DISTANCE) ||
		(ally->returning && now - ally->best_time > ALLY_STUCK_TICKS)))
	{
		ally->teleport_time = now + TICKS_PER_SECOND;
		if (allies_bring_back(actor->meta.unit_index, player_unit_index))
		{
			ally->returning = FALSE;
			ally->surface_index = NONE;
			actor_move_halt(actor_index);
			return;
		}
		ally->best_time = now;
	}

	fighting = actor->state.action == _actor_action_fight || actor->state.action == _actor_action_uncover ||
		actor->state.action == _actor_action_avoid || actor->state.action == _actor_action_flee;
	leash = fighting ? ALLY_LEASH_COMBAT : ALLY_LEASH_FOLLOW;
	if (!ally->returning && distance > ALLY_METRES(leash))
	{
		ally->returning = TRUE;
		ally->best_distance = distance;
		ally->best_time = now;
		ally->surface_index = NONE;
	}
	if (!ally->returning)
		return;
	if (distance < ALLY_METRES(ALLY_LEASH_RETURN))
	{
		ally->returning = FALSE;
		return;
	}
	if (distance < ally->best_distance - ALLY_METRES(1.f))
	{
		ally->best_distance = distance;
		ally->best_time = now;
	}

	/* toward where the player stands, a new path only when they moved on */
	if (ally->surface_index == NONE ||
		distance3d(&ally->destination, &object_get(player_unit_index)->object.position) > ALLY_METRES(ALLY_RETARGET_DISTANCE))
	{
		real_point3d point;
		long surface_index = biped_find_pathfinding_surface_index(player_unit_index, &point);

		if (surface_index == NONE)
			return;
		ally->surface_index = surface_index;
		ally->destination = point;
	}
	actor_move_to_point(actor_index, &ally->destination, ally->surface_index, NONE);
}

/* the players' unit in the campaign (game.coop_character): the level's
marines' biped for "marine", else NONE (the Chief, the globals'). Not on The
Pillar of Autumn, whose scripts animate the Chief's own biped (the cryo bay,
Keyes' bridge) */
long coop_character_unit(void)
{
	char const *map;
	long variant;

	if (game_engine_running() || strcmp(config_string("game.coop_character"), "marine"))
		return NONE;
	map = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	if (map && strstr(map, "a10"))
		return NONE;
	variant = allies_variant();
	return variant == NONE ? NONE : actor_variant_definition_get(variant)->unit_reference.index;
}
