/*
HOST_SYSCALL_TABLES.H

The PS5 host's pure translations between the guest's Linux system call
interface (port/android/include/halo_guest_syscall.h) and the console's
FreeBSD (freebsd_abi.h): flags, errno values, clocks and layouts, with no
call to the system, so that they are compiled and tested on the PC as they
are on the console (port/ps5/tests/host_syscall_tables_test.c,
tools/test_ps5_host.py). host_syscall.c makes the calls.
*/

#ifndef PS5_HOST_SYSCALL_TABLES_H
#define PS5_HOST_SYSCALL_TABLES_H

#include <stddef.h>
#include <stdint.h>

#include "halo_guest_syscall.h"

/* open's flags: FreeBSD's for the guest's, or -GUEST_EINVAL for a bit the
guest's interface does not have. O_LARGEFILE and O_NOATIME have no FreeBSD
flag and are dropped, and so are O_PATH (the descriptor is opened for
reading) and O_TMPFILE's own bit; O_DSYNC and O_SYNC are FreeBSD's O_SYNC,
since FreeBSD 11 has no O_DSYNC. */
long ps5_open_flags_in(uint32_t guest);
/* the guest's flags for FreeBSD's (fcntl's F_GETFL); bits the guest's
interface does not have are left out */
uint32_t ps5_open_flags_out(long host);

/* the *at calls' flags: FreeBSD's for the guest's, or -GUEST_EINVAL for a
bit outside AT_SYMLINK_NOFOLLOW, AT_REMOVEDIR and AT_EMPTY_PATH. FreeBSD 11
has no AT_EMPTY_PATH: *empty_path says the guest asked for it (the caller
then works on the descriptor itself) */
long ps5_at_flags_in(uint32_t guest, int *empty_path);

/* Linux's errno value for FreeBSD's (positive both), for every value
FreeBSD defines; EINVAL for one it does not */
int ps5_errno_out(int freebsd);

/* FreeBSD's clock for the guest's Linux clock id, or -GUEST_EINVAL:
REALTIME, MONOTONIC (and MONOTONIC_RAW), the CPU times, the coarse clocks
and BOOTTIME (FreeBSD's UPTIME) */
long ps5_clock_in(int32_t guest);

/* utimensat's nanoseconds: Linux's UTIME_NOW and UTIME_OMIT to FreeBSD's;
others as they are */
long ps5_utime_nanoseconds_in(int32_t guest);

/* poll's events (ppoll): FreeBSD's for the guest's, and the guest's for
FreeBSD's revents, given the events the guest asked for. Linux's
POLLRDNORM and POLLRDBAND are FreeBSD's; its POLLWRNORM is FreeBSD's
POLLOUT, its POLLWRBAND FreeBSD's POLLWRBAND; bits Linux has and FreeBSD
does not (POLLMSG, POLLREMOVE, POLLRDHUP) are dropped. */
uint32_t ps5_poll_events_in(uint32_t guest);
uint32_t ps5_poll_events_out(uint32_t host, uint32_t asked);

/* a file's status, from whichever struct stat the system has */
struct ps5_stat_fields
{
	uint64_t device, inode;
	uint32_t mode, links, uid, gid;
	uint64_t rdev;
	int64_t size, block_size, blocks;
	int64_t atime_seconds, atime_nanoseconds;
	int64_t mtime_seconds, mtime_nanoseconds;
	int64_t ctime_seconds, ctime_nanoseconds;
};

/* the guest's kernel stat: the mode's type and permission bits are the
same on both */
void ps5_kstat_fill(struct halo_guest_kstat *out, const struct ps5_stat_fields *in);

/* Linux's struct linux_dirent64: d_ino, d_off, d_reclen, d_type, then the
name, each record a multiple of 8 bytes */
#define PS5_DIRENT64_HEADER 19

/* Repacks the FreeBSD directory records in [in, in + in_size) (getdents)
as linux_dirent64 records at out, of out_size bytes, for as many whole
records as fit; records of inode 0 (deleted entries) are skipped. Each
record's d_off is *offset after it, counted up by one a record (an opaque
position, as Linux's is). *consumed is the input taken. Returns the bytes
written, or -GUEST_EINVAL when the input is not whole records or the first
record does not fit (Linux's answer to a buffer too small). */
long ps5_dirents_repack(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, size_t *consumed,
	int64_t *offset);

#endif
