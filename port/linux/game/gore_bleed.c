/*
GORE_BLEED.C
The gore mod's corpse bleed (game.gore_corpse_bleed, with game.gore_level).
A corpse that died whole (not gibbed: gore_unit_gib deletes or hides those)
bleeds once it comes to rest:

- a short wait, then a pool grows under its wound (the neck of a headless
  body, else its torso), eased, stretched downhill as much as the ground
  slopes;
- on ground steeper than GORE_BLEED_FLAT_DEGREES, once the pool has all of
  its share it overflows into rivulets: from the body's own nodes (pelvis,
  spine, neck, limbs: the most downhill of them, at the pool's lower rim),
  as many as the body has blood (a grunt 1-2, a hunter 4-6). Each walks the
  real surface a step a tick: a ray down for the ground and its normal,
  along the local downhill (blended with the slope's overall gradient, a
  slow meander on top), around walls, into dips (a puddle where it comes
  to near flat), off ledges (a drip onto what is below, and on from there).
  Its speed is the slope's sine, slowing as its volume runs out; its width
  narrows with it (and on steeper ground); it ends in a small bead, never
  a hard cut. Now and then one splits (at most two branches a body).
- the trail is one continuous ribbon of quads (effects/decals.c
  decal_quad_new_ex), each the last one's end to its own, the corners put
  on the surface by a ray each, textured across its width by the middle of
  the species' own blood splat sprite and along it by a slow drift through
  it, so the edges are ragged but never stamped. Pools, puddles, beads and
  drips are round sprites of it.
- the color is the species' (gore.c gore_species_blood_color: human red,
  hunter orange, grunt cyan, jackal and elite blue-purple), darkening as it
  dries over game.gore_blood_seconds, then fading away.
- how much blood: the species' (grunt a little, jackal and humans more, an
  elite more, a hunter a lot) times the level's (mild half, heavy all,
  extreme a quarter more; off none). The Flood, sentinels, engineers and
  monitors never bleed.
- budget: 8 corpses bleeding at once (more wait their turn, 10 s at most),
  GORE_BLEED_MAXIMUM_QUADS quads in all (the oldest goes first). Local to
  each machine: nothing here is networked or deterministic (the corpses
  themselves are the network's).

debug.gore_bleed_test (seconds) finds ground of debug.gore_bleed_test_slope
degrees near the player, puts the player above it looking down the slope,
makes debug.gore_bleed_test_count of debug.gore_bleed_test_unit there and
kills them whole; each bleed logs its numbers when it is done.
*/

#include "cseries.h"
#include "gore.h"
#include "cseries/errors.h"
#include "effects/decals.h"
#include "game/game.h"
#include "game/players.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "cache/cache_files.h" /* tag_iterator_new, for the test */
#include "units/bipeds.h"
#include "units/units.h"
#include "halo_port_capacity.h"

#include <math.h>
#include <string.h>

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);
unsigned long system_milliseconds(void);

/* (gore.c) */
real gore_bleed_level_scale(void);
void gore_species_blood_color(long unit_definition_index, real_rgb_color *color);
boolean gore_unit_headless_neck(long unit_index, short *node_index);
boolean gore_unit_gib_pending(long unit_index);

/* ---------- constants */

#define GORE_BLEED_MAXIMUM_WATCHED 32
#define GORE_BLEED_MAXIMUM_ACTIVE 8
#define GORE_BLEED_MAXIMUM_RIVULETS 64
#define GORE_BLEED_MAXIMUM_QUADS 1536
#define GORE_BLEED_MAXIMUM_STAINS 160 /* round, growing: pools, puddles, beads, drips */
#define GORE_BLEED_STAIN_GRID 3 /* a big stain's quads a side */
#define GORE_BLEED_MAXIMUM_NODES 10

#define WU 3.048f /* metres a world unit */
#define GORE_BLEED_FLAT_DEGREES 6.5f /* below: a pool only */
#define GORE_BLEED_PUDDLE_DEGREES 4.f /* a rivulet coming to this: a puddle */
#define GORE_BLEED_MAXIMUM_SPEED 2.1f /* m/s, on a cliff, with all its volume */
#define GORE_BLEED_SEGMENT (0.05f) /* wu: a ribbon quad's length at most */
#define GORE_BLEED_LIFT (0.006f) /* wu off the surface */
#define GORE_BLEED_FLAGS (FLAG(_collision_test_structure_bit))
#define DECAL_TAG 'deca'

#define DEGREES(r) ((r) * 57.29578f)
#define RADIANS(d) ((d) * 0.01745329f)

/* ---------- definitions */

struct gore_bleed_species
{
	char const *name; /* in the tag path */
	real area; /* square metres of blood, heavy */
	short rivulets_minimum;
	short rivulets_maximum;
	real width; /* metres, a rivulet's start */
	char const *decal_name;
	short layers; /* a rivulet drawn this many times over: a pale splat's
		multiply, darker (the elite's light blue is lost on grass once) */
};

/* (the first that matches; the last is anyone else with blood) */
static struct gore_bleed_species const gore_bleed_species[] =
{
	{ "grunt", 0.6f, 1, 2, 0.065f, "effects\\decals\\blood splats\\blood grunt splat", 1 },
	{ "jackal", 1.0f, 2, 3, 0.075f, "effects\\decals\\blood splats\\blood grunt splat", 1 },
	{ "elite", 2.4f, 3, 4, 0.095f, "effects\\decals\\blood splats\\blood elite splat", 2 },
	{ "hunter", 5.0f, 4, 6, 0.125f, "effects\\decals\\blood splats\\blood hunter splat", 1 },
	{ "", 1.6f, 2, 3, 0.08f, "effects\\decals\\blood splats\\blood splat", 1 },
};

static char const *const gore_bleed_never[] = { "flood", "sentinel", "monitor", "engineer" };

/* the nodes rivulets start from (any the model has) */
static char const *const gore_bleed_node_names[GORE_BLEED_MAXIMUM_NODES] =
{
	"bip01 pelvis", "bip01 spine", "bip01 spine1", "bip01 neck", "bip01 head",
	"bip01 l upperarm", "bip01 r upperarm", "bip01 l thigh", "bip01 r thigh", "bip01 l forearm",
};

enum
{
	_bleed_free = 0,
	_bleed_watching, /* dead, not yet still */
	_bleed_queued, /* still, waiting for a slot */
	_bleed_waiting, /* the short wait before the pool */
	_bleed_pooling,
	_bleed_flowing,
	_bleed_done,
};

struct gore_bleed
{
	short state;
	long unit_index;
	long definition_index;
	short species;
	long seen_time;
	long still_ticks;
	long state_time;
	real_point3d probe; /* the torso, last tick */
	/* settled */
	real volume; /* wu^2, all of it */
	real pool_share;
	real slope; /* degrees, the ground's overall */
	real_vector3d downhill; /* along the ground's overall plane */
	real_vector3d normal;
	real_point3d wound; /* the ground under it */
	boolean headless;
	short stain; /* the pool */
	long pool_ticks;
	real pool_radius;
	real pool_stretch;
	short node_count;
	real_point3d nodes[GORE_BLEED_MAXIMUM_NODES]; /* the ground under each */
	short rivulets_planned;
	short rivulets_started;
	short rivulets_live;
	short branches;
	long wait_ticks;
	real_rgb_color color;
	long decal_definition_index;
	/* its numbers */
	real run_longest; /* wu */
	real run_total;
	long run_ticks_longest;
	short quads;
	short puddles;
	short drips;
	short beads;
	long done_time;
	boolean reported;
};

struct gore_rivulet
{
	short bleed; /* NONE: free */
	short depth; /* 0, or a branch */
	long delay;
	long age;
	real_point3d position;
	real_vector3d direction;
	real_vector3d normal;
	short cluster_index;
	real volume;
	real volume0;
	real width0;
	real sine; /* the slope's, smoothed */
	real distance;
	real since;
	real_vector3d since_direction;
	boolean edge; /* the ribbon's last end is below */
	real_point3d edge_left;
	real_point3d edge_right;
	real texture_v;
	real texture_rate;
	real phase;
	real phase_rate;
	short stuck;
};

struct gore_bleed_quad
{
	long decal_index; /* NONE: free */
	long serial;
	long time;
	long definition_index;
	short sequence;
	short cluster_index;
	real_rgb_color color;
	real_point3d corners[4];
	real_point2d uv[4];
};

struct gore_bleed_stain
{
	short quad_count; /* 0: free */
	short quads[GORE_BLEED_STAIN_GRID * GORE_BLEED_STAIN_GRID];
	long serials[GORE_BLEED_STAIN_GRID * GORE_BLEED_STAIN_GRID];
	short grid;
	real_point3d center;
	real_vector3d normal;
	real_vector3d axis; /* its long way */
	real radius; /* its target, across */
	real stretch; /* along the axis, times the radius */
	long time;
	long grow_ticks;
	boolean grown;
};

/* ---------- globals */

static struct gore_bleed gore_bleeds[GORE_BLEED_MAXIMUM_WATCHED];
static struct gore_rivulet gore_rivulets[GORE_BLEED_MAXIMUM_RIVULETS];
static struct gore_bleed_quad gore_bleed_quads[GORE_BLEED_MAXIMUM_QUADS];
static struct gore_bleed_stain gore_bleed_stains[GORE_BLEED_MAXIMUM_STAINS];
static short gore_bleed_quad_next;
static long gore_bleed_serial;
static short gore_bleed_check_next;
static unsigned long gore_bleed_seed = 0x626c6564;
static byte gore_bleed_marked[HALO_PORT_MAXIMUM_DECALS];
/* the numbers: quads made, refused (no cluster, no decal), taken back */
static long gore_bleed_quads_made;
static long gore_bleed_quads_failed;
static long gore_bleed_quads_remade;
/* the test */
static short gore_bleed_test_state;
static long gore_bleed_test_time;
static real_point3d gore_bleed_test_spot;
static real_point3d gore_bleed_test_place;
static real_vector3d gore_bleed_test_facing;
static real_vector3d gore_bleed_test_downhill;
static real gore_bleed_test_found_slope;
static long gore_bleed_test_units[GORE_BLEED_MAXIMUM_ACTIVE];
static short gore_bleed_test_unit_count;
static char const *bleed_test_tag(long unit_index);
/* the cost: cycles in gore_bleed_update, and the frames */
static unsigned long long gore_bleed_cycles;
static unsigned long long gore_bleed_cycles_max;
static long gore_bleed_cycle_ticks;
static unsigned long gore_bleed_window_ms;
static long gore_bleed_window_frame;
static unsigned long long gore_bleed_window_cycles_start;

/* ---------- small things */

static real bleed_random(real low, real high)
{
	return real_seed_random_range(&gore_bleed_seed, low, high);
}

static boolean bleed_log(void)
{
	return config_boolean("game.gore_log") || config_real("debug.gore_bleed_test") > 0.;
}

static unsigned long long bleed_cycles(void)
{
#if defined(__i386__) || defined(__x86_64__)
	return __builtin_ia32_rdtsc();
#else
	return 0;
#endif
}

static void bleed_normalize(real_vector3d *v, real_vector3d const *fallback)
{
	if (normalize3d(v) < 0.0001f)
		*v = *fallback;
}

/* v less its part along n */
static void bleed_project(real_vector3d *v, real_vector3d const *n)
{
	real along = dot_product3d(v, n);

	v->i -= n->i * along;
	v->j -= n->j * along;
	v->k -= n->k * along;
}

/* the ground below a point (from above it to below it): its point,
normal and cluster */
static boolean bleed_ground(real_point3d const *point, real above, real below, struct collision_result *collision)
{
	real_point3d start = *point;
	real_vector3d down = { 0.f, 0.f, 0.f };

	start.z += above;
	down.k = -(above + below);
	if (!collision_test_vector(GORE_BLEED_FLAGS, &start, &down, NONE, collision))
		return FALSE;
	/* (a floor, not a wall or a ceiling's back) */
	return collision->plane.n.k > 0.25f;
}

/* downhill on a surface of this normal (its length: the slope's sine) */
static real bleed_downhill(real_vector3d const *n, real_vector3d *downhill)
{
	downhill->i = n->i * n->k;
	downhill->j = n->j * n->k;
	downhill->k = n->k * n->k - 1.f;
	return magnitude3d(downhill);
}

static void bleed_rotate_about(real_vector3d *v, real_vector3d const *axis, real angle)
{
	real_vector3d cross;
	real c = cosf(angle);
	real s = sinf(angle);
	real d = dot_product3d(axis, v);

	cross_product3d(axis, v, &cross);
	v->i = v->i * c + cross.i * s + axis->i * d * (1.f - c);
	v->j = v->j * c + cross.j * s + axis->j * d * (1.f - c);
	v->k = v->k * c + cross.k * s + axis->k * d * (1.f - c);
}

/* the ground's overall plane around a point (a ring of rays, least
squares): its slope in degrees, downhill along it, and its normal */
static boolean bleed_fit_plane(real_point3d const *center, real radius, real *slope, real_vector3d *downhill,
	real_vector3d *normal, real_point3d *ground)
{
	real sx = 0.f, sy = 0.f, sz = 0.f, sxx = 0.f, syy = 0.f, sxy = 0.f, sxz = 0.f, syz = 0.f;
	short count = 0;
	short index;
	struct collision_result collision;

	if (!bleed_ground(center, 0.6f, 1.5f, &collision))
		return FALSE;
	*ground = collision.point;
	for (index = 0; index < 13; index++)
	{
		real_point3d point = *center;
		real x, y, z;

		if (index > 0)
		{
			real angle = (real)((index - 1) % 8) * 0.785398f + (index > 8 ? 0.39f : 0.f);
			real r = index > 8 ? radius * 0.5f : radius;

			point.x += cosf(angle) * r;
			point.y += sinf(angle) * r;
		}
		if (!bleed_ground(&point, 0.8f, 1.6f, &collision))
			continue;
		x = collision.point.x - center->x;
		y = collision.point.y - center->y;
		z = collision.point.z - ground->z;
		sx += x; sy += y; sz += z; sxx += x * x; syy += y * y; sxy += x * y; sxz += x * z; syz += y * z;
		count++;
	}
	if (count < 6)
	{
		*normal = collision.plane.n;
	}
	else
	{
		/* z = a x + b y + c */
		real n = (real)count;
		real cxx = sxx - sx * sx / n, cyy = syy - sy * sy / n, cxy = sxy - sx * sy / n;
		real cxz = sxz - sx * sz / n, cyz = syz - sy * sz / n;
		real determinant = cxx * cyy - cxy * cxy;
		real a, b;

		if (fabsf(determinant) < 1e-8f)
			return FALSE;
		a = (cxz * cyy - cyz * cxy) / determinant;
		b = (cyz * cxx - cxz * cxy) / determinant;
		normal->i = -a; normal->j = -b; normal->k = 1.f;
		normalize3d(normal);
	}
	*slope = DEGREES(acosf(PIN(normal->k, -1.f, 1.f)));
	if (bleed_downhill(normal, downhill) < 0.0001f)
	{
		downhill->i = 1.f; downhill->j = 0.f; downhill->k = 0.f;
	}
	else
	{
		normalize3d(downhill);
	}
	return TRUE;
}

/* (a rivulet is drawn this much wider than the blood it spends: the
streams of the species' width read as hairlines from a few meters) */
#define GORE_BLEED_DRAWN_WIDTH 1.7f

/* the cluster a point on the ground is in: a point right on the surface
is often in none, so a little above it, else the one the ray found */
static short bleed_cluster(struct collision_result const *collision)
{
	struct location location;
	real_point3d above = collision->point;

	above.z += 0.05f;
	scenario_location_from_point(&location, &above);
	if (location.cluster_index != NONE)
		return location.cluster_index;
	return collision->location.cluster_index;
}

/* ---------- quads */

static void bleed_quad_free(struct gore_bleed_quad *quad)
{
	if (quad->decal_index != NONE)
		decal_quad_delete(quad->decal_index);
	quad->decal_index = NONE;
}

/* a quad on the ground (its corners clockwise seen from above it: the
decals cull the other way); its index, NONE if the decals refused it */
static short bleed_quad_new(long definition_index, short sequence, short cluster_index, real_point3d const corners[4],
	real_point2d const uv[4], real_rgb_color const *color, real_vector3d const *normal)
{
	struct gore_bleed_quad *quad;
	short index = gore_bleed_quad_next;
	real_point3d ordered[4];
	real_point2d ordered_uv[4];
	real_vector3d a, b, cross;
	short corner;

	if (definition_index == NONE || cluster_index == NONE)
	{
		gore_bleed_quads_failed++;
		return NONE;
	}
	gore_bleed_quad_next = (short)((gore_bleed_quad_next + 1) % GORE_BLEED_MAXIMUM_QUADS);
	quad = &gore_bleed_quads[index];
	/* (the ring: the oldest goes) */
	bleed_quad_free(quad);
	vector_from_points3d(&corners[0], &corners[1], &a);
	vector_from_points3d(&corners[0], &corners[2], &b);
	cross_product3d(&a, &b, &cross);
	for (corner = 0; corner < 4; corner++)
	{
		/* (counterclockwise from above: reversed) */
		short from = (dot_product3d(&cross, normal) > 0.f) ? (short)(3 - corner) : corner;

		ordered[corner] = corners[from];
		ordered_uv[corner] = uv[from];
	}
	quad->decal_index = decal_quad_new_ex(definition_index, sequence, cluster_index, ordered, ordered_uv, color, 1.f);
	if (quad->decal_index == NONE)
	{
		gore_bleed_quads_failed++;
		return NONE;
	}
	gore_bleed_quads_made++;
	quad->serial = ++gore_bleed_serial;
	quad->time = game_time_get();
	quad->definition_index = definition_index;
	quad->sequence = sequence;
	quad->cluster_index = cluster_index;
	quad->color = *color;
	csmemcpy(quad->corners, ordered, sizeof(ordered));
	csmemcpy(quad->uv, ordered_uv, sizeof(ordered_uv));
	return index;
}

/* a point lifted onto the surface below it (along the given normal when
there is none) */
static void bleed_on_surface(real_point3d const *point, real_vector3d const *normal, real_point3d *result)
{
	struct collision_result collision;

	if (bleed_ground(point, 0.12f, 0.25f, &collision))
	{
		*result = collision.point;
		point_from_line3d(result, &collision.plane.n, GORE_BLEED_LIFT, result);
	}
	else
	{
		point_from_line3d(point, normal, GORE_BLEED_LIFT, result);
	}
}

/* ---------- stains (round, growing) */

static boolean bleed_stain_alive(struct gore_bleed_stain const *stain, short cell)
{
	short quad = stain->quads[cell];

	return quad != NONE && gore_bleed_quads[quad].decal_index != NONE &&
		gore_bleed_quads[quad].serial == stain->serials[cell];
}

/* its quads' corners at this much of its size (0 to 1) */
static void bleed_stain_corners(struct gore_bleed_stain const *stain, real size, short cell, real_point3d corners[4],
	real_point2d uv[4])
{
	static real const cu[4] = { 0.f, 1.f, 1.f, 0.f };
	static real const cv[4] = { 0.f, 0.f, 1.f, 1.f };
	real_vector3d side;
	short x = (short)(cell % stain->grid);
	short y = (short)(cell / stain->grid);
	short corner;

	cross_product3d(&stain->normal, &stain->axis, &side);
	for (corner = 0; corner < 4; corner++)
	{
		real u = ((real)x + cu[corner]) / (real)stain->grid;
		real v = ((real)y + cv[corner]) / (real)stain->grid;
		real along = (v * 2.f - 1.f) * stain->radius * stain->stretch * size;
		real across = (u * 2.f - 1.f) * stain->radius * size;
		real_point3d point = stain->center;

		point_from_line3d(&point, &stain->axis, along, &point);
		point_from_line3d(&point, &side, across, &point);
		bleed_on_surface(&point, &stain->normal, &corners[corner]);
		uv[corner].x = u;
		uv[corner].y = v;
	}
}

static void bleed_stain_update(struct gore_bleed_stain *stain)
{
	real progress = stain->grow_ticks > 0 ? (real)(game_time_get() - stain->time) / (real)stain->grow_ticks : 1.f;
	real size;
	short cell;

	if (stain->grown)
		return;
	if (progress >= 1.f)
	{
		progress = 1.f;
		stain->grown = TRUE;
	}
	/* eased (fast, then settling) */
	size = 1.f - (1.f - progress) * (1.f - progress) * (1.f - progress);
	size = MAX(size, 0.08f);
	/* (every other tick while it grows) */
	if (!stain->grown && ((game_time_get() - stain->time) & 1))
		return;
	for (cell = 0; cell < stain->quad_count; cell++)
	{
		struct gore_bleed_quad *quad;

		if (!bleed_stain_alive(stain, cell))
			continue;
		quad = &gore_bleed_quads[stain->quads[cell]];
		bleed_stain_corners(stain, size, cell, quad->corners, quad->uv);
		decal_quad_set(quad->decal_index, quad->corners);
	}
	if (stain->grown)
		stain->quad_count = 0; /* (its quads live on as quads) */
}

/* a round stain growing to this radius (wu; stretched along the axis) */
static short bleed_stain_new(struct gore_bleed *bleed, real_point3d const *center, real_vector3d const *normal,
	real_vector3d const *axis, real radius, real stretch, long grow_ticks)
{
	struct gore_bleed_stain *stain = NULL;
	short index;
	short cell;
	struct collision_result collision;
	short cluster_index;
	short sequence;

	if (!bleed_ground(center, 0.15f, 0.3f, &collision))
		return NONE;
	cluster_index = bleed_cluster(&collision);
	for (index = 0; index < GORE_BLEED_MAXIMUM_STAINS; index++)
	{
		if (gore_bleed_stains[index].quad_count == 0)
		{
			stain = &gore_bleed_stains[index];
			break;
		}
	}
	if (!stain)
		return NONE;
	stain->center = collision.point;
	stain->normal = *normal;
	stain->axis = *axis;
	bleed_project(&stain->axis, normal);
	bleed_normalize(&stain->axis, global_forward3d);
	stain->radius = radius;
	stain->stretch = stretch;
	stain->time = game_time_get();
	stain->grow_ticks = grow_ticks;
	stain->grown = FALSE;
	stain->grid = radius * stretch > 0.3f ? 3 : radius * stretch > 0.12f ? 2 : 1;
	/* (a round sprite: the definition's first) */
	sequence = 0;
	stain->quad_count = (short)(stain->grid * stain->grid);
	for (cell = 0; cell < stain->quad_count; cell++)
	{
		real_point3d corners[4];
		real_point2d uv[4];
		short quad;

		bleed_stain_corners(stain, 0.08f, cell, corners, uv);
		quad = bleed_quad_new(bleed->decal_definition_index, sequence, cluster_index, corners, uv, &bleed->color, normal);
		stain->quads[cell] = quad;
		stain->serials[cell] = quad != NONE ? gore_bleed_quads[quad].serial : 0;
		if (quad != NONE)
			bleed->quads++;
	}
	return (short)index;
}

/* ---------- rivulets */

static struct gore_rivulet *bleed_rivulet_new(void)
{
	short index;

	for (index = 0; index < GORE_BLEED_MAXIMUM_RIVULETS; index++)
	{
		if (gore_rivulets[index].bleed == NONE)
		{
			csmemset(&gore_rivulets[index], 0, sizeof(gore_rivulets[index]));
			return &gore_rivulets[index];
		}
	}
	return NULL;
}

static real bleed_rivulet_width(struct gore_rivulet const *rivulet)
{
	real left = rivulet->volume0 > 0.f ? PIN(rivulet->volume / rivulet->volume0, 0.f, 1.f) : 0.f;
	/* wide at the start, narrowing as it runs out, and on steep ground,
	with a little swell and pinch */
	return rivulet->width0 * (0.32f + 0.68f * sqrtf(left)) * (1.15f - 0.4f * rivulet->sine) *
		(1.f + 0.12f * sinf(rivulet->phase * 1.7f + 1.3f));
}

/* the ribbon's edge at the rivulet now (on the surface) */
static void bleed_rivulet_edges(struct gore_rivulet const *rivulet, real width, real_point3d *left, real_point3d *right)
{
	real_vector3d side;
	real_point3d point;

	cross_product3d(&rivulet->normal, &rivulet->direction, &side);
	bleed_normalize(&side, global_left3d);
	point_from_line3d(&rivulet->position, &side, width * 0.5f, &point);
	bleed_on_surface(&point, &rivulet->normal, left);
	point_from_line3d(&rivulet->position, &side, -width * 0.5f, &point);
	bleed_on_surface(&point, &rivulet->normal, right);
}

/* the ribbon from its last end to here */
static void bleed_rivulet_emit(struct gore_rivulet *rivulet, struct gore_bleed *bleed)
{
	real width = bleed_rivulet_width(rivulet) * GORE_BLEED_DRAWN_WIDTH;
	real_point3d left, right;
	real v0 = rivulet->texture_v;
	real v1;

	bleed_rivulet_edges(rivulet, width, &left, &right);
	/* along the flow: a slow drift through the sprite's middle, back and
	forth, so the edges change without a seam */
	rivulet->texture_v += rivulet->since * rivulet->texture_rate;
	v1 = rivulet->texture_v;
	if (rivulet->edge)
	{
		real_point3d corners[4];
		real_point2d uv[4];
		real band_low = 0.4f, band_high = 0.6f;
		real span = band_high - band_low;
		/* (a triangle wave through the band) */
		real t0 = fmodf(v0, 2.f * span), t1 = fmodf(v1, 2.f * span);

		t0 = t0 > span ? 2.f * span - t0 : t0;
		t1 = t1 > span ? 2.f * span - t1 : t1;
		corners[0] = rivulet->edge_left;
		corners[1] = rivulet->edge_right;
		corners[2] = right;
		corners[3] = left;
		/* (across, the splat's dense middle: its whole width is mostly
		its fringe, which leaves a hairline) */
		uv[0].x = 0.3f; uv[0].y = band_low + t0;
		uv[1].x = 0.7f; uv[1].y = band_low + t0;
		uv[2].x = 0.7f; uv[2].y = band_low + t1;
		uv[3].x = 0.3f; uv[3].y = band_low + t1;
		{
			short layer;

			for (layer = 0; layer < gore_bleed_species[bleed->species].layers; layer++)
			{
				if (bleed_quad_new(bleed->decal_definition_index, 0, rivulet->cluster_index, corners, uv, &bleed->color,
						&rivulet->normal) != NONE)
					bleed->quads++;
			}
		}
	}
	rivulet->edge_left = left;
	rivulet->edge_right = right;
	rivulet->edge = TRUE;
	rivulet->since = 0.f;
	rivulet->since_direction = rivulet->direction;
}

static char const *gore_bleed_end_reason = "";
static void bleed_rivulet_end(struct gore_rivulet *rivulet, struct gore_bleed *bleed, boolean puddle)
{
	real radius;

	if (rivulet->since > 0.005f)
		bleed_rivulet_emit(rivulet, bleed);
	if (puddle)
	{
		/* what is left pools where it came to rest */
		radius = PIN(sqrtf(MAX(rivulet->volume, 0.f) / 3.14159f), bleed_rivulet_width(rivulet) * 0.8f, 0.2f);
		bleed_stain_new(bleed, &rivulet->position, &rivulet->normal, &rivulet->direction, radius, 1.15f, 36);
		bleed->puddles++;
	}
	else
	{
		/* a small rounded bead at its tip */
		radius = MAX(bleed_rivulet_width(rivulet) * 0.75f, sqrtf(MAX(rivulet->volume, 0.f) / 3.14159f));
		bleed_stain_new(bleed, &rivulet->position, &rivulet->normal, &rivulet->direction, radius, 1.25f, 10);
		bleed->beads++;
	}
	if (rivulet->distance > bleed->run_longest)
	{
		bleed->run_longest = rivulet->distance;
		bleed->run_ticks_longest = rivulet->age;
	}
	bleed->run_total += rivulet->distance;
	bleed->rivulets_live--;
	if (bleed_log())
		error(_error_silent, "gore bleed: a rivulet ended in a %s after %.2f m in %.1f s (%s, %.3f m^2 left; %s, slope %.1f)",
			puddle ? "puddle" : "bead", rivulet->distance * WU, (real)rivulet->age / 30.f,
			rivulet->depth ? "a branch" : "from the body", rivulet->volume * WU * WU, gore_bleed_end_reason,
			DEGREES(asinf(PIN(rivulet->sine, 0.f, 1.f))));
	rivulet->bleed = NONE;
}

static void bleed_rivulet_start(struct gore_bleed *bleed, short bleed_index, real_point3d const *origin,
	real_vector3d const *direction, real volume, short depth, long delay)
{
	struct gore_rivulet *rivulet = bleed_rivulet_new();
	struct collision_result collision;

	if (!rivulet || volume <= 0.f || !bleed_ground(origin, 0.3f, 0.6f, &collision))
		return;
	rivulet->bleed = bleed_index;
	rivulet->depth = depth;
	rivulet->delay = delay;
	rivulet->position = collision.point;
	rivulet->normal = collision.plane.n;
	rivulet->cluster_index = bleed_cluster(&collision);
	rivulet->direction = *direction;
	bleed_project(&rivulet->direction, &rivulet->normal);
	bleed_normalize(&rivulet->direction, &bleed->downhill);
	rivulet->volume = rivulet->volume0 = volume;
	rivulet->width0 = gore_bleed_species[bleed->species].width / WU * (depth ? 0.75f : 1.f) *
		sqrtf(PIN(gore_bleed_level_scale(), 0.5f, 1.25f));
	rivulet->sine = sinf(RADIANS(bleed->slope));
	rivulet->texture_v = bleed_random(0.f, 1.f);
	rivulet->texture_rate = bleed_random(0.8f, 1.6f);
	rivulet->phase = bleed_random(0.f, 6.283f);
	rivulet->phase_rate = bleed_random(1.6f, 3.2f); /* radians a world unit */
	rivulet->edge = FALSE;
	rivulet->since_direction = rivulet->direction;
	bleed->rivulets_live++;
	bleed->rivulets_started++;
	if (depth)
		bleed->branches++;
}

/* over a ledge: a drip onto what is below, and on from there with most of
what is left (or a puddle) */
static boolean bleed_rivulet_drip(struct gore_rivulet *rivulet, struct gore_bleed *bleed, real_point3d const *beyond)
{
	struct collision_result collision;
	real_point3d top = *beyond;
	real_vector3d down = { 0.f, 0.f, -10.f };

	top.z += 0.05f;
	bleed_rivulet_emit(rivulet, bleed);
	if (!collision_test_vector(GORE_BLEED_FLAGS, &top, &down, NONE, &collision) || collision.plane.n.k < 0.25f)
		return FALSE;
	bleed->drips++;
	bleed_stain_new(bleed, &collision.point, &collision.plane.n, &rivulet->direction,
		MAX(bleed_rivulet_width(rivulet) * 1.4f, 0.03f), 1.f, 6);
	if (bleed_log())
		error(_error_silent, "gore bleed: a rivulet dripped off a ledge %.2f m high", (beyond->z - collision.point.z) * WU);
	rivulet->volume *= 0.7f;
	rivulet->position = collision.point;
	rivulet->normal = collision.plane.n;
	rivulet->cluster_index = bleed_cluster(&collision);
	rivulet->edge = FALSE;
	return TRUE;
}

static void bleed_rivulet_update(struct gore_rivulet *rivulet)
{
	struct gore_bleed *bleed = &gore_bleeds[rivulet->bleed];
	struct collision_result collision;
	real_vector3d local;
	real_vector3d target;
	real_vector3d macro;
	real_point3d next;
	real sine;
	real left;
	real speed;
	real step;

	if (rivulet->delay > 0)
	{
		rivulet->delay--;
		return;
	}
	rivulet->age++;
	if (!bleed_ground(&rivulet->position, 0.12f, 0.25f, &collision))
	{
		gore_bleed_end_reason = "no ground";
		bleed_rivulet_end(rivulet, bleed, FALSE);
		return;
	}
	rivulet->normal = collision.plane.n;
	rivulet->cluster_index = bleed_cluster(&collision);
	sine = bleed_downhill(&rivulet->normal, &local);
	/* the slope it runs on: the surface's here, and the drop to the ground
	a little ahead (faceted, terraced or bumpy ground) */
	{
		real_point3d ahead;
		struct collision_result there;
		real slope_sine = sine;

		point_from_line3d(&rivulet->position, &rivulet->direction, 0.12f, &ahead);
		if (bleed_ground(&ahead, 0.2f, 0.3f, &there))
		{
			real drop = rivulet->position.z - there.point.z;

			slope_sine = drop / sqrtf(0.12f * 0.12f + drop * drop);
		}
		rivulet->sine = rivulet->sine * 0.7f + MAX(0.4f * sine + 0.6f * slope_sine, 0.f) * 0.3f;
	}
	left = rivulet->volume0 > 0.f ? PIN(rivulet->volume / rivulet->volume0, 0.f, 1.f) : 0.f;
	/* m/s: the slope's sine, slowing as it runs out */
	speed = GORE_BLEED_MAXIMUM_SPEED * rivulet->sine * (0.25f + 0.75f * sqrtf(left));
	if (rivulet->sine < sinf(RADIANS(GORE_BLEED_PUDDLE_DEGREES)) || speed < 0.05f)
	{
		gore_bleed_end_reason = rivulet->sine < sinf(RADIANS(GORE_BLEED_PUDDLE_DEGREES)) ? "flat" : "slow";
		bleed_rivulet_end(rivulet, bleed, rivulet->volume > 0.002f);
		return;
	}
	step = speed / WU / 30.f;
	/* the way: the local downhill, some of the slope's overall, a meander */
	macro = bleed->downhill;
	bleed_project(&macro, &rivulet->normal);
	bleed_normalize(&macro, &rivulet->direction);
	if (sine > 0.02f)
		normalize3d(&local);
	else
		local = macro;
	target.i = local.i * 0.7f + macro.i * 0.3f;
	target.j = local.j * 0.7f + macro.j * 0.3f;
	target.k = local.k * 0.7f + macro.k * 0.3f;
	bleed_normalize(&target, &macro);
	rivulet->phase += rivulet->phase_rate * step;
	bleed_rotate_about(&target, &rivulet->normal, RADIANS(14.f) * sinf(rivulet->phase) +
		RADIANS(6.f) * sinf(rivulet->phase * 2.3f + 0.7f));
	rivulet->direction.i = rivulet->direction.i * 0.6f + target.i * 0.4f;
	rivulet->direction.j = rivulet->direction.j * 0.6f + target.j * 0.4f;
	rivulet->direction.k = rivulet->direction.k * 0.6f + target.k * 0.4f;
	bleed_project(&rivulet->direction, &rivulet->normal);
	bleed_normalize(&rivulet->direction, &target);
	/* a wall ahead: along it */
	{
		real_point3d from;
		real_vector3d ahead;
		struct collision_result wall;

		point_from_line3d(&rivulet->position, &rivulet->normal, 0.02f, &from);
		scale_vector3d(&rivulet->direction, step * 1.5f + 0.02f, &ahead);
		if (collision_test_vector(GORE_BLEED_FLAGS, &from, &ahead, NONE, &wall) && wall.plane.n.k < 0.6f)
		{
			bleed_project(&rivulet->direction, &wall.plane.n);
			if (normalize3d(&rivulet->direction) < 0.2f || ++rivulet->stuck > 8)
			{
				gore_bleed_end_reason = "a wall";
				bleed_rivulet_end(rivulet, bleed, TRUE);
				return;
			}
			bleed_project(&rivulet->direction, &rivulet->normal);
			bleed_normalize(&rivulet->direction, &target);
			return;
		}
	}
	point_from_line3d(&rivulet->position, &rivulet->direction, step, &next);
	if (!bleed_ground(&next, 0.15f, 0.12f, &collision))
	{
		/* a ledge (or a step down) */
		real_point3d beyond;

		point_from_line3d(&rivulet->position, &rivulet->direction, step + 0.03f, &beyond);
		if (rivulet->volume > 0.004f && bleed_rivulet_drip(rivulet, bleed, &beyond))
			return;
		gore_bleed_end_reason = "a ledge";
		bleed_rivulet_end(rivulet, bleed, FALSE);
		return;
	}
	rivulet->stuck = 0;
	rivulet->position = collision.point;
	rivulet->normal = collision.plane.n;
	rivulet->cluster_index = bleed_cluster(&collision);
	rivulet->distance += step;
	rivulet->since += step;
	rivulet->volume -= bleed_rivulet_width(rivulet) * step;
	if (!rivulet->edge)
	{
		/* (its start: the ribbon's first end, wide) */
		real_point3d left_edge, right_edge;
		real width = bleed_rivulet_width(rivulet) * 1.3f * GORE_BLEED_DRAWN_WIDTH;

		bleed_rivulet_edges(rivulet, width, &left_edge, &right_edge);
		rivulet->edge_left = left_edge;
		rivulet->edge_right = right_edge;
		rivulet->edge = TRUE;
		rivulet->since = 0.f;
		rivulet->since_direction = rivulet->direction;
	}
	else if (rivulet->since >= GORE_BLEED_SEGMENT ||
		dot_product3d(&rivulet->since_direction, &rivulet->direction) < 0.97f)
	{
		bleed_rivulet_emit(rivulet, bleed);
		/* now and then a branch splits off */
		if (rivulet->depth == 0 && bleed->branches < 2 && left > 0.4f && rivulet->distance > 0.15f &&
			bleed_random(0.f, 1.f) < 0.035f)
		{
			real_vector3d way = rivulet->direction;
			real share = rivulet->volume * 0.35f;

			bleed_rotate_about(&way, &rivulet->normal, RADIANS(bleed_random(25.f, 40.f)) *
				(bleed_random(0.f, 1.f) < 0.5f ? -1.f : 1.f));
			rivulet->volume -= share;
			bleed_rivulet_start(bleed, rivulet->bleed, &rivulet->position, &way, share, 1, 0);
		}
	}
	if (rivulet->volume <= 0.f)
	{
		gore_bleed_end_reason = "spent";
		bleed_rivulet_end(rivulet, bleed, FALSE);
	}
}

/* ---------- bleeds */

static short bleed_species_get(long definition_index)
{
	char const *name = tag_get_name(definition_index);
	short index;

	if (!name)
		return NONE;
	for (index = 0; index < (short)NUMBEROF(gore_bleed_never); index++)
	{
		if (strstr(name, gore_bleed_never[index]))
			return NONE;
	}
	for (index = 0; index < (short)NUMBEROF(gore_bleed_species) - 1; index++)
	{
		if (strstr(name, gore_bleed_species[index].name))
			return index;
	}
	return (short)(NUMBEROF(gore_bleed_species) - 1);
}

static boolean bleed_enabled(void)
{
	return config_boolean("game.gore_corpse_bleed") && gore_bleed_level_scale() > 0.f;
}

/* the corpse's torso (its pelvis or spine), or its centre */
static void bleed_torso(long unit_index, real_point3d *point)
{
	struct object_datum *object = object_get(unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	struct model *model = definition->object.model.index != NONE ?
		(struct model *)tag_get(MODELS_GROUP_TAG, definition->object.model.index) : NULL;
	short count = (short)(object->object.node_matrices.size / (long)sizeof(real_matrix4x3));
	short index;

	*point = object->object.bounding_sphere_center;
	if (!model || count <= 0)
		return;
	for (index = 0; index < (short)MIN(model->nodes.count, count); index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

		if (strstr(name, "pelvis") || strstr(name, "spine1"))
		{
			*point = object_get_node_matrices(unit_index)[index].position;
			return;
		}
	}
}

/* the ground under the corpse's nodes the rivulets come from */
static void bleed_find_nodes(struct gore_bleed *bleed)
{
	struct object_datum *object = object_get(bleed->unit_index);
	struct object_definition *definition = object_definition_get(object->definition_index);
	struct model *model = definition->object.model.index != NONE ?
		(struct model *)tag_get(MODELS_GROUP_TAG, definition->object.model.index) : NULL;
	short count = (short)(object->object.node_matrices.size / (long)sizeof(real_matrix4x3));
	short neck = NONE;
	short name_index;

	bleed->node_count = 0;
	bleed->headless = gore_unit_headless_neck(bleed->unit_index, &neck);
	if (!model || count <= 0)
		return;
	for (name_index = 0; name_index < GORE_BLEED_MAXIMUM_NODES; name_index++)
	{
		short index;

		for (index = 0; index < (short)MIN(model->nodes.count, count); index++)
		{
			struct collision_result collision;

			if (strcmp(TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name, gore_bleed_node_names[name_index]))
				continue;
			if (bleed_ground(&object_get_node_matrices(bleed->unit_index)[index].position, 0.3f, 0.8f, &collision))
				bleed->nodes[bleed->node_count++] = collision.point;
			break;
		}
	}
}

static void bleed_test_aim(struct gore_bleed const *bleed);

/* still: the bleed begins (its slope, its volume, its pool) */
static void bleed_settle(struct gore_bleed *bleed)
{
	struct gore_bleed_species const *species = &gore_bleed_species[bleed->species];
	real_point3d torso;
	real_point3d ground;
	short neck = NONE;

	bleed_torso(bleed->unit_index, &torso);
	bleed_find_nodes(bleed);
	gore_unit_headless_neck(bleed->unit_index, &neck);
	/* the wound: the neck of a headless body, else the torso */
	bleed->wound = torso;
	if (bleed->headless && neck != NONE &&
		neck < (short)(object_get(bleed->unit_index)->object.node_matrices.size / (long)sizeof(real_matrix4x3)))
		bleed->wound = object_get_node_matrices(bleed->unit_index)[neck].position;
	if (!bleed_fit_plane(&bleed->wound, 0.4f, &bleed->slope, &bleed->downhill, &bleed->normal, &ground))
	{
		bleed->state = _bleed_done;
		bleed->reported = TRUE;
		return;
	}
	bleed->wound = ground;
	bleed->volume = species->area / (WU * WU) * gore_bleed_level_scale() * (bleed->headless ? 1.25f : 1.f);
	gore_species_blood_color(bleed->definition_index, &bleed->color);
	bleed->decal_definition_index = tag_loaded(DECAL_TAG, species->decal_name);
	if (bleed->decal_definition_index == NONE)
		bleed->decal_definition_index = tag_loaded(DECAL_TAG, "effects\\decals\\blood splats\\blood splat");
	/* the pool's share: all of it on flat ground, less the steeper */
	if (bleed->slope < GORE_BLEED_FLAT_DEGREES)
		bleed->pool_share = 1.f;
	else
		bleed->pool_share = PIN(1.f - (bleed->slope - GORE_BLEED_FLAT_DEGREES) / 32.f, 0.2f, 0.85f);
	bleed->pool_stretch = 1.f + 1.4f * sinf(RADIANS(bleed->slope));
	bleed->pool_radius = MIN(sqrtf(bleed->volume * bleed->pool_share / (3.14159f * bleed->pool_stretch)), 1.4f / WU);
	bleed->rivulets_planned = 0;
	if (bleed->slope >= GORE_BLEED_FLAT_DEGREES)
	{
		real r = bleed_random(0.f, 0.999f);
		short planned = (short)(species->rivulets_minimum + r * (real)(species->rivulets_maximum - species->rivulets_minimum + 1));

		/* (fewer with less blood) */
		if (gore_bleed_level_scale() < 0.75f)
			planned = (short)MAX(1, planned - 1);
		bleed->rivulets_planned = (short)PIN(planned, 1, species->rivulets_maximum + 1);
	}
	bleed->wait_ticks = (long)bleed_random(12.f, 26.f);
	bleed_test_aim(bleed);
	bleed->state = _bleed_waiting;
	bleed->state_time = game_time_get();
	if (bleed_log())
		error(_error_silent, "gore bleed: %s%s is still on %.1f degrees%s: %.2f m^2 of blood (level x%.2f), a pool of %.2f m "
			"(x%.2f downhill, %.0f%%), %d rivulets",
			tag_get_name(bleed->definition_index), bleed_test_tag(bleed->unit_index), bleed->slope,
			bleed->headless ? " (headless: from the neck)" : "",
			bleed->volume * WU * WU, gore_bleed_level_scale(), bleed->pool_radius * WU, bleed->pool_stretch,
			bleed->pool_share * 100.f, bleed->rivulets_planned);
}

/* the pool has its share: the rivulets, from the body's most downhill
nodes, at the pool's lower rim */
static void bleed_overflow(struct gore_bleed *bleed, short bleed_index)
{
	real_point3d origins[GORE_BLEED_MAXIMUM_NODES + 6];
	real along[GORE_BLEED_MAXIMUM_NODES + 6];
	short origin_count = 0;
	short chosen[8];
	short chosen_count = 0;
	real rivulet_volume = bleed->volume * (1.f - bleed->pool_share);
	real weights[8];
	real weight_sum = 0.f;
	real_vector3d side;
	short index;

	cross_product3d(&bleed->normal, &bleed->downhill, &side);
	for (index = 0; index < bleed->node_count; index++)
		origins[origin_count++] = bleed->nodes[index];
	/* (and across the pool's rim, for a body with few nodes) */
	for (index = 0; index < 5; index++)
	{
		real_point3d point = bleed->wound;

		point_from_line3d(&point, &side, ((real)index - 2.f) * bleed->pool_radius * 0.45f, &point);
		origins[origin_count++] = point;
	}
	for (index = 0; index < origin_count; index++)
	{
		real_vector3d offset;

		vector_from_points3d(&bleed->wound, &origins[index], &offset);
		along[index] = dot_product3d(&offset, &bleed->downhill);
	}
	/* the most downhill, apart from each other */
	while (chosen_count < bleed->rivulets_planned && chosen_count < 8)
	{
		short best = NONE;

		for (index = 0; index < origin_count; index++)
		{
			short other;
			boolean near = FALSE;

			for (other = 0; other < chosen_count && !near; other++)
			{
				real_vector3d apart;

				vector_from_points3d(&origins[chosen[other]], &origins[index], &apart);
				bleed_project(&apart, &bleed->downhill);
				near = magnitude3d(&apart) < MAX(gore_bleed_species[bleed->species].width / WU * 1.6f, 0.04f);
			}
			if (!near && (best == NONE || along[index] > along[best]))
				best = index;
		}
		if (best == NONE)
			break;
		chosen[chosen_count++] = best;
	}
	for (index = 0; index < chosen_count; index++)
	{
		weights[index] = bleed_random(0.6f, 1.4f) * (index == 0 ? 1.3f : 1.f);
		weight_sum += weights[index];
	}
	for (index = 0; index < chosen_count; index++)
	{
		real_point3d origin = origins[chosen[index]];
		real rim = bleed->pool_radius * (bleed->pool_stretch * 0.8f + (bleed->pool_stretch - 1.f) * 0.35f);
		real_vector3d way = bleed->downhill;

		/* (inside the pool: out to its lower rim) */
		if (along[chosen[index]] < rim)
			point_from_line3d(&origin, &bleed->downhill, rim - along[chosen[index]], &origin);
		bleed_rotate_about(&way, &bleed->normal, RADIANS(bleed_random(-12.f, 12.f)));
		bleed_rivulet_start(bleed, bleed_index, &origin, &way, rivulet_volume * weights[index] / weight_sum, 0,
			(long)bleed_random(0.f, 18.f) + index * 4);
	}
	if (bleed_log())
		error(_error_silent, "gore bleed: %s's pool overflows into %d rivulets (%.2f m^2 of blood)",
			tag_get_name(bleed->definition_index), chosen_count, rivulet_volume * WU * WU);
}

/* " (test)" for one of debug.gore_bleed_test's own, in the log */
static char const *bleed_test_tag(long unit_index)
{
	short index;

	for (index = 0; index < gore_bleed_test_unit_count; index++)
	{
		if (gore_bleed_test_units[index] == unit_index)
			return " (test)";
	}
	return "";
}

static void bleed_report(struct gore_bleed *bleed)
{
	if (bleed->reported)
		return;
	bleed->reported = TRUE;
	error(_error_silent, "gore bleed: done: %s%s on %.1f degrees, level x%.2f: %.3f m^2 of blood, pool %.2f m (x%.2f, %.0f%%), "
		"%d rivulets (%d branches), longest run %.2f m in %.1f s, all runs %.2f m, %d puddles, %d beads, %d drips, "
		"%d quads, %.1f s from rest to the last drop",
		tag_get_name(bleed->definition_index), bleed_test_tag(bleed->unit_index), bleed->slope, gore_bleed_level_scale(),
		bleed->volume * WU * WU, bleed->pool_radius * WU, bleed->pool_stretch, bleed->pool_share * 100.f,
		bleed->rivulets_started, bleed->branches, bleed->run_longest * WU, (real)bleed->run_ticks_longest / 30.f,
		bleed->run_total * WU, bleed->puddles, bleed->beads, bleed->drips, bleed->quads,
		(real)(game_time_get() - bleed->state_time) / 30.f);
}

static short bleed_active_count(void)
{
	short index;
	short count = 0;

	for (index = 0; index < GORE_BLEED_MAXIMUM_WATCHED; index++)
	{
		short state = gore_bleeds[index].state;

		if (state == _bleed_waiting || state == _bleed_pooling || state == _bleed_flowing)
			count++;
	}
	return count;
}

static void bleed_update(struct gore_bleed *bleed, short bleed_index)
{
	struct object_datum *object = object_try_and_get_and_verify_type(bleed->unit_index, _object_mask_biped);

	switch (bleed->state)
	{
	case _bleed_watching:
	case _bleed_queued:
	{
		real_point3d torso;

		/* gone (gibbed, deleted), hidden (a gibbed player's), or back up */
		if (!object || object->definition_index != bleed->definition_index ||
			!TEST_FLAG(object->object.damage_flags, _object_dead_bit) ||
			TEST_FLAG(object->object.flags, _object_invisible_bit) || gore_unit_gib_pending(bleed->unit_index) ||
			gore_object_is_piece(bleed->unit_index))
		{
			bleed->state = _bleed_free;
			return;
		}
		bleed_torso(bleed->unit_index, &torso);
		if (distance_squared3d(&torso, &bleed->probe) < 0.004f * 0.004f)
			bleed->still_ticks++;
		else
			bleed->still_ticks = 0;
		bleed->probe = torso;
		if (bleed->state == _bleed_watching && bleed->still_ticks >= 12 && game_time_get() - bleed->seen_time >= 20)
		{
			bleed->state = _bleed_queued;
			bleed->state_time = game_time_get();
		}
		if (bleed->state == _bleed_queued)
		{
			if (bleed_active_count() < GORE_BLEED_MAXIMUM_ACTIVE)
				bleed_settle(bleed);
			else if (game_time_get() - bleed->state_time > 300)
			{
				if (bleed_log())
					error(_error_silent, "gore bleed: %s waited 10 s for one of the %d bleeds: none",
						tag_get_name(bleed->definition_index), GORE_BLEED_MAXIMUM_ACTIVE);
				bleed->state = _bleed_done;
				bleed->reported = TRUE;
			}
		}
		return;
	}
	case _bleed_waiting:
		if (game_time_get() - bleed->state_time >= bleed->wait_ticks)
		{
			/* the pool, under the wound (its centre a little downhill
			on a slope) */
			real_point3d center = bleed->wound;

			point_from_line3d(&center, &bleed->downhill, bleed->pool_radius * (bleed->pool_stretch - 1.f) * 0.5f, &center);
			bleed->pool_ticks = (long)(30.f * (1.6f + 1.4f * sqrtf(bleed->volume * WU * WU / 1.6f)));
			bleed->stain = bleed_stain_new(bleed, &center, &bleed->normal, &bleed->downhill, bleed->pool_radius,
				bleed->pool_stretch, bleed->pool_ticks);
			bleed->state = _bleed_pooling;
			bleed->state_time = game_time_get();
		}
		return;
	case _bleed_pooling:
		/* full enough: it overflows */
		if (game_time_get() - bleed->state_time >= (long)((real)bleed->pool_ticks * 0.7f))
		{
			if (bleed->rivulets_planned > 0)
				bleed_overflow(bleed, bleed_index);
			bleed->state = _bleed_flowing;
		}
		return;
	case _bleed_flowing:
		if (bleed->rivulets_live <= 0 && game_time_get() - bleed->state_time >= bleed->pool_ticks)
		{
			bleed->state = _bleed_done;
			bleed->done_time = game_time_get();
			if (bleed_log())
				bleed_report(bleed);
		}
		return;
	case _bleed_done:
		/* (forgotten once its corpse is gone; till then, it has bled) */
		if (!object || !TEST_FLAG(object->object.damage_flags, _object_dead_bit))
			bleed->state = _bleed_free;
		return;
	}
}

/* new corpses to watch */
static void bleed_watch(void)
{
	struct object_iterator iterator;
	struct biped_datum *biped;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		short index;
		short free_index = NONE;
		boolean known = FALSE;

		if (!TEST_FLAG(biped->object.damage_flags, _object_dead_bit) ||
			TEST_FLAG(biped->object.flags, _object_invisible_bit))
			continue;
		for (index = 0; index < GORE_BLEED_MAXIMUM_WATCHED && !known; index++)
		{
			if (gore_bleeds[index].state == _bleed_free)
			{
				if (free_index == NONE)
					free_index = index;
			}
			else if (gore_bleeds[index].unit_index == iterator.index)
				known = TRUE;
		}
		if (known || free_index == NONE)
			continue;
		if (bleed_species_get(biped->definition_index) == NONE || gore_object_is_piece(iterator.index) ||
			gore_unit_gib_pending(iterator.index))
			continue;
		csmemset(&gore_bleeds[free_index], 0, sizeof(gore_bleeds[free_index]));
		gore_bleeds[free_index].state = _bleed_watching;
		gore_bleeds[free_index].unit_index = iterator.index;
		gore_bleeds[free_index].definition_index = biped->definition_index;
		gore_bleeds[free_index].species = bleed_species_get(biped->definition_index);
		gore_bleeds[free_index].seen_time = game_time_get();
		gore_bleeds[free_index].stain = NONE;
		bleed_torso(iterator.index, &gore_bleeds[free_index].probe);
	}
}

/* drying: darker over game.gore_blood_seconds, then fading */
static void bleed_dry(void)
{
	real seconds = (real)config_real("game.gore_blood_seconds");
	long life = (long)((seconds > 0.f ? seconds : 180.f) * 30.f);
	short index;

	for (index = 0; index < GORE_BLEED_MAXIMUM_QUADS; index++)
	{
		struct gore_bleed_quad *quad = &gore_bleed_quads[index];
		long age;
		real dark;
		real alpha = 1.f;
		real_rgb_color color;

		if (quad->decal_index == NONE)
			continue;
		age = game_time_get() - quad->time;
		if (age > life + 300)
		{
			bleed_quad_free(quad);
			continue;
		}
		if (age > life)
			alpha = 1.f - (real)(age - life) / 300.f;
		/* fresh and wet, a quarter darker in its first minutes, half by
		the end */
		dark = 1.f - 0.25f * (1.f - expf(-(real)age / 1800.f)) - 0.3f * powf(MIN((real)age / (real)life, 1.f), 0.8f);
		color.red = quad->color.red * dark;
		color.green = quad->color.green * dark;
		color.blue = quad->color.blue * dark;
		decal_quad_set_color(quad->decal_index, &color, alpha);
	}
}

/* quads the decals took (their pool was full, or the vertex cache took
them): made again, a few a tick */
static void bleed_check_quads(void)
{
	short count;

	for (count = 0; count < 48; count++)
	{
		struct gore_bleed_quad *quad = &gore_bleed_quads[gore_bleed_check_next];

		gore_bleed_check_next = (short)((gore_bleed_check_next + 1) % GORE_BLEED_MAXIMUM_QUADS);
		if (quad->decal_index == NONE)
			continue;
		if (!decal_quad_exists(quad->decal_index) || !decal_quad_set(quad->decal_index, quad->corners))
		{
			decal_quad_delete(quad->decal_index);
			quad->decal_index = decal_quad_new_ex(quad->definition_index, quad->sequence, quad->cluster_index,
				quad->corners, quad->uv, &quad->color, 1.f);
			gore_bleed_quads_remade++;
		}
	}
	/* (every 30 s: a loaded game's quads, which this no longer knows) */
	if ((game_time_get() % 900) == 450)
	{
		short index;

		csmemset(gore_bleed_marked, 0, sizeof(gore_bleed_marked));
		for (index = 0; index < GORE_BLEED_MAXIMUM_QUADS; index++)
		{
			if (gore_bleed_quads[index].decal_index != NONE)
				gore_bleed_marked[DATUM_INDEX_TO_ABSOLUTE_INDEX(gore_bleed_quads[index].decal_index) % HALO_PORT_MAXIMUM_DECALS] = TRUE;
		}
		decal_quads_delete_unmarked(gore_bleed_marked);
	}
}

/* ---------- the test */

static long bleed_test_player(void)
{
	struct object_iterator iterator;
	struct biped_datum *biped;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)
	{
		if (biped->unit.player_index != NONE)
			return iterator.index;
	}
	return NONE;
}

/* ground of the slope asked for near the player, with room to run: an
even slope (the plane fits it well) over 3.5 world units downhill, and
ground to stand on uphill */
static boolean gore_bleed_test_scan = FALSE;
static short gore_bleed_test_rings = 60; /* (14 around debug.gore_bleed_test_near) */

static boolean bleed_test_find(real_point3d const *around, real want, real_point3d *spot, real_vector3d *downhill, real *found)
{
	real best_score = 1e9f;
	boolean any = FALSE;
	short ring;

	for (ring = 0; ring < gore_bleed_test_rings; ring++)
	{
		short step;
		short steps = (short)MAX(1, ring * 6);

		for (step = 0; step < steps; step++)
		{
			real angle = (real)step / (real)steps * 6.283185f;
			real_point3d point = *around;
			real_point3d ground;
			real_vector3d way, normal;
			real slope;
			real error_degrees;
			real previous_z = 0.f;
			real worst = 0.f;
			short check;
			boolean room = TRUE;

			point.x += cosf(angle) * (real)ring * 1.5f;
			point.y += sinf(angle) * (real)ring * 1.5f;
			point.z += 3.f;
			if (!bleed_fit_plane(&point, 0.9f, &slope, &way, &normal, &ground))
				continue;
			error_degrees = fabsf(slope - want);
			if (error_degrees > 4.f)
				continue;
			/* along the way: from the player's place uphill to 3.5 down,
			every step on the plane (no ledge, no wall, no bump) */
			for (check = -8; check <= 14 && room; check++)
			{
				real_point3d along = ground;
				struct collision_result collision;
				real expected;

				point_from_line3d(&along, &way, (real)check * 0.25f, &along);
				expected = along.z;
				room = bleed_ground(&along, 1.5f, 1.5f, &collision);
				if (room)
				{
					real off = fabsf(collision.point.z - expected);

					worst = MAX(worst, off);
					room = off < (want < 5.f ? 0.08f : 0.25f);
					if (check > -8 && room)
						room = previous_z - collision.point.z > -0.05f; /* (never uphill) */
					previous_z = collision.point.z;
				}
			}
			{
				struct collision_result collision;
				real_point3d top = ground;
				real_vector3d up = { 0.f, 0.f, 2.f };

				top.z += 0.1f;
				if (collision_test_vector(GORE_BLEED_FLAGS, &top, &up, NONE, &collision))
					room = FALSE;
			}
			if (!room)
				continue;
			if (gore_bleed_test_scan)
			{
				/* (debug.gore_bleed_test_near "scan": every site, 5 wu apart) */
				static real_point3d logged[64];
				static short logged_count;
				short index;

				for (index = 0; index < logged_count && distance_squared3d(&logged[index], &ground) > 25.f; index++);
				if (index == logged_count && logged_count < 64)
				{
					logged[logged_count++] = ground;
					error(_error_silent, "gore bleed test: a site of %.1f degrees at %.2f %.2f %.2f (unevenness %.3f)",
						slope, ground.x, ground.y, ground.z, worst);
				}
			}
			/* (the evenest near the slope asked, then the nearest) */
			if (error_degrees + worst * 20.f + (real)ring * 0.02f >= best_score)
				continue;
			best_score = error_degrees + worst * 20.f + (real)ring * 0.02f;
			*spot = ground;
			*downhill = way;
			*found = slope;
			any = TRUE;
		}
		if (any && ring > 12 && !gore_bleed_test_scan)
			break;
	}
	return any;
}

/* the test's first corpse at rest: the player above its wound, looking
down its slope */
static void bleed_test_aim(struct gore_bleed const *bleed)
{
	long player_index = bleed_test_player();
	real_point3d place;
	real_vector3d side;
	real_point3d look;
	struct collision_result collision;

	if (config_real("debug.gore_bleed_test") <= 0. || gore_bleed_test_unit_count <= 0 ||
		bleed->unit_index != gore_bleed_test_units[0] || player_index == NONE)
		return;
	cross_product3d(global_up3d, &bleed->downhill, &side);
	bleed_normalize(&side, global_left3d);
	place = bleed->wound;
	/* (beside the way down, a little above: the body hides none of it) */
	point_from_line3d(&place, &bleed->downhill, -0.5f, &place);
	point_from_line3d(&place, &side, 1.9f, &place);
	place.z = bleed->wound.z + 1.5f;
	if (bleed_ground(&place, 4.f, 6.f, &collision))
		place.z = collision.point.z;
	place.z += 0.35f;
	gore_bleed_test_place = place;
	look = bleed->wound;
	point_from_line3d(&look, &bleed->downhill, 1.1f, &look);
	gore_bleed_test_facing.i = look.x - place.x;
	gore_bleed_test_facing.j = look.y - place.y;
	gore_bleed_test_facing.k = look.z - (place.z + 0.6f);
	normalize3d(&gore_bleed_test_facing);
	error(_error_silent, "gore bleed test: the player at %.2f %.2f %.2f, looking at the wound's slope (%.2f %.2f %.2f)",
		place.x, place.y, place.z, gore_bleed_test_facing.i, gore_bleed_test_facing.j, gore_bleed_test_facing.k);
}

static void bleed_test_update(void)
{
	real seconds = (real)config_real("debug.gore_bleed_test");
	long player_index;
	struct object_datum *player;

	if (seconds <= 0.f)
		return;
	player_index = bleed_test_player();
	if (player_index == NONE)
		return;
	player = object_get(player_index);
	/* (the player held where the test put it, looking on: a slope that
	steep would slide it off) */
	if (gore_bleed_test_state == 1 || gore_bleed_test_state == 2)
	{
		{
			/* (its body turned too: a unit's look is held within reach of
			its body's facing) */
			real_vector3d forward = gore_bleed_test_facing;

			forward.k = 0.f;
			bleed_normalize(&forward, global_forward3d);
			object_set_position(player_index, &gore_bleed_test_place, &forward, global_up3d);
		}
		player->object.translational_velocity = *global_zero_vector3d;
		player_control_set_facing(0, &gore_bleed_test_facing);
		if ((game_time_get() % 60) == 0)
		{
			struct unit_datum *unit = (struct unit_datum *)player;

			error(_error_silent, "gore bleed test: the player looks %.2f %.2f %.2f", unit->unit.looking_vector.i,
				unit->unit.looking_vector.j, unit->unit.looking_vector.k);
		}
		if (game_time_get() > gore_bleed_test_time + 60 * 30)
			gore_bleed_test_state = 3;
	}
	if (gore_bleed_test_state == 0 && game_time_get() >= (long)(seconds * 30.f))
	{
		real want = (real)config_real("debug.gore_bleed_test_slope");
		real_point3d place;
		real_vector3d facing;

		real_point3d around = player->object.position;
		{
			char const *near = config_string("debug.gore_bleed_test_near");
			float near_x, near_y;

			gore_bleed_test_scan = near && strcmp(near, "scan") == 0;
			if (near && sscanf(near, "%f %f", &near_x, &near_y) == 2)
			{
				struct collision_result collision;

				around.x = near_x;
				around.y = near_y;
				around.z += 40.f;
				if (bleed_ground(&around, 0.f, 120.f, &collision))
					around.z = collision.point.z;
				gore_bleed_test_rings = 14;
				error(_error_silent, "gore bleed test: looking around %.2f %.2f %.2f (debug.gore_bleed_test_near)", around.x, around.y, around.z);
			}
		}
		if (!bleed_test_find(&around, want, &gore_bleed_test_spot, &gore_bleed_test_downhill,
				&gore_bleed_test_found_slope))
		{
			error(_error_silent, "gore bleed test: no ground of %.0f degrees near the player", want);
			gore_bleed_test_state = 3;
			return;
		}
		/* the player above it, looking down the slope at it */
		place = gore_bleed_test_spot;
		place.x -= gore_bleed_test_downhill.i * 1.6f;
		place.y -= gore_bleed_test_downhill.j * 1.6f;
		{
			struct collision_result collision;

			if (bleed_ground(&place, 6.f, 3.f, &collision))
				place = collision.point;
		}
		place.z += 0.35f;
		gore_bleed_test_place = place;
		object_set_position(player_index, &place, NULL, NULL);
		player->object.translational_velocity = *global_zero_vector3d;
		/* (from the eyes, about 0.6 above the feet, to the spot) */
		facing.i = gore_bleed_test_spot.x + gore_bleed_test_downhill.i * 0.5f - place.x;
		facing.j = gore_bleed_test_spot.y + gore_bleed_test_downhill.j * 0.5f - place.y;
		facing.k = gore_bleed_test_spot.z - (place.z + 0.6f);
		normalize3d(&facing);
		gore_bleed_test_facing = facing;
		player_control_set_facing(0, &facing);
		error(_error_silent, "gore bleed test: the player at %.2f %.2f %.2f facing %.2f %.2f %.2f",
			place.x, place.y, place.z, facing.i, facing.j, facing.k);
		error(_error_silent, "gore bleed test: ground of %.1f degrees (asked %.0f) at %.2f %.2f %.2f, the player above it",
			gore_bleed_test_found_slope, want, gore_bleed_test_spot.x, gore_bleed_test_spot.y, gore_bleed_test_spot.z);
		gore_bleed_test_state = 1;
		gore_bleed_test_time = game_time_get();
		return;
	}
	if (gore_bleed_test_state == 1 && game_time_get() >= gore_bleed_test_time + 15)
	{
		/* the units, side by side across the slope */
		struct tag_iterator tags;
		long definition_index;
		long found = NONE;
		short count = (short)PIN(config_integer("debug.gore_bleed_test_count"), 1, GORE_BLEED_MAXIMUM_ACTIVE);
		char const *unit_name = config_string("debug.gore_bleed_test_unit");
		real_vector3d side;
		short index;

		tag_iterator_new(&tags, 'bipd');
		while ((definition_index = tag_iterator_next(&tags)) != NONE)
		{
			if (strstr(tag_get_name(definition_index), unit_name) && !strstr(tag_get_name(definition_index), "flood"))
			{
				found = definition_index;
				break;
			}
		}
		gore_bleed_test_state = 2;
		if (found == NONE)
		{
			error(_error_silent, "gore bleed test: no biped \"%s\" on this map", unit_name);
			return;
		}
		side.i = -gore_bleed_test_downhill.j;
		side.j = gore_bleed_test_downhill.i;
		side.k = 0.f;
		normalize3d(&side);
		gore_bleed_test_unit_count = 0;
		for (index = 0; index < count; index++)
		{
			struct object_placement_data placement;
			struct collision_result collision;
			real offset = ((real)index - (real)(count - 1) * 0.5f) * 0.75f;

			object_placement_data_new(&placement, found, NONE);
			placement.position = gore_bleed_test_spot;
			point_from_line3d(&placement.position, &side, offset, &placement.position);
			if (bleed_ground(&placement.position, 1.f, 2.f, &collision))
				placement.position = collision.point;
			placement.position.z += 0.05f;
			placement.forward = side;
			placement.up = *global_up3d;
			gore_bleed_test_units[gore_bleed_test_unit_count] = object_new(&placement);
			if (gore_bleed_test_units[gore_bleed_test_unit_count] != NONE)
				gore_bleed_test_unit_count++;
		}
		error(_error_silent, "gore bleed test: made %d %s", gore_bleed_test_unit_count, tag_get_name(found));
		gore_bleed_test_time = game_time_get();
		return;
	}
	if (gore_bleed_test_state == 2 && game_time_get() == gore_bleed_test_time + 2)
	{
		short index;

		for (index = 0; index < gore_bleed_test_unit_count; index++)
		{
			if (object_try_and_get_and_verify_type(gore_bleed_test_units[index], _object_mask_biped))
			{
				object_get(gore_bleed_test_units[index])->object.body_vitality = 0.f;
				unit_kill(gore_bleed_test_units[index]);
			}
		}
		error(_error_silent, "gore bleed test: killed them whole (level %s)", config_string("game.gore_level"));
	}
}

/* the cost, every 5 s while anything bleeds (and in the test) */
static void bleed_cost_report(void)
{
	unsigned long now = system_milliseconds();
	long frame = render.frame_index;

	if (gore_bleed_window_ms == 0)
	{
		gore_bleed_window_ms = now;
		gore_bleed_window_frame = frame;
		gore_bleed_window_cycles_start = bleed_cycles();
		return;
	}
	if ((game_time_get() % 150) != 0)
		return;
	if (bleed_log())
	{
		unsigned long elapsed = now - gore_bleed_window_ms;
		long frames = frame - gore_bleed_window_frame;
		unsigned long long wall_cycles = bleed_cycles() - gore_bleed_window_cycles_start;
		/* (the cycle counter's rate, from the wall clock) */
		real cycles_per_us = elapsed > 0 && wall_cycles ? (real)wall_cycles / ((real)elapsed * 1000.f) : 0.f;
		short active = bleed_active_count();
		short live = 0;
		short quads = 0;
		short index;

		for (index = 0; index < GORE_BLEED_MAXIMUM_RIVULETS; index++)
			live += gore_rivulets[index].bleed != NONE;
		for (index = 0; index < GORE_BLEED_MAXIMUM_QUADS; index++)
			quads += gore_bleed_quads[index].decal_index != NONE;
		if (active > 0 || live > 0 || config_real("debug.gore_bleed_test") > 0.)
			error(_error_silent, "gore bleed: cost: %d bleeding, %d rivulets, %d quads (made %ld, refused %ld, made again %ld); "
				"update %.1f us a tick (most %.1f); %ld frames in %lu ms (%.2f ms a frame)",
				active, live, quads, gore_bleed_quads_made, gore_bleed_quads_failed, gore_bleed_quads_remade,
				cycles_per_us > 0.f && gore_bleed_cycle_ticks ? (real)gore_bleed_cycles / gore_bleed_cycle_ticks / cycles_per_us : 0.f,
				cycles_per_us > 0.f ? (real)gore_bleed_cycles_max / cycles_per_us : 0.f,
				frames, elapsed, frames > 0 ? (real)elapsed / (real)frames : 0.f);
	}
	gore_bleed_window_ms = now;
	gore_bleed_window_frame = frame;
	gore_bleed_window_cycles_start = bleed_cycles();
	gore_bleed_cycles = 0;
	gore_bleed_cycles_max = 0;
	gore_bleed_cycle_ticks = 0;
}

/* ---------- public code */

void gore_bleed_reset(void)
{
	short index;

	/* (the decals went with the map or the loaded game: forgotten) */
	for (index = 0; index < GORE_BLEED_MAXIMUM_WATCHED; index++)
		gore_bleeds[index].state = _bleed_free;
	for (index = 0; index < GORE_BLEED_MAXIMUM_RIVULETS; index++)
		gore_rivulets[index].bleed = NONE;
	for (index = 0; index < GORE_BLEED_MAXIMUM_QUADS; index++)
		gore_bleed_quads[index].decal_index = NONE;
	for (index = 0; index < GORE_BLEED_MAXIMUM_STAINS; index++)
		gore_bleed_stains[index].quad_count = 0;
	gore_bleed_quad_next = 0;
	gore_bleed_check_next = 0;
	gore_bleed_test_state = 0;
	gore_bleed_test_unit_count = 0;
	gore_bleed_quads_made = gore_bleed_quads_failed = gore_bleed_quads_remade = 0;
	gore_bleed_window_ms = 0;
}

void gore_bleed_update(void)
{
	unsigned long long start = bleed_cycles();
	unsigned long long spent;
	short index;

	bleed_test_update();
	if (bleed_enabled())
	{
		if ((game_time_get() % 5) == 0)
			bleed_watch();
		for (index = 0; index < GORE_BLEED_MAXIMUM_WATCHED; index++)
		{
			if (gore_bleeds[index].state != _bleed_free)
				bleed_update(&gore_bleeds[index], index);
		}
	}
	/* (what is already on the ground runs its course, settings or not) */
	for (index = 0; index < GORE_BLEED_MAXIMUM_RIVULETS; index++)
	{
		if (gore_rivulets[index].bleed != NONE)
			bleed_rivulet_update(&gore_rivulets[index]);
	}
	for (index = 0; index < GORE_BLEED_MAXIMUM_STAINS; index++)
	{
		if (gore_bleed_stains[index].quad_count > 0)
			bleed_stain_update(&gore_bleed_stains[index]);
	}
	if ((game_time_get() % 15) == 0)
		bleed_dry();
	bleed_check_quads();
	spent = bleed_cycles() - start;
	gore_bleed_cycles += spent;
	gore_bleed_cycles_max = MAX(gore_bleed_cycles_max, spent);
	gore_bleed_cycle_ticks++;
	bleed_cost_report();
}
