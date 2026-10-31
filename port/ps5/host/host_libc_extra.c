/*
HOST_LIBC_EXTRA.C

What pacbrew's libiconv.a (which SDL2 brings) wants of the C library and no
system module exports to a title: the SDL2 probe found it
(port/ps5/probe/sdl2/build-extra.sh). The rest of what it wants,
nl_langinfo, is bound to the platform layer's by the RADV link recipe the
title is linked with (tools/ps5_title.sh).
*/

int ___mb_cur_max(void);

/* FreeBSD's MB_CUR_MAX: the most bytes a character takes in the current
locale. A title runs in the C locale, where that is one. */
int ___mb_cur_max(void)
{
	return 1;
}
