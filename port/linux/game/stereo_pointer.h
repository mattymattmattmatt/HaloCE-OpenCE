/*
STEREO_POINTER.H
The side-by-side menus' pointer arithmetic (stereo.c): a point of the
screen's drawing as the menus' 640-wide x in the eye it falls in. No game
types, so that the build machine can test it
(port/linux/tests/stereo_pointer_test.c).
*/
#ifndef __STEREO_POINTER_H
#define __STEREO_POINTER_H

/* each eye's menus are the whole 640-wide design squeezed into its half
(stereo.c): the left eye is 0..eye_width of the screen, the right
eye_width..2*eye_width; a point in either is the same menu point, so a tap
on what either eye shows hits it. Vertically an eye is the screen's whole
height, so y needs nothing. eye_width <= 0: x unchanged */
static inline float stereo_pointer_menu_x(float screen_x, long eye_width, long design_width)
{
	float eye;

	if (eye_width <= 0 || design_width <= 0)
		return screen_x;
	eye = (float)eye_width;
	/* (the right eye's half, the middle column included) */
	if (screen_x >= eye)
		screen_x -= eye;
	return screen_x * (float)design_width / eye;
}

#endif
