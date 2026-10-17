/*
STEREO.C
Side-by-side (SBS) stereoscopic 3D, for cardboard-style VR viewers and
SBS 3D displays: with display.stereo_sbs on, the one local player's view
is rendered twice a frame - a camera at each eye - into the left and
right halves of the screen (main.c, main_game_render, builds two player
windows for the one player instead of one).

Method: parallel cameras offset sideways, never toed in. Each eye's
window keeps the whole screen as its window bounds and takes half of it
as its viewport, and render_camera_build_frustum_bounds derives the
eye's frustum from exactly that pairing - so each eye gets the correct
asymmetric (off-axis) frustum for its half, the projection the halves
of one shared full-screen frustum, with no projection code touched.
The HUD and the menus a player window draws go through the same window
(render_window, render.c), so both eyes see them, each in its half.

Eye distance: Halo's world unit is 3.048 m (a metre is 1/3.048 wu;
gore.c's gravity says the same), so the default display.stereo_separation
0.021 wu is a 64 mm interpupillary distance across both eyes, each eye
0.0105 wu from the view's own camera. (The Quest/OpenXR builds aim at
the same 64 mm; there is no VR build in this repo to borrow code from -
branches checked 2026-10-18 - so the constant is taken from the scale,
not from code.) Separation is a config value because viewers and
displays differ; 0 gives mono in two halves, which is the quick check
that the two views are the same picture.

Guards (main.c): one local player only - two or more keep split screen
as it is; cinematics and anything that forces a single screen are
untouched, as are screenshots and films. The main menu on its own (no
player window) draws full-screen as before: only a player view is
doubled. In split-screen the setting simply does nothing.

Settings: display.stereo_sbs (Settings > Video Setup on Android, and
config.toml everywhere), display.stereo_separation (world units across
both eyes). The view's own field of view is unchanged; each half draws
at the half's aspect, as a split-screen view does.
*/

#include "cseries.h"
#include "stereo.h"
#include "cseries/errors.h"
#include "math/real_math.h"
#include "render/render_cameras.h"

#include <math.h>

int config_boolean(const char *name);
double config_real(const char *name); /* port_config.c: a double, not the game's real */

boolean stereo_sbs_enabled(void)
{
	return config_boolean("display.stereo_sbs");
}

real stereo_separation(void)
{
	real separation = (real)config_real("display.stereo_separation");

	return separation > 0.f ? separation : 0.f;
}

void stereo_eye_apply(struct render_camera *camera, long eye)
{
	real_vector3d right;
	real offset;

	if (!camera)
		return;
	/* sideways, level with the view: forward x up, normalised (the
	observer's up is not quite unit everywhere) */
	cross_product3d(&camera->forward, &camera->up, &right);
	if (normalize3d(&right) == 0.f)
		return;
	offset = (eye ? 0.5f : -0.5f) * stereo_separation();
	camera->position.x += right.i * offset;
	camera->position.y += right.j * offset;
	camera->position.z += right.k * offset;
}

void stereo_log_state(long eye_viewport_width, long eye_viewport_height)
{
	static boolean logged;
	boolean on = stereo_sbs_enabled();
	real separation = stereo_separation();

	if (logged == on)
		return;
	logged = on;
	if (on)
		error(_error_silent, "[Stereo] SBS on: separation %.3f wu (%.0f mm across the eyes), each eye drawn in %ldx%ld",
			separation, separation * 3048.f, eye_viewport_width, eye_viewport_height);
	else
		error(_error_silent, "[Stereo] SBS off");
}
