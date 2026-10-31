/*
HOST_SYSCALL_TABLES.C

The PS5 host's translations between the guest's Linux interface and the
console's FreeBSD, as tables and arithmetic only (host_syscall_tables.h):
no system call, and no header of the system's, so the file compiles the
same on the PC (tools/test_ps5_host.py) and the console. Its FreeBSD
numbers are freebsd_abi.h's, which host_syscall.c checks against the SDK's
headers.
*/

#include "host_syscall_tables.h"
#include "freebsd_abi.h"

#include <string.h>

/* ---------- open's flags */

struct open_flag
{
	uint32_t guest;
	long host;
};

/* each of the guest's bits and FreeBSD's for it (0: dropped); O_SYNC and
O_TMPFILE are a bit of their own with O_DSYNC and O_DIRECTORY */
static const struct open_flag open_flags[] =
{
	{ GUEST_O_CREAT, FREEBSD_O_CREAT },
	{ GUEST_O_EXCL, FREEBSD_O_EXCL },
	{ GUEST_O_NOCTTY, FREEBSD_O_NOCTTY },
	{ GUEST_O_TRUNC, FREEBSD_O_TRUNC },
	{ GUEST_O_APPEND, FREEBSD_O_APPEND },
	{ GUEST_O_NONBLOCK, FREEBSD_O_NONBLOCK },
	{ GUEST_O_DSYNC, FREEBSD_O_SYNC },
	{ GUEST_O_ASYNC, FREEBSD_O_ASYNC },
	{ GUEST_O_DIRECTORY, FREEBSD_O_DIRECTORY },
	{ GUEST_O_NOFOLLOW, FREEBSD_O_NOFOLLOW },
	{ GUEST_O_DIRECT, FREEBSD_O_DIRECT },
	{ GUEST_O_LARGEFILE, 0 },
	{ GUEST_O_NOATIME, 0 },
	{ GUEST_O_CLOEXEC, FREEBSD_O_CLOEXEC },
	{ GUEST_O_SYNC_BIT, FREEBSD_O_SYNC },
	{ GUEST_O_PATH, 0 },
	{ GUEST_O_TMPFILE_BIT, 0 },
};

#define OPEN_FLAG_COUNT ((int)(sizeof(open_flags) / sizeof(open_flags[0])))

long ps5_open_flags_in(uint32_t guest)
{
	uint32_t remaining = guest & ~(uint32_t)GUEST_O_ACCMODE;
	long host = guest & GUEST_O_ACCMODE;
	int index;

	/* (O_PATH asks for a descriptor that only names the file: one opened
	for reading does that, and more) */
	if (guest & GUEST_O_PATH)
		host = GUEST_O_RDONLY;
	for (index = 0; index < OPEN_FLAG_COUNT; index++)
	{
		if (remaining & open_flags[index].guest)
		{
			host |= open_flags[index].host;
			remaining &= ~open_flags[index].guest;
		}
	}
	return remaining ? -GUEST_EINVAL : host;
}

uint32_t ps5_open_flags_out(long host)
{
	uint32_t guest = (uint32_t)(host & FREEBSD_O_ACCMODE);
	int index;

	for (index = 0; index < OPEN_FLAG_COUNT; index++)
	{
		if (open_flags[index].host && (host & open_flags[index].host) == open_flags[index].host)
			guest |= open_flags[index].guest;
	}
	return guest;
}

/* ---------- the *at calls' flags */

long ps5_at_flags_in(uint32_t guest, int *empty_path)
{
	long host = 0;

	*empty_path = (guest & GUEST_AT_EMPTY_PATH) != 0;
	if (guest & ~(uint32_t)(GUEST_AT_SYMLINK_NOFOLLOW | GUEST_AT_REMOVEDIR | GUEST_AT_EMPTY_PATH))
		return -GUEST_EINVAL;
	if (guest & GUEST_AT_SYMLINK_NOFOLLOW)
		host |= FREEBSD_AT_SYMLINK_NOFOLLOW;
	if (guest & GUEST_AT_REMOVEDIR)
		host |= FREEBSD_AT_REMOVEDIR;
	return host;
}

/* ---------- errno */

/* FreeBSD's value, and Linux's (asm-generic/errno*.h) */
static const unsigned char errno_table[][2] =
{
	{ FREEBSD_EPERM, 1 },
	{ FREEBSD_ENOENT, 2 },
	{ FREEBSD_ESRCH, 3 },
	{ FREEBSD_EINTR, 4 },
	{ FREEBSD_EIO, 5 },
	{ FREEBSD_ENXIO, 6 },
	{ FREEBSD_E2BIG, 7 },
	{ FREEBSD_ENOEXEC, 8 },
	{ FREEBSD_EBADF, 9 },
	{ FREEBSD_ECHILD, 10 },
	{ FREEBSD_EDEADLK, 35 },
	{ FREEBSD_ENOMEM, 12 },
	{ FREEBSD_EACCES, 13 },
	{ FREEBSD_EFAULT, 14 },
	{ FREEBSD_ENOTBLK, 15 },
	{ FREEBSD_EBUSY, 16 },
	{ FREEBSD_EEXIST, 17 },
	{ FREEBSD_EXDEV, 18 },
	{ FREEBSD_ENODEV, 19 },
	{ FREEBSD_ENOTDIR, 20 },
	{ FREEBSD_EISDIR, 21 },
	{ FREEBSD_EINVAL, 22 },
	{ FREEBSD_ENFILE, 23 },
	{ FREEBSD_EMFILE, 24 },
	{ FREEBSD_ENOTTY, 25 },
	{ FREEBSD_ETXTBSY, 26 },
	{ FREEBSD_EFBIG, 27 },
	{ FREEBSD_ENOSPC, 28 },
	{ FREEBSD_ESPIPE, 29 },
	{ FREEBSD_EROFS, 30 },
	{ FREEBSD_EMLINK, 31 },
	{ FREEBSD_EPIPE, 32 },
	{ FREEBSD_EDOM, 33 },
	{ FREEBSD_ERANGE, 34 },
	{ FREEBSD_EAGAIN, 11 },
	{ FREEBSD_EINPROGRESS, 115 },
	{ FREEBSD_EALREADY, 114 },
	{ FREEBSD_ENOTSOCK, 88 },
	{ FREEBSD_EDESTADDRREQ, 89 },
	{ FREEBSD_EMSGSIZE, 90 },
	{ FREEBSD_EPROTOTYPE, 91 },
	{ FREEBSD_ENOPROTOOPT, 92 },
	{ FREEBSD_EPROTONOSUPPORT, 93 },
	{ FREEBSD_ESOCKTNOSUPPORT, 94 },
	{ FREEBSD_EOPNOTSUPP, 95 },
	{ FREEBSD_EPFNOSUPPORT, 96 },
	{ FREEBSD_EAFNOSUPPORT, 97 },
	{ FREEBSD_EADDRINUSE, 98 },
	{ FREEBSD_EADDRNOTAVAIL, 99 },
	{ FREEBSD_ENETDOWN, 100 },
	{ FREEBSD_ENETUNREACH, 101 },
	{ FREEBSD_ENETRESET, 102 },
	{ FREEBSD_ECONNABORTED, 103 },
	{ FREEBSD_ECONNRESET, 104 },
	{ FREEBSD_ENOBUFS, 105 },
	{ FREEBSD_EISCONN, 106 },
	{ FREEBSD_ENOTCONN, 107 },
	{ FREEBSD_ESHUTDOWN, 108 },
	{ FREEBSD_ETOOMANYREFS, 109 },
	{ FREEBSD_ETIMEDOUT, 110 },
	{ FREEBSD_ECONNREFUSED, 111 },
	{ FREEBSD_ELOOP, 40 },
	{ FREEBSD_ENAMETOOLONG, 36 },
	{ FREEBSD_EHOSTDOWN, 112 },
	{ FREEBSD_EHOSTUNREACH, 113 },
	{ FREEBSD_ENOTEMPTY, 39 },
	{ FREEBSD_EUSERS, 87 },
	{ FREEBSD_EDQUOT, 122 },
	{ FREEBSD_ESTALE, 116 },
	{ FREEBSD_EREMOTE, 66 },
	{ FREEBSD_ENOLCK, 37 },
	{ FREEBSD_ENOSYS, 38 },
	{ FREEBSD_EIDRM, 43 },
	{ FREEBSD_ENOMSG, 42 },
	{ FREEBSD_EOVERFLOW, 75 },
	{ FREEBSD_ECANCELED, 125 },
	{ FREEBSD_EILSEQ, 84 },
	/* (Linux's ENODATA, which it also calls ENOATTR) */
	{ FREEBSD_ENOATTR, 61 },
	{ FREEBSD_EBADMSG, 74 },
	{ FREEBSD_EMULTIHOP, 72 },
	{ FREEBSD_ENOLINK, 67 },
	{ FREEBSD_EPROTO, 71 },
	{ FREEBSD_ENOTRECOVERABLE, 131 },
	{ FREEBSD_EOWNERDEAD, 130 },
};

int ps5_errno_out(int freebsd)
{
	size_t index;

	for (index = 0; index < sizeof(errno_table) / sizeof(errno_table[0]); index++)
	{
		if (errno_table[index][0] == freebsd)
			return errno_table[index][1];
	}
	return GUEST_EINVAL;
}

/* ---------- clocks */

long ps5_clock_in(int32_t guest)
{
	/* Linux's clock ids (linux/time.h) */
	switch (guest)
	{
	case 0: /* CLOCK_REALTIME */
		return FREEBSD_CLOCK_REALTIME;
	case 1: /* CLOCK_MONOTONIC */
	case 4: /* CLOCK_MONOTONIC_RAW */
		return FREEBSD_CLOCK_MONOTONIC;
	case 2: /* CLOCK_PROCESS_CPUTIME_ID */
		return FREEBSD_CLOCK_PROCESS_CPUTIME_ID;
	case 3: /* CLOCK_THREAD_CPUTIME_ID */
		return FREEBSD_CLOCK_THREAD_CPUTIME_ID;
	case 5: /* CLOCK_REALTIME_COARSE */
		return FREEBSD_CLOCK_REALTIME_FAST;
	case 6: /* CLOCK_MONOTONIC_COARSE */
		return FREEBSD_CLOCK_MONOTONIC_FAST;
	case 7: /* CLOCK_BOOTTIME: the time since start, as FreeBSD's uptime */
		return FREEBSD_CLOCK_UPTIME;
	}
	return -GUEST_EINVAL;
}

long ps5_utime_nanoseconds_in(int32_t guest)
{
	/* Linux's UTIME_NOW and UTIME_OMIT (linux/stat.h) */
	if (guest == (1 << 30) - 1)
		return FREEBSD_UTIME_NOW;
	if (guest == (1 << 30) - 2)
		return FREEBSD_UTIME_OMIT;
	return guest;
}

/* ---------- poll's events */

/* Linux's (asm-generic/poll.h): the first six are halo_guest_syscall.h's */
#define LINUX_POLLRDNORM 0x0040
#define LINUX_POLLRDBAND 0x0080
#define LINUX_POLLWRNORM 0x0100
#define LINUX_POLLWRBAND 0x0200

uint32_t ps5_poll_events_in(uint32_t guest)
{
	uint32_t host = guest & (GUEST_POLLIN | GUEST_POLLPRI | GUEST_POLLOUT | GUEST_POLLERR | GUEST_POLLHUP |
		GUEST_POLLNVAL | LINUX_POLLRDNORM | LINUX_POLLRDBAND);

	if (guest & LINUX_POLLWRNORM)
		host |= FREEBSD_POLLWRNORM;
	if (guest & LINUX_POLLWRBAND)
		host |= FREEBSD_POLLWRBAND;
	return host;
}

uint32_t ps5_poll_events_out(uint32_t host, uint32_t asked)
{
	uint32_t guest = host & (FREEBSD_POLLIN | FREEBSD_POLLPRI | FREEBSD_POLLOUT | FREEBSD_POLLERR | FREEBSD_POLLHUP |
		FREEBSD_POLLNVAL | FREEBSD_POLLRDNORM | FREEBSD_POLLRDBAND);

	/* (FreeBSD's POLLWRNORM is POLLOUT: the guest is told the one it asked for) */
	if (!(asked & GUEST_POLLOUT))
		guest &= ~(uint32_t)FREEBSD_POLLOUT;
	if ((host & FREEBSD_POLLWRNORM) && (asked & LINUX_POLLWRNORM))
		guest |= LINUX_POLLWRNORM;
	if (host & FREEBSD_POLLWRBAND)
		guest |= LINUX_POLLWRBAND;
	return guest;
}

/* ---------- layouts */

void ps5_kstat_fill(struct halo_guest_kstat *out, const struct ps5_stat_fields *in)
{
	memset(out, 0, sizeof(*out));
	out->st_dev = in->device;
	out->st_ino = in->inode;
	out->st_mode = in->mode;
	out->st_nlink = in->links;
	out->st_uid = in->uid;
	out->st_gid = in->gid;
	out->st_rdev = in->rdev;
	out->st_size = in->size;
	out->st_blksize = (int32_t)in->block_size;
	out->st_blocks = in->blocks;
	out->atime_seconds = in->atime_seconds;
	out->atime_nanoseconds = in->atime_nanoseconds;
	out->mtime_seconds = in->mtime_seconds;
	out->mtime_nanoseconds = in->mtime_nanoseconds;
	out->ctime_seconds = in->ctime_seconds;
	out->ctime_nanoseconds = in->ctime_nanoseconds;
}

/* Linux's DT_ values are FreeBSD's (both BSD's), checked here once */
_Static_assert(FREEBSD_DT_UNKNOWN == 0 && FREEBSD_DT_FIFO == 1 && FREEBSD_DT_CHR == 2 && FREEBSD_DT_DIR == 4 &&
	FREEBSD_DT_BLK == 6 && FREEBSD_DT_REG == 8 && FREEBSD_DT_LNK == 10 && FREEBSD_DT_SOCK == 12,
	"FreeBSD's directory entry types are not Linux's");

long ps5_dirents_repack(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, size_t *consumed,
	int64_t *offset)
{
	size_t read = 0, written = 0;

	while (read < in_size)
	{
		uint32_t inode;
		uint16_t record_length;
		uint8_t name_length;
		size_t length;
		uint64_t inode64;
		int64_t next;
		uint16_t out_length;

		if (in_size - read < FREEBSD_DIRENT_NAME)
			return -GUEST_EINVAL;
		memcpy(&inode, in + read + FREEBSD_DIRENT_FILENO, 4);
		memcpy(&record_length, in + read + FREEBSD_DIRENT_RECLEN, 2);
		name_length = in[read + FREEBSD_DIRENT_NAMLEN];
		if (record_length < FREEBSD_DIRENT_NAME + (size_t)name_length + 1 || record_length > in_size - read)
			return -GUEST_EINVAL;
		if (inode)
		{
			length = (PS5_DIRENT64_HEADER + (size_t)name_length + 1 + 7) & ~(size_t)7;
			if (length > out_size - written)
			{
				if (!written)
					return -GUEST_EINVAL;
				break;
			}
			memset(out + written, 0, length);
			inode64 = inode;
			next = *offset + 1;
			out_length = (uint16_t)length;
			memcpy(out + written, &inode64, 8);
			memcpy(out + written + 8, &next, 8);
			memcpy(out + written + 16, &out_length, 2);
			out[written + 18] = in[read + FREEBSD_DIRENT_TYPE];
			memcpy(out + written + PS5_DIRENT64_HEADER, in + read + FREEBSD_DIRENT_NAME, name_length);
			*offset = next;
			written += length;
		}
		read += record_length;
	}
	*consumed = read;
	return (long)written;
}
