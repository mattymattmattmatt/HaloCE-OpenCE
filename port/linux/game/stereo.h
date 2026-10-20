/*
STEREO.H
Side-by-side stereoscopic 3D (port/linux/game/stereo.c): the single
player's view drawn twice, once for each eye, into the screen's halves.
*/
#ifndef __STEREO_H
#define __STEREO_H

struct render_camera;

/* display.stereo_sbs at all */
boolean stereo_sbs_enabled(void);
/* the eye cameras' sideways offset from the view's own, in world units
(display.stereo_separation across both eyes) */
real stereo_separation(void);
/* move a camera to one eye's place: eye 0 left, 1 right, from the view
camera main.c built; call on the render and rasterizer cameras alike */
void stereo_eye_apply(struct render_camera *camera, long eye);
/* log the mode once per change of state (main.c's frame, so the log
says what a test ran with) */
void stereo_log_state(long eye_viewport_width, long eye_viewport_height);
/* the menus' draws (render.c, around render_ui_widgets): while an eye
window's menus are drawn, their full-screen design width, so the
rasterizer's 2D transform squeezes the whole 640-wide menu into the
eye's half instead of clipping its right side; 0 outside, and for any
window that is not a stereo eye */
void stereo_ui_begin(union rectangle2d const *viewport_bounds);
void stereo_ui_end(void);
long stereo_ui_design_width(long viewport_width);
/* the menus' pointer (d3d8_gl.c, ui_point_from_window_on): while the
latest menus drawn were an eye's (squeezed into a half), a point of the
screen's drawing (0..its design width) as the menus' own 640-wide x in
whichever eye it falls in, into *menu_x, and TRUE; FALSE while the menus
are drawn whole (mono, the main menu, split screen): the caller maps as
before */
boolean stereo_ui_pointer_x(float screen_x, float *menu_x);

#endif
