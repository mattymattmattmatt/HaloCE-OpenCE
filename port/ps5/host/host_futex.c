/*
HOST_FUTEX.C

Futexes for the PS5 port: the guest's musl synchronises its threads with
Linux's futex call on 32-bit words in guest memory, and the console's
kernel, FreeBSD's, has _umtx_op instead, which libkernel exports to a
title. Phase 0 B of port/ps5/PS5.md proved the forms used here on the
console, from threads whose stacks are guest memory: the 32-bit private wait
and wake, a wake reaching its waiter in 5 to 6 us, and the relative and
absolute timeouts ending in ETIMEDOUT. The table there is what this file
does:

- FUTEX_WAIT compares the word first and answers -EAGAIN on a mismatch:
  FreeBSD's wait returns 0 for a word that already differs, where Linux's
  says EAGAIN. Then UMTX_OP_WAIT_UINT_PRIVATE, with the relative timeout as
  a struct _umtx_time on CLOCK_MONOTONIC, or none. (UMTX_OP_WAIT compares
  64 bits: never on the guest's words.)
- FUTEX_WAIT_BITSET is the same with an absolute timeout on the clock the
  guest named (FUTEX_CLOCK_REALTIME, else monotonic); the bitset is ignored,
  as the Switch's does: musl only ever waits on all bits.
- FUTEX_WAKE and _BITSET are UMTX_OP_WAKE_PRIVATE, and answer 0: FreeBSD
  does not say how many it woke, and musl's callers test only for -ENOSYS.
- FUTEX_REQUEUE and FUTEX_CMP_REQUEUE wake the count asked for on the first
  word and every waiter on the second, a superset of moving them, which
  musl's loops tolerate (they re-test their own condition).
- The priority-inheritance operations answer -ENOSYS, as on the Switch:
  musl then takes its plain path.

FUTEX_PRIVATE_FLAG is ignored (the guest is one process, and the private
forms are always used) and FUTEX_CLOCK_REALTIME is taken off the operation
first. The word is the guest's address below 4 GB, used as it is.
*/

#include "ps5_host.h"
#include "freebsd_abi.h"
#include "host_syscall_tables.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/umtx.h>
#include <time.h>

_Static_assert(FREEBSD_UMTX_OP_WAIT_UINT_PRIVATE == UMTX_OP_WAIT_UINT_PRIVATE, "sys/umtx.h's UMTX_OP_WAIT_UINT_PRIVATE");
_Static_assert(FREEBSD_UMTX_OP_WAKE_PRIVATE == UMTX_OP_WAKE_PRIVATE, "sys/umtx.h's UMTX_OP_WAKE_PRIVATE");
_Static_assert(FREEBSD_UMTX_ABSTIME == UMTX_ABSTIME, "sys/umtx.h's UMTX_ABSTIME");

/* Linux's futex operations (linux/futex.h), the guest's */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define GUEST_WORD(address) ((volatile uint32_t *)(uintptr_t)(uint32_t)(address))

/* the guest's timeout, a 32-bit struct timespec (halo_guest_syscall.h) */
static void timeout_in(uint64_t address, struct timespec *timeout)
{
	const struct halo_guest_timespec *value = (const struct halo_guest_timespec *)(uintptr_t)(uint32_t)address;

	timeout->tv_sec = value->seconds;
	timeout->tv_nsec = value->nanoseconds;
}

static long futex_wait(uint32_t address, uint32_t value, uint64_t timeout_address, int absolute, int realtime)
{
	struct _umtx_time timeout;

	if (*GUEST_WORD(address) != value)
		return -GUEST_EAGAIN;
	if (!timeout_address)
	{
		if (_umtx_op((void *)(uintptr_t)address, UMTX_OP_WAIT_UINT_PRIVATE, value, NULL, NULL) == 0)
			return 0;
	}
	else
	{
		memset(&timeout, 0, sizeof(timeout));
		timeout_in(timeout_address, &timeout._timeout);
		timeout._flags = absolute ? UMTX_ABSTIME : 0;
		timeout._clockid = realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC;
		/* (the size of the timeout in uaddr says which struct it is) */
		if (_umtx_op((void *)(uintptr_t)address, UMTX_OP_WAIT_UINT_PRIVATE, value, (void *)sizeof(timeout),
			&timeout) == 0)
		{
			return 0;
		}
	}
	return -ps5_errno_out(errno);
}

static void futex_wake(uint32_t address, int count)
{
	if (count > 0)
		_umtx_op((void *)(uintptr_t)address, UMTX_OP_WAKE_PRIVATE, (u_long)count, NULL, NULL);
}

long host_futex(uint32_t address, int operation, uint32_t value, uint64_t timeout_address, uint64_t address2,
	uint32_t value3)
{
	int realtime = (operation & FUTEX_CLOCK_REALTIME) != 0;
	int command = operation & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

	switch (command)
	{
	case FUTEX_WAIT:
		/* (a relative timeout, on the monotonic clock whatever the flag) */
		return futex_wait(address, value, timeout_address, 0, 0);
	case FUTEX_WAIT_BITSET:
		return futex_wait(address, value, timeout_address, 1, realtime);
	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		futex_wake(address, (int)value);
		return 0;
	case FUTEX_CMP_REQUEUE:
		if (*GUEST_WORD(address) != value3)
			return -GUEST_EAGAIN;
		/* FALLTHROUGH: the rest is FUTEX_REQUEUE's */
	case FUTEX_REQUEUE:
		/* (the fourth argument is the count to move, not a timeout) */
		futex_wake(address, (int)value);
		futex_wake((uint32_t)address2, 0x7fffffff);
		(void)timeout_address;
		return 0;
	case FUTEX_WAKE_OP:
		/* (musl never asks; the wake it can do, as the Switch's) */
		futex_wake(address, (int)value);
		return 0;
	}
	/* FUTEX_LOCK_PI, FUTEX_UNLOCK_PI, FUTEX_TRYLOCK_PI, FUTEX_WAIT_REQUEUE_PI,
	FUTEX_CMP_REQUEUE_PI, FUTEX_LOCK_PI2 and anything newer */
	return -GUEST_ENOSYS;
}
