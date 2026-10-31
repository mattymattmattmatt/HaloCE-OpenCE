/*
FREEBSD_ABI.H

The numbers of the console's system (FreeBSD's, as the payload SDK fork's
headers have them) that the PS5 host translates the guest's Linux interface
to (port/android/include/halo_guest_syscall.h). They are written here as
literals, each with the SDK header it comes from, so that the translations
(host_syscall_tables.c) compile and are tested on the PC, whose own headers
are Linux's (tools/test_ps5_host.py). host_syscall.c, compiled against the
SDK's headers, checks every one of them with _Static_assert, so the PC's
test and the console cannot disagree.

Nothing here is copied from the SDK: these are the system's numbers, one
per line. The console's kernel is FreeBSD 11's, and so are its layouts: a
32-bit inode in struct stat, and directory records of 32-bit inodes with
an 8-bit name length (sys/dirent.h).
*/

#ifndef PS5_FREEBSD_ABI_H
#define PS5_FREEBSD_ABI_H

/* ---------- open's flags (fcntl.h) */

#define FREEBSD_O_ACCMODE 0x0003
#define FREEBSD_O_NONBLOCK 0x0004
#define FREEBSD_O_APPEND 0x0008
#define FREEBSD_O_ASYNC 0x0040
/* O_SYNC and O_FSYNC are the same bit; FreeBSD 11 has no O_DSYNC */
#define FREEBSD_O_SYNC 0x0080
#define FREEBSD_O_NOFOLLOW 0x0100
#define FREEBSD_O_CREAT 0x0200
#define FREEBSD_O_TRUNC 0x0400
#define FREEBSD_O_EXCL 0x0800
#define FREEBSD_O_NOCTTY 0x8000
#define FREEBSD_O_DIRECT 0x00010000
#define FREEBSD_O_DIRECTORY 0x00020000
#define FREEBSD_O_CLOEXEC 0x00100000

/* ---------- the *at calls' flags (fcntl.h); AT_FDCWD is -100 on both */

#define FREEBSD_AT_FDCWD (-100)
#define FREEBSD_AT_SYMLINK_NOFOLLOW 0x200
#define FREEBSD_AT_REMOVEDIR 0x800

/* ---------- fcntl's commands (fcntl.h) */

#define FREEBSD_F_DUPFD 0
#define FREEBSD_F_GETFD 1
#define FREEBSD_F_SETFD 2
#define FREEBSD_F_GETFL 3
#define FREEBSD_F_SETFL 4
#define FREEBSD_F_DUPFD_CLOEXEC 17
#define FREEBSD_FD_CLOEXEC 1

/* ---------- errno (sys/errno.h): the values a file, memory, clock or
futex call can leave, and the rest of the table host_syscall_tables.c
translates */

#define FREEBSD_EPERM 1
#define FREEBSD_ENOENT 2
#define FREEBSD_ESRCH 3
#define FREEBSD_EINTR 4
#define FREEBSD_EIO 5
#define FREEBSD_ENXIO 6
#define FREEBSD_E2BIG 7
#define FREEBSD_ENOEXEC 8
#define FREEBSD_EBADF 9
#define FREEBSD_ECHILD 10
/* (Linux's 11 is EAGAIN: the first 34 agree but for this one) */
#define FREEBSD_EDEADLK 11
#define FREEBSD_ENOMEM 12
#define FREEBSD_EACCES 13
#define FREEBSD_EFAULT 14
#define FREEBSD_ENOTBLK 15
#define FREEBSD_EBUSY 16
#define FREEBSD_EEXIST 17
#define FREEBSD_EXDEV 18
#define FREEBSD_ENODEV 19
#define FREEBSD_ENOTDIR 20
#define FREEBSD_EISDIR 21
#define FREEBSD_EINVAL 22
#define FREEBSD_ENFILE 23
#define FREEBSD_EMFILE 24
#define FREEBSD_ENOTTY 25
#define FREEBSD_ETXTBSY 26
#define FREEBSD_EFBIG 27
#define FREEBSD_ENOSPC 28
#define FREEBSD_ESPIPE 29
#define FREEBSD_EROFS 30
#define FREEBSD_EMLINK 31
#define FREEBSD_EPIPE 32
#define FREEBSD_EDOM 33
#define FREEBSD_ERANGE 34
#define FREEBSD_EAGAIN 35
#define FREEBSD_EINPROGRESS 36
#define FREEBSD_EALREADY 37
#define FREEBSD_ENOTSOCK 38
#define FREEBSD_EDESTADDRREQ 39
#define FREEBSD_EMSGSIZE 40
#define FREEBSD_EPROTOTYPE 41
#define FREEBSD_ENOPROTOOPT 42
#define FREEBSD_EPROTONOSUPPORT 43
#define FREEBSD_ESOCKTNOSUPPORT 44
#define FREEBSD_EOPNOTSUPP 45
#define FREEBSD_EPFNOSUPPORT 46
#define FREEBSD_EAFNOSUPPORT 47
#define FREEBSD_EADDRINUSE 48
#define FREEBSD_EADDRNOTAVAIL 49
#define FREEBSD_ENETDOWN 50
#define FREEBSD_ENETUNREACH 51
#define FREEBSD_ENETRESET 52
#define FREEBSD_ECONNABORTED 53
#define FREEBSD_ECONNRESET 54
#define FREEBSD_ENOBUFS 55
#define FREEBSD_EISCONN 56
#define FREEBSD_ENOTCONN 57
#define FREEBSD_ESHUTDOWN 58
#define FREEBSD_ETOOMANYREFS 59
#define FREEBSD_ETIMEDOUT 60
#define FREEBSD_ECONNREFUSED 61
#define FREEBSD_ELOOP 62
#define FREEBSD_ENAMETOOLONG 63
#define FREEBSD_EHOSTDOWN 64
#define FREEBSD_EHOSTUNREACH 65
#define FREEBSD_ENOTEMPTY 66
#define FREEBSD_EUSERS 68
#define FREEBSD_EDQUOT 69
#define FREEBSD_ESTALE 70
#define FREEBSD_EREMOTE 71
#define FREEBSD_ENOLCK 77
#define FREEBSD_ENOSYS 78
#define FREEBSD_EIDRM 82
#define FREEBSD_ENOMSG 83
#define FREEBSD_EOVERFLOW 84
#define FREEBSD_ECANCELED 85
#define FREEBSD_EILSEQ 86
#define FREEBSD_ENOATTR 87
#define FREEBSD_EBADMSG 89
#define FREEBSD_EMULTIHOP 90
#define FREEBSD_ENOLINK 91
#define FREEBSD_EPROTO 92
#define FREEBSD_ENOTRECOVERABLE 95
#define FREEBSD_EOWNERDEAD 96

/* ---------- clocks (time.h) */

#define FREEBSD_CLOCK_REALTIME 0
#define FREEBSD_CLOCK_MONOTONIC 4
#define FREEBSD_CLOCK_UPTIME 5
#define FREEBSD_CLOCK_REALTIME_FAST 10
#define FREEBSD_CLOCK_MONOTONIC_FAST 12
#define FREEBSD_CLOCK_THREAD_CPUTIME_ID 14
#define FREEBSD_CLOCK_PROCESS_CPUTIME_ID 15
#define FREEBSD_TIMER_ABSTIME 1

/* utimensat's special nanoseconds (sys/stat.h) */
#define FREEBSD_UTIME_NOW (-1)
#define FREEBSD_UTIME_OMIT (-2)

/* ---------- directory records (sys/dirent.h): getdents fills a buffer
with these, each d_reclen long */

#define FREEBSD_DIRENT_FILENO 0 /* uint32_t */
#define FREEBSD_DIRENT_RECLEN 4 /* uint16_t */
#define FREEBSD_DIRENT_TYPE 6   /* uint8_t */
#define FREEBSD_DIRENT_NAMLEN 7 /* uint8_t */
#define FREEBSD_DIRENT_NAME 8
#define FREEBSD_DT_UNKNOWN 0
#define FREEBSD_DT_FIFO 1
#define FREEBSD_DT_CHR 2
#define FREEBSD_DT_DIR 4
#define FREEBSD_DT_BLK 6
#define FREEBSD_DT_REG 8
#define FREEBSD_DT_LNK 10
#define FREEBSD_DT_SOCK 12

/* ---------- futexes (sys/umtx.h): the 32-bit private wait and wake that
phase 0 B proved (UMTX_OP_WAIT, 2, compares 64 bits and is never used) */

#define FREEBSD_UMTX_OP_WAIT_UINT_PRIVATE 15
#define FREEBSD_UMTX_OP_WAKE_PRIVATE 16
#define FREEBSD_UMTX_ABSTIME 0x01

/* ---------- poll's events (poll.h): the first eight bits are Linux's
too; FreeBSD's POLLWRNORM is POLLOUT, and its POLLWRBAND Linux's
POLLWRNORM's bit */

#define FREEBSD_POLLIN 0x0001
#define FREEBSD_POLLPRI 0x0002
#define FREEBSD_POLLOUT 0x0004
#define FREEBSD_POLLERR 0x0008
#define FREEBSD_POLLHUP 0x0010
#define FREEBSD_POLLNVAL 0x0020
#define FREEBSD_POLLRDNORM 0x0040
#define FREEBSD_POLLRDBAND 0x0080
#define FREEBSD_POLLWRNORM 0x0004
#define FREEBSD_POLLWRBAND 0x0100

/* ---------- memory (sys/mman.h): the host's own mmap flags, which
__wrap_mmap reads (host_memory.c) */

#define FREEBSD_MAP_PRIVATE 0x0002
#define FREEBSD_MAP_FIXED 0x0010
#define FREEBSD_MAP_ANON 0x1000

#endif
