/*
GORE.C
The gore prototype. A biped whose body is depleted by a strong enough hit
comes apart a tick later, when the game has run its death (statistics, AI,
dropped weapon):

- explosions: the blast's distance from the unit's centre against the
  unit's gib radius (the table below: a frag grenade's 120 damage within
  2 m for a marine), grown by the square root of the effect's full damage
  over the frag's (a rocket's 330: 1.66 times), over
  game.gore_threshold_scale. Vehicle hits: the vehicle's speed (physics.c
  fills damage_data.vehicle_impact_speed) against the unit's threshold.
  Hunters never come apart, and the Flood never does: no path here touches
  a Flood form (combat forms, carriers, infection forms), so it dies its
  stock death (a carrier's burst, a combat form's own Flood bits).
- the pieces: six copies of the unit, each drawing one piece of the stock
  bip01 skeleton (head, each upper arm, each thigh, the rest as the torso):
  the nodes outside the piece get zero axes at the cut's joint, so the
  vertices weighted to them shrink to it and those weighted to both sides
  stretch into a stump. No new art: every model with the bip01 skeleton
  (Grunt, Jackal, Elite, marines...) gets true left
  and right limbs. Other models split by their regions (head, arms, legs)
  where they have them, or throw the map's stock Flood bits (garbage
  objects with their own physics), or bleed only.
- the copies are posed by the dying unit's node matrices and moved here as
  rigid bodies (their own node matrices are never recomputed); they collide
  with the world from their third tick, fly along the force with a random
  spin, bounce, and leave blood: a burst at the cut, a few drops in the air,
  a splat decal where they first land.
- the dying unit is deleted (its kill was counted by its death).

Any shotgun kill at point blank also gibs (below).

One exception to the hunters below: a hunter killed by a direct hit from
a scorpion's cannon shell gibs, with more blood (game.gore_hunter_tank).

game.gore_level: off (the stock deaths), mild (every blood event at half
the count, and the pieces thrown at half the speed and spin), heavy (all of
the above as it was before the level), extreme (heavy's pieces, a quarter
more blood events, and small chunks of the victim's flesh with each gib:
copies of the unit drawing one node's mesh only, shrunk, tinted wholly in
the victim's blood colour; gore_spawn_chunks).

Settings: game.gore_level, game.gore_threshold_scale, game.gore_max_pieces,
game.gore_piece_seconds, game.gore_piece_far_distance, game.gore_blood_seconds,
game.gore_vehicle_blood, game.gore_vehicle_marks; debug.gore_test (seconds) brings the biped nearest
the player's view (debug.gore_test_unit: whose tag path has this in it) in
front of the player and gibs it, to try it.
*/

#include "cseries.h"
#include "gore.h"
#include "cseries/errors.h"
#include "effects/decals.h"
#include "effects/effects.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/damage.h"
#include "game/game_globals.h" /* MAXIMUM_NUMBER_OF_MATERIAL_TYPES, for the next */
#include "objects/damage_effect_definitions.h"
#include "objects/damage_resistances.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collision_model_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "cache/cache_files.h" /* tag_iterator_new, for the test */
#include "units/bipeds.h"
#include "units/units.h"

#include <math.h>

float cosf(float);
float sinf(float);
float fabsf(float);
double sqrt(double);
#include <string.h>

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);

/* ---------- constants */

#define GORE_MAXIMUM_PIECES 512
#define GORE_MAXIMUM_VEHICLE_MARKS 256
#define VEHICLE_TAG 'vehi'
/* the world and scenery: not units, vehicles or each other (a piece came
to rest on the player's head and hung there) */
#define GORE_PIECE_COLLISION_FLAGS (FLAG(_collision_test_structure_bit) | \
	FLAG(_collision_test_media_bit) | FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) | \
	FLAG(_collision_test_objects_machines_bit))
#define GORE_MAXIMUM_PENDING 16
#define GORE_MAXIMUM_NODES 64
#define GORE_GRAVITY (9.8f/3.048f/900.f) /* world units a tick squared */
#define GARBAGE_TAG 'garb'
#define EFFECT_TAG 'effe'
#define DECAL_TAG 'deca'

enum
{
	_gore_part_head = 0,
	_gore_part_arms,
	_gore_part_left_arm,
	_gore_part_right_arm,
	_gore_part_legs,
	_gore_part_torso,
	NUMBER_OF_GORE_PARTS
};

/* ---------- definitions */

struct gore_unit_type
{
	char const *tag_path_part; /* a substring of the biped's tag path */
	real gib_radius; /* metres: a frag grenade (120 damage) this close gibs */
	real vehicle_threshold; /* world units a second */
	char const *decal_name;
	boolean disabled; /* never gibs: the normal death */
};

/* a frag grenade's explosion (weapons\frag grenade\explosion, every map):
falloff 1.5 and cutoff 2.5 world units (4.6 m and 7.6 m), 80 to 120 damage,
so its damage alone cannot tell 2 m from 4 m; the gib radius is the blast's
distance from the unit's centre, grown by the square root of the effect's
full damage over the frag's 120 (a rocket's 330: 1.66 times) */
static struct gore_unit_type const gore_unit_types[] =
{
	/* the Flood, every form (characters\floodcombat_human,
	floodcombat elite, floodcarrier, floodinfection): never, and first,
	before "elite" matches "floodcombat elite". Gibbing one replaced its
	stock destruction (damage.c: the gib is taken instead of
	object_destroy, so a carrier never burst, nor a combat form threw its
	own bits) and gore_unit_gib then deleted it: it vanished. */
	{ "flood", 0.f, 0.f, NULL, TRUE },
	{ "hunter", 0.f, 0.f, NULL, TRUE },
	{ "grunt", 2.8f, 3.f, "effects\\decals\\blood splats\\blood grunt splat", FALSE },
	{ "jackal", 2.5f, 3.5f, "effects\\decals\\blood splats\\blood grunt splat", FALSE },
	{ "elite", 2.f, 5.f, "effects\\decals\\blood splats\\blood elite splat", FALSE },
	{ "cyborg", 2.5f, 4.f, "effects\\decals\\blood splats\\blood splat", FALSE },
	{ "marine", 2.5f, 4.f, "effects\\decals\\blood splats\\blood splat", FALSE },
	{ "crewman", 2.5f, 4.f, "effects\\decals\\blood splats\\blood splat", FALSE },
	{ "captain", 2.5f, 4.f, "effects\\decals\\blood splats\\blood splat", FALSE },
	{ "pilot", 2.5f, 4.f, "effects\\decals\\blood splats\\blood splat", FALSE },
	{ "", 2.2f, 4.5f, "effects\\decals\\blood splats\\blood splat", FALSE },
};

#define GORE_METRES_PER_WORLD_UNIT 3.048f
#define GORE_FRAG_DAMAGE 120.f

/* the region names each part keeps (the stock models', d20 .. a10) */
static char const *const gore_part_regions[NUMBER_OF_GORE_PARTS][3] =
{
	{ "head", NULL, NULL },
	{ "arms", NULL, NULL },
	{ "left arm", "left_arm", NULL },
	{ "right arm", "right_arm", NULL },
	{ "legs", NULL, NULL },
	{ "torso", "shield", NULL },
};

static char const *const gore_part_pivots[NUMBER_OF_GORE_PARTS] =
{
	"bip01 head", "bip01 spine1", "bip01 l upperarm", "bip01 r upperarm", "bip01 pelvis", "bip01 spine",
};

struct gore_piece
{
	long object_index;
	long garbage; /* TRUE: a stock bit flying on its own physics */
	boolean hunter; /* a hunter's piece: more blood, bigger splats (below) */
	boolean chunk; /* an Extreme chunk: one node's flesh, tinted (gore_spawn_chunks) */
	long age;
	long life;
	boolean resting;
	boolean impacted;
	long blood_effect_index;
	long decal_index;
	long unit_definition_index; /* (its blood's color) */
	real_point3d pivot0;
	real_point3d position;
	real_vector3d velocity;
	real_vector3d spin; /* axis times radians a tick */
	real rotation[3][3];
	short node_count;
	real_matrix4x3 base_matrices[GORE_MAXIMUM_NODES];
	/* NONE: the node is drawn; else the node whose position its vertices
	collapse to (a node of the unit outside this piece) */
	signed char collapse[GORE_MAXIMUM_NODES];
};

struct gore_pending
{
	long unit_index;
	long time;
	real_vector3d direction;
	real force; /* over the threshold, 1 and up */
	boolean player; /* a player's unit (kept, hidden; its player lets go of it as it dies) */
	boolean hunter; /* killed by a direct scorpion shell: more blood */
};

/* ---------- globals */

static struct gore_piece gore_pieces[GORE_MAXIMUM_PIECES];
static boolean gore_last_player;
static boolean gore_last_hunter;
static real_vector3d gore_last_direction;
static boolean gore_last_direction_valid;

/* a scorpion shell's direct hits on hunters, remembered a third of a
second: the shell's own detonation, not its impact damage, is usually the
killing blow of a direct hit, so the kill that follows within the window
counts as the direct hit's (a shell that lands beside a hunter records
nothing here, and its splash kill does not gib) */
#define GORE_HUNTER_DIRECT_TICKS 10
#define GORE_MAXIMUM_HUNTER_HITS 8
struct gore_hunter_hit
{
	long unit_index;
	long time;
	real_vector3d direction; /* the shell's way */
	real_point3d epicenter; /* where the shell went off (the kill's match) */
	long owner_object_index; /* who fired it (the kill's other match) */
};
static struct gore_hunter_hit gore_hunter_hits[GORE_MAXIMUM_HUNTER_HITS];
static struct gore_headless *gore_headless_find(long unit_index);
static struct gore_pending gore_pending[GORE_MAXIMUM_PENDING];

/* a shotgun blast's pellets, aggregated as they land (gore_unit_damage_begin
is called before each pellet's damage): the victim's vitality before the
first pellet, the clear shooter-to-victim distance then, and how many
pellets have hit. 6 ticks is 200 ms at 30 ticks a second: one trigger
pull's pellets land inside it. The grouping only attributes a kill to a
blast (and counts its pellets for the log); how hurt the victim already
was does not matter. */
#define GORE_SHOTGUN_WINDOW_TICKS 6
#define GORE_MAXIMUM_SHOTGUN_BLASTS 16
struct gore_shotgun_blast
{
	long victim_index;
	long owner_object_index;
	long owner_player_index;
	long damage_definition_index;
	long first_tick;
	long last_tick;
	short pellets;
	boolean decided; /* a kill from this blast was judged (gib or not) */
	real distance; /* metres, clear of the two bounding spheres */
};
static struct gore_shotgun_blast gore_shotgun_blasts[GORE_MAXIMUM_SHOTGUN_BLASTS];
static unsigned long gore_seed = 0x6f726531;
static short gore_test_done;
static long gore_piece_unit_definition = NONE; /* (the unit coming apart) */
static real gore_last_force = 1.f; /* gore_unit_hit's, for gore_unit_gib_later */

/* ---------- the level (game.gore_level) */

enum
{
	_gore_level_off = 0,
	_gore_level_mild,
	_gore_level_heavy,
	_gore_level_extreme,
	NUMBER_OF_GORE_LEVELS
};

static char const *const gore_level_names[NUMBER_OF_GORE_LEVELS] = { "off", "mild", "heavy", "extreme" };

static boolean gore_name_equal(char const *a, char const *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
		char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;

		if (x != y)
			return FALSE;
	}
	return !*a && !*b;
}

static short gore_level(void)
{
	char const *name = config_string("game.gore_level");
	static boolean warned;
	short level;

	if (!name)
		return _gore_level_heavy;
	for (level = 0; level < NUMBER_OF_GORE_LEVELS; level++)
	{
		if (gore_name_equal(name, gore_level_names[level]))
			return level;
	}
	/* (the old switch's words) */
	if (gore_name_equal(name, "false") || gore_name_equal(name, "0") || gore_name_equal(name, "no"))
		return _gore_level_off;
	if (!warned && *name && !gore_name_equal(name, "true") && !gore_name_equal(name, "on"))
	{
		warned = TRUE;
		error(_error_silent, "gore: game.gore_level \"%s\" is none of off, mild, heavy, extreme: heavy", name);
	}
	return _gore_level_heavy;
}

/* how many blood events the level makes of each one heavy makes */
static real gore_blood_scale(void)
{
	switch (gore_level())
	{
	case _gore_level_mild: return 0.5f;
	case _gore_level_extreme: return 1.25f;
	default: return 1.f;
	}
}

/* the pieces' launch speed and spin, of heavy's */
static real gore_launch_scale(void)
{
	return gore_level() == _gore_level_mild ? 0.5f : 1.f;
}

/* blood events, counted for the log: each one heavy would make is
requested; the level's share of them is made (gore_blood_count) */
enum
{
	_gore_blood_burst = 0, /* an effect: the cut's burst, a drop in the air, a landing's burst, a neck's bleed */
	_gore_blood_splat, /* a decal on the ground */
	NUMBER_OF_GORE_BLOOD_KINDS
};

static real gore_blood_credit[NUMBER_OF_GORE_BLOOD_KINDS];
static long gore_blood_requested[NUMBER_OF_GORE_BLOOD_KINDS];
static long gore_blood_emitted[NUMBER_OF_GORE_BLOOD_KINDS];
/* (the extra events' scatter, and the chunks, have their own random
sequences: the pieces fly the same at every level) */
static unsigned long gore_jitter_seed = 0x626c6f6f;
static unsigned long gore_chunk_seed = 0x6368756e;
/* the launches of the gib coming apart (gore_unit_gib's log) */
static real gore_launch_speed_sum;
static real gore_launch_spin_sum;
static short gore_launch_count;
static long gore_test_gib_time = NONE;
static long gore_test_made = NONE; /* the unit the test made, if it did */
static long gore_test_target = NONE; /* the test's unit, for its summary */
static short gore_test_infection_before; /* Flood infection forms at the test's hit */

static struct gore_unit_type const *gore_unit_type_get(long definition_index);
static boolean gore_is_flood(long definition_index);
static short gore_blood_count(short kind);
static void gore_blood_decal(long decal_index, struct collision_result const *collision,
	real_vector3d const *velocity, real radius);
static boolean gore_vehicle_node_is_wheel(long vehicle_index, short node_index);
static struct model *gore_model_get(long definition_index);

/* ---------- blood on vehicles

A mark is a one-quad decal (effects/decals.c decal_quad_new) whose corners
are kept in the space of the vehicle node it is on (the hull's, a wheel's)
and are put back into the world every tick, so the blood rides on the
vehicle and turns with its wheels. Marks come from hits (where the unit met
the hull; more and bigger the harder the hit) and from driving over corpses
and pieces (on the wheel that went over, and a smear on the ground). The
cap (game.gore_vehicle_marks) takes the oldest mark first. */

struct gore_vehicle_mark
{
	long vehicle_index; /* NONE: free */
	long decal_index;
	long definition_index;
	long time;
	short node_index;
	short cluster_index;
	real alpha;
	real_point3d local[4];
};

static struct gore_vehicle_mark gore_vehicle_marks[GORE_MAXIMUM_VEHICLE_MARKS];

/* ---------- blood coverage

How much of each of a vehicle's nodes (the hull's, each wheel's) is covered
in blood, 0 to 1, and its color. The renderer (models.c's
render_model_part_blood_proc, d3d8_gl.c's xgpu_set_blood) covers each part
of the vehicle as much as its node is: noise over the part's texture
coordinates, so it spreads in splatters and soaks in as more comes. A hit
adds to the node it hit, a little to the hull; driving over a body adds to
the wheel that went over it. It dries off over game.gore_blood_seconds
twice over. Keyed by the vehicle's whole handle (index and salt): a new
vehicle in an old one's slot starts clean. */

#define GORE_MAXIMUM_COVERED_VEHICLES 32

struct gore_coverage
{
	long vehicle_index; /* NONE: free */
	long time;
	real_rgb_color color[GORE_MAXIMUM_NODES];
	real total[GORE_MAXIMUM_NODES]; /* (the color's weight) */
	real amount[GORE_MAXIMUM_NODES];
};

static struct gore_coverage gore_coverages[GORE_MAXIMUM_COVERED_VEHICLES];

void xgpu_set_blood(float red, float green, float blue, float amount);
extern void (*render_model_part_blood_proc)(long object_index, short node_index);

/* human red, covenant blue (grunts, jackals) or purple (elites), flood
yellow-green */
static void gore_blood_color(long unit_definition_index, real_rgb_color *color)
{
	char const *name = tag_get_name(unit_definition_index);

	/* dark, dried tones */
	color->red = 0.24f; color->green = 0.03f; color->blue = 0.03f;
	if (!name)
		return;
	if (strstr(name, "grunt") || strstr(name, "jackal"))
	{
		color->red = 0.05f; color->green = 0.08f; color->blue = 0.2f;
	}
	else if (strstr(name, "elite"))
	{
		color->red = 0.16f; color->green = 0.05f; color->blue = 0.2f;
	}
	else if (strstr(name, "flood"))
	{
		color->red = 0.2f; color->green = 0.2f; color->blue = 0.06f;
	}
}

static struct gore_coverage *gore_coverage_get(long vehicle_index, boolean add)
{
	struct gore_coverage *oldest = NULL;
	short index;

	for (index = 0; index < GORE_MAXIMUM_COVERED_VEHICLES; index++)
	{
		struct gore_coverage *coverage = &gore_coverages[index];

		if (coverage->vehicle_index == vehicle_index && vehicle_index != NONE)
			return coverage;
	}
	if (!add || !object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle))
		return NULL;
	for (index = 0; index < GORE_MAXIMUM_COVERED_VEHICLES; index++)
	{
		struct gore_coverage *coverage = &gore_coverages[index];

		if (coverage->vehicle_index == NONE ||
			!object_try_and_get_and_verify_type(coverage->vehicle_index, _object_mask_vehicle))
		{
			oldest = coverage;
			break;
		}
		if (!oldest || coverage->time < oldest->time)
			oldest = coverage;
	}
	csmemset(oldest, 0, sizeof(*oldest));
	oldest->vehicle_index = vehicle_index;
	return oldest;
}

static void gore_coverage_add(long vehicle_index, short node_index, real amount, long unit_definition_index)
{
	struct gore_coverage *coverage;
	real_rgb_color color;

	if (!config_boolean("game.gore_vehicle_blood") || !config_boolean("game.gore_vehicle_coverage") ||
		node_index < 0 || node_index >= GORE_MAXIMUM_NODES || amount <= 0.f)
		return;
	coverage = gore_coverage_get(vehicle_index, TRUE);
	if (!coverage)
		return;
	/* (as much of it as the level's blood) */
	amount *= gore_blood_scale();
	gore_blood_color(unit_definition_index, &color);
	/* the node's color: the blood so far and this, by how much of each */
	{
		real_rgb_color *c = &coverage->color[node_index];
		real total = coverage->total[node_index];

		c->red = (c->red * total + color.red * amount) / (total + amount);
		c->green = (c->green * total + color.green * amount) / (total + amount);
		c->blue = (c->blue * total + color.blue * amount) / (total + amount);
		coverage->total[node_index] = MIN(total + amount, 4.f);
	}
	coverage->amount[node_index] = MIN(coverage->amount[node_index] + amount, 1.f);
	coverage->time = game_time_get();
	if (config_boolean("game.gore_log"))
		error(_error_silent, "gore: blood coverage of %s node %d now %.2f",
			tag_get_name(object_get(vehicle_index)->definition_index), node_index, coverage->amount[node_index]);
}

static void gore_coverage_update(void)
{
	real seconds = (real)config_real("game.gore_blood_seconds");
	real dry = seconds > 0.f ? 1.f / (seconds * 2.f * 30.f) : 0.f;
	short index;

	for (index = 0; index < GORE_MAXIMUM_COVERED_VEHICLES; index++)
	{
		struct gore_coverage *coverage = &gore_coverages[index];
		short node;

		if (coverage->vehicle_index == NONE)
			continue;
		if (!object_try_and_get_and_verify_type(coverage->vehicle_index, _object_mask_vehicle))
		{
			coverage->vehicle_index = NONE;
			continue;
		}
		for (node = 0; node < GORE_MAXIMUM_NODES; node++)
			coverage->amount[node] = MAX(coverage->amount[node] - dry, 0.f);
	}
}

/* an Extreme chunk's flesh: the victim's blood colour, bright and whole
(not gore_blood_color's dried tones): human red, grunt cyan, jackal and
elite blue-purple, hunter orange */
static void gore_chunk_color(long unit_definition_index, real_rgb_color *color)
{
	char const *name = tag_get_name(unit_definition_index);

	color->red = 0.62f; color->green = 0.05f; color->blue = 0.04f;
	if (!name)
		return;
	if (strstr(name, "hunter"))
	{
		color->red = 0.95f; color->green = 0.42f; color->blue = 0.04f;
	}
	else if (strstr(name, "grunt"))
	{
		color->red = 0.08f; color->green = 0.62f; color->blue = 0.78f;
	}
	else if (strstr(name, "jackal") || strstr(name, "elite"))
	{
		color->red = 0.32f; color->green = 0.16f; color->blue = 0.85f;
	}
}

/* the live chunks, for the renderer (made again each tick, gore_update) */
#define GORE_MAXIMUM_CHUNKS 48
struct gore_chunk_render
{
	long object_index;
	real_rgb_color color;
};
static struct gore_chunk_render gore_chunk_renders[GORE_MAXIMUM_CHUNKS];
static short gore_chunk_render_count;

static void gore_render_part_blood(long object_index, short node_index)
{
	struct gore_coverage *coverage;
	short chunk;

	if (object_index == NONE)
	{
		xgpu_set_blood(0.f, 0.f, 0.f, 0.f);
		return;
	}
	/* a chunk: flesh all over (an amount over 1: d3d8_gl.c, nv2a_psh.c) */
	for (chunk = 0; chunk < gore_chunk_render_count; chunk++)
	{
		if (gore_chunk_renders[chunk].object_index == object_index)
		{
			xgpu_set_blood(gore_chunk_renders[chunk].color.red, gore_chunk_renders[chunk].color.green,
				gore_chunk_renders[chunk].color.blue, 2.f);
			return;
		}
	}
	coverage = gore_coverage_get(object_index, FALSE);
	if (!coverage || node_index < 0 || node_index >= GORE_MAXIMUM_NODES || coverage->amount[node_index] <= 0.01f)
	{
		xgpu_set_blood(0.f, 0.f, 0.f, 0.f);
		return;
	}
	xgpu_set_blood(coverage->color[node_index].red, coverage->color[node_index].green, coverage->color[node_index].blue,
		coverage->amount[node_index]);
}

struct gore_vehicle_wheel_contact
{
	long vehicle_index;
	long object_index;
	long time;
};

#define GORE_MAXIMUM_WHEEL_CONTACTS 32
static struct gore_vehicle_wheel_contact gore_wheel_contacts[GORE_MAXIMUM_WHEEL_CONTACTS];

static boolean gore_far_from_players(real_point3d const *point)
{
	real far_metres = (real)config_real("game.gore_piece_far_distance");
	real far_units = far_metres / 3.048f;
	struct object_iterator iterator;
	struct biped_datum *biped;
	boolean any = FALSE;

	if (far_metres <= 0.f)
		return FALSE;
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (biped->unit.player_index == NONE)
			continue;
		any = TRUE;
		if (distance_squared3d(&biped->object.position, point) < far_units * far_units)
			return FALSE;
	}
	return any;
}

static void gore_vehicle_mark_free(struct gore_vehicle_mark *mark)
{
	decal_quad_delete(mark->decal_index);
	mark->decal_index = NONE;
	mark->vehicle_index = NONE;
}

/* the mark's corners in the world now; FALSE if its vehicle is gone */
static boolean gore_vehicle_mark_corners(struct gore_vehicle_mark const *mark, real_point3d corners[4], short *cluster_index)
{
	struct object_datum *vehicle = object_try_and_get_and_verify_type(mark->vehicle_index, _object_mask_vehicle);
	real_matrix4x3 const *matrix;
	short corner;

	if (!vehicle || mark->node_index < 0 ||
		mark->node_index >= (short)(vehicle->object.node_matrices.size / sizeof(real_matrix4x3)))
		return FALSE;
	matrix = &object_get_node_matrices(mark->vehicle_index)[mark->node_index];
	for (corner = 0; corner < 4; corner++)
		matrix4x3_transform_point(matrix, &mark->local[corner], &corners[corner]);
	*cluster_index = vehicle->object.location.cluster_index;
	return TRUE;
}

/* blood on the vehicle at a world point, facing out along the normal */
static void gore_vehicle_mark_add(long vehicle_index, short node_index, real_point3d const *point,
	real_vector3d const *normal, real radius, long definition_index, real alpha)
{
	struct object_datum *vehicle = object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle);
	/* Vehicle marks are hard-disabled: their corners come from hits on the vehicle's collision
	geometry, so the quads float off the visible mesh. An older default wrote
	game.gore_vehicle_marks = 160 into existing config.toml files, so the setting is ignored. */
	long maximum = 0;
	struct gore_vehicle_mark *mark = NULL;
	real_matrix4x3 const *matrix;
	real_vector3d n = *normal;
	real_vector3d u;
	real_vector3d v;
	real angle;
	real c;
	real si;
	short index;
	short corner;

	if (!vehicle || maximum <= 0 || definition_index == NONE ||
		node_index < 0 || node_index >= (short)(vehicle->object.node_matrices.size / sizeof(real_matrix4x3)))
		return;
	for (index = 0; index < maximum; index++)
	{
		struct gore_vehicle_mark *candidate = &gore_vehicle_marks[index];

		if (candidate->vehicle_index == NONE)
		{
			mark = candidate;
			break;
		}
		if (!mark || candidate->time < mark->time)
			mark = candidate;
	}
	if (!mark)
		return;
	if (mark->vehicle_index != NONE)
		gore_vehicle_mark_free(mark);
	if (normalize3d(&n) == 0.f)
		n = *global_up3d;
	perpendicular3d(&n, &u);
	normalize3d(&u);
	cross_product3d(&n, &u, &v);
	/* a random turn, so marks do not all line up */
	angle = real_seed_random_range(&gore_seed, 0.f, 6.2831853f);
	c = cosf(angle);
	si = sinf(angle);
	{
		real_vector3d a;
		real_vector3d b;

		a.i = u.i * c + v.i * si; a.j = u.j * c + v.j * si; a.k = u.k * c + v.k * si;
		b.i = v.i * c - u.i * si; b.j = v.j * c - u.j * si; b.k = v.k * c - u.k * si;
		u = a;
		v = b;
	}
	matrix = &object_get_node_matrices(vehicle_index)[node_index];
	for (corner = 0; corner < 4; corner++)
	{
		/* (clockwise seen from outside: the decals cull counterclockwise) */
		static real const su[4] = { -1.f, -1.f, 1.f, 1.f };
		static real const sv[4] = { -1.f, 1.f, 1.f, -1.f };
		real_point3d world;

		world.x = point->x + n.i * 0.02f + (u.i * su[corner] + v.i * sv[corner]) * radius;
		world.y = point->y + n.j * 0.02f + (u.j * su[corner] + v.j * sv[corner]) * radius;
		world.z = point->z + n.k * 0.02f + (u.k * su[corner] + v.k * sv[corner]) * radius;
		matrix4x3_inverse_transform_point(matrix, &world, &mark->local[corner]);
	}
	mark->vehicle_index = vehicle_index;
	mark->node_index = node_index;
	mark->definition_index = definition_index;
	mark->alpha = alpha;
	mark->time = game_time_get();
	mark->decal_index = NONE;
	mark->cluster_index = NONE;
}

static void gore_vehicle_marks_update(void)
{
	short index;

	/* moving quads no mark has (a loaded or reverted game's) go */
	if ((game_time_get() % 30) == 0)
	{
		long keep[GORE_MAXIMUM_VEHICLE_MARKS];
		long count = 0;

		for (index = 0; index < GORE_MAXIMUM_VEHICLE_MARKS; index++)
		{
			if (gore_vehicle_marks[index].vehicle_index != NONE && gore_vehicle_marks[index].decal_index != NONE)
				keep[count++] = gore_vehicle_marks[index].decal_index;
		}
		decal_quads_delete_except(keep, count);
	}

	for (index = 0; index < GORE_MAXIMUM_VEHICLE_MARKS; index++)
	{
		struct gore_vehicle_mark *mark = &gore_vehicle_marks[index];
		real_point3d corners[4];
		short cluster_index;

		if (mark->vehicle_index == NONE)
			continue;
		if (!gore_vehicle_mark_corners(mark, corners, &cluster_index))
		{
			gore_vehicle_mark_free(mark);
			continue;
		}
		if (cluster_index == NONE)
			continue;
		/* a new cluster, or the vertex cache took it: made again */
		if (cluster_index != mark->cluster_index || !decal_quad_set(mark->decal_index, corners))
		{
			decal_quad_delete(mark->decal_index);
			mark->decal_index = decal_quad_new(mark->definition_index, cluster_index, corners, mark->alpha);
			if (config_boolean("game.gore_log") && mark->cluster_index == NONE)
				error(_error_silent, "gore: vehicle mark decal %s (node %d, cluster %d)",
					mark->decal_index != NONE ? "made" : "NOT made", mark->node_index, cluster_index);
			mark->cluster_index = cluster_index;
		}
	}
}

static long gore_vehicle_blood_decal(long unit_definition_index)
{
	struct gore_unit_type const *type = gore_unit_type_get(unit_definition_index);
	long decal_index;

	/* (a hunter's has no name: tag_loaded would compare against NULL) */
	if (type->disabled || !type->decal_name)
		return NONE;
	decal_index = type->decal_name ? tag_loaded(DECAL_TAG, type->decal_name) : NONE;
	if (decal_index == NONE)
		decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat");
	return decal_index;
}

/* a unit hit by a vehicle: blood where it met the hull, as much as the
hit was hard (force: the speed over the unit's gib speed) */
static void gore_vehicle_hit_blood(long vehicle_index, long unit_index, real force)
{
	struct object_datum *vehicle = object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle);
	struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	long decal_index;
	short count;
	short index;

	if (!vehicle || !unit || !config_boolean("game.gore_vehicle_blood") || gore_is_flood(unit->definition_index))
		return;
	decal_index = gore_vehicle_blood_decal(unit->definition_index);
	/* the hull, a little all over (its parts are mostly the root's) */
	count = (short)PIN(1.f + force * 2.f, 1.f, 6.f);
	for (index = 0; index < count; index++)
	{
		struct collision_result collision;
		real_point3d from = unit->object.bounding_sphere_center;
		real_vector3d to;
		real_vector3d jitter;

		seed_random_direction3d(&gore_seed, &jitter);
		from.x += jitter.i * unit->object.bounding_sphere_radius * 0.6f;
		from.y += jitter.j * unit->object.bounding_sphere_radius * 0.6f;
		from.z += jitter.k * unit->object.bounding_sphere_radius * 0.6f;
		vector_from_points3d(&from, &vehicle->object.bounding_sphere_center, &to);
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_objects_bit) |
				FLAG(_collision_test_objects_vehicles_bit), &from, &to, unit_index, &collision) &&
			collision.object_index == vehicle_index)
		{
			gore_vehicle_mark_add(vehicle_index, collision.node_index != NONE ? collision.node_index : 0,
				&collision.point, &collision.plane.n,
				real_seed_random_range(&gore_seed, 0.08f, 0.14f) * PIN(force, 0.7f, 1.6f),
				decal_index, PIN(0.5f + 0.3f * force, 0.5f, 1.f));
			/* (the wheels only: the hull tinted looked painted) */
			if (collision.node_index != NONE && gore_vehicle_node_is_wheel(vehicle_index, collision.node_index))
				gore_coverage_add(vehicle_index, collision.node_index, 0.07f * PIN(force, 0.5f, 3.f), unit->definition_index);
			if (config_boolean("game.gore_log"))
				error(_error_silent, "gore: blood on %s node %d (force %.2f)",
					tag_get_name(vehicle->definition_index), collision.node_index, force);
		}
	}
}

static boolean gore_vehicle_node_is_wheel(long vehicle_index, short node_index)
{
	struct model *model = gore_model_get(object_get(vehicle_index)->definition_index);
	char const *name;

	if (!model || node_index < 0 || node_index >= (short)model->nodes.count)
		return FALSE;
	name = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node)->name;
	/* (not the warthog's "frame steering wheel") */
	return !strstr(name, "steering") && (strstr(name, "wheel") || strstr(name, "tire") || strstr(name, "tread"));
}

/* the vehicle's wheel nodes (names with wheel or tire in them) */
static short gore_vehicle_wheel_nearest(long vehicle_index, real_point3d const *point, real *distance)
{
	struct object_datum *vehicle = object_get(vehicle_index);
	struct model *model = gore_model_get(vehicle->definition_index);
	real_matrix4x3 const *matrices = object_get_node_matrices(vehicle_index);
	short count = (short)(vehicle->object.node_matrices.size / sizeof(real_matrix4x3));
	short best = NONE;
	short index;

	if (!model)
		return NONE;
	for (index = 0; index < MIN(count, (short)model->nodes.count); index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;
		real d;

		if (strstr(name, "steering") || (!strstr(name, "wheel") && !strstr(name, "tire") && !strstr(name, "tread")))
			continue;
		d = distance_squared3d(&matrices[index].position, point);
		if (best == NONE || d < *distance)
		{
			best = index;
			*distance = d;
		}
	}
	if (best != NONE)
		*distance = (real)sqrt(*distance);
	return best;
}

/* a vehicle going over a corpse or a piece: blood on the wheel that went
over it (on its side, so it turns with it), a smear on the ground */
static void gore_vehicle_run_over(long vehicle_index, long object_index, real_point3d const *point, long decal_index,
	long unit_definition_index)
{
	struct object_datum *vehicle = object_get(vehicle_index);
	real distance = 0.f;
	short wheel = gore_vehicle_wheel_nearest(vehicle_index, point, &distance);
	short index;
	short free_index = NONE;
	short oldest = 0;

	if (wheel == NONE || distance > 0.6f)
		return;
	/* once for each wheel and body a second */
	for (index = 0; index < GORE_MAXIMUM_WHEEL_CONTACTS; index++)
	{
		struct gore_vehicle_wheel_contact *contact = &gore_wheel_contacts[index];

		if (contact->vehicle_index == vehicle_index && contact->object_index == object_index &&
			game_time_get() - contact->time < 30)
			return;
		if (free_index == NONE && (contact->vehicle_index == NONE || game_time_get() - contact->time >= 30))
			free_index = index;
		if (contact->time < gore_wheel_contacts[oldest].time)
			oldest = index;
	}
	if (free_index == NONE)
		free_index = oldest;
	gore_wheel_contacts[free_index].vehicle_index = vehicle_index;
	gore_wheel_contacts[free_index].object_index = object_index;
	gore_wheel_contacts[free_index].time = game_time_get();
	if (unit_definition_index != NONE)
		gore_coverage_add(vehicle_index, wheel, 0.22f, unit_definition_index);
	{
		real_matrix4x3 const *matrix = &object_get_node_matrices(vehicle_index)[wheel];
		real_vector3d side;
		real_vector3d out;
		real_point3d center;
		short marks;

		/* the wheel's outer face: away from the vehicle's middle, along
		the vehicle's left */
		side = object_get_node_matrices(vehicle_index)[0].left;
		vector_from_points3d(&vehicle->object.bounding_sphere_center, &matrix->position, &out);
		if (dot_product3d(&out, &side) < 0.f)
			scale_vector3d(&side, -1.f, &side);
		for (marks = 0; marks < 2; marks++)
		{
			real_vector3d offset;

			seed_random_direction3d(&gore_seed, &offset);
			offset.i -= side.i * dot_product3d(&offset, &side);
			offset.j -= side.j * dot_product3d(&offset, &side);
			offset.k -= side.k * dot_product3d(&offset, &side);
			center.x = matrix->position.x + side.i * 0.12f + offset.i * 0.12f;
			center.y = matrix->position.y + side.j * 0.12f + offset.j * 0.12f;
			center.z = matrix->position.z + side.k * 0.12f + offset.k * 0.12f;
			gore_vehicle_mark_add(vehicle_index, wheel, &center, &side,
				real_seed_random_range(&gore_seed, 0.06f, 0.1f), decal_index, 0.9f);
		}
		/* the tread: on the wheel's rolling face, under it now */
		center = matrix->position;
		center.z -= 0.12f;
		gore_vehicle_mark_add(vehicle_index, wheel, &center, global_down3d, 0.09f, decal_index, 0.9f);
	}
	{
		struct collision_result collision;
		real_vector3d down = { 0.f, 0.f, -0.6f };
		real_point3d above = *point;

		above.z += 0.2f;
		if (decal_index != NONE &&
			collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
				&above, &down, NONE, &collision))
		{
			gore_blood_decal(decal_index, &collision, &down, 0.8f);
		}
	}
	if (config_boolean("game.gore_log"))
		error(_error_silent, "gore: %s ran over a body (wheel node %d)", tag_get_name(vehicle->definition_index), wheel);
}

static void gore_vehicles_update(void)
{
	struct object_iterator iterator;
	struct object_datum *vehicle;

	if (!config_boolean("game.gore_vehicle_blood") || (game_time_get() % 3) != 0)
		return;
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while ((vehicle = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long vehicle_index = iterator.index;
		real reach = vehicle->object.bounding_sphere_radius;
		struct object_iterator bodies;
		struct biped_datum *biped;
		short index;

		if (magnitude3d(&vehicle->object.translational_velocity) < 0.03f) /* about 1 m/s */
			continue;
		object_iterator_new(&bodies, _object_mask_biped, 0);
		while ((biped = (struct biped_datum *)object_iterator_next(&bodies)) != NULL)
		{
			if (!TEST_FLAG(biped->object.damage_flags, _object_dead_bit) ||
				TEST_FLAG(biped->object.flags, _object_invisible_bit) ||
				distance_squared3d(&biped->object.position, &vehicle->object.bounding_sphere_center) > reach * reach)
				continue;
			/* units that never gib (hunters) leave no blood on vehicles */
			if (gore_unit_type_get(biped->definition_index)->disabled)
				continue;
			gore_vehicle_run_over(vehicle_index, bodies.index, &biped->object.position,
				gore_vehicle_blood_decal(biped->definition_index), biped->definition_index);
		}
		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			struct gore_piece *piece = &gore_pieces[index];
			struct object_datum *object;

			if (piece->object_index == NONE ||
				!(object = object_try_and_get_and_verify_type(piece->object_index, 0xFFFFFFFF)) ||
				distance_squared3d(&object->object.bounding_sphere_center, &vehicle->object.bounding_sphere_center) > reach * reach)
				continue;
			gore_vehicle_run_over(vehicle_index, piece->object_index, &object->object.bounding_sphere_center,
				piece->decal_index, piece->unit_definition_index);
		}
	}
}

/* ---------- private code */

static boolean gore_enabled(void)
{
	if (gore_level() == _gore_level_off)
		return FALSE;
	/* campaign, multiplayer and co-op alike: the gib is decided from
	the damage itself, which the host deals and every client replays
	(a killing blow whole: damage_replay_kill calls object_cause_damage
	on each machine), so every machine gibs the same victim from the
	same synced kill. What it makes of it is local only: the pieces
	are this machine's own objects (a client's from the local upper
	half; network_objects.c never tells of them and never culls them:
	gore_object_is_piece), they collide with nothing, take no damage
	and deal none, and the victim's own end stays the network's (a
	client cannot delete the host's corpse: object_delete refuses;
	the host's delete is what removes it everywhere). Pieces need
	not look alike across machines. */
	return TRUE;
}

boolean gore_object_is_piece(long object_index)
{
	short index;

	if (object_index == NONE)
		return FALSE;
	for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
	{
		if (gore_pieces[index].object_index == object_index)
			return TRUE;
	}
	return FALSE;
}

static struct gore_unit_type const *gore_unit_type_get(long definition_index)
{
	char const *name = tag_get_name(definition_index);
	short index;

	for (index = 0; index < (short)NUMBEROF(gore_unit_types) - 1; index++)
	{
		if (name && strstr(name, gore_unit_types[index].tag_path_part))
			return &gore_unit_types[index];
	}
	return &gore_unit_types[NUMBEROF(gore_unit_types) - 1];
}

/* a Flood form (combat human or elite, carrier, infection form, by its tag
path, characters\flood...): gore leaves it alone everywhere */
static boolean gore_is_flood(long definition_index)
{
	char const *name = definition_index != NONE ? tag_get_name(definition_index) : NULL;

	return name && strstr(name, "flood") != NULL;
}

static struct damage_resistance const *gore_resistance_get(long definition_index)
{
	struct object_definition *definition = object_definition_get(definition_index);

	if (definition->object.collision_model.index == NONE)
		return NULL;
	return &collision_model_definition_get(definition->object.collision_model.index)->resistance;
}

static struct model *gore_model_get(long definition_index)
{
	struct object_definition *definition = object_definition_get(definition_index);

	if (definition->object.model.index == NONE)
		return NULL;
	return (struct model *)tag_get(MODELS_GROUP_TAG, definition->object.model.index);
}

static short gore_model_node_find(struct model *model, char const *name)
{
	short index;

	for (index = 0; index < (short)model->nodes.count; index++)
	{
		if (!strcmp(TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name, name))
			return index;
	}
	return NONE;
}

static short gore_model_region_find(struct model *model, char const *name)
{
	short index;

	for (index = 0; index < (short)MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT); index++)
	{
		if (!strcmp(TAG_BLOCK_GET_ELEMENT(&model->regions, index, struct model_region)->name, name))
			return index;
	}
	return NONE;
}

/* the regions of the part the model has; 0 if none */
static unsigned long gore_part_region_mask(struct model *model, short part)
{
	unsigned long mask = 0;
	short name_index;

	for (name_index = 0; name_index < 3 && gore_part_regions[part][name_index]; name_index++)
	{
		short region_index = gore_model_region_find(model, gore_part_regions[part][name_index]);

		if (region_index != NONE)
			mask |= 1UL << region_index;
	}
	return mask;
}

static void gore_effect_at(long effect_index, real_point3d const *position, real_vector3d const *normal)
{
	char const *marker_names[5] = { "normal", "incident", "negative incident", "reflection", "gravity" };
	real_point3d points[5];
	real_vector3d forwards[5];
	short index;

	if (effect_index == NONE)
		return;
	for (index = 0; index < 5; index++)
	{
		points[index] = *position;
		forwards[index] = *normal;
	}
	forwards[4] = *global_down3d;
	effect_new_unattached_from_markers(effect_index, NONE, global_zero_vector3d, 5, marker_names, points,
		forwards, 1.f, 0.f, NULL, NULL, FALSE);
}

/* how many of a blood event to make, for each one heavy makes: a running
credit of the level's scale, so mild makes every other one, heavy each one
once (as before the levels) and extreme each one, and every fourth twice:
exactly half and a quarter more, counted */
static short gore_blood_count(short kind)
{
	short count = 0;

	gore_blood_requested[kind]++;
	gore_blood_credit[kind] += gore_blood_scale();
	while (gore_blood_credit[kind] >= 0.999f)
	{
		gore_blood_credit[kind] -= 1.f;
		count++;
	}
	gore_blood_emitted[kind] += count;
	return count;
}

/* a blood effect, as many times as the level's blood makes of it (the
extra ones a little apart) */
static void gore_blood_effect_at(long effect_index, real_point3d const *position, real_vector3d const *normal)
{
	short count;
	short index;

	if (effect_index == NONE)
		return;
	count = gore_blood_count(_gore_blood_burst);
	for (index = 0; index < count; index++)
	{
		real_point3d point = *position;

		if (index > 0)
		{
			real_vector3d jitter;

			seed_random_direction3d(&gore_jitter_seed, &jitter);
			point_from_line3d(&point, &jitter, 0.08f, &point);
		}
		gore_effect_at(effect_index, &point, normal);
	}
}

/* a blood decal, likewise (the extra ones beside it on its surface) */
static void gore_blood_decal(long decal_index, struct collision_result const *collision,
	real_vector3d const *velocity, real radius)
{
	short count;
	short index;

	if (decal_index == NONE)
		return;
	count = gore_blood_count(_gore_blood_splat);
	for (index = 0; index < count; index++)
	{
		struct collision_result moved = *collision;

		if (index > 0)
		{
			real_vector3d jitter;
			real along;

			seed_random_direction3d(&gore_jitter_seed, &jitter);
			along = dot_product3d(&jitter, &collision->plane.n);
			jitter.i -= collision->plane.n.i * along;
			jitter.j -= collision->plane.n.j * along;
			jitter.k -= collision->plane.n.k * along;
			point_from_line3d(&moved.point, &jitter, 0.12f, &moved.point);
		}
		decal_new_from_collision(decal_index, &moved, velocity, radius, FALSE, NONE, NULL);
	}
}

static struct gore_piece *gore_piece_new(boolean chunk)
{
	long maximum = PIN(config_integer("game.gore_max_pieces"), 6, GORE_MAXIMUM_PIECES);
	short index;
	short oldest = NONE;

	/* chunks count toward game.gore_max_pieces, and have a cap of their
	own (GORE_MAXIMUM_CHUNKS): past it, the oldest chunk goes, not a piece */
	if (chunk)
	{
		short chunks = 0;
		short oldest_chunk = NONE;

		for (index = 0; index < maximum; index++)
		{
			if (gore_pieces[index].object_index == NONE || !gore_pieces[index].chunk)
				continue;
			chunks++;
			if (oldest_chunk == NONE || gore_pieces[index].age > gore_pieces[oldest_chunk].age)
				oldest_chunk = index;
		}
		if (chunks >= GORE_MAXIMUM_CHUNKS && oldest_chunk != NONE)
		{
			if (object_try_and_get_and_verify_type(gore_pieces[oldest_chunk].object_index, 0xFFFFFFFF))
				object_delete(gore_pieces[oldest_chunk].object_index);
			gore_pieces[oldest_chunk].object_index = NONE;
			return &gore_pieces[oldest_chunk];
		}
	}
	for (index = 0; index < maximum; index++)
	{
		if (gore_pieces[index].object_index == NONE)
			return &gore_pieces[index];
		if (oldest == NONE || gore_pieces[index].age > gore_pieces[oldest].age)
			oldest = index;
	}
	/* the cap: the oldest piece goes */
	if (oldest != NONE)
	{
		if (object_try_and_get_and_verify_type(gore_pieces[oldest].object_index, 0xFFFFFFFF))
			object_delete(gore_pieces[oldest].object_index);
		gore_pieces[oldest].object_index = NONE;
		return &gore_pieces[oldest];
	}
	return NULL;
}

static void gore_piece_launch(struct gore_piece *piece, real_vector3d const *direction, real force)
{
	real_vector3d random;
	real speed = PIN(force, 1.f, 3.f) * real_seed_random_range(&gore_seed, 0.05f, 0.12f); /* world units a tick */

	seed_random_direction3d(&gore_seed, &random);
	piece->velocity.i = direction->i * speed + random.i * speed * 0.5f;
	piece->velocity.j = direction->j * speed + random.j * speed * 0.5f;
	piece->velocity.k = direction->k * speed + fabsf(random.k) * speed * 0.6f + 0.05f;
	seed_random_direction3d(&gore_seed, &piece->spin);
	scale_vector3d(&piece->spin, real_seed_random_range(&gore_seed, 0.1f, 0.45f), &piece->spin);
	/* the level's explosiveness (mild: half; the same draws, so a mild
	piece flies at exactly half its heavy speed) */
	{
		real scale = gore_launch_scale();

		scale_vector3d(&piece->velocity, scale, &piece->velocity);
		scale_vector3d(&piece->spin, scale, &piece->spin);
	}
	gore_launch_speed_sum += magnitude3d(&piece->velocity) * GORE_METRES_PER_WORLD_UNIT * 30.f;
	gore_launch_spin_sum += magnitude3d(&piece->spin);
	gore_launch_count++;
}

/* a chunk's launch: about a piece's speed, scattered wider and spinning
more (its own random sequence) */
static void gore_chunk_launch(struct gore_piece *piece, real_vector3d const *direction, real force)
{
	real_vector3d random;
	real speed = PIN(force, 1.f, 3.f) * real_seed_random_range(&gore_chunk_seed, 0.04f, 0.1f);

	seed_random_direction3d(&gore_chunk_seed, &random);
	piece->velocity.i = direction->i * speed * 0.6f + random.i * speed;
	piece->velocity.j = direction->j * speed * 0.6f + random.j * speed;
	piece->velocity.k = direction->k * speed * 0.6f + fabsf(random.k) * speed * 0.8f + 0.05f;
	seed_random_direction3d(&gore_chunk_seed, &piece->spin);
	scale_vector3d(&piece->spin, real_seed_random_range(&gore_chunk_seed, 0.3f, 0.8f), &piece->spin);
}

/* a copy of the unit showing the given regions only (a chunk: launched
as one, gore_chunk_launch); the piece, or NULL */
static struct gore_piece *gore_spawn_copy(long unit_index, unsigned long region_mask, short pivot_node,
	signed char const *collapse, real_vector3d const *direction, real force, long blood_effect_index, long decal_index,
	boolean chunk)
{
	struct object_datum *unit = object_get(unit_index);
	struct object_placement_data placement;
	struct gore_piece *piece;
	struct object_datum *object;
	real_matrix4x3 *source_matrices;
	real_matrix4x3 *matrices;
	short node_count;
	short region_index;
	short node_index;
	long object_index;

	object_placement_data_new(&placement, unit->definition_index, NONE);
	placement.position = unit->object.position;
	placement.forward = unit->object.forward;
	placement.up = unit->object.up;
	placement.variant_number = unit->object.variant_number;
	csmemcpy(placement.change_colors, unit->object.base_change_colors, sizeof(placement.change_colors));
	object_index = object_new(&placement);
	if (object_index == NONE)
		return NULL;
	piece = gore_piece_new(chunk);
	if (!piece)
	{
		object_delete(object_index);
		return NULL;
	}
	unit = object_get(unit_index);
	object = object_get(object_index);

	node_count = (short)MIN(unit->object.node_matrices.size, object->object.node_matrices.size) /
		(short)sizeof(real_matrix4x3);
	node_count = MIN(node_count, GORE_MAXIMUM_NODES);
	source_matrices = object_get_node_matrices(unit_index);
	matrices = object_get_node_matrices(object_index);
	for (node_index = 0; node_index < node_count; node_index++)
	{
		matrices[node_index] = source_matrices[node_index];
		piece->base_matrices[node_index] = source_matrices[node_index];
	}
	piece->node_count = node_count;
	for (node_index = 0; node_index < node_count; node_index++)
		piece->collapse[node_index] = collapse ? collapse[node_index] : NONE;

	for (region_index = 0; region_index < MAXIMUM_REGIONS_PER_OBJECT; region_index++)
	{
		object->object.region_permutations[region_index] = TEST_FLAG(region_mask, region_index) ?
			unit->object.region_permutations[region_index] : (byte)NONE;
	}
	/* the unit's look, not a fresh random one: its change colours (a red
	grunt's pack stays red), its regions' damage, its shader permutation */
	csmemcpy(object->object.base_change_colors, unit->object.base_change_colors, sizeof(object->object.base_change_colors));
	csmemcpy(object->object.outgoing_change_colors, unit->object.outgoing_change_colors, sizeof(object->object.outgoing_change_colors));
	csmemcpy(object->object.region_damage, unit->object.region_damage, sizeof(object->object.region_damage));
	object->object.regions_destroyed_flags = unit->object.regions_destroyed_flags;
	object->object.forced_shader_permutation_index = unit->object.forced_shader_permutation_index;
	object->object.variant_number = unit->object.variant_number;
	object->object.owner_team_index = unit->object.owner_team_index;
	SET_FLAG(object->object.flags, _object_do_not_recompute_node_matrices_bit, TRUE);
	SET_FLAG(object->object.flags, _object_no_collisions_bit, TRUE);
	SET_FLAG(object->object.flags, _object_cannot_be_garbage_bit, TRUE);
	SET_FLAG(object->object.damage_flags, _object_dead_bit, TRUE);
	SET_FLAG(object->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
	object->object.body_vitality = 0.f;
	{
		/* no limp body: its relaxing would move the nodes this moves */
		struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);

		if (biped)
		{
			SET_FLAG(biped->biped.flags, _biped_limp_body_physics_active_bit, TRUE);
			biped->biped.limp_body_current_relaxation_iterations = 0;
			biped->biped.limp_body_max_relaxation_iterations = 0;
		}
	}

	csmemset(piece->rotation, 0, sizeof(piece->rotation));
	piece->rotation[0][0] = piece->rotation[1][1] = piece->rotation[2][2] = 1.f;
	if (pivot_node == NONE || pivot_node >= node_count)
		pivot_node = 0;
	piece->pivot0 = matrices[pivot_node].position;
	piece->position = piece->pivot0;
	piece->object_index = object_index;
	piece->garbage = FALSE;
	piece->hunter = FALSE;
	piece->chunk = chunk;
	piece->age = 0;
	piece->life = (long)(PIN(config_real("game.gore_piece_seconds"), 1.0, 3600.0) * 30.0);
	piece->resting = FALSE;
	piece->impacted = FALSE;
	piece->blood_effect_index = blood_effect_index;
	piece->decal_index = decal_index;
	piece->unit_definition_index = gore_piece_unit_definition;
	if (chunk)
	{
		gore_chunk_launch(piece, direction, force);
		return piece;
	}
	gore_piece_launch(piece, direction, force);
	/* the cut's burst */
	gore_blood_effect_at(blood_effect_index, &piece->pivot0, direction);
	return piece;
}

static void gore_spawn_garbage(char const *name, real_point3d const *position,
	real_vector3d const *direction, real force, long blood_effect_index, long decal_index)
{
	long definition_index = tag_loaded(GARBAGE_TAG, name);
	struct object_placement_data placement;
	struct gore_piece *piece;
	long object_index;

	if (definition_index == NONE)
		return;
	piece = gore_piece_new(FALSE);
	if (!piece)
		return;
	gore_piece_launch(piece, direction, force);
	object_placement_data_new(&placement, definition_index, NONE);
	placement.position = *position;
	placement.translational_velocity = piece->velocity;
	placement.angular_velocity = piece->spin;
	object_index = object_new(&placement);
	if (object_index == NONE)
		return;
	piece->object_index = object_index;
	piece->garbage = TRUE;
	piece->hunter = FALSE;
	piece->chunk = FALSE;
	piece->age = 0;
	piece->life = (long)(PIN(config_real("game.gore_piece_seconds"), 1.0, 3600.0) * 30.0);
	piece->resting = FALSE;
	piece->impacted = FALSE;
	piece->blood_effect_index = blood_effect_index;
	piece->decal_index = decal_index;
	piece->unit_definition_index = gore_piece_unit_definition;
	piece->node_count = 0;
	piece->position = *position;
}

/* true left and right limbs for any model with the stock bip01 skeleton:
six copies of the whole unit, each drawing the nodes of its piece only
(the others' vertices collapse to the cut, gore_piece_pose) */
static void gore_mark_subtree(struct model *model, short node_index, signed char *in, signed char value)
{
	while (node_index != NONE && node_index < (short)model->nodes.count && node_index < GORE_MAXIMUM_NODES)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

		in[node_index] = value;
		gore_mark_subtree(model, node->first_child_node_index, in, value);
		node_index = node->next_sibling_node_index;
	}
}

static boolean gore_split_by_nodes(long unit_index, struct model *model, struct gore_pending const *pending,
	long blood_effect_index, long decal_index)
{
	static char const *const roots[] =
	{
		"bip01 head", "bip01 l upperarm", "bip01 r upperarm", "bip01 l thigh", "bip01 r thigh",
	};
	short root_nodes[NUMBEROF(roots)];
	short node_count = (short)MIN(model->nodes.count, GORE_MAXIMUM_NODES);
	signed char owner[GORE_MAXIMUM_NODES]; /* which piece each node is in: 0 the torso, 1.. the roots */
	short index;
	short piece;

	for (index = 0; index < (short)NUMBEROF(roots); index++)
	{
		root_nodes[index] = gore_model_node_find(model, roots[index]);
		if (root_nodes[index] == NONE || root_nodes[index] >= node_count)
			return FALSE;
	}
	csmemset(owner, 0, sizeof(owner));
	for (index = 0; index < (short)NUMBEROF(roots); index++)
	{
		struct model_node const *root = TAG_BLOCK_GET_ELEMENT(&model->nodes, root_nodes[index], struct model_node);

		owner[root_nodes[index]] = (signed char)(index + 1);
		gore_mark_subtree(model, root->first_child_node_index, owner, (signed char)(index + 1));
	}
	for (piece = 0; piece <= (short)NUMBEROF(roots); piece++)
	{
		signed char collapse[GORE_MAXIMUM_NODES];

		/* (a sniper took its head already) */
		if (piece == 1 && gore_headless_find(unit_index))
			continue;
		short pivot = piece ? root_nodes[piece - 1] : gore_model_node_find(model, "bip01 spine");

		for (index = 0; index < GORE_MAXIMUM_NODES; index++)
		{
			if (index >= node_count || owner[index] == piece)
			{
				collapse[index] = NONE;
			}
			else if (piece)
			{
				/* a limb: the rest of the body shrinks to the limb's joint */
				collapse[index] = (signed char)root_nodes[piece - 1];
			}
			else
			{
				/* the torso: each limb shrinks to its own joint */
				collapse[index] = (signed char)root_nodes[owner[index] - 1];
			}
		}
		gore_spawn_copy(unit_index, 0xFFFFFFFF, pivot, collapse, &pending->direction, pending->force,
			blood_effect_index, decal_index, FALSE);
	}
	return TRUE;
}

/* ---------- Extreme's chunks

Small meaty bits of the victim, beside its pieces: each a copy of the unit
(as the pieces are, gore_spawn_copy, so the same local, uncollidable,
never-networked objects: gore_object_is_piece) drawing the mesh of one of
its nodes only, every other node collapsed to that node's joint, and that
node shrunk to half or two thirds: a lump of forearm, calf, spine or the
like (any model: the nodes its geometry's parts centre on). The renderer
tints the whole of it in the victim's blood colour (gore_chunk_color;
gore_render_part_blood). They scatter wider and spin more than the pieces,
draw no blood of their own (the level's blood is the pieces', counted),
and are gone in a minute at most. */

/* the model's geometry, as models.c has it (there file-local): the node
each part centres on, which has mesh */
struct gore_model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct gore_model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	byte rest[0x68 - 10];
};

typedef char verify_gore_model_geometry_part_size[sizeof(struct gore_model_geometry_part) == 0x68 ? 1 : -1];

#define GORE_CHUNKS_PER_GIB 6
#define GORE_CHUNKS_PER_HUNTER_GIB 8

static short gore_spawn_chunks(long unit_index, struct model *model, struct gore_pending const *pending)
{
	struct object_datum *unit = object_get(unit_index);
	short node_count = (short)MIN(MIN(model->nodes.count, GORE_MAXIMUM_NODES),
		unit->object.node_matrices.size / (long)sizeof(real_matrix4x3));
	short candidates[GORE_MAXIMUM_NODES];
	short candidate_count = 0;
	short wanted = pending->hunter ? GORE_CHUNKS_PER_HUNTER_GIB : GORE_CHUNKS_PER_GIB;
	short made = 0;
	short geometry_index;
	short chunk;

	for (geometry_index = 0; geometry_index < (short)model->geometries.count; geometry_index++)
	{
		struct gore_model_geometry *geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index,
			struct gore_model_geometry);
		short part_index;

		for (part_index = 0; part_index < (short)geometry->parts.count; part_index++)
		{
			short node = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index, struct gore_model_geometry_part)->centroid_primary_node_index;
			short known;

			/* (not the root: the whole body's middle) */
			if (node <= 0 || node >= node_count)
				continue;
			for (known = 0; known < candidate_count && candidates[known] != node; known++)
				;
			if (known == candidate_count)
				candidates[candidate_count++] = node;
		}
	}
	if (!candidate_count)
		return 0;
	for (chunk = 0; chunk < wanted; chunk++)
	{
		signed char collapse[GORE_MAXIMUM_NODES];
		short pick = (short)(real_seed_random_range(&gore_chunk_seed, 0.f, (real)candidate_count - 0.001f));
		short node = candidates[PIN(pick, 0, candidate_count - 1)];
		struct gore_piece *piece;
		short index;

		for (index = 0; index < GORE_MAXIMUM_NODES; index++)
			collapse[index] = index == node ? NONE : (signed char)node;
		piece = gore_spawn_copy(unit_index, 0xFFFFFFFF, node, collapse, &pending->direction, pending->force,
			NONE, NONE, TRUE);
		if (!piece)
			continue;
		piece->base_matrices[node].scale *= real_seed_random_range(&gore_chunk_seed, 0.5f, 0.7f);
		piece->life = MIN(piece->life, 60 * 30);
		made++;
	}
	return made;
}

static void gore_unit_gib(struct gore_pending const *pending)
{
	long unit_index = pending->unit_index;
	struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct gore_unit_type const *type;
	struct damage_resistance const *resistance;
	struct model *model;
	long blood_effect_index = NONE;
	long trail_effect_index = NONE;
	long decal_index;
	unsigned long used = 0;
	short parts = 0;
	short part;
	real_point3d center;

	if (!unit)
		return;
	type = gore_unit_type_get(unit->definition_index);
	/* (the Flood: never, whoever asked; the hunter only by its tank gib) */
	if (gore_is_flood(unit->definition_index) || (type->disabled && !pending->hunter))
	{
		error(_error_silent, "gore: %s never comes apart: its own death", tag_get_name(unit->definition_index));
		return;
	}
	gore_launch_speed_sum = 0.f;
	gore_launch_spin_sum = 0.f;
	gore_launch_count = 0;
	gore_piece_unit_definition = unit->definition_index;
	model = gore_model_get(unit->definition_index);
	resistance = gore_resistance_get(unit->definition_index);
	if (resistance)
	{
		blood_effect_index = resistance->area_damage_effect.index != NONE ?
			resistance->area_damage_effect.index : resistance->body_destroyed_effect.index;
		trail_effect_index = resistance->localized_damage_effect.index;
	}
	decal_index = type->decal_name ? tag_loaded(DECAL_TAG, type->decal_name) : NONE;
	if (decal_index == NONE)
		decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat");
	if (pending->hunter)
	{
		/* a hunter's own orange splat if the map has one (both spellings
		tried, as stock maps differ); never the red human splat, and never
		a NULL name (tag_loaded on one crashed here before): with neither,
		the hunter's own blood effects still give the colour, decal-less */
		decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood hunter splat");
		if (decal_index == NONE)
			decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\hunter blood splat");
		if (config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] hunter tank gib: model %ld nodes, %ld regions, splat decal %s",
				model ? (long)model->nodes.count : 0L, model ? (long)model->regions.count : 0L,
				decal_index != NONE ? "found" : "none on this map");
	}
	center = unit->object.bounding_sphere_center;

	if (model && unit->object.node_matrices.size > 0 && gore_split_by_nodes(unit_index, model, pending,
		blood_effect_index, decal_index))
	{
		parts = NUMBER_OF_GORE_PARTS;
	}
	else if (model && unit->object.node_matrices.size > 0)
	{
		/* each part that has its own regions; what is left goes with the torso */
		for (part = 0; part < NUMBER_OF_GORE_PARTS; part++)
		{
			unsigned long mask;

			if (part == _gore_part_torso)
			{
				mask = 0;
				{
					short region_index;

					for (region_index = 0; region_index < (short)MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT); region_index++)
					{
						if (!TEST_FLAG(used, region_index))
							mask |= 1UL << region_index;
					}
				}
				if (!parts)
					break; /* a unit of one piece: it is not split */
			}
			else
			{
				mask = gore_part_region_mask(model, part) & ~used;
			}
			if (!mask)
				continue;
			used |= mask;
			parts++;
			gore_spawn_copy(unit_index, mask, gore_model_node_find(model, gore_part_pivots[part]), NULL,
				&pending->direction, pending->force, blood_effect_index, decal_index, FALSE);
		}
	}
	if (parts < 2)
	{
		/* not split by regions: the map's Flood bits, if it has them */
		static char const *const bits[] =
		{
			"characters\\floodcombat_human\\flood_bit_limbs\\flood_bit_limbs",
			"characters\\floodcombat_human\\flood_bit_limbs\\flood_bit_limbs",
			"characters\\floodcombat_human\\flood_bit_limbs\\flood_bit_limbs",
			"characters\\floodcombat_human\\flood_bit_limbs\\flood_bit_limbs",
			"characters\\floodcombat_human\\flood_bit_innards\\flood_bit_innards",
			"characters\\floodcombat_human\\flood_bit_skin\\flood_bit_skin",
		};
		short bit;

		if (tag_loaded(GARBAGE_TAG, bits[0]) == NONE)
		{
			/* no bits on this map: blood only, and the unit stays a corpse */
			gore_blood_effect_at(blood_effect_index, &center, &pending->direction);
			error(_error_silent, "gore: %s has no regions to split and the map no Flood bits: blood only",
				tag_get_name(unit->definition_index));
			return;
		}
		for (bit = 0; bit < (short)NUMBEROF(bits); bit++)
			gore_spawn_garbage(bits[bit], &center, &pending->direction, pending->force, trail_effect_index, decal_index);
		gore_blood_effect_at(blood_effect_index, &center, &pending->direction);
	}

	/* a hunter carries a lot of blood: three more bursts about its centre
	(the split already bursts once a piece), ground splats at once, and its
	pieces marked for longer trails and bigger landing splats below */
	if (pending->hunter)
	{
		short burst;
		short splat;
		short made = 0;

		for (burst = 0; burst < 3; burst++)
		{
			real_point3d point = center;
			real_vector3d jitter;

			seed_random_direction3d(&gore_seed, &jitter);
			point.x += jitter.i * unit->object.bounding_sphere_radius * 0.6f;
			point.y += jitter.j * unit->object.bounding_sphere_radius * 0.6f;
			point.z += jitter.k * unit->object.bounding_sphere_radius * 0.3f;
			gore_blood_effect_at(blood_effect_index, &point, &pending->direction);
		}
		if (decal_index != NONE)
		{
			for (splat = 0; splat < 5; splat++)
			{
				struct collision_result collision;
				real_point3d from = center;
				real_vector3d down = { 0.f, 0.f, -1.5f };
				real_vector3d jitter;

				seed_random_direction3d(&gore_seed, &jitter);
				from.x += jitter.i * unit->object.bounding_sphere_radius * 0.9f;
				from.y += jitter.j * unit->object.bounding_sphere_radius * 0.9f;
				from.z += 0.5f;
				if (collision_test_vector(_collision_test_environment_flags, &from, &down, unit_index, &collision))
				{
					gore_blood_decal(decal_index, &collision, &down, 1.4f);
					made++;
				}
			}
		}
		if (config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] hunter tank gib: 3 extra blood bursts, %d ground splats at the split", made);
	}

	/* the trail effect for the region copies */
	{
		short index;

		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index != NONE && gore_pieces[index].age == 0 && !gore_pieces[index].garbage &&
				!gore_pieces[index].chunk)
				gore_pieces[index].blood_effect_index = trail_effect_index != NONE ? trail_effect_index : blood_effect_index;
			if (pending->hunter && gore_pieces[index].object_index != NONE && gore_pieces[index].age == 0)
				gore_pieces[index].hunter = TRUE;
		}
	}
	{
		short index;
		short count = 0;
		short chunks = 0;
		short launches = gore_launch_count;
		real speed = launches ? gore_launch_speed_sum / launches : 0.f;
		real spin = launches ? gore_launch_spin_sum / launches : 0.f;

		/* Extreme: the chunks, after the pieces (whose launches the
		log's means are) */
		if (gore_level() == _gore_level_extreme && model && unit->object.node_matrices.size > 0 && parts >= 2)
			gore_spawn_chunks(unit_index, model, pending);
		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index != NONE && gore_pieces[index].age == 0)
			{
				if (gore_pieces[index].chunk)
					chunks++;
				else
					count++;
			}
		}
		error(_error_silent, "gore: %s came apart into %d pieces (%d split), force %.2f",
			tag_get_name(unit->definition_index), count, parts, pending->force);
		error(_error_silent, "gore: level %s: %d pieces + %d chunks, launch mean %.3f m/s, spin mean %.4f rad/tick (%d launches); blood so far: bursts %ld of %ld, splats %ld of %ld",
			gore_level_names[gore_level()], count, chunks, speed, spin, launches,
			gore_blood_emitted[_gore_blood_burst], gore_blood_requested[_gore_blood_burst],
			gore_blood_emitted[_gore_blood_splat], gore_blood_requested[_gore_blood_splat]);
	}
	if (pending->player || ((struct biped_datum *)object_get(unit_index))->unit.player_index != NONE)
	{
		/* a player's unit stays (hidden, untouchable): the death camera, the
		revert to the checkpoint and the respawn all go on with it */
		object_set_visibility(unit_index, FALSE);
		SET_FLAG(object_get(unit_index)->object.flags, _object_no_collisions_bit, TRUE);
		error(_error_silent, "gore: the player's unit is hidden, not deleted");
		return;
	}
	object_delete(unit_index);
}

static void gore_rotate(real rotation[3][3], real_vector3d const *spin)
{
	real angle = magnitude3d(spin);
	real_vector3d axis;
	real c, s, t;
	real r[3][3];
	real result[3][3];
	short i, j, k;

	if (angle < 1e-5f)
		return;
	scale_vector3d(spin, 1.f / angle, &axis);
	c = cosf(angle); s = sinf(angle); t = 1.f - c;
	r[0][0] = t*axis.i*axis.i + c; r[0][1] = t*axis.i*axis.j - s*axis.k; r[0][2] = t*axis.i*axis.k + s*axis.j;
	r[1][0] = t*axis.i*axis.j + s*axis.k; r[1][1] = t*axis.j*axis.j + c; r[1][2] = t*axis.j*axis.k - s*axis.i;
	r[2][0] = t*axis.i*axis.k - s*axis.j; r[2][1] = t*axis.j*axis.k + s*axis.i; r[2][2] = t*axis.k*axis.k + c;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
		{
			result[i][j] = 0.f;
			for (k = 0; k < 3; k++)
				result[i][j] += r[i][k] * rotation[k][j];
		}
	csmemcpy(rotation, result, sizeof(result));
}

static void gore_apply(real const rotation[3][3], real_vector3d const *v, real_vector3d *result)
{
	real_vector3d out;

	out.i = rotation[0][0]*v->i + rotation[0][1]*v->j + rotation[0][2]*v->k;
	out.j = rotation[1][0]*v->i + rotation[1][1]*v->j + rotation[1][2]*v->k;
	out.k = rotation[2][0]*v->i + rotation[2][1]*v->j + rotation[2][2]*v->k;
	*result = out;
}

static void gore_piece_pose(struct gore_piece *piece)
{
	real_matrix4x3 *matrices = object_get_node_matrices(piece->object_index);
	short node_index;
	real_vector3d forward, up;

	gore_apply(piece->rotation, &object_get(piece->object_index)->object.forward, &forward);
	for (node_index = 0; node_index < piece->node_count; node_index++)
	{
		real_matrix4x3 const *base = &piece->base_matrices[node_index];
		real_matrix4x3 *matrix = &matrices[node_index];
		real_vector3d offset;

		if (piece->collapse[node_index] != NONE)
		{
			/* outside the piece: its vertices shrink to the cut (those
			weighted to both sides stretch into a stump) */
			vector_from_points3d(&piece->pivot0, &piece->base_matrices[(short)piece->collapse[node_index]].position, &offset);
			gore_apply(piece->rotation, &offset, &offset);
			point_from_line3d(&piece->position, &offset, 1.f, &matrix->position);
			matrix->forward = *global_zero_vector3d;
			matrix->left = *global_zero_vector3d;
			matrix->up = *global_zero_vector3d;
			matrix->scale = base->scale;
			continue;
		}
		vector_from_points3d(&piece->pivot0, &base->position, &offset);
		gore_apply(piece->rotation, &offset, &offset);
		point_from_line3d(&piece->position, &offset, 1.f, &matrix->position);
		gore_apply(piece->rotation, &base->forward, &matrix->forward);
		gore_apply(piece->rotation, &base->left, &matrix->left);
		gore_apply(piece->rotation, &base->up, &matrix->up);
		matrix->scale = base->scale;
	}
	(void)forward; (void)up;
}

static void gore_piece_update(struct gore_piece *piece)
{
	struct object_datum *object = object_try_and_get_and_verify_type(piece->object_index, 0xFFFFFFFF);

	if (!object)
	{
		if (config_boolean("game.gore_log"))
			error(_error_silent, "gore: a piece (age %ld s) was deleted by the game", piece->age / 30);
		piece->object_index = NONE;
		return;
	}
	piece->age++;
	/* far from the player for a while: gone sooner (every second) */
	if (piece->age > 300 && (piece->age % 30) == 0 && gore_far_from_players(&object->object.position))
		piece->age = piece->life;
	if (piece->age >= piece->life)
	{
		object_delete(piece->object_index);
		piece->object_index = NONE;
		return;
	}
	if (piece->garbage)
	{
		/* its own physics; blood drops while it flies, a splat where it lands */
		if (piece->age == 2)
			SET_FLAG(object->object.flags, _object_no_collisions_bit, FALSE);
		if (!piece->impacted && piece->age > 3 && magnitude3d(&object->object.translational_velocity) < 0.02f)
		{
			struct collision_result collision;
			real_vector3d down = { 0.f, 0.f, -0.5f };

			piece->impacted = TRUE;
			if (piece->decal_index != NONE &&
				collision_test_vector(_collision_test_environment_flags, &object->object.position, &down, piece->object_index, &collision))
			{
				gore_blood_decal(piece->decal_index, &collision, &down, piece->hunter ? 1.4f : 1.f);
			}
		}
		else if (!piece->impacted && piece->age <= (piece->hunter ? 90 : 36) && (piece->age % (piece->hunter ? 6 : 12)) == 0)
		{
			gore_blood_effect_at(piece->blood_effect_index, &object->object.position, global_down3d);
		}
		return;
	}

	if (!piece->resting)
	{
		real_vector3d step = piece->velocity;

		piece->velocity.k -= GORE_GRAVITY;
		/* collision from the third tick: the copies start inside each other
		and the body */
		if (piece->age >= 3)
		{
			struct collision_result collision;

			if (collision_test_vector(GORE_PIECE_COLLISION_FLAGS, &piece->position, &step, piece->object_index, &collision))
			{
				real into = dot_product3d(&piece->velocity, &collision.plane.n);

				piece->position = collision.point;
				point_from_line3d(&piece->position, &collision.plane.n, 0.03f, &piece->position);
				/* bounce, lose most of it */
				point_from_line3d((real_point3d *)&piece->velocity, &collision.plane.n, -1.6f*into,
					(real_point3d *)&piece->velocity);
				scale_vector3d(&piece->velocity, 0.35f, &piece->velocity);
				scale_vector3d(&piece->spin, 0.5f, &piece->spin);
				if (!piece->impacted)
				{
					if (!piece->chunk)
					error(_error_silent, "gore: piece %ld landed at tick %d (%.2f %.2f %.2f)", piece->object_index,
						piece->age, piece->position.x, piece->position.y, piece->position.z);
					piece->impacted = TRUE;
					if (piece->decal_index != NONE)
						gore_blood_decal(piece->decal_index, &collision, &step, piece->hunter ? 1.3f : 0.6f);
					gore_blood_effect_at(piece->blood_effect_index, &collision.point, &collision.plane.n);
				}
				if (magnitude3d(&piece->velocity) < 0.02f && collision.plane.n.k > 0.5f)
					piece->resting = TRUE;
			}
			else
			{
				point_from_line3d(&piece->position, &step, 1.f, &piece->position);
			}
		}
		else
		{
			point_from_line3d(&piece->position, &step, 1.f, &piece->position);
		}
		gore_rotate(piece->rotation, &piece->spin);
		if (!piece->impacted && piece->age <= (piece->hunter ? 90 : 36) && (piece->age % (piece->hunter ? 6 : 12)) == 0)
			gore_blood_effect_at(piece->blood_effect_index, &piece->position, global_down3d);

		/* the object follows the piece (its clusters, its bounds) */
		{
			real_point3d origin;
			real_vector3d offset;

			vector_from_points3d(&piece->pivot0, &piece->base_matrices[0].position, &offset);
			gore_apply(piece->rotation, &offset, &offset);
			point_from_line3d(&piece->position, &offset, 1.f, &origin);
			object_set_position(piece->object_index, &origin, NULL, NULL);
		}
	}
	object = object_get(piece->object_index);
	object->object.translational_velocity = *global_zero_vector3d;
	gore_piece_pose(piece);
}

static short gore_test_count_infection(void)
{
	struct object_iterator iterator;
	struct biped_datum *biped;
	short count = 0;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (strstr(tag_get_name(biped->definition_index), "infection") &&
			!TEST_FLAG(biped->object.damage_flags, _object_dead_bit))
			count++;
	}
	return count;
}

static void gore_test_update(void)
{
	real seconds = (real)config_real("debug.gore_test");
	struct object_iterator iterator;
	struct biped_datum *biped;
	struct object_datum *player = NULL;
	long best = NONE;
	real best_distance = 0.f;

	static real const sweep[] = { 1.f, 1.5f, 2.f, 2.5f, 3.f };
	real test_distance = (real)config_real("debug.gore_test_distance");
	short sweep_count = test_distance < 0.f ? (short)NUMBEROF(sweep) : 1;
	real vehicle_speed = (real)config_real("debug.gore_test_vehicle"); /* metres a second */
	static long test_vehicle_index = NONE;
	long player_object_index = NONE;
	static real_vector3d test_vehicle_side;

	if (vehicle_speed > 0.f)
		sweep_count = 2;
	/* six seconds after the test's hit: what came of it */
	if (gore_test_gib_time != NONE && game_time_get() == gore_test_gib_time + 180)
	{
		short index;
		short pieces = 0;
		short chunks = 0;
		short landed = 0;

		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index == NONE)
				continue;
			if (gore_pieces[index].chunk)
				chunks++;
			else
				pieces++;
			if (gore_pieces[index].impacted)
				landed++;
		}
		{
			struct object_datum *target = object_try_and_get_and_verify_type(gore_test_target, _object_mask_biped);

			error(_error_silent, "gore: test summary: the unit is %s; Flood infection forms %d (%d at the hit)",
				!target ? "gone (deleted)" :
				TEST_FLAG(target->object.flags, _object_invisible_bit) ? "hidden" :
				TEST_FLAG(target->object.damage_flags, _object_dead_bit) ? "a corpse (its stock death)" : "alive",
				gore_test_count_infection(), gore_test_infection_before);
		}
		error(_error_silent, "gore: test summary (level %s, 6 s on): %d pieces + %d chunks alive (%d landed); blood bursts %ld made of %ld (%.3f), splats %ld made of %ld (%.3f)",
			gore_level_names[gore_level()], pieces, chunks, landed,
			gore_blood_emitted[_gore_blood_burst], gore_blood_requested[_gore_blood_burst],
			gore_blood_requested[_gore_blood_burst] ? (real)gore_blood_emitted[_gore_blood_burst] / gore_blood_requested[_gore_blood_burst] : 0.f,
			gore_blood_emitted[_gore_blood_splat], gore_blood_requested[_gore_blood_splat],
			gore_blood_requested[_gore_blood_splat] ? (real)gore_blood_emitted[_gore_blood_splat] / gore_blood_requested[_gore_blood_splat] : 0.f);
	}
	if (seconds <= 0.f || gore_test_done >= sweep_count ||
		game_time_get() < (long)((seconds + 4.f * gore_test_done) * 30.f))
		return;
	if (test_distance < 0.f)
		test_distance = sweep[gore_test_done];
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (biped->unit.player_index != NONE)
		{
			player = object_get(iterator.index);
			player_object_index = iterator.index;
		}
	}
	if (!player)
		return;
	if (vehicle_speed > 0.f && gore_test_done == 1)
	{
		/* the second pass: the warthog back the other way, slowly, over
		what is left */
		struct object_datum *vehicle = object_try_and_get_and_verify_type(test_vehicle_index, _object_mask_vehicle);
		struct unit_datum *player_unit = (struct unit_datum *)player;
		real_vector3d forward = player_unit->unit.looking_vector;
		real_point3d position;
		real_vector3d back;

		gore_test_done++;
		if (!vehicle)
			return;
		forward.k = 0.f;
		normalize3d(&forward);
		/* (farther out than the remains, to be in view) */
		point_from_line3d(&player->object.position, &forward, 3.5f, &position);
		position.z += 0.8f;
		back = test_vehicle_side;
		object_set_position(test_vehicle_index, &position, &back, global_up3d);
		/* parked, side on, to be seen */
		vehicle->object.translational_velocity = *global_zero_vector3d;
		vehicle->object.angular_velocity = *global_zero_vector3d;
		error(_error_silent, "gore: test: the warthog parked in view");
		/* and a big mark on its hood, to see that marks draw and ride */
		{
			real_point3d top = vehicle->object.bounding_sphere_center;
			struct collision_result collision;
			real_vector3d down = { 0.f, 0.f, -2.f };

			top.z += 1.f;
			if (collision_test_vector(FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_vehicles_bit),
					&top, &down, NONE, &collision) && collision.object_index == test_vehicle_index)
			{
				gore_vehicle_mark_add(test_vehicle_index, collision.node_index != NONE ? collision.node_index : 0,
					&collision.point, &collision.plane.n, 0.35f,
					tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat"), 1.f);
				error(_error_silent, "gore: test: a big mark on the hood (node %d)", collision.node_index);
			}
		}
		return;
	}
	/* debug.gore_test_unit "player": the player's own unit, its shield
	down and its body nearly gone, to see the player come apart */
	if (!strcmp(config_string("debug.gore_test_unit"), "player"))
	{
		best = player_object_index;
		player->object.shield_vitality = 0.f;
		player->object.body_vitality = 0.05f;
	}
	else
	{
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real distance;

		if (biped->unit.player_index != NONE || TEST_FLAG(biped->object.damage_flags, _object_dead_bit))
			continue;
		if (*config_string("debug.gore_test_unit") &&
			!strstr(tag_get_name(biped->definition_index), config_string("debug.gore_test_unit")))
			continue;
		distance = distance_squared3d(&biped->object.position, &player->object.position);
		/* one in front of the player (in view), when there is one */
		{
			real_vector3d to;
			struct unit_datum *player_unit = (struct unit_datum *)player;

			vector_from_points3d(&player->object.position, &biped->object.position, &to);
			normalize3d(&to);
			if (dot_product3d(&to, &player_unit->unit.looking_vector) < 0.7f)
				distance += 10000.f;
		}
		if (best == NONE || distance < best_distance)
		{
			best = iterator.index;
			best_distance = distance;
		}
	}
	}
	/* none on the map yet: one of the unit's kind made, when the map has
	it (debug.gore_test_unit "floodcarrier": the map's carrier) */
	if (best == NONE && *config_string("debug.gore_test_unit"))
	{
		struct tag_iterator tags;
		long definition_index;

		tag_iterator_new(&tags, 'bipd');
		while ((definition_index = tag_iterator_next(&tags)) != NONE)
		{
			if (strstr(tag_get_name(definition_index), config_string("debug.gore_test_unit")))
			{
				struct object_placement_data placement;
				real_vector3d forward = ((struct unit_datum *)player)->unit.looking_vector;

				forward.k = 0.f;
				normalize3d(&forward);
				object_placement_data_new(&placement, definition_index, NONE);
				point_from_line3d(&player->object.position, &forward, 1.2f, &placement.position);
				placement.position.z += 0.1f;
				gore_test_made = object_new(&placement);
				error(_error_silent, "gore: test: made a %s (none on the map); the test's hit is next tick",
					tag_get_name(definition_index));
				/* (a tick for it to settle into the world: a blast the tick it
				is made misses it) */
				return;
			}
		}
	}
	gore_test_done++;
	if (best != NONE)
	{
		struct damage_data damage;

		gore_test_gib_time = game_time_get();
		gore_test_target = best;
		/* (a unit the test made: nearly dead, so the test's frag or
		warthog kills it through the stock damage path, as in play) */
		if (best == gore_test_made)
		{
			object_get(best)->object.body_vitality = 0.05f;
			object_get(best)->object.shield_vitality = 0.f;
		}
		gore_test_infection_before = gore_test_count_infection();
		error(_error_silent, "gore: test gib of %s", tag_get_name(object_get(best)->definition_index));
		/* in front of the player, to see it */
		if (best != player_object_index)
		{
			struct unit_datum *player_unit = (struct unit_datum *)player;
			real_point3d position;
			real_vector3d forward = player_unit->unit.looking_vector;

			forward.k = 0.f;
			normalize3d(&forward);
			point_from_line3d(&player->object.position, &forward, 1.2f, &position);
			object_set_position(best, &position, NULL, NULL);
			object_compute_node_matrices_recursive(best);
		}
		if (config_boolean("debug.gore_test_sniper"))
		{
			/* a sniper rifle bullet's damage on the unit's head material,
			along the player's view */
			long bullet = tag_loaded('jpt!', "weapons\\sniper rifle\\sniper bullet");
			struct damage_resistance const *resistance = gore_resistance_get(object_get(best)->definition_index);
			struct model *model = gore_model_get(object_get(best)->definition_index);
			short head_material = NONE;
			short head_node = model ? gore_model_node_find(model, "bip01 head") : NONE;
			real_vector3d normal;
			short material;

			if (bullet == NONE || !resistance)
			{
				error(_error_silent, "gore: test: no sniper rifle bullet on this map");
				return;
			}
			for (material = 0; material < (short)resistance->materials.count; material++)
			{
				if (TEST_FLAG(TAG_BLOCK_GET_ELEMENT(&resistance->materials, material, struct damage_resistance_material)->flags,
						0 /* damage.c _damage_material_head_bit */))
				{
					head_material = material;
					break;
				}
			}
			damage_data_new(&damage, bullet);
			/* from the side, to see the head go */
			damage.direction.i = -((struct unit_datum *)player)->unit.looking_vector.j;
			damage.direction.j = ((struct unit_datum *)player)->unit.looking_vector.i;
			damage.direction.k = 0.1f;
			normalize3d(&damage.direction);
			damage.origin = player->object.position;
			damage.epicenter = object_get(best)->object.bounding_sphere_center;
			scenario_location_from_point(&damage.location, &damage.epicenter);
			scale_vector3d(&damage.direction, -1.f, &normal);
			error(_error_silent, "gore: test: a sniper bullet on the head (material %d, node %d) of %s",
				head_material, head_node, tag_get_name(object_get(best)->definition_index));
			object_cause_damage(&damage, best, head_node, NONE, head_material, &normal);
			return;
		}
		if (vehicle_speed > 0.f)
		{
			/* a warthog from the side, across in front of the player, at
			this speed */
			long warthog = tag_loaded(VEHICLE_TAG, "vehicles\\warthog\\warthog");
			struct unit_datum *player_unit = (struct unit_datum *)player;
			struct object_datum *target = object_get(best);
			struct object_placement_data placement;
			real_vector3d forward = player_unit->unit.looking_vector;
			real_vector3d side;

			if (warthog == NONE)
			{
				error(_error_silent, "gore: test: no warthog on this map");
				return;
			}
			forward.k = 0.f;
			normalize3d(&forward);
			side.i = -forward.j;
			side.j = forward.i;
			side.k = 0.f;
			test_vehicle_side = side;
			object_placement_data_new(&placement, warthog, NONE);
			point_from_line3d(&target->object.position, &side, 2.5f, &placement.position);
			placement.position.z += 0.3f;
			scale_vector3d(&side, -1.f, &placement.forward);
			placement.up = *global_up3d;
			scale_vector3d(&side, -vehicle_speed / 3.048f / 30.f, &placement.translational_velocity);
			test_vehicle_index = object_new(&placement);
			error(_error_silent, "gore: test: a warthog at %.1f m/s at %s", vehicle_speed,
				tag_get_name(target->definition_index));
			return;
		}
		if (test_distance > 0.f)
		{
			/* a frag grenade's explosion this many metres from the unit's
			feet, on the ground on the side away from the player */
			long frag = tag_loaded('jpt!', "weapons\\frag grenade\\explosion");
			real_vector3d away;
			real_point3d point;
			struct object_datum *target = object_get(best);

			if (frag == NONE)
			{
				error(_error_silent, "gore: test: no frag grenade explosion on this map");
				return;
			}
			vector_from_points3d(&player->object.position, &target->object.position, &away);
			away.k = 0.f;
			if (normalize3d(&away) == 0.f)
			{
				away = ((struct unit_datum *)player)->unit.looking_vector;
				away.k = 0.f;
				normalize3d(&away);
			}
			point_from_line3d(&target->object.position, &away,
				test_distance / GORE_METRES_PER_WORLD_UNIT, &point);
			point.z += 0.1f; /* (a grenade going off just above the ground by the unit's feet) */
			error(_error_silent, "gore: test: frag at %.2f m from %s (body %.2f shield %.2f)",
				test_distance, tag_get_name(target->definition_index),
				target->object.body_vitality, target->object.shield_vitality);
			if (config_boolean("debug.gore_test_projectile"))
			{
				/* a real frag grenade, dropped there: its fuse, its
				detonation and the AI's reaction as in play */
				long grenade = tag_loaded('proj', "weapons\\frag grenade\\frag grenade");

				if (grenade != NONE)
				{
					struct object_placement_data placement;

					point.z += 0.15f;
					object_placement_data_new(&placement, grenade, NONE);
					placement.position = point;
					object_new(&placement);
				}
				return;
			}
			damage_data_new(&damage, frag);
			damage.origin = point;
			damage.epicenter = point;
			damage.direction = *global_down3d;
			scenario_location_from_point(&damage.location, &point);
			area_of_effect_cause_damage(&damage, NONE);
			error(_error_silent, "gore: test: after the blast %s is %s (body %.2f)",
				tag_get_name(target->definition_index),
				object_try_and_get_and_verify_type(best, _object_mask_biped) ?
					(TEST_FLAG(object_get(best)->object.damage_flags, _object_dead_bit) ? "dead" : "alive") : "gone",
				object_try_and_get_and_verify_type(best, _object_mask_biped) ? object_get(best)->object.body_vitality : 0.f);
			return;
		}
		damage_data_new(&damage, NONE);
		SET_FLAG(damage.flags, _damage_area_of_effect_bit, TRUE);
		vector_from_points3d(&player->object.position, &object_get(best)->object.position, &damage.direction);
		normalize3d(&damage.direction);
		object_get(best)->object.body_vitality = 0.f;
		unit_kill(best);
		gore_last_force = 1.2f;
		gore_unit_gib_later(best, &damage, 1.2f);
	}
	else
	{
		error(_error_silent, "gore: test found no biped");
	}
}

/* ---------- public code */

/* ---------- sniper headshots

A sniper rifle kill on the head (the damage tag's name has "sniper" in it,
and the hit was on a material the collision model flags as the head, which
is what the engine's own headshot multiplier goes by) takes off the head
only: a copy of the unit showing just the head flies off along the shot, and
the body keeps its head drawn shrunk to the neck (only as drawn,
gore_render_node_matrices: its own node matrices, which its limp body
works on, are left alone) while it dies its normal death, the neck bleeding
for a while. */

#define GORE_MAXIMUM_HEADLESS 64
#define GORE_NECK_BLEED_TICKS 150

struct gore_headless
{
	long unit_index; /* NONE: free */
	short head_node;
	long time;
	long bleed_effect_index;
	signed char in_head[GORE_MAXIMUM_NODES]; /* the head's nodes */
};

static struct gore_headless gore_headless[GORE_MAXIMUM_HEADLESS];

static struct gore_headless *gore_headless_find(long unit_index)
{
	short index;

	if (unit_index == NONE)
		return NULL;
	for (index = 0; index < GORE_MAXIMUM_HEADLESS; index++)
	{
		if (gore_headless[index].unit_index == unit_index)
			return &gore_headless[index];
	}
	return NULL;
}

static boolean gore_headshot_unit_kind(long definition_index)
{
	static char const *const kinds[] = { "grunt", "jackal", "elite", "marine", "crewman", "cyborg" };
	char const *name = tag_get_name(definition_index);
	short index;

	if (!name || strstr(name, "flood") || strstr(name, "hunter"))
		return FALSE;
	for (index = 0; index < (short)NUMBEROF(kinds); index++)
	{
		if (strstr(name, kinds[index]))
			return TRUE;
	}
	return FALSE;
}

boolean gore_unit_headshot(long unit_index, struct damage_data const *damage, boolean head)
{
	struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct damage_resistance const *resistance;
	struct gore_headless *headless = NULL;
	struct model *model;
	char const *damage_name;
	signed char collapse[GORE_MAXIMUM_NODES];
	long blood_effect_index = NONE;
	long decal_index;
	short node_count;
	short head_node;
	short index;
	real_vector3d direction;

	if (!unit || !head || !gore_enabled() || !config_boolean("game.gore_headshots") ||
		TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || unit->object.node_matrices.size <= 0)
		return FALSE;
	damage_name = damage->definition_index != NONE ? tag_get_name(damage->definition_index) : NULL;
	if (!damage_name || !strstr(damage_name, "sniper") || !gore_headshot_unit_kind(unit->definition_index) ||
		gore_is_flood(unit->definition_index))
		return FALSE;
	model = gore_model_get(unit->definition_index);
	if (!model)
		return FALSE;
	head_node = gore_model_node_find(model, "bip01 head");
	node_count = (short)MIN(MIN(model->nodes.count, GORE_MAXIMUM_NODES),
		unit->object.node_matrices.size / (long)sizeof(real_matrix4x3));
	if (head_node == NONE || head_node >= node_count || gore_headless_find(unit_index))
		return FALSE;
	for (index = 0; index < GORE_MAXIMUM_HEADLESS && !headless; index++)
	{
		if (gore_headless[index].unit_index == NONE ||
			!object_try_and_get_and_verify_type(gore_headless[index].unit_index, _object_mask_biped))
			headless = &gore_headless[index];
	}
	if (!headless)
	{
		/* the oldest makes room */
		headless = &gore_headless[0];
		for (index = 1; index < GORE_MAXIMUM_HEADLESS; index++)
		{
			if (gore_headless[index].time < headless->time)
				headless = &gore_headless[index];
		}
	}
	csmemset(headless->in_head, 0, sizeof(headless->in_head));
	headless->in_head[head_node] = 1;
	gore_mark_subtree(model, TAG_BLOCK_GET_ELEMENT(&model->nodes, head_node, struct model_node)->first_child_node_index,
		headless->in_head, 1);
	for (index = 0; index < GORE_MAXIMUM_NODES; index++)
		collapse[index] = (index < node_count && headless->in_head[index]) ? NONE : (signed char)head_node;

	resistance = gore_resistance_get(unit->definition_index);
	headless->bleed_effect_index = NONE;
	if (resistance)
	{
		blood_effect_index = resistance->area_damage_effect.index != NONE ?
			resistance->area_damage_effect.index : resistance->body_destroyed_effect.index;
		headless->bleed_effect_index = resistance->localized_damage_effect.index != NONE ?
			resistance->localized_damage_effect.index : blood_effect_index;
	}
	{
		struct gore_unit_type const *type = gore_unit_type_get(unit->definition_index);

		decal_index = type->decal_name ? tag_loaded(DECAL_TAG, type->decal_name) : NONE;
		if (decal_index == NONE)
			decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat");
	}
	direction = damage->direction;
	if (normalize3d(&direction) == 0.f)
		direction = *global_up3d;
	gore_piece_unit_definition = unit->definition_index;
	/* (the head piece's burst at the neck is gore_spawn_copy's) */
	gore_spawn_copy(unit_index, 0xFFFFFFFF, head_node, collapse, &direction, 1.f, blood_effect_index, decal_index, FALSE);
	headless->unit_index = unit_index;
	headless->head_node = head_node;
	headless->time = game_time_get();
	if (config_boolean("game.gore_log"))
		error(_error_silent, "gore: sniper headshot: %s lost its head (%s)", tag_get_name(unit->definition_index), damage_name);
	return TRUE;
}

static void gore_headless_update(void)
{
	short index;

	for (index = 0; index < GORE_MAXIMUM_HEADLESS; index++)
	{
		struct gore_headless *headless = &gore_headless[index];
		struct object_datum *unit;
		long age;

		if (headless->unit_index == NONE)
			continue;
		unit = object_try_and_get_and_verify_type(headless->unit_index, _object_mask_biped);
		/* gone, or alive again (a revert to a checkpoint from before) */
		if (!unit || (!TEST_FLAG(unit->object.damage_flags, _object_dead_bit) && game_time_get() - headless->time > 2))
		{
			headless->unit_index = NONE;
			continue;
		}
		age = game_time_get() - headless->time;
		if (age > 0 && age < GORE_NECK_BLEED_TICKS && (age % 10) == 0 && headless->bleed_effect_index != NONE)
		{
			real_matrix4x3 const *matrices = object_get_node_matrices(headless->unit_index);

			if (headless->head_node < (short)(unit->object.node_matrices.size / (long)sizeof(real_matrix4x3)))
				gore_blood_effect_at(headless->bleed_effect_index, &matrices[headless->head_node].position,
					&matrices[headless->head_node].forward);
		}
	}
}

real_matrix4x3 const *gore_render_node_matrices(long object_index, real_matrix4x3 const *nodes)
{
	static real_matrix4x3 drawn[GORE_MAXIMUM_NODES];
	struct gore_headless *headless;
	struct object_datum *object;
	short count;
	short index;

	if (!nodes || !(headless = gore_headless_find(object_index)) ||
		!(object = object_try_and_get_and_verify_type(object_index, _object_mask_biped)))
		return nodes;
	count = (short)MIN(object->object.node_matrices.size / (long)sizeof(real_matrix4x3), GORE_MAXIMUM_NODES);
	if (headless->head_node >= count)
		return nodes;
	for (index = 0; index < count; index++)
	{
		drawn[index] = nodes[index];
		if (headless->in_head[index])
		{
			/* shrunk to the neck (the head node's own origin) */
			drawn[index].position = nodes[headless->head_node].position;
			drawn[index].forward = *global_zero_vector3d;
			drawn[index].left = *global_zero_vector3d;
			drawn[index].up = *global_zero_vector3d;
		}
	}
	return drawn;
}

void gore_reset(void)
{
	short index;

	for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		gore_pieces[index].object_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
		gore_pending[index].unit_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_HEADLESS; index++)
		gore_headless[index].unit_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_SHOTGUN_BLASTS; index++)
		gore_shotgun_blasts[index].victim_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_HUNTER_HITS; index++)
		gore_hunter_hits[index].unit_index = NONE;
	/* (the decals went with the map or the loaded game: forgotten, not
	deleted) */
	for (index = 0; index < GORE_MAXIMUM_VEHICLE_MARKS; index++)
	{
		gore_vehicle_marks[index].vehicle_index = NONE;
		gore_vehicle_marks[index].decal_index = NONE;
	}
	for (index = 0; index < GORE_MAXIMUM_WHEEL_CONTACTS; index++)
		gore_wheel_contacts[index].vehicle_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_COVERED_VEHICLES; index++)
		gore_coverages[index].vehicle_index = NONE;
	render_model_part_blood_proc = gore_render_part_blood;
	gore_test_done = 0;
	gore_test_gib_time = NONE;
	gore_test_made = NONE;
	gore_test_target = NONE;
	gore_chunk_render_count = 0;
	for (index = 0; index < NUMBER_OF_GORE_BLOOD_KINDS; index++)
	{
		gore_blood_credit[index] = 0.f;
		gore_blood_requested[index] = 0;
		gore_blood_emitted[index] = 0;
	}
}

/* a hunter's own tag (its gore type is the disabled one) */
static boolean gore_is_hunter(long definition_index)
{
	char const *name = tag_get_name(definition_index);

	return name && strstr(name, "hunter") != NULL;
}

/* damage categories, mirroring damage.c's own (file-local) enum, for the
hunter log below */
static char const *gore_damage_category_name(short category)
{
	static char const *const names[] = {
		"none", "falling", "bullet", "grenade", "highexplosive", "sniper",
		"melee", "flame", "mountedweapon", "vehicle", "plasma", "needle",
		"shotgun",
	};

	return category >= 0 && category < NUMBEROF(names) ? names[category] : "?";
}

/* every damage event on a hunter, logged behind game.gore_log: a retest
must be able to see exactly what the tank (or anything else) delivered */
static void gore_hunter_log_damage(long unit_index, struct damage_data const *damage)
{
	struct object_datum *victim = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct object_datum *owner;
	real dx, dy, dz;

	if (!victim || !gore_is_hunter(victim->definition_index))
		return;
	owner = damage->owner_object_index != NONE ?
		(struct object_datum *)object_try_and_get(damage->owner_object_index) : NULL;
	dx = damage->epicenter.x - victim->object.bounding_sphere_center.x;
	dy = damage->epicenter.y - victim->object.bounding_sphere_center.y;
	dz = damage->epicenter.z - victim->object.bounding_sphere_center.z;
	error(_error_silent,
		"[Gore] hunter damage: '%s' cat %s(%ld) aoe %ld from_weapon %ld owner '%s' epicentre %.2f wu from centre, %.2f clear (radius %.2f)",
		damage->definition_index != NONE ? tag_get_name(damage->definition_index) : "(no damage tag)",
		damage->definition_index != NONE ?
			gore_damage_category_name(damage_effect_definition_get(damage->definition_index)->damage.category) : "?",
		damage->definition_index != NONE ?
			(long)damage_effect_definition_get(damage->definition_index)->damage.category : -1L,
		(long)TEST_FLAG(damage->flags, _damage_area_of_effect_bit),
		(long)TEST_FLAG(damage->flags, _damage_from_weapon_bit),
		owner ? tag_get_name(owner->definition_index) : "(none)",
		(double)sqrtf(dx * dx + dy * dy + dz * dz),
		(double)(sqrtf(dx * dx + dy * dy + dz * dz) - victim->object.bounding_sphere_radius),
		(double)victim->object.bounding_sphere_radius);
	(void)unit_index;
}

/* a scorpion cannon shell's direct hit on a hunter, remembered for the
kill that follows (GORE_HUNTER_DIRECT_TICKS).

WHY THIS SHAPE (found the hard way, 2026-10-17: a reported tank kill on a
hunter did not gib): this build's first version asked for a NON-area-of-
effect, from-a-weapon, scorpion-named damage event - the shell's own
impact damage. That event never happens. In this engine a projectile
applies a separate impact damage on an object collision only if its tag
has one (projectiles.c, projectile_collision), and the scorpion shell's
tag has none: it only detonates. The detonation is an effect whose
damage-effect part calls area_of_effect_cause_damage (effects.c), which
flags the damage area-of-effect BEFORE object_cause_damage sees it
(damage.c, area_of_effect_cause_damage_to_object) and never flags it
from-a-weapon - so the old record condition refused every real hit, and
the kill judgement's own scorpion-name demand refused the rest (one
detonation also delivers a generic 'weapons\frag grenade\shock wave'
event at the same epicentre, which can be the killing event). Signals
that DO exist, and that a frag grenade cannot fake:
  A) a scorpion-named non-AoE impact (kept, in case a map's shell has one);
  B) a scorpion-named AoE blast whose epicentre is at the hunter's body
     (inside its bounding sphere, or within 0.30 wu past it): the shell
     went off ON the hunter, not beside it. A grenade's damage is never
     scorpion-named ('weapons\frag grenade\...', category grenade), so
     it records nothing, at the feet or anywhere else; the record this
     leaves is then matched to the kill by epicentre and owner (below).
The candidate category (explosive or mounted-weapon) is damage.c's part;
the name, geometry and bookkeeping are here. Name guards: the map's own
strings, compared only, never passed on as a name to load. */
static void gore_hunter_direct_record(long unit_index, struct damage_data const *damage)
{
	struct object_datum *victim;
	struct gore_hunter_hit *hit = NULL;
	char const *damage_name;
	boolean area_of_effect, direct;
	real dx, dy, dz, centre_distance, clear;
	short index;

	if (!gore_enabled() || !config_boolean("game.gore_hunter_tank"))
		return;
	victim = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	if (!victim || !gore_is_hunter(victim->definition_index) ||
		TEST_FLAG(victim->object.damage_flags, _object_dead_bit))
		return;
	damage_name = damage->definition_index != NONE ? tag_get_name(damage->definition_index) : NULL;
	if (!damage_name || !strstr(damage_name, "scorpion"))
	{
		if (config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] hunter hit by '%s': not scorpion damage, no record",
				damage_name ? damage_name : "(unnamed damage)");
		return;
	}
	dx = damage->epicenter.x - victim->object.bounding_sphere_center.x;
	dy = damage->epicenter.y - victim->object.bounding_sphere_center.y;
	dz = damage->epicenter.z - victim->object.bounding_sphere_center.z;
	centre_distance = sqrtf(dx * dx + dy * dy + dz * dz);
	clear = centre_distance - victim->object.bounding_sphere_radius;
	area_of_effect = TEST_FLAG(damage->flags, _damage_area_of_effect_bit);
	direct = !area_of_effect || clear <= 0.30f;
	if (config_boolean("game.gore_log"))
		error(_error_silent, "[Gore] hunter hit by scorpion damage '%s': %s (epicentre %.2f wu from centre, %.2f clear) %s",
			damage_name,
			area_of_effect ? "blast" : "impact",
			(double)centre_distance, (double)clear,
			direct ? "-> DIRECT HIT recorded" : "-> splash, no record");
	if (!direct)
		return;
	for (index = 0; index < GORE_MAXIMUM_HUNTER_HITS; index++)
	{
		if (gore_hunter_hits[index].unit_index == unit_index || !hit)
			hit = &gore_hunter_hits[index];
		if (gore_hunter_hits[index].unit_index == unit_index)
			break;
	}
	if (!hit)
		return;
	hit->unit_index = unit_index;
	hit->time = game_time_get();
	hit->epicenter = damage->epicenter;
	hit->owner_object_index = damage->owner_object_index;
	hit->direction = damage->direction;
	if (normalize3d(&hit->direction) == 0.f)
	{
		/* an AoE hit's direction already points from its epicentre into
		the body (damage.c sets it so); rebuild it if it was empty */
		hit->direction.i = dx; hit->direction.j = dy; hit->direction.k = dz;
		if (normalize3d(&hit->direction) == 0.f)
			hit->direction = *global_up3d;
	}
	return;
}

/* a hunter's killing hit judged: a recorded direct shell hit (above)
qualifies the kill that follows within GORE_HUNTER_DIRECT_TICKS - but the
killing event itself need not be scorpion-named, because one detonation
delivers both the scorpion blast and a generic shock-wave event at the
same epicentre, and either can land the kill. So the kill matches the
record on: its epicentre within a metre of the shell's, OR its own tag
naming the scorpion - and, when both sides name an owner, the same owner
(the tank's gunner), which a frag grenade thrown at the same corpse in
the same third of a second would have to share to fake it; a grenade on
its own never records, so it can never reach this test qualified. A
scorpion kill with no record is the shell landing beside the hunter:
splash, refused. Anything else is refused as hunters always were. */
static boolean gore_hunter_tank_gib(long unit_index, struct damage_data const *damage, real *force_out)
{
	char const *damage_name;
	boolean scorpion_named, owner_matches;
	real dx, dy, dz, epicentre_distance;
	short index;

	*force_out = 0.f;
	if (!config_boolean("game.gore_hunter_tank") || !gore_is_hunter(object_get(unit_index)->definition_index))
		return FALSE;
	damage_name = damage->definition_index != NONE ? tag_get_name(damage->definition_index) : NULL;
	scorpion_named = damage_name && strstr(damage_name, "scorpion") != NULL;
	for (index = 0; index < GORE_MAXIMUM_HUNTER_HITS; index++)
	{
		struct gore_hunter_hit *hit = &gore_hunter_hits[index];

		if (hit->unit_index != unit_index ||
			game_time_get() - hit->time > GORE_HUNTER_DIRECT_TICKS)
			continue;
		dx = damage->epicenter.x - hit->epicenter.x;
		dy = damage->epicenter.y - hit->epicenter.y;
		dz = damage->epicenter.z - hit->epicenter.z;
		epicentre_distance = sqrtf(dx * dx + dy * dy + dz * dz);
		owner_matches = hit->owner_object_index == NONE ||
			damage->owner_object_index == NONE ||
			hit->owner_object_index == damage->owner_object_index;
		if (config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] hunter kill judged: killing damage '%s', direct hit %ld ticks ago, killing epicentre %.2f wu from the shell's, owner %s -> %s",
				damage_name ? damage_name : "(unnamed damage)",
				game_time_get() - hit->time, (double)epicentre_distance,
				owner_matches ? "matches" : "differs",
				owner_matches && (scorpion_named || epicentre_distance <= 1.f) ?
					"GIBS" : "not this shell's kill");
		if (!owner_matches || (!scorpion_named && epicentre_distance > 1.f))
			return FALSE;
		gore_last_direction = hit->direction;
		gore_last_direction_valid = TRUE;
		hit->unit_index = NONE; /* spent */
		/* large body, hard shell: the launch caps at 3 */
		*force_out = 2.8f;
		return TRUE;
	}
	if (config_boolean("game.gore_log"))
		error(_error_silent, "[Gore] hunter killed by %s not gibbed: %s",
			damage_name ? damage_name : "(unnamed damage)",
			scorpion_named ? "scorpion splash, no direct shell hit on record" :
				"only a direct scorpion shell gibs a hunter");
	return FALSE;
}

/* ---------- point-blank shotgun gibs

A shotgun's damage arrives as a pellet at a time (15 projectiles a shot in
the stock tags: weapons.c fires trigger projectiles_per_shot of them, each
its own projectile and its own object_cause_damage). gore_unit_damage_begin
(damage.c, object_cause_damage, before the pellet's damage is dealt)
aggregates them per shooter, victim and damage tag, and the first pellet
captures the clear shooter-to-victim distance. Any kill from such a blast
within game.gore_shotgun_range gibs, however hurt the victim already was:
only the shotgun, the kill and the distance matter. The clear distance is
the centre distance less the two bounding radii, in metres: two bipeds
touching are about two metres centre to centre, so game.gore_shotgun_range
(2.0 m clear) is arm's length, not two metres of body. A kill judged within
the window gibs with a fixed force along the killing pellet's direction
(the shot's way, with gore_piece_launch's own upward bias); the pieces,
blood and the player's hidden-not-deleted unit are gore_unit_gib's, as for
an explosion. Hunters (and any type gore never gibs) are refused by
gore_unit_hit's own type check, as are victims in a vehicle (below) and
any non-shotgun damage. The player as victim follows the explosion rules:
gibbed and hidden, not deleted. */

void gore_unit_damage_begin(long unit_index, struct damage_data const *damage, boolean shotgun, boolean shell_candidate)
{
	struct object_datum *victim;

	/* (every hunter hit logged when asked, so a retest shows what came) */
	if (config_boolean("game.gore_log"))
		gore_hunter_log_damage(unit_index, damage);
	/* (a tank shell's direct hit on a hunter, for the kill that follows) */
	if (shell_candidate)
		gore_hunter_direct_record(unit_index, damage);
	struct object_datum *shooter;
	struct gore_shotgun_blast *blast = NULL;
	struct gore_shotgun_blast *free_blast = NULL;
	long now = game_time_get();
	short index;

	if (!shotgun || !gore_enabled() || !config_boolean("game.gore_shotgun"))
		return;
	victim = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	if (!victim || gore_is_flood(victim->definition_index) || TEST_FLAG(victim->object.damage_flags, _object_dead_bit) ||
		victim->object.parent_object_index != NONE ||
		damage->owner_object_index == NONE || damage->owner_object_index == unit_index)
		return;
	shooter = object_try_and_get_and_verify_type(damage->owner_object_index, _object_mask_biped);
	if (!shooter)
		return;
	for (index = 0; index < GORE_MAXIMUM_SHOTGUN_BLASTS; index++)
	{
		struct gore_shotgun_blast *candidate = &gore_shotgun_blasts[index];

		if (!free_blast && (candidate->victim_index == NONE ||
			now - candidate->last_tick > GORE_SHOTGUN_WINDOW_TICKS))
			free_blast = candidate; /* an expired blast's slot serves again */
		if (candidate->victim_index == unit_index &&
			candidate->owner_object_index == damage->owner_object_index &&
			candidate->damage_definition_index == damage->definition_index &&
			now - candidate->last_tick <= GORE_SHOTGUN_WINDOW_TICKS)
			blast = candidate;
	}
	if (!blast)
	{
		blast = free_blast;
		if (!blast)
			return; /* sixteen blasts in flight at once: this one is untracked, and cannot gib */
		blast->victim_index = unit_index;
		blast->owner_object_index = damage->owner_object_index;
		blast->owner_player_index = damage->owner_player_index;
		blast->damage_definition_index = damage->definition_index;
		blast->first_tick = now;
		blast->pellets = 0;
		blast->decided = FALSE;
		blast->distance = (real)sqrt(distance_squared3d(&shooter->object.position, &victim->object.position)) *
			GORE_METRES_PER_WORLD_UNIT -
			(shooter->object.bounding_sphere_radius + victim->object.bounding_sphere_radius) *
			GORE_METRES_PER_WORLD_UNIT;
		if (blast->distance < 0.f)
			blast->distance = 0.f;
	}
	blast->last_tick = now;
	blast->pellets++;
	return;
}

/* a killing shotgun hit's blast, judged once: gib (with its force), or the
reason not, logged with game.gore_log */
static boolean gore_shotgun_gib(long unit_index, struct damage_data const *damage, real *force_out)
{
	struct gore_shotgun_blast *blast = NULL;
	long now = game_time_get();
	short index;
	real range = (real)config_real("game.gore_shotgun_range");

	*force_out = 0.f;
	if (!config_boolean("game.gore_shotgun"))
		return FALSE;
	for (index = 0; index < GORE_MAXIMUM_SHOTGUN_BLASTS; index++)
	{
		struct gore_shotgun_blast *candidate = &gore_shotgun_blasts[index];

		if (candidate->victim_index == unit_index &&
			candidate->owner_object_index == damage->owner_object_index &&
			candidate->damage_definition_index == damage->definition_index &&
			now - candidate->last_tick <= GORE_SHOTGUN_WINDOW_TICKS &&
			now - candidate->first_tick <= GORE_SHOTGUN_WINDOW_TICKS)
			blast = candidate;
	}
	if (!blast)
	{
		char const *damage_name = damage->definition_index != NONE ?
			tag_get_name(damage->definition_index) : NULL;

		if (damage_name && strstr(damage_name, "shotgun") && config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] shotgun kill on %s not gibbed: no blast tracked (its first pellet was not seen)",
				tag_get_name(object_get(unit_index)->definition_index));
		return FALSE;
	}
	if (blast->decided)
		return FALSE;
	blast->decided = TRUE;
	if (range <= 0.f)
		range = 2.f;
	if (blast->distance > range)
	{
		if (config_boolean("game.gore_log"))
			error(_error_silent, "[Gore] shotgun kill on %s not gibbed: not point blank (%.2f m clear, limit %.2f m; %d pellets)",
				tag_get_name(object_get(unit_index)->definition_index), blast->distance, range,
				blast->pellets);
		return FALSE;
	}
	if (config_boolean("game.gore_log"))
		error(_error_silent, "[Gore] shotgun point-blank kill gibs %s (%d pellets, %.2f m)",
			tag_get_name(object_get(unit_index)->definition_index),
			blast->pellets, blast->distance);
	/* a hard shove the shot's way; gore_piece_launch caps it and adds up */
	*force_out = 2.2f;
	return TRUE;
}

static real gore_unit_force(long unit_index, struct damage_data const *damage, real normalized_damage,
	struct gore_unit_type const **type_out)
{
	struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct gore_unit_type const *type;
	real scale = (real)config_real("game.gore_threshold_scale");
	real force = 0.f;
	char const *kind = "other";
	real measure = 0.f;
	real threshold = 0.f;

	*type_out = NULL;
	if (!unit)
		return 0.f;
	if (scale <= 0.f)
		scale = 1.f;
	type = gore_unit_type_get(unit->definition_index);
	*type_out = type;
	if (damage->vehicle_impact_speed > 0.f)
	{
		kind = "vehicle";
		measure = damage->vehicle_impact_speed;
		threshold = type->vehicle_threshold * scale;
		force = threshold > 0.f ? measure / threshold : 0.f;
	}
	else if (damage->area_of_effect_full_damage > 0.f)
	{
		/* the blast's distance from the unit's centre against the gib
		radius (metres) */
		/* the blast's distance from the unit's body: the nearest point of
		a line up its middle, feet to head (the area of effect measures to
		the unit's centre, a metre above a grenade at its feet: a frag 2 m
		from a marine's feet was 2.2 m from its centre and did not gib) */
		real_point3d nearest = unit->object.bounding_sphere_center;
		real radius = unit->object.bounding_sphere_radius;
		real bottom = nearest.z - radius * 0.9f;
		real top = nearest.z + radius * 0.6f;

		nearest.z = PIN(damage->epicenter.z, bottom, top);
		kind = "explosion";
		measure = (real)sqrt(distance_squared3d(&damage->epicenter, &nearest)) * GORE_METRES_PER_WORLD_UNIT;
		threshold = type->gib_radius * (real)sqrt(damage->area_of_effect_full_damage / GORE_FRAG_DAMAGE) / scale;
		force = threshold / MAX(measure, 0.15f);
	}
	else if (TEST_FLAG(damage->flags, _damage_area_of_effect_bit))
	{
		/* (an area of effect with no distance: the debug test) */
		kind = "test";
		force = normalized_damage;
	}
	if (config_boolean("game.gore_log") && force > 0.f)
	{
		error(_error_silent, "gore: %s hit on %s: %s %.2f, gibs within %.2f, force %.2f%s%s",
			kind, tag_get_name(unit->definition_index),
			damage->vehicle_impact_speed > 0.f ? "speed" : "distance (m)", measure, threshold, force,
			type->disabled ? " (never gibs)" : force >= 1.f ? ": gibs" : ": normal death",
			TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? " (a corpse)" : "");
	}
	return force;
}

boolean gore_unit_hit(long unit_index, struct damage_data const *damage, real normalized_damage)
{
	struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct gore_unit_type const *type;
	short index;
	real force;

	if (!unit || !gore_enabled())
		return FALSE;
	/* (a gibbed player's hidden unit: once is enough) */
	if (TEST_FLAG(unit->object.flags, _object_invisible_bit))
		return FALSE;
	force = gore_unit_force(unit_index, damage, normalized_damage, &type);
	/* the Flood: its own death, no blood on the vehicle, no shotgun gib */
	if (gore_is_flood(unit->definition_index))
	{
		if (config_boolean("game.gore_log"))
			error(_error_silent, "gore: %s is Flood: its own death, no gore%s", tag_get_name(unit->definition_index),
				TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? " (a corpse)" : "");
		return FALSE;
	}
	gore_last_force = force;
	gore_last_player = ((struct biped_datum *)unit)->unit.player_index != NONE;
	gore_last_hunter = FALSE;
	gore_last_direction_valid = FALSE;
	/* the one hunter exception: a direct scorpion shell (above) */
	if (type && type->disabled && gore_is_hunter(unit->definition_index))
	{
		real hunter_force;

		if (gore_hunter_tank_gib(unit_index, damage, &hunter_force))
		{
			force = hunter_force;
			gore_last_force = force;
			gore_last_hunter = TRUE;
		}
	}
	if (damage->vehicle_impact_speed > 0.f && type && force > 0.f)
	{
		decal_lifetime_minimum = (real)config_real("game.gore_blood_seconds");
		gore_vehicle_hit_blood(damage->vehicle_impact_object_index, unit_index, force);
		decal_lifetime_minimum = 0.f;
	}
	/* port: a point-blank shotgun kill gibs whatever its pellet
	force (tracked in gore_unit_damage_begin, judged there once); a type
	gore never gibs (the hunter) is refused here as for a blast */
	if (type && !type->disabled && force < 1.f)
	{
		real shotgun_force;

		if (gore_shotgun_gib(unit_index, damage, &shotgun_force))
		{
			force = shotgun_force;
			gore_last_force = force;
		}
	}
	if (!type || (type->disabled && !gore_last_hunter) || force < 1.f)
		return FALSE;
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
	{
		if (gore_pending[index].unit_index == unit_index)
			return TRUE; /* (already coming apart: this hit is spent) */
	}
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
	{
		if (gore_pending[index].unit_index == NONE)
			return TRUE;
	}
	return FALSE;
}

void gore_unit_gib_later(long unit_index, struct damage_data const *damage, real normalized_damage)
{
	struct gore_unit_type const *type;
	short index;
	real force;

	{
		struct object_datum *unit = object_try_and_get_and_verify_type(unit_index, _object_mask_biped);

		if (!unit || gore_is_flood(unit->definition_index))
			return;
	}
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
	{
		if (gore_pending[index].unit_index == unit_index)
			return;
	}
	force = gore_last_force;
	(void)type; (void)normalized_damage;
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
	{
		if (gore_pending[index].unit_index == NONE)
		{
			struct gore_pending *pending = &gore_pending[index];

			pending->unit_index = unit_index;
			pending->time = game_time_get();
			pending->force = MAX(force, 1.f);
			pending->player = gore_last_player;
			pending->hunter = gore_last_hunter;
			pending->direction = gore_last_direction_valid ? gore_last_direction : damage->direction;
			if (normalize3d(&pending->direction) == 0.f)
				pending->direction = *global_up3d;
			return;
		}
	}
}

void gore_update(void)
{
	short index;
	static boolean initialized;

	if (!initialized)
	{
		gore_reset();
		initialized = TRUE;
	}
	decal_lifetime_minimum = (real)config_real("game.gore_blood_seconds");
	gore_test_update();
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
	{
		struct gore_pending *pending = &gore_pending[index];

		/* a tick after the hit: the unit's death has run */
		if (pending->unit_index != NONE && game_time_get() > pending->time)
		{
			struct gore_pending copy = *pending;

			pending->unit_index = NONE;
			if (gore_enabled())
				gore_unit_gib(&copy);
		}
	}
	for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
	{
		if (gore_pieces[index].object_index != NONE)
			gore_piece_update(&gore_pieces[index]);
	}
	gore_chunk_render_count = 0;
	for (index = 0; index < GORE_MAXIMUM_PIECES && gore_chunk_render_count < GORE_MAXIMUM_CHUNKS; index++)
	{
		struct gore_piece *piece = &gore_pieces[index];

		if (piece->object_index != NONE && piece->chunk)
		{
			gore_chunk_renders[gore_chunk_render_count].object_index = piece->object_index;
			gore_chunk_color(piece->unit_definition_index, &gore_chunk_renders[gore_chunk_render_count].color);
			gore_chunk_render_count++;
		}
	}
	gore_headless_update();
	gore_vehicles_update();
	gore_vehicle_marks_update();
	gore_coverage_update();
	if (config_boolean("game.gore_log") && (game_time_get() % 300) == 0)
	{
		long pieces = 0, bytes = 0;

		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index != NONE && object_try_and_get_and_verify_type(gore_pieces[index].object_index, 0xFFFFFFFF))
			{
				pieces++;
				bytes += object_header_get(gore_pieces[index].object_index)->data_size;
			}
		}
		error(_error_silent, "gore: decals now %ld (refused off the structure: %ld), pieces %ld (%d chunks) using %ld bytes of object memory; level %s, blood bursts %ld of %ld, splats %ld of %ld",
			decal_count_get(), decal_refused_count, pieces, gore_chunk_render_count, bytes, gore_level_names[gore_level()],
			gore_blood_emitted[_gore_blood_burst], gore_blood_requested[_gore_blood_burst],
			gore_blood_emitted[_gore_blood_splat], gore_blood_requested[_gore_blood_splat]);
	}
	decal_lifetime_minimum = 0.f;
}
