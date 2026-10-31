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
is in a vehicle (the allies regroup when they are on foot again). An ally
riding a vehicle no player rides gets out (allies_tidy). They come in, and
are brought back, behind the player against the way they move, else beside
them, never ahead in their path (allies_place). In combat they keep within
ALLY_LEASH_COMBAT and come back only to ALLY_LEASH_COMBAT_RETURN. A killed
ally comes back ALLY_RESPAWN_TICKS later, behind a player. Fewer wanted
(the menu) takes away the extras; 0 takes them all.
*/
#include "cseries.h"
#include "cseries/errors.h"
#include "bitmaps/bitmap_group.h"
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
#include "game/game_globals.h"
#include "game/player_rumble.h"
#include "input/input.h"
#include "interface/interface.h"
#include "interface/hud_definitions.h"
#include "interface/hud_draw.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "coop_allies.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int config_write(char const *name, char const *value);

long config_integer(char const *name);
int config_boolean(char const *name);
char const *config_string(char const *name);
void platform_log(char const *format, ...);

static short allies_units(long *unit_indices, short maximum);
static short allies_hold_grace(void);
static short allies_pick_name(long unit_index);
/* the HUD's names (allies_slot picks one unused, from the unit's datum
index, so a revert's same allies get theirs again) */
static char const *const allies_names[50] =
{
	"James", "John", "Robert", "Michael", "William", "David", "Richard", "Joseph", "Thomas", "Charles",
	"Daniel", "Matthew", "Anthony", "Mark", "Steven", "Paul", "Andrew", "Joshua", "Kevin", "Brian",
	"George", "Edward", "Ryan", "Jacob", "Gary", "Nicholas", "Eric", "Jonathan", "Stephen", "Larry",
	"Justin", "Scott", "Brandon", "Frank", "Benjamin", "Gregory", "Samuel", "Raymond", "Patrick", "Alexander",
	"Jack", "Dennis", "Jerry", "Tyler", "Aaron", "Henry", "Peter", "Adam", "Nathan", "Zachary",
};

/* to stderr (logcat) and to the game's log (halo.log, debug.txt), where a
player's report has it */
static void allies_log(char const *format, ...)
{
	char text[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	platform_log("%s", text);
	error(_error_silent, "%s", text);
}

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
#define ALLY_LEASH_COMBAT 14.f
#define ALLY_LEASH_RETURN 4.f
/* (in combat they come back only to within this: room to fight, out of the
player's way) */
#define ALLY_LEASH_COMBAT_RETURN 8.f
#define ALLY_TELEPORT_DISTANCE 25.f
#define ALLY_SPAWN_BEHIND 1.5f
/* (farther behind a player on the move: out of their run) */
#define ALLY_SPAWN_BEHIND_MOVING 3.f
/* (an ally riding a vehicle no player rides gets out after this) */
#define ALLY_VEHICLE_ALONE_TICKS (2 * TICKS_PER_SECOND)
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
	/* (its name on the HUD: allies_names) */
	short name;
	/* (riding with no player since, and the next try to get out) */
	long alone_since;
	long exit_time;
};

static struct
{
	long last_tick;
	long next_spawn_time;
	long next_log_time;
	long next_report_time;
	short last_count;
	short last_wanted;
	/* (every sixth report to the game's log too) */
	short reports;
	boolean logged_variant;
	boolean logged_voices;
	boolean told_orders;
	/* (the order: hold there, else follow) */
	boolean holding;
	long hold_surface;
	real_point3d hold_point;
	short order_ticks[ALLY_ORDER_PLAYERS];
	boolean order_down[ALLY_ORDER_PLAYERS];
	/* (HALO_ALLIES_TEST: the button held until this tick, the next step) */
	long test_hold_until;
	boolean test_pressed;
	short test_step;
	struct coop_ally allies[ALLY_MAXIMUM * 2];
} coop_allies;

/* coop_allies_arm: a menu chose ALLIES out of a level, for the next start;
allies_level_started: that start's check done */
static boolean allies_armed = FALSE;
static boolean allies_level_started = FALSE;

void coop_allies_arm(void)
{
	allies_armed = TRUE;
}

/* an online co-op game's host only with Server Setup's ALLOW ALLIES
(network.coop_allies); a game alone or in split screen always */
boolean coop_allies_allowed(void)
{
	return game_connection() == _game_connection_local || config_boolean("network.coop_allies");
}

static short allies_wanted(void)
{
	return coop_allies_allowed() ? (short)PIN(config_integer("bots.coop"), 0, ALLY_MAXIMUM) : 0;
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
			allies_log("campaign allies: said \"%s\" (%s)", names[index], tag_get_name(sound_definition_index));
			return TRUE;
		}
	}
	return FALSE;
}

/* once a level, the sounds of the vocalizations an order could use
(halo.log): their tags and the permutations' names (once the unit's first
update has picked its dialogue: FALSE before) */
static boolean allies_log_voices(long unit_index)
{
	static char const *const names[] =
	{
		"advance", "advance re", "retreat", "retreat re", "cover", "sighted friend player",
	};
	struct unit_datum *unit = unit_get(unit_index);
	short index;

	if (unit->unit.dialogue_index == NONE)
		return FALSE;
	allies_log("campaign allies: orders: the action button held %d ticks (the hold seen after %d)",
		(int)ALLY_ORDER_TICKS, (int)allies_hold_grace());
	for (index = 0; index < (short)NUMBEROF(names); index++)
	{
		short vocalization_type = dialogue_get_vocalization_type_by_name(names[index]);
		long sound_definition_index = NONE;
		char permutations[256] = "";

		if (vocalization_type == NONE)
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
		allies_log("campaign allies: voice \"%s\": %s [%s]", names[index],
			sound_definition_index == NONE ? "none" : tag_get_name(sound_definition_index), permutations);
	}
	return TRUE;
}

/* an armed actor variant on foot of the type (marine, crewman), else NONE */
static boolean allies_variant_fits(long tag_index, short type)
{
	struct actor_variant_definition *variant;

	if (tag_index == NONE || tag_get_group_tag(tag_index) != ACTOR_VARIANT_DEFINITION_TAG)
		return FALSE;
	variant = actor_variant_definition_get(tag_index);
	return variant->actor_reference.index != NONE && variant->unit_reference.index != NONE &&
		variant->ranged_combat.reference.index != NONE &&
		tag_get_group_tag(variant->unit_reference.index) == BIPED_DEFINITION_TAG &&
		actor_definition_get(variant->actor_reference.index)->type == type;
}

/* the level's marines' variant, armed, on foot; else its crewmen's; else
(a level whose scenario places none: Two Betrayals) any such variant the
map's tags have */
static long allies_variant_search(boolean quiet)
{
	struct scenario *scenario = global_scenario_get();
	short pass, index;

	for (pass = 0; pass < 4; pass++)
	{
		short type = pass & 1 ? _actor_crew : _actor_marine;

		if (pass < 2)
		{
			for (index = 0; index < scenario->ai_actor_palette.count; index++)
			{
				struct tag_reference *entry =
					TAG_BLOCK_GET_ELEMENT(&scenario->ai_actor_palette, index, struct tag_reference);

				if (allies_variant_fits(entry->index, type))
					return entry->index;
			}
		}
		else
		{
			struct tag_iterator iterator;
			long tag_index;

			tag_iterator_new(&iterator, ACTOR_VARIANT_DEFINITION_TAG);
			while ((tag_index = tag_iterator_next(&iterator)) != NONE)
			{
				if (allies_variant_fits(tag_index, type))
					return tag_index;
			}
		}
	}
	/* (none: what the map has, for halo.log, once) */
	if (!quiet && !coop_allies.logged_variant)
	{
		struct tag_iterator iterator;
		long tag_index;
		short listed = 0;

		tag_iterator_new(&iterator, ACTOR_VARIANT_DEFINITION_TAG);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE && listed++ < 40)
		{
			struct actor_variant_definition *variant = actor_variant_definition_get(tag_index);

			allies_log("campaign allies: the map's %s: type %d, %s", tag_get_name(tag_index),
				variant->actor_reference.index == NONE ? -1 :
					(int)actor_definition_get(variant->actor_reference.index)->type,
				variant->ranged_combat.reference.index == NONE ? "unarmed" : "armed");
		}
	}
	return NONE;
}

static long allies_variant(void)
{
	return allies_variant_search(FALSE);
}

/* the menus: TRUE when the level loaded has a marine or crewman to be allies
(the spawner's own search: FALSE greys the pause menu's ALLIES) */
boolean coop_allies_available(void)
{
	return global_scenario_get() == NULL || allies_variant_search(TRUE) != NONE;
}

/* the player's way: where they move (running, strafing, backing up), else
where they face; flat */
static void allies_heading(long player_unit_index, real_vector3d *heading, boolean *moving)
{
	struct object_datum *player = object_get(player_unit_index);
	real_vector3d const *velocity = &player->object.translational_velocity;
	real speed = velocity->i * velocity->i + velocity->j * velocity->j;

	/* (faster than a walk's start: about 0.6 m/s) */
	*moving = speed > ALLY_METRES(0.02f) * ALLY_METRES(0.02f);
	heading->i = *moving ? velocity->i : player->object.forward.i;
	heading->j = *moving ? velocity->j : player->object.forward.j;
	heading->k = 0.f;
	if (normalize3d(heading) == 0.f)
	{
		heading->i = 1.f;
		heading->j = 0.f;
	}
}

/* a place by the player: behind them (side 0: against their way), else to
their left (1) or right (2); never ahead, in their path. FALSE if it is
outside the level or on a loading zone */
static boolean allies_place(long player_unit_index, short side, real_point3d *position, real *facing)
{
	struct object_datum *player = object_get(player_unit_index);
	real_vector3d heading, away;
	boolean moving;
	real distance;

	allies_heading(player_unit_index, &heading, &moving);
	distance = player->object.bounding_sphere_radius +
		ALLY_METRES(side == 0 && moving ? ALLY_SPAWN_BEHIND_MOVING : ALLY_SPAWN_BEHIND);
	if (side == 0)
	{
		away.i = -heading.i;
		away.j = -heading.j;
	}
	else
	{
		/* (beside, and a little behind) */
		away.i = (side == 1 ? -heading.j : heading.j) - 0.5f * heading.i;
		away.j = (side == 1 ? heading.i : -heading.i) - 0.5f * heading.j;
	}
	away.k = 0.f;
	normalize3d(&away);
	point_from_line3d(&player->object.position, &away, distance, position);
	if (facing)
		*facing = arctangent(heading.j, heading.i);
	return scenario_leaf_index_from_point(position) != NONE && !player_position_switches_bsp(position);
}

/* the player's biped on the ground: not falling (off a cliff, where an ally
brought beside it would fall too) */
static boolean allies_player_grounded(long player_unit_index)
{
	struct biped_datum *biped = biped_try_and_get(player_unit_index);

	return biped && !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
}

/* moves the ally's biped by the player (behind, else beside), with room and
a line of sight to them; FALSE (not moved) when none of them has room */
static boolean allies_bring_back(long unit_index, long player_unit_index)
{
	short side;

	for (side = 0; side < 3; side++)
	{
		real_point3d position;

		if (allies_place(player_unit_index, side, &position, NULL) &&
			biped_fix_position(unit_index, player_unit_index, &position, NULL, 2.f, FALSE, FALSE, TRUE))
		{
			return TRUE;
		}
	}
	return FALSE;
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
	coop_allies.allies[free_index].actor_index = NONE;
	coop_allies.allies[free_index].name = allies_pick_name(actor_get(actor_index)->meta.unit_index);
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
	allies_log("campaign allies: %s", why);
}

static boolean allies_spawn(long player_unit_index)
{
	long variant = allies_variant();
	struct actor_starting_location location;
	long actor_index;

	if (!coop_allies.logged_variant)
	{
		coop_allies.logged_variant = TRUE;
		allies_log("campaign allies: %s", variant == NONE ? "this level has no armed marine or crewman to be them" :
			tag_get_name(variant));
	}
	if (variant == NONE)
	{
		/* (ALLIES back to NONE: the pause menu shows NO MARINES) */
		if (config_integer("bots.coop") != 0 && !getenv("HALO_COOP_ALLIES") && !config_write("bots.coop", "0"))
			allies_log("campaign allies: could not write bots.coop");
		return FALSE;
	}
	csmemset(&location, 0, sizeof(location));
	{
		short side;

		/* (behind, else beside; else later: never ahead, in their run) */
		for (side = 0; side < 3 && !allies_place(player_unit_index, side, &location.position, &location.facing); side++)
			;
		if (side == 3)
		{
			allies_note("no room behind or beside the player (waiting)");
			return FALSE;
		}
	}
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
		return FALSE;
	}
	{
		struct actor_datum *actor = actor_get(actor_index);

		if (!ally_actor(actor))
		{
			allies_note("the actor made is not one of ours");
			actor_erase(actor_index, TRUE);
			return FALSE;
		}
		/* (behind the player, else beside them, with room; else none yet) */
		if (!allies_bring_back(actor->meta.unit_index, player_unit_index))
		{
			allies_note("no room behind or beside the player (waiting)");
			actor_erase(actor_index, TRUE);
			return FALSE;
		}
		actor->meta.force_active = TRUE;
		{
			struct coop_ally *slot = allies_slot(actor_index);
			char const *name = slot ? allies_names[slot->name] : "A Marine";
			char text[128];

			allies_log("campaign allies: %s joined behind or beside the player (%d wanted)", name, allies_wanted());
			short letter;

			/* (short: a phone's HUD lines are narrow) */
			snprintf(text, sizeof(text), "%s JOINED", name);
			for (letter = 0; text[letter]; letter++)
				text[letter] = (char)toupper((unsigned char)text[letter]);
			allies_tell(text);
			if (!coop_allies.told_orders)
				allies_tell("HOLD RELOAD: SEND ALLIES");
			coop_allies.told_orders = TRUE;
		}
	}
	return TRUE;
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
		coop_allies.test_pressed = FALSE;
		allies_log("campaign allies: test: holding the action button");
		return;
	}
	data_iterator_new(&iterator, player_data);
	if ((player = data_iterator_next(&iterator)) != NULL && player->unit_index != NONE)
	{
		allies_log("campaign allies: test: killing the player");
		unit_kill(player->unit_index);
	}
}

/* each update: the order's state with none alive (a hold with nobody to
hold, a dead player's half-held button), and an ally riding a vehicle no
player rides (a Warthog's gun the player left): out of it */
static void allies_tidy(long now)
{
	struct data_iterator iterator;
	struct actor_datum *actor;
	struct player_datum *player;
	long units[1];

	if (coop_allies.holding && !allies_units(units, 1))
	{
		coop_allies.holding = FALSE;
		allies_log("campaign allies: none left to hold the place (following again)");
	}
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		short slot = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		if (player->unit_index == NONE && slot >= 0 && slot < ALLY_ORDER_PLAYERS)
		{
			coop_allies.order_ticks[slot] = 0;
			coop_allies.order_down[slot] = FALSE;
		}
	}
	if (allies_quiet())
		return;
	data_iterator_new(&iterator, actor_data);
	while ((actor = data_iterator_next(&iterator)) != NULL)
	{
		long actor_index = iterator.datum_index, root_index;
		struct unit_datum *unit;
		struct coop_ally *ally;
		struct data_iterator players;
		boolean shared = FALSE;

		if (!ally_actor(actor) || actor->meta.erase_next_tick)
			continue;
		unit = unit_try_and_get(actor->meta.unit_index);
		if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		root_index = object_get_ultimate_parent(actor->meta.unit_index);
		if (root_index == actor->meta.unit_index || (ally = allies_slot(actor_index)) == NULL)
			continue;
		data_iterator_new(&players, player_data);
		while ((player = data_iterator_next(&players)) != NULL)
		{
			if (player->unit_index != NONE && object_try_and_get(player->unit_index) &&
				object_get_ultimate_parent(player->unit_index) == root_index)
			{
				shared = TRUE;
			}
		}
		if (shared || !ally->alone_since)
		{
			ally->alone_since = shared ? 0 : now + 1;
			continue;
		}
		if (now + 1 - ally->alone_since < ALLY_VEHICLE_ALONE_TICKS || now < ally->exit_time)
			continue;
		ally->exit_time = now + TICKS_PER_SECOND;
		if (unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE)
		{
			unit_try_and_exit_seat(actor->meta.unit_index);
			allies_note("an ally got out of a vehicle no player rides");
		}
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
	/* a level's start (its first seconds, once): none, unless a menu
	(Server Setup's ALLIES) chose them since the last level was played */
	if (now < 10 * TICKS_PER_SECOND)
	{
		if (!allies_level_started)
		{
			allies_level_started = TRUE;
			/* (HALO_COOP_ALLIES sets them for the run: kept) */
			if (!allies_armed && !game_engine_running() && !network_game_distributed_client() &&
				config_integer("bots.coop") != 0 && !getenv("HALO_COOP_ALLIES"))
			{
				allies_log("campaign allies: none at the level's start (ALLIES set back to NONE)");
				if (!config_write("bots.coop", "0"))
					allies_log("campaign allies: could not write bots.coop");
			}
			allies_armed = FALSE;
		}
	}
	else
	{
		allies_level_started = FALSE;
		allies_armed = FALSE;
	}
	if (!allies_running())
		return;
	allies_test(now);
	if (!coop_allies.logged_voices)
	{
		long units[1];

		if (allies_units(units, 1) && allies_log_voices(units[0]))
			coop_allies.logged_voices = TRUE;
	}

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

			if (!ally_actor(actor) || actor->meta.erase_next_tick || !object_try_and_get(actor->meta.unit_index))
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

				if (!ally_actor(actor) || actor->meta.erase_next_tick || !object_try_and_get(actor->meta.unit_index))
					continue;
				distance = distance3d(&object_get(actor->meta.unit_index)->object.position, &coop_allies.hold_point);
				hold_nearest = MIN(hold_nearest, distance);
				hold_farthest = MAX(hold_farthest, distance);
			}
			(coop_allies.reports++ % 6 ? platform_log : allies_log)("campaign allies: %d holding, %.1f to %.1f m from the held point (%.1f to %.1f m from the "
				"nearest player)", count, hold_nearest / ALLY_METRES(1.f), hold_farthest / ALLY_METRES(1.f),
				nearest / ALLY_METRES(1.f), farthest / ALLY_METRES(1.f));
		}
		else
			(coop_allies.reports++ % 6 ? platform_log : allies_log)("campaign allies: %d with the players, %.1f to %.1f m from the nearest", count,
				nearest / ALLY_METRES(1.f), farthest / ALLY_METRES(1.f));
	}
	allies_tidy(now);
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
	if (!allies_player_grounded(player_unit_index))
		return;
	/* (no room: a moment later, as the player moves on) */
	coop_allies.next_spawn_time = now + (allies_spawn(player_unit_index) ? ALLY_RETRY_TICKS / 4 : TICKS_PER_SECOND / 2);
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
		actor->state.action == _actor_action_avoid || actor->state.action == _actor_action_flee ||
		actor_in_combat(actor_index);

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
	if (now >= ally->teleport_time && allies_player_grounded(player_unit_index) &&
		(distance > ALLY_METRES(ALLY_TELEPORT_DISTANCE) ||
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
	if (distance < ALLY_METRES(fighting ? ALLY_LEASH_COMBAT_RETURN : ALLY_LEASH_RETURN))
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
	down.i = down.j = 0.f;
	if (!collision_test_vector(flags, &origin, &ray, unit_index, &hit))
	{
		/* (nothing within reach, as across open ground or at the sky: the
		ground below the end of the reach) */
		point_from_line3d(&origin, &ray, 1.f, &above);
		down.k = -ALLY_METRES(ALLY_ORDER_REACH);
		if (!collision_test_vector(flags, &above, &down, unit_index, &hit) || hit.plane.n.k < 0.6f)
		{
			allies_log("campaign allies: no ground within reach (aim %.2f %.2f %.2f)",
				unit->unit.aiming_vector.i, unit->unit.aiming_vector.j, unit->unit.aiming_vector.k);
			return FALSE;
		}
	}
	else if (hit.plane.n.k < 0.6f)
	{
		point_from_line3d(&hit.point, &unit->unit.aiming_vector, -ALLY_METRES(0.75f), &above);
		down.k = -ALLY_METRES(8.f);
		if (!collision_test_vector(flags, &above, &down, unit_index, &hit) || hit.plane.n.k < 0.6f)
		{
			allies_log("campaign allies: no ground below the wall aimed at (aim %.2f %.2f %.2f)",
				unit->unit.aiming_vector.i, unit->unit.aiming_vector.j, unit->unit.aiming_vector.k);
			return FALSE;
		}
	}
	*point = hit.point;
	*surface_index = hit.surface_index;
	return hit.surface_index != NONE;
}

/* a short pulse on the ordering player's controller (the phone vibrates
for the touch controls' player: xinput_sdl.c) */
static void allies_order_rumble(long unit_index)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		if (player->unit_index == unit_index && player->local_player_index != NONE)
		{
			struct rumble_definition rumble;

			csmemset(&rumble, 0, sizeof(rumble));
			rumble.motors[0].scale = 0.8f;
			rumble.motors[0].duration = 0.2f;
			rumble.motors[0].transition_function = 3;
			rumble.motors[1] = rumble.motors[0];
			rumble_player_impulse(player->local_player_index, &rumble, 1.f, 1.f);
		}
	}
}

/* the order given: hold the aimed point, or (holding) follow again */
static void allies_order(long unit_index)
{
	static char const *const go[] = { "advance", "cover" };
	static char const *const regroup[] = { "retreat re", "retreat", "sighted friend player" };
	long units[ALLY_MAXIMUM * 2];
	short count = allies_units(units, (short)NUMBEROF(units)), index;

	/* (all died while the button was held: nobody to order) */
	if (count <= 0)
	{
		coop_allies.holding = FALSE;
		allies_log("campaign allies: ordered, but none is alive");
		allies_tell("NO ALLIES");
		return;
	}
	if (coop_allies.holding)
	{
		coop_allies.holding = FALSE;
		for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
			coop_allies.allies[index].returning = FALSE;
		allies_log("campaign allies: ordered to regroup (%d)", count);
		allies_tell("ALLIES: REGROUP");
		allies_order_rumble(unit_index);
		allies_say(units[0], regroup, (short)NUMBEROF(regroup));
		return;
	}
	if (!allies_order_target(unit_index, &coop_allies.hold_point, &coop_allies.hold_surface))
	{
		allies_log("campaign allies: ordered, but no ground where the player aims");
		allies_tell("ALLIES: NO GROUND THERE");
		return;
	}
	coop_allies.holding = TRUE;
	for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
		coop_allies.allies[index].returning = FALSE;
	allies_log("campaign allies: ordered to hold %.1f m away (%d)",
		distance3d(&object_get(unit_index)->object.position, &coop_allies.hold_point) / ALLY_METRES(1.f), count);
	allies_tell("ALLIES: MOVE");
	allies_order_rumble(unit_index);
	allies_say(units[0], go, (short)NUMBEROF(go));
}

/* the ticks the button is held before the game reports it held (the swap
weapons bit: game globals' minimum weapon swap ticks), and a few more */
static short allies_hold_grace(void)
{
	struct game_globals *globals = scenario_get_game_globals();
	short swap_ticks = TICKS_PER_SECOND / 2;

	if (globals && globals->player_control.count > 0)
	{
		swap_ticks = TAG_BLOCK_GET_ELEMENT(&globals->player_control, 0,
			struct game_globals_player_control)->minimum_weapon_swap_ticks;
	}
	return (short)PIN(swap_ticks + 6, 6, ALLY_ORDER_TICKS - 6);
}

void coop_allies_order_input(long player_index, boolean pressed, boolean held, boolean nothing_near)
{
	short slot = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	short *ticks;
	long unit_index, units[1];
	long now = game_time_get();

	if (slot < 0 || slot >= ALLY_ORDER_PLAYERS)
		return;
	ticks = &coop_allies.order_ticks[slot];
	/* (HALO_ALLIES_TEST: as the game reports a person's hold: pressed on
	the first tick, held only once the swap ticks have passed) */
	if (now < coop_allies.test_hold_until)
	{
		long since = now - (coop_allies.test_hold_until - 2 * TICKS_PER_SECOND);

		if (!coop_allies.test_pressed)
		{
			coop_allies.test_pressed = TRUE;
			pressed = TRUE;
		}
		held = held || since >= allies_hold_grace() - 6;
	}
	/* the action button reports a press once (player_queues_new.c's latched
	controls), then, held past the swap ticks, the swap weapons bit every tick
	(and where it is not latched, every tick: held) */
	{
		boolean was_down = coop_allies.order_down[slot];

		coop_allies.order_down[slot] = pressed || held;
		held = held || pressed;
		pressed = pressed && !was_down;
	}
	if (pressed)
	{
		*ticks = 0;
		unit_index = player_get(player_index)->unit_index;
		/* (a press beside a weapon or a vehicle is theirs) */
		if (!nothing_near || allies_on_foot(unit_index) == NONE || !allies_units(units, 1))
			return;
		*ticks = 1;
		return;
	}
	if (*ticks <= 0)
		return;
	if (!held && *ticks >= allies_hold_grace())
	{
		/* (let go: a tap, the reload's) */
		*ticks = 0;
		return;
	}
	if (++*ticks < ALLY_ORDER_TICKS)
		return;
	*ticks = 0;
	unit_index = player_get(player_index)->unit_index;
	if (allies_on_foot(unit_index) != NONE)
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

	/* (on foot first; then one riding, which the respawn stands beside its
	vehicle: players.c) */
	for (index = 0; index < count; index++)
	{
		if (allies_on_foot(units[index]) != NONE && (!test || test(units[index])))
			return units[index];
	}
	for (index = 0; index < count; index++)
	{
		if (allies_on_foot(units[index]) == NONE && (!test || test(units[index])))
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


static short allies_pick_name(long unit_index)
{
	unsigned long hash = (unsigned long)unit_index * 2654435761UL;
	short name = (short)((hash >> 7) % NUMBEROF(allies_names)), tries;

	for (tries = 0; tries < (short)NUMBEROF(allies_names); tries++, name = (short)((name + 1) % NUMBEROF(allies_names)))
	{
		short index;

		for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
		{
			struct coop_ally const *ally = &coop_allies.allies[index];
			struct actor_datum *actor;

			if (ally->actor_index != NONE && ally->name == name &&
				(actor = datum_try_and_get(actor_data, ally->actor_index)) != NULL && ally_actor(actor))
			{
				break;
			}
		}
		if (index == NUMBEROF(coop_allies.allies))
			break;
	}
	return name;
}

static char const *allies_name(long unit_index)
{
	short index;

	for (index = 0; index < NUMBEROF(coop_allies.allies); index++)
	{
		struct coop_ally const *ally = &coop_allies.allies[index];
		struct actor_datum *actor;

		if (ally->actor_index != NONE && (actor = datum_try_and_get(actor_data, ally->actor_index)) != NULL &&
			actor->meta.unit_index == unit_index && ally->name >= 0 && ally->name < (short)NUMBEROF(allies_names))
		{
			return allies_names[ally->name];
		}
	}
	return "Marine";
}

/* the allies' rows on the HUD, at the left edge below the player's shield
and health (the top ALLY_HUD_TOP of the height) and above the touch
buttons there (Light, at about half the height): each its name, then the
passenger's health meter beneath */
#define ALLY_HUD_TOP 0.22f
#define ALLY_HUD_LEFT 12

short coop_allies_hud_units(short local_player_index, boolean multiplayer, long *unit_indices, long *hud_indices,
	struct unit_hud_interface_definition **definitions, short maximum)
{
	static struct unit_hud_interface_definition copies[ALLY_MAXIMUM];
	long seat_huds[ALLY_MAXIMUM];
	short count, index, line_height = 12, meter_height = 8, top;
	long font_index;
	struct unit_hud_interface_definition const *seat;
	struct bitmap_data const *bitmap = NULL;
	real_rectangle2d const *clip = NULL;
	rectangle2d window;
	real_argb_color color;

	(void)local_player_index;
	count = allies_units(unit_indices, (short)MIN(maximum, ALLY_MAXIMUM));
	if (!count || !allies_seat_huds(multiplayer, seat_huds))
		return 0;
	seat = unit_hud_interface_definition_get(seat_huds[0]);
	font_index = interface_get_tag_index(_interface_font_terminal);
	if (font_index != NONE)
	{
		struct font_header *font = font_definition_get(font_index);

		line_height = (short)MAX(font->ascending_height + font->descending_height + font->leading_height, 8);
	}
	hud_retrieve_bitmap_and_bounding_rect(seat->health_meter.meter.meter_bitmap.index,
		seat->health_meter.meter.sequence_index, 0, &bitmap, &clip);
	if (bitmap)
	{
		meter_height = (short)(clip ? (clip->y1 - clip->y0) * bitmap->height : bitmap->height);
		meter_height = (short)PIN(meter_height, 4, 24);
	}
	window = render.camera.window_bounds;
	offset_rectangle2d(&window, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	top = (short)((window.y1 - window.y0) * ALLY_HUD_TOP);
	color.alpha = 1.f;
	color.red = 0.55f;
	color.green = 0.8f;
	color.blue = 1.f;
	for (index = 0; index < count; index++)
	{
		struct unit_hud_interface_definition *copy = &copies[index];
		short row = (short)(top + index * (line_height + meter_height + 4));

		/* (the passenger's meter alone: its label and the shield's are not
		this ally's) */
		*copy = *seat;
		copy->absolute_placement.corner = _hud_anchor_top_left;
		copy->background.interface_bitmap.index = NONE;
		copy->shield_meter.background.interface_bitmap.index = NONE;
		copy->shield_meter.meter.meter_bitmap.index = NONE;
		copy->health_meter.background.interface_bitmap.index = NONE;
		copy->health_meter.meter.placement.offset.x = ALLY_HUD_LEFT;
		copy->health_meter.meter.placement.offset.y = (short)(row + line_height);
		hud_indices[index] = seat_huds[0];
		definitions[index] = copy;
		if (font_index != NONE)
		{
			rectangle2d bounds;
			char text[32];
			short letter;

			snprintf(text, sizeof(text), "%s", allies_name(unit_indices[index]));
			for (letter = 0; text[letter]; letter++)
				text[letter] = (char)toupper((unsigned char)text[letter]);
			bounds.x0 = (short)(window.x0 + ALLY_HUD_LEFT);
			bounds.x1 = (short)(bounds.x0 + 200);
			bounds.y0 = (short)(window.y0 + row);
			bounds.y1 = (short)(bounds.y0 + line_height);
			draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
			rasterizer_draw_string(&bounds, NULL, NULL, 0, text);
		}
	}
	return count;
}

/* ---------- the death camera on an ally (director.c, hud.c) */

/* each local player's watched ally's unit (NONE: its own body), and
whether that changed since director.c last looked */
static long allies_watched[MAXIMUM_LOCAL_PLAYERS] = { NONE, NONE, NONE, NONE };
static boolean allies_watched_changed[MAXIMUM_LOCAL_PLAYERS];
static boolean allies_watch_held[MAXIMUM_GAMEPADS][2];
static long allies_watch_deaths[MAXIMUM_LOCAL_PLAYERS];

/* a press (not held over) of right (D-pad right, or Y: the touch controls'
Weapon) or left (D-pad left) on the controller */
static boolean allies_watch_pressed(short controller_index, short side)
{
	struct gamepad_state const *gamepad;
	boolean down, pressed;

	if (controller_index < 0 || controller_index >= MAXIMUM_GAMEPADS)
		return FALSE;
	gamepad = input_get_gamepad_state(controller_index);
	down = gamepad && (side ?
		gamepad->buttons[_gamepad_binary_button_dpad_right] > 0 ||
			gamepad->buttons[FIRST_GAMEPAD_ANALOG_BUTTON + _gamepad_analog_button_y] > 0 :
		gamepad->buttons[_gamepad_binary_button_dpad_left] > 0);
	pressed = down && !allies_watch_held[controller_index][side];
	allies_watch_held[controller_index][side] = down;
	return pressed;
}

long coop_allies_spectate_unit(short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player;
	long units[ALLY_MAXIMUM * 2];
	short count, index, current = NONE, step = 0;
	long *watched;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || player_index == NONE)
		return NONE;
	watched = &allies_watched[local_player_index];
	player = player_get(player_index);
	if (allies_watch_pressed(player->network_player_data.controller_index, 1))
		step = 1;
	if (allies_watch_pressed(player->network_player_data.controller_index, 0))
		step = -1;
	if (player->unit_index != NONE || !allies_running() || cinematic_in_progress())
	{
		if (*watched != NONE)
			allies_watched_changed[local_player_index] = TRUE;
		*watched = NONE;
		return NONE;
	}
	/* (each death starts on the body) */
	if (allies_watch_deaths[local_player_index] != player->statistics.deaths)
	{
		allies_watch_deaths[local_player_index] = player->statistics.deaths;
		if (*watched != NONE)
			allies_watched_changed[local_player_index] = TRUE;
		*watched = NONE;
	}
	count = allies_units(units, (short)NUMBEROF(units));
	for (index = 0; index < count; index++)
	{
		if (units[index] == *watched)
			current = index;
	}
	/* (a watched ally gone: back to the body) */
	if (*watched != NONE && current == NONE)
	{
		*watched = NONE;
		allies_watched_changed[local_player_index] = TRUE;
	}
	if (step && count)
	{
		/* (the body, then each ally, round) */
		short position = (short)((current == NONE ? 0 : current + 1) + step + count + 1) % (count + 1);

		*watched = position ? units[position - 1] : NONE;
		allies_watched_changed[local_player_index] = TRUE;
	}
	return *watched;
}

boolean coop_allies_spectate_changed(short local_player_index)
{
	boolean changed;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return FALSE;
	changed = allies_watched_changed[local_player_index];
	allies_watched_changed[local_player_index] = FALSE;
	return changed;
}

void coop_allies_spectate_draw(short local_player_index)
{
	long font_index = hud_get_font_index();
	long watched, units[1];
	struct font_header *font;
	real_argb_color color;
	rectangle2d bounds;
	wchar_t text[64];
	char name[48];
	short height, letter;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || font_index == NONE ||
		!allies_running() || cinematic_in_progress() || !allies_units(units, 1))
	{
		return;
	}
	watched = allies_watched[local_player_index];
	if (watched != NONE)
		snprintf(name, sizeof(name), "WATCHING %s  (Y / < >)", allies_name(watched));
	else
		snprintf(name, sizeof(name), "Y / < >: WATCH ALLIES");
	for (letter = 0; name[letter] && letter < (short)NUMBEROF(text) - 1; letter++)
		text[letter] = (wchar_t)toupper((unsigned char)name[letter]);
	text[letter] = 0;
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	/* (above manual_respawn.c's line) */
	bounds = render.camera.window_bounds;
	bounds.y1 = (short)(bounds.y1 - 22 - height);
	bounds.y0 = (short)(bounds.y1 - height - 2);
	color.alpha = 0.55f;
	color.red = color.green = color.blue = 1.f;
	draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}
