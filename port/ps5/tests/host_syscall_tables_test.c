/*
HOST_SYSCALL_TABLES_TEST.C

The PS5 host's translations between the guest's Linux interface and the
console's FreeBSD (port/ps5/host/host_syscall_tables.c), tested on the PC
with its own compiler (tools/test_ps5_host.py): the open flags both ways
and round trip, the *at flags, every FreeBSD errno value, the clocks,
utimensat's special times, poll's events both ways, the guest's kernel stat filled from given
fields, and FreeBSD's directory records repacked as Linux's. The FreeBSD
numbers are freebsd_abi.h's, which the console's build checks against the
SDK's headers (host_syscall.c).
*/

#include "host_syscall_tables.h"
#include "freebsd_abi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

/* ---------- open's flags */

static void test_open_flags(void)
{
	static const struct
	{
		uint32_t guest;
		long freebsd;
	} pairs[] =
	{
		{ GUEST_O_RDONLY, 0 },
		{ GUEST_O_WRONLY, 1 },
		{ GUEST_O_RDWR, 2 },
		{ GUEST_O_CREAT, FREEBSD_O_CREAT },
		{ GUEST_O_EXCL, FREEBSD_O_EXCL },
		{ GUEST_O_NOCTTY, FREEBSD_O_NOCTTY },
		{ GUEST_O_TRUNC, FREEBSD_O_TRUNC },
		{ GUEST_O_APPEND, FREEBSD_O_APPEND },
		{ GUEST_O_NONBLOCK, FREEBSD_O_NONBLOCK },
		{ GUEST_O_ASYNC, FREEBSD_O_ASYNC },
		{ GUEST_O_DIRECTORY, FREEBSD_O_DIRECTORY },
		{ GUEST_O_NOFOLLOW, FREEBSD_O_NOFOLLOW },
		{ GUEST_O_DIRECT, FREEBSD_O_DIRECT },
		{ GUEST_O_CLOEXEC, FREEBSD_O_CLOEXEC },
	};
	size_t index;
	uint32_t all = 0;
	long freebsd_all = 0;

	for (index = 0; index < sizeof(pairs) / sizeof(pairs[0]); index++)
	{
		CHECK(ps5_open_flags_in(pairs[index].guest) == pairs[index].freebsd);
		CHECK(ps5_open_flags_out(pairs[index].freebsd) == pairs[index].guest);
		if (pairs[index].guest & ~(uint32_t)GUEST_O_ACCMODE)
		{
			all |= pairs[index].guest;
			freebsd_all |= pairs[index].freebsd;
		}
	}
	/* together, and back: a round trip */
	CHECK(ps5_open_flags_in(all | GUEST_O_RDWR) == (freebsd_all | 2));
	CHECK(ps5_open_flags_out(freebsd_all | 2) == (all | GUEST_O_RDWR));
	/* musl's fopen("w") and opendir */
	CHECK(ps5_open_flags_in(GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC | GUEST_O_LARGEFILE | GUEST_O_CLOEXEC) ==
		(1 | FREEBSD_O_CREAT | FREEBSD_O_TRUNC | FREEBSD_O_CLOEXEC));
	CHECK(ps5_open_flags_in(GUEST_O_RDONLY | GUEST_O_DIRECTORY | GUEST_O_CLOEXEC | GUEST_O_LARGEFILE) ==
		(FREEBSD_O_DIRECTORY | FREEBSD_O_CLOEXEC));
	/* O_SYNC and O_DSYNC are FreeBSD's one O_SYNC, which comes back as O_SYNC */
	CHECK(ps5_open_flags_in(GUEST_O_WRONLY | GUEST_O_DSYNC) == (1 | FREEBSD_O_SYNC));
	CHECK(ps5_open_flags_in(GUEST_O_WRONLY | GUEST_O_SYNC) == (1 | FREEBSD_O_SYNC));
	CHECK(ps5_open_flags_out(FREEBSD_O_SYNC) == GUEST_O_SYNC);
	/* dropped: no FreeBSD flag */
	CHECK(ps5_open_flags_in(GUEST_O_LARGEFILE) == 0);
	CHECK(ps5_open_flags_in(GUEST_O_NOATIME) == 0);
	/* O_PATH: a descriptor for reading, whatever the access asked */
	CHECK(ps5_open_flags_in(GUEST_O_PATH | GUEST_O_RDWR) == 0);
	CHECK(ps5_open_flags_in(GUEST_O_PATH | GUEST_O_DIRECTORY | GUEST_O_CLOEXEC) ==
		(FREEBSD_O_DIRECTORY | FREEBSD_O_CLOEXEC));
	/* a bit the interface does not have */
	CHECK(ps5_open_flags_in(040000000) == -GUEST_EINVAL);
	CHECK(ps5_open_flags_in(0x80000000u) == -GUEST_EINVAL);
	/* FreeBSD's bits the guest has no name for are left out */
	CHECK(ps5_open_flags_out(0x0010 | 0x0020 | 2) == GUEST_O_RDWR);
}

/* ---------- the *at flags */

static void test_at_flags(void)
{
	int empty_path = -1;

	CHECK(ps5_at_flags_in(0, &empty_path) == 0 && empty_path == 0);
	CHECK(ps5_at_flags_in(GUEST_AT_SYMLINK_NOFOLLOW, &empty_path) == FREEBSD_AT_SYMLINK_NOFOLLOW && !empty_path);
	CHECK(ps5_at_flags_in(GUEST_AT_REMOVEDIR, &empty_path) == FREEBSD_AT_REMOVEDIR);
	CHECK(ps5_at_flags_in(GUEST_AT_EMPTY_PATH, &empty_path) == 0 && empty_path == 1);
	CHECK(ps5_at_flags_in(GUEST_AT_EMPTY_PATH | GUEST_AT_SYMLINK_NOFOLLOW, &empty_path) ==
		FREEBSD_AT_SYMLINK_NOFOLLOW && empty_path == 1);
	CHECK(ps5_at_flags_in(0x800, &empty_path) == -GUEST_EINVAL);
	CHECK(GUEST_AT_FDCWD == FREEBSD_AT_FDCWD);
}

/* ---------- errno */

static void test_errno(void)
{
	/* every value FreeBSD defines, and Linux's for it (asm-generic/errno*.h) */
	static const int expected[][2] =
	{
		{ 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 5 }, { 6, 6 }, { 7, 7 }, { 8, 8 }, { 9, 9 }, { 10, 10 },
		{ 11, 35 }, { 12, 12 }, { 13, 13 }, { 14, 14 }, { 15, 15 }, { 16, 16 }, { 17, 17 }, { 18, 18 },
		{ 19, 19 }, { 20, 20 }, { 21, 21 }, { 22, 22 }, { 23, 23 }, { 24, 24 }, { 25, 25 }, { 26, 26 },
		{ 27, 27 }, { 28, 28 }, { 29, 29 }, { 30, 30 }, { 31, 31 }, { 32, 32 }, { 33, 33 }, { 34, 34 },
		{ 35, 11 }, { 36, 115 }, { 37, 114 }, { 38, 88 }, { 39, 89 }, { 40, 90 }, { 41, 91 }, { 42, 92 },
		{ 43, 93 }, { 44, 94 }, { 45, 95 }, { 46, 96 }, { 47, 97 }, { 48, 98 }, { 49, 99 }, { 50, 100 },
		{ 51, 101 }, { 52, 102 }, { 53, 103 }, { 54, 104 }, { 55, 105 }, { 56, 106 }, { 57, 107 },
		{ 58, 108 }, { 59, 109 }, { 60, 110 }, { 61, 111 }, { 62, 40 }, { 63, 36 }, { 64, 112 },
		{ 65, 113 }, { 66, 39 }, { 68, 87 }, { 69, 122 }, { 70, 116 }, { 71, 66 }, { 77, 37 }, { 78, 38 },
		{ 82, 43 }, { 83, 42 }, { 84, 75 }, { 85, 125 }, { 86, 84 }, { 87, 61 }, { 89, 74 }, { 90, 72 },
		{ 91, 67 }, { 92, 71 }, { 95, 131 }, { 96, 130 },
	};
	size_t index;

	for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++)
	{
		if (ps5_errno_out(expected[index][0]) != expected[index][1])
		{
			printf("errno: FreeBSD's %d gives %d, not %d\n", expected[index][0], ps5_errno_out(expected[index][0]),
				expected[index][1]);
			failures++;
		}
	}
	/* the guest's header's values, which the calls return */
	CHECK(ps5_errno_out(FREEBSD_EAGAIN) == GUEST_EAGAIN);
	CHECK(ps5_errno_out(FREEBSD_EDEADLK) == GUEST_EDEADLK);
	CHECK(ps5_errno_out(FREEBSD_ETIMEDOUT) == GUEST_ETIMEDOUT);
	CHECK(ps5_errno_out(FREEBSD_ENOSYS) == GUEST_ENOSYS);
	CHECK(ps5_errno_out(FREEBSD_ENOTEMPTY) == GUEST_ENOTEMPTY);
	CHECK(ps5_errno_out(FREEBSD_EOVERFLOW) == GUEST_EOVERFLOW);
	CHECK(ps5_errno_out(FREEBSD_ELOOP) == GUEST_ELOOP);
	CHECK(ps5_errno_out(FREEBSD_ENAMETOOLONG) == GUEST_ENAMETOOLONG);
	CHECK(ps5_errno_out(FREEBSD_EOPNOTSUPP) == GUEST_EOPNOTSUPP);
	/* FreeBSD's own (EFTYPE, EDOOFUS ...) and anything unknown */
	CHECK(ps5_errno_out(79) == GUEST_EINVAL);
	CHECK(ps5_errno_out(88) == GUEST_EINVAL);
	CHECK(ps5_errno_out(0) == GUEST_EINVAL);
	CHECK(ps5_errno_out(1000) == GUEST_EINVAL);
}

/* ---------- clocks and times */

static void test_clocks(void)
{
	CHECK(ps5_clock_in(0) == FREEBSD_CLOCK_REALTIME);
	CHECK(ps5_clock_in(1) == FREEBSD_CLOCK_MONOTONIC);
	CHECK(ps5_clock_in(2) == FREEBSD_CLOCK_PROCESS_CPUTIME_ID);
	CHECK(ps5_clock_in(3) == FREEBSD_CLOCK_THREAD_CPUTIME_ID);
	CHECK(ps5_clock_in(4) == FREEBSD_CLOCK_MONOTONIC);
	CHECK(ps5_clock_in(5) == FREEBSD_CLOCK_REALTIME_FAST);
	CHECK(ps5_clock_in(6) == FREEBSD_CLOCK_MONOTONIC_FAST);
	CHECK(ps5_clock_in(7) == FREEBSD_CLOCK_UPTIME);
	CHECK(ps5_clock_in(8) == -GUEST_EINVAL);
	CHECK(ps5_clock_in(-1) == -GUEST_EINVAL);
	CHECK(GUEST_TIMER_ABSTIME == FREEBSD_TIMER_ABSTIME);
	CHECK(ps5_utime_nanoseconds_in((1 << 30) - 1) == FREEBSD_UTIME_NOW);
	CHECK(ps5_utime_nanoseconds_in((1 << 30) - 2) == FREEBSD_UTIME_OMIT);
	CHECK(ps5_utime_nanoseconds_in(123456789) == 123456789);
	CHECK(ps5_utime_nanoseconds_in(0) == 0);
}

/* ---------- poll's events */

static void test_poll_events(void)
{
	/* the six of halo_guest_syscall.h are FreeBSD's */
	CHECK(GUEST_POLLIN == FREEBSD_POLLIN && GUEST_POLLPRI == FREEBSD_POLLPRI && GUEST_POLLOUT == FREEBSD_POLLOUT);
	CHECK(GUEST_POLLERR == FREEBSD_POLLERR && GUEST_POLLHUP == FREEBSD_POLLHUP && GUEST_POLLNVAL == FREEBSD_POLLNVAL);
	CHECK(ps5_poll_events_in(GUEST_POLLIN | GUEST_POLLOUT) == (FREEBSD_POLLIN | FREEBSD_POLLOUT));
	/* Linux's POLLRDNORM and POLLRDBAND, POLLWRNORM and POLLWRBAND */
	CHECK(ps5_poll_events_in(0x40 | 0x80) == (FREEBSD_POLLRDNORM | FREEBSD_POLLRDBAND));
	CHECK(ps5_poll_events_in(0x100) == FREEBSD_POLLWRNORM);
	CHECK(ps5_poll_events_in(0x200) == FREEBSD_POLLWRBAND);
	/* POLLMSG, POLLREMOVE and POLLRDHUP are dropped */
	CHECK(ps5_poll_events_in(0x400 | 0x1000 | 0x2000) == 0);
	CHECK(ps5_poll_events_out(FREEBSD_POLLIN | FREEBSD_POLLHUP, GUEST_POLLIN) == (GUEST_POLLIN | GUEST_POLLHUP));
	CHECK(ps5_poll_events_out(FREEBSD_POLLOUT, GUEST_POLLOUT) == GUEST_POLLOUT);
	/* writable, asked as POLLWRNORM alone: POLLWRNORM, not POLLOUT */
	CHECK(ps5_poll_events_out(FREEBSD_POLLOUT, 0x100) == 0x100);
	CHECK(ps5_poll_events_out(FREEBSD_POLLOUT, GUEST_POLLOUT | 0x100) == (GUEST_POLLOUT | 0x100));
	CHECK(ps5_poll_events_out(FREEBSD_POLLWRBAND, 0x200) == 0x200);
	CHECK(ps5_poll_events_out(FREEBSD_POLLNVAL, GUEST_POLLIN) == GUEST_POLLNVAL);
	CHECK(ps5_poll_events_out(FREEBSD_POLLERR, 0) == GUEST_POLLERR);
}

/* ---------- the kernel stat */

static void test_kstat(void)
{
	struct ps5_stat_fields fields;
	struct halo_guest_kstat out;
	unsigned char raw[sizeof(out)];

	memset(&fields, 0, sizeof(fields));
	fields.device = 0x1122334455667788ull;
	fields.inode = 0x99aabbccu;
	fields.mode = 0100644;
	fields.links = 3;
	fields.uid = 1000;
	fields.gid = 1001;
	fields.rdev = 7;
	fields.size = 0x123456789ll;
	fields.block_size = 65536;
	fields.blocks = 4242;
	fields.atime_seconds = 1792281372;
	fields.atime_nanoseconds = 1;
	fields.mtime_seconds = 1792281373;
	fields.mtime_nanoseconds = 2;
	fields.ctime_seconds = 1792281374;
	fields.ctime_nanoseconds = 999999999;
	memset(&out, 0xee, sizeof(out));
	ps5_kstat_fill(&out, &fields);
	memcpy(raw, &out, sizeof(out));
	/* the AArch64 kernel's struct stat, at its offsets (halo_guest_syscall.h) */
	CHECK(sizeof(out) == 128);
	CHECK(*(uint64_t *)(raw + 0) == 0x1122334455667788ull);
	CHECK(*(uint64_t *)(raw + 8) == 0x99aabbccu);
	CHECK(*(uint32_t *)(raw + 16) == 0100644);
	CHECK(*(uint32_t *)(raw + 20) == 3);
	CHECK(*(uint32_t *)(raw + 24) == 1000);
	CHECK(*(uint32_t *)(raw + 28) == 1001);
	CHECK(*(uint64_t *)(raw + 32) == 7);
	CHECK(*(uint64_t *)(raw + 40) == 0);
	CHECK(*(int64_t *)(raw + 48) == 0x123456789ll);
	CHECK(*(int32_t *)(raw + 56) == 65536);
	CHECK(*(int32_t *)(raw + 60) == 0);
	CHECK(*(int64_t *)(raw + 64) == 4242);
	CHECK(*(int64_t *)(raw + 72) == 1792281372);
	CHECK(*(int64_t *)(raw + 80) == 1);
	CHECK(*(int64_t *)(raw + 88) == 1792281373);
	CHECK(*(int64_t *)(raw + 96) == 2);
	CHECK(*(int64_t *)(raw + 104) == 1792281374);
	CHECK(*(int64_t *)(raw + 112) == 999999999);
	CHECK(*(uint32_t *)(raw + 120) == 0 && *(uint32_t *)(raw + 124) == 0);
}

/* ---------- directory records */

/* a FreeBSD record (sys/dirent.h) at out: its length, a multiple of 4 */
static size_t freebsd_record(uint8_t *out, uint32_t inode, uint8_t type, const char *name)
{
	size_t name_length = strlen(name);
	uint16_t length = (uint16_t)((FREEBSD_DIRENT_NAME + name_length + 1 + 3) & ~(size_t)3);

	memset(out, 0, length);
	memcpy(out + FREEBSD_DIRENT_FILENO, &inode, 4);
	memcpy(out + FREEBSD_DIRENT_RECLEN, &length, 2);
	out[FREEBSD_DIRENT_TYPE] = type;
	out[FREEBSD_DIRENT_NAMLEN] = (uint8_t)name_length;
	memcpy(out + FREEBSD_DIRENT_NAME, name, name_length);
	return length;
}

/* a linux_dirent64 at record: its fields */
static void linux_record(const uint8_t *record, uint64_t *inode, int64_t *offset, uint16_t *length, uint8_t *type,
	const char **name)
{
	memcpy(inode, record, 8);
	memcpy(offset, record + 8, 8);
	memcpy(length, record + 16, 2);
	*type = record[18];
	*name = (const char *)record + PS5_DIRENT64_HEADER;
}

static void test_dirents(void)
{
	uint8_t in[512], out[512];
	size_t used = 0, consumed = 0;
	int64_t offset = 0;
	long written;
	uint64_t inode;
	int64_t next;
	uint16_t length;
	uint8_t type;
	const char *name;

	used += freebsd_record(in + used, 2, FREEBSD_DT_DIR, ".");
	used += freebsd_record(in + used, 0, FREEBSD_DT_REG, "deleted");
	used += freebsd_record(in + used, 0x80000001u, FREEBSD_DT_REG, "a30.map");
	used += freebsd_record(in + used, 77, FREEBSD_DT_DIR, "maps.partial");
	memset(out, 0xee, sizeof(out));
	written = ps5_dirents_repack(in, used, out, sizeof(out), &consumed, &offset);
	/* three records (the deleted one skipped), each a multiple of 8 */
	CHECK(consumed == used);
	CHECK(written == 24 + 32 + 32);
	CHECK(offset == 3);
	linux_record(out, &inode, &next, &length, &type, &name);
	CHECK(inode == 2 && next == 1 && length == 24 && type == FREEBSD_DT_DIR && !strcmp(name, "."));
	linux_record(out + 24, &inode, &next, &length, &type, &name);
	CHECK(inode == 0x80000001u && next == 2 && length == 32 && type == FREEBSD_DT_REG && !strcmp(name, "a30.map"));
	linux_record(out + 56, &inode, &next, &length, &type, &name);
	CHECK(inode == 77 && next == 3 && length == 32 && type == FREEBSD_DT_DIR && !strcmp(name, "maps.partial"));

	/* a buffer the second record does not fit: the first, and the rest left */
	offset = 0;
	written = ps5_dirents_repack(in, used, out, 40, &consumed, &offset);
	CHECK(written == 24);
	CHECK(consumed == freebsd_record(out + 100, 2, FREEBSD_DT_DIR, ".") + freebsd_record(out + 100, 0, 0, "deleted"));
	CHECK(offset == 1);
	/* the rest, from there */
	{
		size_t second = consumed;

		written = ps5_dirents_repack(in + second, used - second, out, sizeof(out), &consumed, &offset);
		CHECK(written == 64 && consumed == used - second && offset == 3);
	}
	/* a buffer the first record does not fit: Linux's EINVAL */
	offset = 0;
	CHECK(ps5_dirents_repack(in, used, out, 16, &consumed, &offset) == -GUEST_EINVAL);
	/* a record cut short, or whose length is smaller than its name */
	CHECK(ps5_dirents_repack(in, 6, out, sizeof(out), &consumed, &offset) == -GUEST_EINVAL);
	{
		uint8_t bad[16];
		uint16_t short_length = 8;

		freebsd_record(bad, 5, FREEBSD_DT_REG, "abc");
		memcpy(bad + FREEBSD_DIRENT_RECLEN, &short_length, 2);
		CHECK(ps5_dirents_repack(bad, sizeof(bad), out, sizeof(out), &consumed, &offset) == -GUEST_EINVAL);
	}
	/* nothing in, nothing out */
	CHECK(ps5_dirents_repack(in, 0, out, sizeof(out), &consumed, &offset) == 0 && consumed == 0);
	/* the longest name FreeBSD has (255) */
	{
		static uint8_t long_in[300], long_out[300];
		char long_name[256];

		memset(long_name, 'x', 255);
		long_name[255] = 0;
		used = freebsd_record(long_in, 9, FREEBSD_DT_REG, long_name);
		offset = 0;
		written = ps5_dirents_repack(long_in, used, long_out, sizeof(long_out), &consumed, &offset);
		CHECK(written == ((PS5_DIRENT64_HEADER + 255 + 1 + 7) & ~7));
		CHECK(long_out[PS5_DIRENT64_HEADER + 254] == 'x' && long_out[PS5_DIRENT64_HEADER + 255] == 0);
	}
}

int main(void)
{
	test_open_flags();
	test_at_flags();
	test_errno();
	test_clocks();
	test_poll_events();
	test_kstat();
	test_dirents();
	if (failures)
	{
		printf("%d failures\n", failures);
		return 1;
	}
	printf("host_syscall_tables: all passed\n");
	return 0;
}
