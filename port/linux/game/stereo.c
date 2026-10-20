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

Each eye's window bounds are its half too (main.c sets both bounds to
the half, exactly as compute_window_bounds does for split screen), so
the HUD anchors inside the half as a split-screen HUD does. The menus
are the one thing split screen never has to squeeze: they are drawn in
the whole screen's 640-wide design space, so while an eye's menus are
drawn (render.c's render_ui_widgets calls) stereo_ui_begin makes the
rasterizer's 2D transform (rasterizer_xbox_dynavobgeom.c) use the full
design width for that window, and the whole menu lands in the half,
smaller, instead of its right side being clipped away. The Quest VR
build (HaloCE-DecompRemix-VR-PC-Android, vr_render.c) solves the same
problem at the other end - its menus live on a panel in the world -
which a flat SBS screen cannot do; squeezing is the flat equivalent.
Its eye work otherwise confirms this one's shape: it duplicates the
whole player window per eye (windows[eye] = player) and offsets along
the view's right vector (cinema mode), with the runtime's eye poses
about 64 mm apart at the same 3.048 m world unit (vr.world_scale
0.328084 wu per metre, vr.cinema_separation 0.064 m).

Wide screens: the design is as many columns as the display's shape
gives (halo_screen_width), and mono centres the 640-wide menus in it
with an offset (halo_screen_ui_offset). The squeeze maps the whole
design into the eye, offset included in the design's own margins, so
an eye's menus take no offset (stereo_ui_begin) - with it, a 20:9
phone pushed every eye's menu a fifth of the eye to the right.
*/

#include "cseries.h"
#include "stereo.h"
#include "stereo_pointer.h"
#include "cseries/errors.h"
#include "math/real_math.h"
#include "render/render_cameras.h"

#include <math.h>

int config_boolean(const char *name);
double config_real(const char *name); /* port_config.c: a double, not the game's real */
long halo_screen_width(void); /* d3d8_gl.c: the design width (640, more when wide) */
void halo_screen_ui_offset(unsigned char centered); /* d3d8_gl.c: the wide screen's menu centering */

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

static long ui_design_width;
/* the eye's width while the latest menus drawn were an eye's (the right
eye's are the last a frame draws), 0 while they were drawn whole: the
menus' pointer maps by what the screen last showed (stereo_ui_pointer_x) */
static long ui_pointer_eye_width;

void stereo_ui_begin(union rectangle2d const *viewport_bounds)
{
	long width = viewport_bounds ? viewport_bounds->x1 - viewport_bounds->x0 : 0;

	/* the menus' design is the Xbox's 640 columns, however wide the
	screen's design runs (halo_screen_width); mono centres those in
	the wide design with an offset (d3d8_gl.c). An eye takes the 640
	whole: the squeeze maps them onto the eye and the offset stays
	off, or a wide phone's menus sit a fifth of the eye to the right
	(the offset) or left (the margins) */
	ui_design_width = stereo_sbs_enabled() && width > 0 &&
		2 * width == halo_screen_width() ? 640 : 0;
	if (ui_design_width)
		halo_screen_ui_offset(FALSE);
	/* (the menus' taps follow: an eye's menus take a tap on either half;
	menus drawn whole, the main menu's or split screen's, as before) */
	ui_pointer_eye_width = ui_design_width ? width : 0;
}

void stereo_ui_end(void)
{
	ui_design_width = 0;
}

long stereo_ui_design_width(long viewport_width)
{
	return ui_design_width ? ui_design_width : viewport_width;
}

boolean stereo_ui_pointer_x(float screen_x, float *menu_x)
{
	long eye_width = ui_pointer_eye_width;

	/* (the setting turned off since: the menus are drawn whole again at
	the next frame, and a tap until then is the screen's) */
	if (!eye_width || !stereo_sbs_enabled())
		return FALSE;
	*menu_x = stereo_pointer_menu_x(screen_x, eye_width, 640);
	return TRUE;
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
