/*
LIBC_EXTRA.C

What pacbrew's libiconv.a (which SDL2 brings) wants of the C library and no
system module exports to a title (build-extra.sh says how it was found).
*/

#ifndef SDL2_PLAIN_LINK

int ___mb_cur_max(void);

/* FreeBSD's MB_CUR_MAX: the most bytes a character takes in the current
locale. A title runs in the C locale, where that is one. */
int ___mb_cur_max(void)
{
	return 1;
}

#endif
