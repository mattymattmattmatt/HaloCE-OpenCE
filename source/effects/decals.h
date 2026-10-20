/*
DECALS.H

header included in hcex build.
*/

#ifndef __DECALS_H
#define __DECALS_H
#pragma once

/* ---------- headers */

#include "cseries/cseries.h"
#include "math/real_math.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

struct collision_result;
struct decal_editor_geometry;

/* ---------- prototypes/DECALS.C */

void decals_initialize(
	void);
void decals_initialize_for_new_map(
	void);
void decals_unlock(
	boolean permanent);
void decal_delete(
	long decal_index);
long decal_get_first_decal_index(
	short cluster_index,
	short layer);
void decal_new_from_media_collision(
	long decal_definition_index,
	struct collision_result const *collision,
	real_vector3d const *velocity,
	real radius_modifier,
	boolean permanent,
	short forced_sequence_index,
	struct decal_editor_geometry *editor_geometry);
void render_debug_decals(
	void);
void decal_new_from_collision(
	long decal_definition_index,
	struct collision_result const *collision,
	real_vector3d const *velocity,
	real radius_modifier,
	boolean permanent,
	short forced_sequence_index,
	struct decal_editor_geometry *editor_geometry);
void decal_new(
	long decal_definition_index,
	real_point3d const *origin,
	real_vector3d const *velocity,
	real radius_modifier,
	boolean permanent,
	short forced_sequence_index,
	struct decal_editor_geometry *editor_geometry);
pixel32 real_a_rgb_color_to_pixel32(
	real alpha,
	real_rgb_color const *color);

/* ---------- globals */

extern boolean decals_enabled;
extern boolean debug_decals;
/* port: a decal made while this is over 0 lasts at least this many seconds
(the gore mod's blood) */
extern real decal_lifetime_minimum;
/* port: decals refused (collisions off the structure) and the decals now */
extern long decal_refused_count;
long decal_count_get(void);

/* port: one-quad decals the caller moves (blood on vehicles) */
long decal_quad_new(long decal_definition_index, short cluster_index, real_point3d const corners[4], real alpha);
boolean decal_quad_set(long decal_index, real_point3d const corners[4]);
void decal_quad_delete(long decal_index);
void decal_quads_delete_except(long const *keep, long keep_count);
/* port: the gore mod's corpse bleed (port/linux/game/gore_bleed.c): a quad
of a sprite sequence, its own texture coordinates (within the sprite, 0 to
1) and color; its color and alpha later */
long decal_quad_new_ex(long decal_definition_index, short sequence_index, short cluster_index,
	real_point3d const corners[4], real_point2d const uv[4], real_rgb_color const *color, real alpha);
void decal_quad_set_color(long decal_index, real_rgb_color const *color, real alpha);
short decal_definition_sequence_count(long decal_definition_index);
short decal_definition_blend_function(long decal_definition_index);
void decal_quads_delete_unmarked(byte const *marked); /* (marked: by absolute index, HALO_PORT_MAXIMUM_DECALS) */
boolean decal_quad_exists(long decal_index);
void decal_definitions_report(void);

/* ---------- public code */

void decals_dispose(
	void);
void decals_dispose_from_old_map(
	void);
void decals_update(
	void);
void decals_delete_permanent_from_cluster(
	short cluster_index);
void decals_disconnect_from_structure_bsp(
	void);
void decals_reconnect_to_structure_bsp(
	void);

#endif // __DECALS_H
