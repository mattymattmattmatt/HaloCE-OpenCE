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
#include "ai/ai_communication.h"
#include "interface/hud_messaging.h"
#include "interface/unit_hud_interface_definition.h"
#include "physics/collisions.h"
#include "sound/sound_definitions.h"
#include "units/dialogue_definitions.h"
#include "units/unit_definitions.h"
#include "network_distributed.h"
#include "coop_allies.h"

#include <stdio.h>
#include <stdlib.h>
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
/* orders (coop_allies_order_input): the action button held this long, on
foot with nothing to pick up or enter; the aimed point at most this far;
held there, an ally walks to within ALLY_HOLD_ARRIVE of it, fights within
ALLY_HOLD_RANGE of it and stops walking back within ALLY_HOLD_STOP */
#define ALLY_ORDER_TICKS (3 * TICKS_PER_SECOND / 2)
#define ALLY_ORDER_REACH 80.f
#define ALLY_HOLD_ARRIVE 3.f
#define ALLY_HOLD_RANGE 20.f
#define ALLY_HOLD_STOP 2.f
/* the players whose held button is counted (by absolute index) */
#define ALLY_ORDER_PLAYERS 16
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
	long next_report_time;
	short last_count;
	short last_wanted;
	boolean logged_variant;
	boolean logged_voices;
	/* (the order: hold there, else follow) */
	boolean holding;
	long hold_surface;
	real_point3d hold_point;
	short order_ticks[ALLY_ORDER_PLAYERS];
	boolean order_spent[ALLY_ORDER_PLAYERS];
	/* (HALO_ALLIES_TEST: the button held until this tick, the next step) */
	long test_hold_until;
	short test_step;
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

/* tells the players (a local one on the HUD, a client's by a notice) */
static void allies_tell(char const *text)
{
	struct data_iterator iterator;
	struct player_datum *player;
	wchar_t wide[128];
	long index;

	for (index = 0; text[index] && index < (long)NUMBEROF(wide) - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		if (player->local_player_index != NONE)
			hud_print_message(player->local_player_index, wide);
		else if (global_network_game_server_get())
		{
			long machine_index = distributed_player_machine((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index));

			if (machine_index != NONE)
				distributed_send_notice_to_machine(machine_index, text);
		}
	}
}

/* the first of the vocalizations (dialogue_definitions.c's names) the
unit's dialogue has a sound for, said now; FALSE if none */
static boolean allies_say(long unit_index, char const *const *names, short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		short vocalization_type = dialogue_get_vocalization_type_by_name(names[index]);
		long sound_definition_index = NONE;
		short play_type;

		if (vocalization_type == NONE)
			continue;
		play_type = unit_test_speech(unit_index, _unit_speech_scripted, FALSE, TRUE, NULL, &vocalization_type,
			&sound_definition_index);
		if (play_type > 0 && sound_definition_index != NONE)
		{
			struct unit_speech_item speech_item;

			csmemset(&speech_item, 0, sizeof(speech_item));
			speech_item.vocalization_type = vocalization_type;
			speech_item.sound_definition_index = sound_definition_index;
			speech_item.priority = _unit_speech_scripted;
			ai_communication_packet_new(&speech_item.ai);
			unit_speak(unit_index, play_type, &speech_item);
			platform_log("campaign allies: said \"%s\" (%s)", names[index], tag_get_name(sound_definition_index));
			return TRUE;
		}
	}
	return FALSE;
}

/* once a level, the sounds of the vocalizations an order could use
(halo.log): their tags and the permutations' names */
static void allies_log_voices(long unit_index)
{
	static char const *const names[] =
	{
		"advance", "advance re", "retreat", "retreat re", "cover", "sighted friend player",
	};
	struct unit_datum *unit = unit_get(unit_index);
	short index;

	for (index = 0; index < (short)NUMBEROF(names); index++)
	{
		short vocalization_type = dialogue_get_vocalization_type_by_name(names[index]);
		long sound_definition_index = NONE;
		char permutations[256] = "";

		if (vocalization_type == NONE || unit->unit.dialogue_index == NONE)
			continue;
		unit_test_speech(unit_index, _unit_speech_none, FALSE, FALSE, NULL, &vocalization_type, &sound_definition_index);
		if (sound_definition_index != NONE)
		{
			struct sound_definition *sound = sound_definition_get(sound_definition_index);

			if (sound->pitch_ranges.count > 0)
			{
				struct sound_pitch_range *range = TAG_BLOCK_GET_ELEMENT(&sound->pitch_ranges, 0, struct sound_pitch_range);
				short permutation;

				for (permutation = 0; permutation < range->permutations.count && permutation < 12; permutation++)
				{
					struct sound_permutation *entry =
						TAG_BLOCK_GET_ELEMENT(&range->permutations, permutation, struct sound_permutation);
					size_t used = strlen(permutations);

					snprintf(permutations + used, sizeof(permutations) - used, "%s%.31s", used ? ", " : "", entry->name);
				}
			}
		}
		platform_log("campaign allies: voice \"%s\": %s [%s]", names[index],
			sound_definition_index == NONE ? "none" : tag_get_name(sound_definition_index), permutations);
	}
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
		boolean behind = allies_bring_back(actor->meta.unit_index, player_unit_index);

		if (!behind &&
			!biped_fix_position(actor->meta.unit_index, NONE, &location.position, NULL, 2.f, FALSE, FALSE, TRUE))
		{
			allies_note("no room behind the player");
			actor_erase(actor_index, TRUE);
			return;
		}
		actor->meta.force_active = TRUE;
		platform_log("campaign allies: one joined %s (%d wanted)",
			behind ? "behind the player" : "where the player stands (no clear room behind)", allies_wanted());
		if (!coop_allies.logged_voices)
		{
			coop_allies.logged_voices = TRUE;
			allies_tell("A Marine ally joined you. Hold the action button to send the allies where you aim");
			allies_log_voices(actor->meta.unit_index);
		}
		else
			allies_tell("A Marine ally joined you");
	}
}

/* HALO_ALLIES_TEST (automated tests, no person playing): "order:<s>" holds
the first player's action button 2 seconds from that second of game time
(an order, as a person's hold), "kill:<s>" kills that player; separated by
commas, in order */
static void allies_test(long now)
{
	char const *script = getenv("HALO_ALLIES_TEST");
	char step[32];
	short index;
	long seconds;
	struct data_iterator iterator;
	struct player_datum *player;

	if (!script || !*script)
		return;
	for (index = 0; index <= coop_allies.test_step; index++)
	{
		size_t length = strcspn(script, ",");

		if (!length)
			return;
		if (index == coop_allies.test_step)
		{
			snprintf(step, sizeof(step), "%.*s", (int)MIN(length, sizeof(step) - 1), script);
			break;
		}
		script += length + (script[length] == ',');
	}
	seconds = atol(strchr(step, ':') ? strchr(step, ':') + 1 : step);
	if (now < seconds * TICKS_PER_SECOND)
		return;
	coop_allies.test_step++;
	if (!strncmp(step, "order", 5))
	{
		coop_allies.test_hold_until = now + 2 * TICKS_PER_SECOND;
		platform_log("campaign allies: test: holding the action button");
		return;
	}
	data_iterator_new(&iterator, player_data);
	if ((player = data_iterator_next(&iterator)) != NULL && player->unit_index != NONE)
	{
		platform_log("campaign allies: test: killing the player");
		unit_kill(player->unit_index);
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
	allies_test(now);

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
	/* (every 10 seconds, how near they keep: halo.log) */
	if (count && now >= coop_allies.next_report_time)
	{
		real nearest = REAL_MAX, farthest = 0.f;

		coop_allies.next_report_time = now + 10 * TICKS_PER_SECOND;
		data_iterator_new(&iterator, actor_data);
		while ((actor = data_iterator_next(&iterator)) != NULL)
		{
			real distance;

			if (!ally_actor(actor) || actor->meta.erase_next_tick)
				continue;
			allies_nearest_player(&object_get(actor->meta.unit_index)->object.position, &distance);
			nearest = MIN(nearest, distance);
			farthest = MAX(farthest, distance);
		}
		if (coop_allies.holding)
		{
			real hold_nearest = REAL_MAX, hold_farthest = 0.f;

			data_iterator_new(&iterator, actor_data);
			while ((actor = data_iterator_next(&iterator)) != NULL)
			{
				real distance;

				if (!ally_actor(actor) || actor->meta.erase_next_tick)
					continue;
				distance = distance3d(&object_get(actor->meta.unit_index)->object.position, &coop_allies.hold_point);
				hold_nearest = MIN(hold_nearest, distance);
				hold_farthest = MAX(hold_farthest, distance);
			}
			platform_log("campaign allies: %d holding, %.1f to %.1f m from the held point (%.1f to %.1f m from the "
				"nearest player)", count, hold_nearest / ALLY_METRES(1.f), hold_farthest / ALLY_METRES(1.f),
				nearest / ALLY_METRES(1.f), farthest / ALLY_METRES(1.f));
		}
		else
			platform_log("campaign allies: %d with the players, %.1f to %.1f m from the nearest", count,
				nearest / ALLY_METRES(1.f), farthest / ALLY_METRES(1.f));
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
	if (!coop_allies.holding && allies_on_foot(player_unit_index) == NONE)
		return;
	ally = allies_slot(actor_index);
	if (!ally)
		return;
	now = game_time_get();
	fighting = actor->state.action == _actor_action_fight || actor->state.action == _actor_action_uncover ||
		actor->state.action == _actor_action_avoid || actor->state.action == _actor_action_flee;

	/* (told to hold a place: there, fighting within ALLY_HOLD_RANGE of it,
	whoever is near; no bringing back beside the players) */
	if (coop_allies.holding)
	{
		real from_point = distance3d(&actor->input.position.body_position, &coop_allies.hold_point);

		if (!ally->returning && from_point > ALLY_METRES(fighting ? ALLY_HOLD_RANGE : ALLY_HOLD_ARRIVE))
			ally->returning = TRUE;
		if (ally->returning && from_point < ALLY_METRES(ALLY_HOLD_STOP))
			ally->returning = FALSE;
		if (ally->returning)
			actor_move_to_point(actor_index, &coop_allies.hold_point, coop_allies.hold_surface, NONE);
		return;
	}

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

/* the living allies' units, on foot or riding */
static short allies_units(long *unit_indices, short maximum)
{
	struct data_iterator iterator;
	struct actor_datum *actor;
	short count = 0;

	if (!actor_data || !allies_running())
		return 0;
	data_iterator_new(&iterator, actor_data);
	while ((actor = data_iterator_next(&iterator)) != NULL && count < maximum)
	{
		struct unit_datum *unit;

		if (!ally_actor(actor) || actor->meta.erase_next_tick)
			continue;
		unit = unit_try_and_get(actor->meta.unit_index);
		if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		unit_indices[count++] = actor->meta.unit_index;
	}
	return count;
}

/* where the player aims: the structure hit within ALLY_ORDER_REACH, else
(a wall, a slope) the ground just in front of it */
static boolean allies_order_target(long unit_index, real_point3d *point, long *surface_index)
{
	unsigned long const flags = FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit);
	struct unit_datum *unit = unit_get(unit_index);
	real_point3d origin, above;
	real_vector3d ray, down;
	struct collision_result hit;

	unit_get_camera_position(unit_index, &origin);
	scale_vector3d(&unit->unit.aiming_vector, ALLY_METRES(ALLY_ORDER_REACH), &ray);
	if (!collision_test_vector(flags, &origin, &ray, unit_index, &hit))
		return FALSE;
	if (hit.plane.n.k < 0.6f)
	{
		point_from_line3d(&hit.point, &unit->unit.aiming_vector, -ALLY_METRES(0.75f), &above);
		down.i = down.j = 0.f;
		down.k = -ALLY_METRES(8.f);
		if (!collision_test_vector(flags, &above, &down, unit_index, &hit) || hit.plane.n.k < 0.6f)
			return FALSE;
	}
	*point = hit.point;
	*surface_index = hit.surface_index;
	return hit.surface_index != NONE;
}

/* the order given: hold the aimed point, or (holding) follow again */
static void allies_order(long unit_index)
{
	static char const *const go[] = { "advance", "cover" };
	static char const *const regroup[] = { "advance re", "retreat", "sighted friend player" };
	long units[ALLY_MAXIMUM * 2];
	short count = allies_units(units, (short)NUMBEROF(units)), index;

	if (coop_allies.holding)
	{
		coop_allies.holding = FALSE;
		for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
			coop_allies.allies[index].returning = FALSE;
		platform_log("campaign allies: ordered to regroup (%d)", count);
		allies_tell("Allies: regrouping on you");
		allies_say(units[0], regroup, (short)NUMBEROF(regroup));
		return;
	}
	if (!allies_order_target(unit_index, &coop_allies.hold_point, &coop_allies.hold_surface))
	{
		platform_log("campaign allies: ordered, but no ground where the player aims");
		allies_tell("Allies: no ground to hold there");
		return;
	}
	coop_allies.holding = TRUE;
	for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
		coop_allies.allies[index].returning = FALSE;
	platform_log("campaign allies: ordered to hold %.1f m away (%d)",
		distance3d(&object_get(unit_index)->object.position, &coop_allies.hold_point) / ALLY_METRES(1.f), count);
	allies_tell("Allies: moving up to hold there (hold the action button again to regroup)");
	allies_say(units[0], go, (short)NUMBEROF(go));
}

void coop_allies_order_input(long player_index, boolean held, boolean nothing_near)
{
	short slot = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	long unit_index, units[1];

	if (slot < 0 || slot >= ALLY_ORDER_PLAYERS)
		return;
	if (game_time_get() < coop_allies.test_hold_until)
		held = TRUE;
	if (!held)
	{
		coop_allies.order_ticks[slot] = 0;
		coop_allies.order_spent[slot] = FALSE;
		return;
	}
	if (coop_allies.order_spent[slot])
		return;
	unit_index = player_get(player_index)->unit_index;
	/* (a hold that began beside a weapon or a vehicle is theirs) */
	if (!nothing_near || allies_on_foot(unit_index) == NONE || !allies_units(units, 1))
	{
		coop_allies.order_spent[slot] = TRUE;
		return;
	}
	if (++coop_allies.order_ticks[slot] < ALLY_ORDER_TICKS)
		return;
	coop_allies.order_spent[slot] = TRUE;
	allies_order(unit_index);
}

boolean coop_allies_alive(void)
{
	long units[1];

	return allies_wanted() > 0 && allies_units(units, 1) > 0;
}

long coop_allies_respawn_unit(boolean (*test)(long unit_index))
{
	long units[ALLY_MAXIMUM * 2];
	short count = allies_units(units, (short)NUMBEROF(units)), index;

	for (index = 0; index < count; index++)
	{
		if (allies_on_foot(units[index]) != NONE && (!test || test(units[index])))
			return units[index];
	}
	return NONE;
}

/* the vehicles' passengers' HUDs of the level (a seat not the driver's with
a health meter), up to ALLY_MAXIMUM different ones */
static short allies_seat_huds(boolean multiplayer, long *hud_indices)
{
	struct scenario *scenario = global_scenario_get();
	short count = 0, entry;

	for (entry = 0; entry < scenario->vehicle_palette.count && count < ALLY_MAXIMUM; entry++)
	{
		struct scenario_object_palette_entry *palette =
			TAG_BLOCK_GET_ELEMENT(&scenario->vehicle_palette, entry, struct scenario_object_palette_entry);
		struct unit_definition *definition;
		short seat;

		if (palette->reference.index == NONE)
			continue;
		definition = unit_definition_get(palette->reference.index);
		for (seat = 0; seat < definition->unit.seats.count && count < ALLY_MAXIMUM; seat++)
		{
			struct unit_seat *seat_definition = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat, struct unit_seat);
			long hud_index;
			short other;

			if (TEST_FLAG(seat_definition->flags, _unit_seat_driver_bit) || seat_definition->seat_huds.count <= 0)
				continue;
			hud_index = unit_definition_get_seat_active_hud_index(definition, seat, multiplayer);
			if (hud_index == NONE ||
				unit_hud_interface_definition_get(hud_index)->health_meter.meter.meter_bitmap.index == NONE)
				continue;
			for (other = 0; other < count && hud_indices[other] != hud_index; other++)
				;
			if (other == count)
				hud_indices[count++] = hud_index;
		}
	}
	return count;
}

static void allies_hud_shift(struct hud_placement_definition *placement, short x, short y)
{
	placement->offset.x = (short)(placement->offset.x + x);
	placement->offset.y = (short)(placement->offset.y + y);
}

short coop_allies_hud_units(boolean multiplayer, long *unit_indices, long *hud_indices,
	struct unit_hud_interface_definition **definitions, short maximum)
{
	static struct unit_hud_interface_definition copies[ALLY_MAXIMUM];
	long seat_huds[ALLY_MAXIMUM];
	short seat_count, count, index, step_x = 0, step_y = 20;

	count = allies_units(unit_indices, (short)MIN(maximum, ALLY_MAXIMUM));
	if (!count || !(seat_count = allies_seat_huds(multiplayer, seat_huds)))
		return 0;
	/* (the seats' HUDs as they are, then again past them, as far apart as
	the first two are) */
	if (seat_count > 1)
	{
		struct meter_hud_element_definition const *first =
			&unit_hud_interface_definition_get(seat_huds[0])->health_meter.meter;
		struct meter_hud_element_definition const *second =
			&unit_hud_interface_definition_get(seat_huds[1])->health_meter.meter;

		if (first->placement.offset.x != second->placement.offset.x ||
			first->placement.offset.y != second->placement.offset.y)
		{
			step_x = (short)(second->placement.offset.x - first->placement.offset.x);
			step_y = (short)(second->placement.offset.y - first->placement.offset.y);
		}
	}
	for (index = 0; index < count; index++)
	{
		short round = (short)(index / seat_count);
		short x = (short)(round * seat_count * step_x), y = (short)(round * seat_count * step_y);
		struct unit_hud_interface_definition *copy = &copies[index];

		*copy = *unit_hud_interface_definition_get(seat_huds[index % seat_count]);
		allies_hud_shift(&copy->background.placement, x, y);
		allies_hud_shift(&copy->shield_meter.background.placement, x, y);
		allies_hud_shift(&copy->shield_meter.meter.placement, x, y);
		allies_hud_shift(&copy->health_meter.background.placement, x, y);
		allies_hud_shift(&copy->health_meter.meter.placement, x, y);
		hud_indices[index] = seat_huds[index % seat_count];
		definitions[index] = copy;
	}
	return count;
}
