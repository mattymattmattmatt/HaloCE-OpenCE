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
  Hunters never come apart.
- the pieces: six copies of the unit, each drawing one piece of the stock
  bip01 skeleton (head, each upper arm, each thigh, the rest as the torso):
  the nodes outside the piece get zero axes at the cut's joint, so the
  vertices weighted to them shrink to it and those weighted to both sides
  stretch into a stump. No new art: every model with the bip01 skeleton
  (Grunt, Jackal, Elite, marines, Flood combat forms...) gets true left
  and right limbs. Other models split by their regions (head, arms, legs)
  where they have them, or throw the map's stock Flood bits (garbage
  objects with their own physics), or bleed only.
- the copies are posed by the dying unit's node matrices and moved here as
  rigid bodies (their own node matrices are never recomputed); they collide
  with the world from their third tick, fly along the force with a random
  spin, bounce, and leave blood: a burst at the cut, a few drops in the air,
  a splat decal where they first land.
- the dying unit is deleted (its kill was counted by its death).

Settings: game.gore, game.gore_threshold_scale, game.gore_max_pieces,
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
#include "objects/damage_resistances.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collision_model_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
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
boolean network_game_distributed_client(void);
boolean network_coop_active(void);

/* ---------- constants */

#define GORE_MAXIMUM_PIECES 256
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
	{ "hunter", 0.f, 0.f, NULL, TRUE },
	{ "grunt", 2.8f, 3.f, "effects\\decals\\blood splats\\blood grunt splat", FALSE },
	{ "jackal", 2.5f, 3.5f, "effects\\decals\\blood splats\\blood grunt splat", FALSE },
	{ "elite", 2.f, 5.f, "effects\\decals\\blood splats\\blood elite splat", FALSE },
	{ "flood", 2.8f, 3.f, "effects\\decals\\blood splats\\blood elite splat", FALSE },
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
	long age;
	long life;
	boolean resting;
	boolean impacted;
	long blood_effect_index;
	long decal_index;
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
};

/* ---------- globals */

static struct gore_piece gore_pieces[GORE_MAXIMUM_PIECES];
static struct gore_pending gore_pending[GORE_MAXIMUM_PENDING];
static unsigned long gore_seed = 0x6f726531;
static short gore_test_done;
static real gore_last_force = 1.f; /* gore_unit_hit's, for gore_unit_gib_later */

static struct gore_unit_type const *gore_unit_type_get(long definition_index);
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
	long maximum = PIN(config_integer("game.gore_vehicle_marks"), 0, GORE_MAXIMUM_VEHICLE_MARKS);
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
	long decal_index = tag_loaded(DECAL_TAG, gore_unit_type_get(unit_definition_index)->decal_name);

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

	if (!vehicle || !unit || !config_boolean("game.gore_vehicle_blood"))
		return;
	decal_index = gore_vehicle_blood_decal(unit->definition_index);
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
			if (config_boolean("game.gore_log"))
				error(_error_silent, "gore: blood on %s node %d (force %.2f)",
					tag_get_name(vehicle->definition_index), collision.node_index, force);
		}
	}
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

		if (!strstr(name, "wheel") && !strstr(name, "tire") && !strstr(name, "tread"))
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
static void gore_vehicle_run_over(long vehicle_index, long object_index, real_point3d const *point, long decal_index)
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
			decal_new_from_collision(decal_index, &collision, &down, 0.8f, FALSE, NONE, NULL);
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
				distance_squared3d(&biped->object.position, &vehicle->object.bounding_sphere_center) > reach * reach)
				continue;
			gore_vehicle_run_over(vehicle_index, bodies.index, &biped->object.position,
				gore_vehicle_blood_decal(biped->definition_index));
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
				piece->decal_index);
		}
	}
}

/* ---------- private code */

static boolean gore_enabled(void)
{
	if (!config_boolean("game.gore"))
		return FALSE;
	/* single player only for now: network games keep their pieces in step
	with no one */
	if (game_engine_running() || game_is_cooperative() || network_game_distributed_client() || network_coop_active())
		return FALSE;
	return TRUE;
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

static struct gore_piece *gore_piece_new(void)
{
	long maximum = PIN(config_integer("game.gore_max_pieces"), 6, GORE_MAXIMUM_PIECES);
	short index;
	short oldest = NONE;

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
}

/* a copy of the unit showing the given regions only */
static void gore_spawn_copy(long unit_index, unsigned long region_mask, short pivot_node,
	signed char const *collapse, real_vector3d const *direction, real force, long blood_effect_index, long decal_index)
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
	object_index = object_new(&placement);
	if (object_index == NONE)
		return;
	piece = gore_piece_new();
	if (!piece)
	{
		object_delete(object_index);
		return;
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
	piece->age = 0;
	piece->life = (long)(PIN(config_real("game.gore_piece_seconds"), 1.0, 3600.0) * 30.0);
	piece->resting = FALSE;
	piece->impacted = FALSE;
	piece->blood_effect_index = blood_effect_index;
	piece->decal_index = decal_index;
	gore_piece_launch(piece, direction, force);
	/* the cut's burst */
	gore_effect_at(blood_effect_index, &piece->pivot0, direction);
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
	piece = gore_piece_new();
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
	piece->age = 0;
	piece->life = (long)(PIN(config_real("game.gore_piece_seconds"), 1.0, 3600.0) * 30.0);
	piece->resting = FALSE;
	piece->impacted = FALSE;
	piece->blood_effect_index = blood_effect_index;
	piece->decal_index = decal_index;
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
			blood_effect_index, decal_index);
	}
	return TRUE;
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
	model = gore_model_get(unit->definition_index);
	resistance = gore_resistance_get(unit->definition_index);
	if (resistance)
	{
		blood_effect_index = resistance->area_damage_effect.index != NONE ?
			resistance->area_damage_effect.index : resistance->body_destroyed_effect.index;
		trail_effect_index = resistance->localized_damage_effect.index;
	}
	decal_index = tag_loaded(DECAL_TAG, type->decal_name);
	if (decal_index == NONE)
		decal_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat");
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
				&pending->direction, pending->force, blood_effect_index, decal_index);
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
			gore_effect_at(blood_effect_index, &center, &pending->direction);
			error(_error_silent, "gore: %s has no regions to split and the map no Flood bits: blood only",
				tag_get_name(unit->definition_index));
			return;
		}
		for (bit = 0; bit < (short)NUMBEROF(bits); bit++)
			gore_spawn_garbage(bits[bit], &center, &pending->direction, pending->force, trail_effect_index, decal_index);
		gore_effect_at(blood_effect_index, &center, &pending->direction);
	}

	/* the trail effect for the region copies */
	{
		short index;

		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index != NONE && gore_pieces[index].age == 0 && !gore_pieces[index].garbage)
				gore_pieces[index].blood_effect_index = trail_effect_index != NONE ? trail_effect_index : blood_effect_index;
		}
	}
	{
		short index;
		short count = 0;

		for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		{
			if (gore_pieces[index].object_index != NONE && gore_pieces[index].age == 0)
				count++;
		}
		error(_error_silent, "gore: %s came apart into %d pieces (%d split), force %.2f",
			tag_get_name(unit->definition_index), count, parts, pending->force);
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
				decal_new_from_collision(piece->decal_index, &collision, &down, 1.f, FALSE, NONE, NULL);
			}
		}
		else if (!piece->impacted && piece->age <= 36 && (piece->age % 12) == 0)
		{
			gore_effect_at(piece->blood_effect_index, &object->object.position, global_down3d);
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
					error(_error_silent, "gore: piece %ld landed at tick %d (%.2f %.2f %.2f)", piece->object_index,
						piece->age, piece->position.x, piece->position.y, piece->position.z);
					piece->impacted = TRUE;
					if (piece->decal_index != NONE)
						decal_new_from_collision(piece->decal_index, &collision, &step, 0.6f, FALSE, NONE, NULL);
					gore_effect_at(piece->blood_effect_index, &collision.point, &collision.plane.n);
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
		if (!piece->impacted && piece->age <= 36 && (piece->age % 12) == 0)
			gore_effect_at(piece->blood_effect_index, &piece->position, global_down3d);

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
	static real_vector3d test_vehicle_side;

	if (vehicle_speed > 0.f)
		sweep_count = 2;
	if (seconds <= 0.f || gore_test_done >= sweep_count ||
		game_time_get() < (long)((seconds + 4.f * gore_test_done) * 30.f))
		return;
	if (test_distance < 0.f)
		test_distance = sweep[gore_test_done];
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (biped->unit.player_index != NONE)
			player = object_get(iterator.index);
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
		position.z += 0.3f;
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
	gore_test_done++;
	if (best != NONE)
	{
		struct damage_data damage;

		error(_error_silent, "gore: test gib of %s", tag_get_name(object_get(best)->definition_index));
		/* in front of the player, to see it */
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
			normalize3d(&away);
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

void gore_reset(void)
{
	short index;

	for (index = 0; index < GORE_MAXIMUM_PIECES; index++)
		gore_pieces[index].object_index = NONE;
	for (index = 0; index < GORE_MAXIMUM_PENDING; index++)
		gore_pending[index].unit_index = NONE;
	/* (the decals went with the map or the loaded game: forgotten, not
	deleted) */
	for (index = 0; index < GORE_MAXIMUM_VEHICLE_MARKS; index++)
	{
		gore_vehicle_marks[index].vehicle_index = NONE;
		gore_vehicle_marks[index].decal_index = NONE;
	}
	for (index = 0; index < GORE_MAXIMUM_WHEEL_CONTACTS; index++)
		gore_wheel_contacts[index].vehicle_index = NONE;
	gore_test_done = 0;
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

	if (!unit || !gore_enabled() || ((struct biped_datum *)unit)->unit.player_index != NONE)
		return FALSE;
	force = gore_unit_force(unit_index, damage, normalized_damage, &type);
	gore_last_force = force;
	if (damage->vehicle_impact_speed > 0.f && type && force > 0.f)
	{
		decal_lifetime_minimum = (real)config_real("game.gore_blood_seconds");
		gore_vehicle_hit_blood(damage->vehicle_impact_object_index, unit_index, force);
		decal_lifetime_minimum = 0.f;
	}
	if (!type || type->disabled || force < 1.f)
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
			pending->direction = damage->direction;
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
	gore_vehicles_update();
	gore_vehicle_marks_update();
	decal_lifetime_minimum = 0.f;
}
