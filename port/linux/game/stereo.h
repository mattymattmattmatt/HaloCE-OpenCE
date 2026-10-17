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

#endif
