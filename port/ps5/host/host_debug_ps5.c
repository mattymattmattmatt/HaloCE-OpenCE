/*
HOST_DEBUG_PS5.C

The PS5's part of port/android/host/host_debug.c, which it replaces in the
PS5 build: the thread id, and the guest threads' bookkeeping. Android's
sampler and profiler (config.toml's debug.sample_seconds and
debug.profile_hz) interrupt each guest thread with a signal sent by
tgkill or rt_tgsigqueueinfo and read /proc for the threads' CPU times and
names and the map, none of which a title has; they answer "not on the PS5"
when asked. That is most of host_debug.c (its sampler and profiler are 400
of its 481 lines), so this is a file of the PS5's, not a HALO_PS5 branch
there (port/ps5/PS5.md, phase 2: a branch is for less than a third of a
file).

Each guest thread sets its floating point mode here, as it starts
(host_thread.c calls host_debug_thread_started first thing): the mode is a
thread's own, and the game computes as IEEE.

The thread id is the console's pthread_getthreadid_np, which libkernel
exports to a title: the kernel's id of the calling thread, distinct per
thread (the host checks it once at start-up, host_main.c).
*/

#include "host.h"
#include "ps5_host.h"

#include <ps5platform/fp.h>

#include <pthread.h>
#include <pthread_np.h>

/* the guest threads running now, and the most at once */
static int threads_running, threads_most;

int ps5_thread_id(void)
{
	return pthread_getthreadid_np();
}

/* (also each thread's floating point mode, which a thread of a title starts
with as the title does, flush-to-zero: PS5_FINDINGS.md) */
void host_debug_thread_started(void)
{
	static int logged;
	uint32_t mxcsr = 0;
	int running = __atomic_add_fetch(&threads_running, 1, __ATOMIC_RELAXED);
	int most = __atomic_load_n(&threads_most, __ATOMIC_RELAXED);

	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	ps5_fp_ieee();
	if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
		host_logf(HOST_LOG_INFO, "threads: the first made has MXCSR 0x%08x, set to IEEE (ps5_fp_ieee) as each is", mxcsr);
	while (running > most && !__atomic_compare_exchange_n(&threads_most, &most, running, 0, __ATOMIC_RELAXED,
		__ATOMIC_RELAXED))
		;
}

void host_debug_thread_exited(void)
{
	__atomic_sub_fetch(&threads_running, 1, __ATOMIC_RELAXED);
}

void host_debug_start_sampler(const char *setting)
{
	host_logf(HOST_LOG_INFO, "debug.sample_seconds (%s): the sampler is not on the PS5", setting ? setting : "");
}

void host_debug_start_profiler(int hz, const char *data_root)
{
	(void)data_root;
	if (hz > 0)
		host_logf(HOST_LOG_INFO, "debug.profile_hz (%d): the profiler is not on the PS5", hz);
}
