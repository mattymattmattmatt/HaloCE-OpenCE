/*
HOST_SYSCALL_ARM.H

host_syscall of the 32-bit app's host (armeabi-v7a), included by
host_syscall.c in place of its own.

The guest's C library is the same on both ABIs: it numbers its system calls
as AArch64 does (guest/libc/arch/arm64_32/bits/syscall.h.in, here
GUEST_SYS_*, from guest_syscall_numbers.h) and lays out its structures as
the 64-bit host passes them on. On 32-bit ARM the numbers and some calls
differ (fstat64, _llseek, pread64's register pairs...), so each call is
made here with bionic's function for it, converting what differs:

- struct stat: the AArch64 kernel's layout (kstat.h) from bionic's stat64;
- 64-bit file offsets: bionic's *64 functions;
- fcntl's record locks: the 32-bit kernel's 64-bit-offset commands, whose
  struct flock64 is the guest's struct flock;
- the arguments of syscall(), which are 32-bit here.

The open flags, mmap flags, errno values and signal numbers are the same on
ARM and AArch64.
*/

#include "guest_syscall_numbers.h"

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/utsname.h>

/* the AArch64 kernel's struct stat (guest/libc/arch/arm64_32/kstat.h) */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t __pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t __pad2;
	int64_t st_blocks;
	int64_t atime_seconds;
	int64_t atime_nanoseconds;
	int64_t mtime_seconds;
	int64_t mtime_nanoseconds;
	int64_t ctime_seconds;
	int64_t ctime_nanoseconds;
	uint32_t unused[2];
};

_Static_assert(sizeof(struct guest_kstat) == 128, "the AArch64 kernel's struct stat is 128 bytes");

static long stat_out(int result, const struct stat64 *value, uint64_t address)
{
	struct guest_kstat *out = GUEST(struct guest_kstat *, address);

	if (result != 0)
		return -errno;
	memset(out, 0, sizeof(*out));
	out->st_dev = value->st_dev;
	out->st_ino = value->st_ino;
	out->st_mode = value->st_mode;
	out->st_nlink = value->st_nlink;
	out->st_uid = value->st_uid;
	out->st_gid = value->st_gid;
	out->st_rdev = value->st_rdev;
	out->st_size = value->st_size;
	out->st_blksize = (int32_t)value->st_blksize;
	out->st_blocks = (int64_t)value->st_blocks;
	out->atime_seconds = value->st_atim.tv_sec;
	out->atime_nanoseconds = value->st_atim.tv_nsec;
	out->mtime_seconds = value->st_mtim.tv_sec;
	out->mtime_nanoseconds = value->st_mtim.tv_nsec;
	out->ctime_seconds = value->st_ctim.tv_sec;
	out->ctime_nanoseconds = value->st_ctim.tv_nsec;
	return 0;
}

/* a pointer or 32-bit integer argument of syscall(), which is variadic in
longs on this host */
#define ARGUMENT(value) ((long)(uint32_t)(value))

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case GUEST_SYS_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case GUEST_SYS_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);

	case GUEST_SYS_clock_gettime:
	case GUEST_SYS_clock_getres:
	{
		struct timespec value;
		int result = number == GUEST_SYS_clock_gettime ? clock_gettime((clockid_t)a, &value) :
			clock_getres((clockid_t)a, &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case GUEST_SYS_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case GUEST_SYS_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)a, &request))
			return -EFAULT;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -EINTR)
			timespec_out((uint64_t)b, &remaining);
		return result;
	}
	case GUEST_SYS_clock_nanosleep:
	{
		struct timespec request, remaining;
		int result;

		if (!timespec_in((uint64_t)c, &request))
			return -EFAULT;
		result = clock_nanosleep((clockid_t)a, (int)b, &request, &remaining);
		if (result == EINTR)
			timespec_out((uint64_t)d, &remaining);
		return -result;
	}
	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case GUEST_SYS_ppoll:
	{
		struct timespec timeout;
		int has_timeout = timespec_in((uint64_t)c, &timeout);

		return result_of(ppoll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, has_timeout ? &timeout : NULL, NULL));
	}
	case GUEST_SYS_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, c);

		if (guest_times)
		{
			timespec_in((uint64_t)c, &times[0]);
			timespec_in((uint64_t)c + sizeof(struct guest_timespec), &times[1]);
		}
		return result_of(utimensat((int)a, GUEST(const char *, b), guest_times ? times : NULL, (int)d));
	}

	case GUEST_SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		if ((uint64_t)a + (uint64_t)b > 0x100000000ULL)
			return -EINVAL;
		return result_of(madvise(GUEST(void *, a), (size_t)(uint32_t)b, (int)c));
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		/* musl then falls back to mmap and copying */
		return -ENOMEM;

	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);

	case GUEST_SYS_set_tid_address:
		return gettid();
	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case GUEST_SYS_ioctl:
		return -ENOTTY;
	case GUEST_SYS_statx:
	case GUEST_SYS_statfs:
	case GUEST_SYS_fstatfs:
	case GUEST_SYS_clone:
	case GUEST_SYS_clone3:
	case GUEST_SYS_execve:
	case GUEST_SYS_rt_sigtimedwait:
	case GUEST_SYS_pselect6:
	case GUEST_SYS_epoll_pwait:
	case GUEST_SYS_sysinfo:
	case GUEST_SYS_sendmsg:
	case GUEST_SYS_recvmsg:
	case GUEST_SYS_timer_create:
	case GUEST_SYS_timer_settime:
	case GUEST_SYS_timer_gettime:
	case GUEST_SYS_setitimer:
	case GUEST_SYS_getitimer:
	case GUEST_SYS_times:
	case GUEST_SYS_getrusage:
	case GUEST_SYS_wait4:
	case GUEST_SYS_waitid:
		return -ENOSYS;

	/* files */
	case GUEST_SYS_read:
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_openat:
		return result_of(openat((int)a, GUEST(const char *, b), (int)c | O_LARGEFILE, (mode_t)d));
	case GUEST_SYS_close:
		return result_of(close((int)a));
	case GUEST_SYS_lseek:
		return result_of(lseek64((int)a, (off64_t)b, (int)c));
	case GUEST_SYS_pread64:
		return result_of(pread64((int)a, GUEST(void *, b), (size_t)(uint32_t)c, (off64_t)d));
	case GUEST_SYS_pwrite64:
		return result_of(pwrite64((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, (off64_t)d));
	case GUEST_SYS_getdents64:
		/* (struct linux_dirent64 is the same on both) */
		return result_of(syscall(__NR_getdents64, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c)));
	case GUEST_SYS_unlinkat:
		return result_of(unlinkat((int)a, GUEST(const char *, b), (int)c));
	case GUEST_SYS_renameat:
		return result_of(renameat((int)a, GUEST(const char *, b), (int)c, GUEST(const char *, d)));
	case GUEST_SYS_renameat2:
		return result_of(syscall(__NR_renameat2, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c), ARGUMENT(d), ARGUMENT(e)));
	case GUEST_SYS_mkdirat:
		return result_of(mkdirat((int)a, GUEST(const char *, b), (mode_t)c));
	case GUEST_SYS_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case GUEST_SYS_fchmodat:
		return result_of(syscall(__NR_fchmodat, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c)));
	case GUEST_SYS_ftruncate:
		return result_of(ftruncate64((int)a, (off64_t)b));
	case GUEST_SYS_fsync:
		return result_of(fsync((int)a));
	case GUEST_SYS_fdatasync:
		return result_of(fdatasync((int)a));
	case GUEST_SYS_fcntl:
	{
		int command = (int)b;

		/* F_GETLK, F_SETLK, F_SETLKW: the guest's struct flock has 64-bit
		offsets, as F_GETLK64, F_SETLK64 and F_SETLKW64 (12 to 14) take */
		if (command >= 5 && command <= 7)
			command += 7;
		return result_of(syscall(__NR_fcntl64, ARGUMENT(a), (long)command, ARGUMENT(c)));
	}
	case GUEST_SYS_getcwd:
		return result_of(syscall(__NR_getcwd, ARGUMENT(a), ARGUMENT(b)));
	case GUEST_SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case GUEST_SYS_readlinkat:
		return result_of(readlinkat((int)a, GUEST(const char *, b), GUEST(char *, c), (size_t)(uint32_t)d));
	case GUEST_SYS_faccessat:
		return result_of(syscall(__NR_faccessat, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c)));
	case GUEST_SYS_dup:
		return result_of(dup((int)a));
	case GUEST_SYS_dup3:
		return result_of(dup3((int)a, (int)b, (int)c));
	case GUEST_SYS_pipe2:
		return result_of(pipe2(GUEST(int *, a), (int)b));
	case GUEST_SYS_fstat:
	{
		struct stat64 value;

		return stat_out(fstat64((int)a, &value), &value, (uint64_t)b);
	}
	case GUEST_SYS_newfstatat:
	{
		struct stat64 value;

		return stat_out(fstatat64((int)a, GUEST(const char *, b), &value, (int)d), &value, (uint64_t)c);
	}
	case GUEST_SYS_flock:
		return result_of(flock((int)a, (int)b));
	case GUEST_SYS_umask:
		return umask((mode_t)a);

	/* the process */
	case GUEST_SYS_getpid:
		return getpid();
	case GUEST_SYS_getppid:
		return getppid();
	case GUEST_SYS_gettid:
		return gettid();
	case GUEST_SYS_getuid:
		return getuid();
	case GUEST_SYS_geteuid:
		return geteuid();
	case GUEST_SYS_getgid:
		return getgid();
	case GUEST_SYS_getegid:
		return getegid();
	case GUEST_SYS_sched_yield:
		return result_of(sched_yield());
	case GUEST_SYS_getrandom:
		return result_of(syscall(__NR_getrandom, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c)));
	case GUEST_SYS_kill:
		return result_of(kill((pid_t)a, (int)b));
	case GUEST_SYS_tkill:
		return result_of(syscall(__NR_tkill, ARGUMENT(a), ARGUMENT(b)));
	case GUEST_SYS_tgkill:
		return result_of(syscall(__NR_tgkill, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c)));
	case GUEST_SYS_rt_sigprocmask:
		/* (a kernel sigset_t of 8 bytes on both) */
		return result_of(syscall(__NR_rt_sigprocmask, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c), ARGUMENT(d)));
	case GUEST_SYS_uname:
		/* (struct new_utsname: character arrays) */
		return result_of(syscall(__NR_uname, ARGUMENT(a)));
	case GUEST_SYS_prlimit64:
		/* (struct rlimit64: 64-bit members on both) */
		return result_of(syscall(__NR_prlimit64, ARGUMENT(a), ARGUMENT(b), ARGUMENT(c), ARGUMENT(d)));
	case GUEST_SYS_getrlimit:
		/* (the guest's unsigned long[2]: the 32-bit kernel's ugetrlimit) */
		return result_of(syscall(__NR_ugetrlimit, ARGUMENT(a), ARGUMENT(b)));
	case GUEST_SYS_membarrier:
		return result_of(syscall(__NR_membarrier, ARGUMENT(a), ARGUMENT(b)));

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -ENOSYS;
	}
}
