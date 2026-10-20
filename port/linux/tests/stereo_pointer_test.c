/*
STEREO_POINTER_TEST.C

Tests of the side-by-side menus' pointer (port/linux/game/stereo_pointer.h).
Built and run by tools/test_stereo_pointer.py; exits nonzero on a failure.
*/

#include "../game/stereo_pointer.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK_NEAR(value, expected) \
	do \
	{ \
		float got = (value); \
		if (fabsf(got - (expected)) > 0.01f) \
		{ \
			printf("FAIL %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, #value, got, (float)(expected)); \
			failures++; \
		} \
	} while (0)

int main(void)
{
	/* the Xbox's 640x480: each eye 320 wide */
	CHECK_NEAR(stereo_pointer_menu_x(0.0f, 320, 640), 0.0f);
	CHECK_NEAR(stereo_pointer_menu_x(160.0f, 320, 640), 320.0f);
	CHECK_NEAR(stereo_pointer_menu_x(480.0f, 320, 640), 320.0f);
	CHECK_NEAR(stereo_pointer_menu_x(319.5f, 320, 640), 639.0f);
	CHECK_NEAR(stereo_pointer_menu_x(320.0f, 320, 640), 0.0f);
	CHECK_NEAR(stereo_pointer_menu_x(639.5f, 320, 640), 639.0f);

	/* a 20:9 phone (the Redmi Note 13 Pro+'s 2712x1220 and the S23's
	2340x1080): the design is 1066 or 1040 columns, each eye half of it;
	a menu point is the same in either eye */
	{
		static const long designs[] = { 1066, 1040, 854 };
		int d;

		for (d = 0; d < 3; d++)
		{
			long eye = designs[d] / 2;
			float menu;

			for (menu = 0.0f; menu < 640.0f; menu += 37.0f)
			{
				float left = menu * (float)eye / 640.0f;
				float right = (float)eye + left;

				CHECK_NEAR(stereo_pointer_menu_x(left, eye, 640), menu);
				CHECK_NEAR(stereo_pointer_menu_x(right, eye, 640), menu);
			}
			/* the menu's middle, in each eye's middle */
			CHECK_NEAR(stereo_pointer_menu_x((float)eye / 2.0f, eye, 640), 320.0f);
			CHECK_NEAR(stereo_pointer_menu_x((float)eye * 1.5f, eye, 640), 320.0f);
		}
	}

	/* the old (single screen) mapping of a right-eye tap, for contrast: a
	1066-wide design's mono menus are centred at 213; a tap on the right
	eye's centre (799.5) was menu x 586, now 320 */
	CHECK_NEAR(799.5f - 213.0f, 586.5f);
	CHECK_NEAR(stereo_pointer_menu_x(799.5f, 533, 640), 320.0f);

	/* no eye: unchanged */
	CHECK_NEAR(stereo_pointer_menu_x(123.0f, 0, 640), 123.0f);

	if (failures)
		printf("%d failure(s)\n", failures);
	else
		printf("stereo pointer: all passed\n");
	return failures != 0;
}
