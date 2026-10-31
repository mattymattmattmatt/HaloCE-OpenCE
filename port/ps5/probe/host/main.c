/*
PS5 HOST PROBE

What the game's PS5 host will stand on, asked of the console in the state
the host will be in: the guest's memory mapped first (the image's 64 MB at
0x70000000, read, write and execute; the Xbox window's 512 MB at the first
256 MB boundary from 0x80000000 where it fits; Custom Edition's 23 MB at
0x40440000; a 256 MB pool at 0x10000000; all direct memory, fixed and
zeroed), RADV linked in (PS5_Vulkan's link recipe, as the host will link
it), then:

1. Futexes: FreeBSD's _umtx_op on a 32-bit word in the pool, waited on by
   threads whose stacks are in the pool: the 32-bit wait when the word
   already differs, a wake's latency, the timeouts, a wake of four, the
   _PRIVATE forms, and the 64-bit UMTX_OP_WAIT on a 32-bit word (wrong).
2. Faults: a write to a write-protected page of the window, with
   sceKernelMprotect and then libc's mprotect, and of flexible memory, on a
   thread whose stack is guest memory, handled on a sigaltstack: every field
   the handler sees; 1,000 cycles of protect, fault, unprotect; a read of
   an unmapped address below 4 GB.
3. The data folder: /data/halo (made empty by the user over FTP), then
   /app0, for comparison: stat, listing, mkdir, chmod, writes of 16 MB and
   100 MB, three ways to read 100 MB, stat of a file, rename, unlink, rmdir,
   access, realpath; whether /download0 is there and writable; and getcwd,
   asked last of all (it crashed the second run).
4. The host's memory: flexible memory and direct memory before anything,
   after the guest's ranges, while malloc takes 16 MB at a time up to 8 GB,
   after; then Vulkan: the device, its memory heaps and types, and which
   pool vkAllocateMemory takes 256 MB of device-local and 64 MB of
   host-visible memory from.
5. The display: VK_KHR_display's displays, modes and planes;
   vkCreateDisplayModeKHR for 1080p and 720p at 59.94 and 60 Hz and 4K at
   60; each mode's plane capabilities; a plane surface on the native mode,
   its capabilities, formats and present modes; a FIFO swapchain presenting
   60 frames of changing gray, the last 50 timed; and the same on a 1080p
   mode if there is one.
6. Leaving and returning: 90 seconds of presenting with the pad open, while
   the person at the console presses the PS button and comes back and opens
   and closes the Quick Menu: the frames and their intervals each second,
   every acquire or present that is not VK_SUCCESS, the pad's connected and
   intercepted bits, the system service's events; and whether the swapchain
   presents afterwards.

The sections that may kill the process are guarded: the section's name goes
to /app0/host-skip.txt before it runs and the file is deleted after, so a
run that crashes skips that section the next time it is started, and says
so. The Vulkan sequences are written from the Vulkan specification.

Build: port/ps5/probe/title/build.sh port/ps5/probe/host (build-extra.sh
       takes RADV's link recipe)
Run:   make /data/halo over FTP (empty), then
       port/ps5/probe/title/deploy.sh 192.168.2.157 PPSA99103 and start
       "Halo PS5 host probe" from the home screen; do section 6's steps
       when the notification asks.

The report goes to the kernel log and /app0/host-probe.txt (over FTP,
/data/homebrew/PPSA99103/host-probe.txt).
*/

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "probe_report.h"

#include "halo_android_abi.h"

#include <ps5platform/context.h>
#include <ps5platform/fp.h>
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/umtx.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MEGABYTE 0x100000ULL
#define DIRECT_UNIT 0x10000ULL
#define PROT_CPU_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define PROT_CPU_RWX (PROT_CPU_RW | PS5_KERNEL_PROT_CPU_EXEC)
#define POOL_BASE 0x10000000ULL
#define POOL_SIZE (256 * MEGABYTE)
#define THREAD_STACK_SIZE (256 * 1024ULL)
#define SKIP_FILE "/app0/host-skip.txt"

/* ---------- declarations the SDK has no header for */

/* RADV's archive: the ICD's entry point (the loader's vkGetInstanceProcAddr
is not in it) */
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

/* the user service and the pad, as the io probe declares them (PS5
RetroArch's input_ps5.cpp) */
int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user_id);
int32_t scePadInit(void);
int32_t scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int32_t scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int32_t scePadRead(int32_t handle, void *samples, int32_t capacity);
int32_t scePadClose(int32_t handle);

/* one pad sample (the io probe's layout, 120 bytes): only the buttons and
the connected flag are read here */
struct pad_sample
{
	uint32_t buttons;
	uint8_t before_connected[0x48];
	int32_t connected;
	uint8_t after_connected[0x28];
};
_Static_assert(sizeof(struct pad_sample) == 120, "the console's pad samples are 120 bytes");
#define PAD_INTERCEPTED 0x80000000u

/* The system service's event queue and status, from the PS4's public
homebrew SDKs (OpenOrbis's SceSystemServiceEvent and
SceSystemServiceStatus): each takes one pointer to a structure the call
fills. The event starts with its type (an int32) and the status with the
count of events waiting (an int32), then flags a byte each, the first
isSystemUiOverlaid; the full sizes are not known with confidence, so each
is given a buffer far larger than either and only those first fields are
read. */
int32_t sceSystemServiceReceiveEvent(void *event);
int32_t sceSystemServiceGetStatus(void *status);

/* ---------- the crash guard */

static char skipped[64];

static void guard_load(void)
{
	FILE *file = fopen(SKIP_FILE, "r");

	if (!file)
		return;
	if (fgets(skipped, sizeof(skipped), file))
		skipped[strcspn(skipped, "\r\n")] = 0;
	fclose(file);
	unlink(SKIP_FILE);
	if (skipped[0])
		say("the last run did not finish \"%s\": it is skipped this time\n", skipped);
}

/* 1 if the section is to run (its name is then on file until guard_end) */
static int guard_begin(const char *section)
{
	FILE *file;

	if (!strcmp(section, skipped))
	{
		say("%s: skipped, the last run crashed in it (its kernel log says how)\n", section);
		return 0;
	}
	file = fopen(SKIP_FILE, "w");
	if (file)
	{
		fprintf(file, "%s\n", section);
		fclose(file);
		chmod(SKIP_FILE, 0666);
	}
	return 1;
}

static void guard_end(void)
{
	unlink(SKIP_FILE);
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* ---------- memory figures */

static void say_memory(const char *when)
{
	size_t flexible = 0;
	int64_t direct_start = 0;
	size_t direct_largest = 0;
	struct ps5_heap_stats heap;
	int32_t flexible_result, direct_result;

	flexible_result = sceKernelAvailableFlexibleMemorySize(&flexible);
	direct_result = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), DIRECT_UNIT, &direct_start,
		&direct_largest);
	memset(&heap, 0, sizeof(heap));
	ps5_heap_stats(&heap);
	say("memory %s: flexible available %.1f MB (0x%08x); direct memory's largest free block %.1f MB at 0x%llx "
		"(0x%08x); the platform heap maps %.1f MB, at most %.1f MB, %llu fallbacks to libc\n",
		when, (double)flexible / MEGABYTE, (unsigned)flexible_result, (double)direct_largest / MEGABYTE,
		(unsigned long long)direct_start, (unsigned)direct_result, (double)heap.mapped_bytes / MEGABYTE,
		(double)heap.peak_bytes / MEGABYTE, heap.libc_fallbacks);
}

/* ---------- the guest's ranges (the guest probe's map_direct and
reserve_exactly) */

/* reserves size bytes exactly at address, or nothing: 0, or the kernel's
result, or -1 when it placed the range elsewhere */
static int32_t reserve_exactly(uint64_t address, uint64_t size)
{
	void *at = (void *)address;
	int32_t result = sceKernelReserveVirtualRange(&at, size, 0, DIRECT_UNIT);

	if (result != 0)
		return result;
	if ((uint64_t)at != address)
	{
		sceKernelMunmap(at, size);
		return -1;
	}
	return 0;
}

/* reserves [address, address + size), allocates direct memory and maps it
there, read and write (and execute when asked), zeroed; 1 on success */
static int map_direct(const char *what, uint64_t address, uint64_t size, int executable)
{
	int64_t physical = 0;
	void *at;
	int32_t result;
	uint64_t start = now_us();

	result = reserve_exactly(address, size);
	if (result != 0)
	{
		say("%s: reserving 0x%llx-0x%llx: %s 0x%08x\n", what, (unsigned long long)address,
			(unsigned long long)(address + size), result == -1 ? "placed elsewhere," : "refused,", (unsigned)result);
		return 0;
	}
	result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, DIRECT_UNIT,
		PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
	if (result != 0)
	{
		say("%s: allocating %llu MB of direct memory: 0x%08x\n", what, (unsigned long long)(size / MEGABYTE),
			(unsigned)result);
		sceKernelMunmap((void *)address, size);
		return 0;
	}
	at = (void *)address;
	result = sceKernelMapDirectMemory(&at, size, PROT_CPU_RW, PS5_KERNEL_MAP_FIXED, physical, DIRECT_UNIT);
	if (result != 0 || (uint64_t)at != address)
	{
		say("%s: mapping at 0x%llx: 0x%08x, placed at %p\n", what, (unsigned long long)address, (unsigned)result,
			at);
		sceKernelMunmap((void *)address, size);
		sceKernelReleaseDirectMemory(physical, size);
		return 0;
	}
	if (executable)
	{
		result = sceKernelMprotect((void *)address, size, PROT_CPU_RWX);
		if (result != 0)
		{
			say("%s: sceKernelMprotect(read, write, execute): 0x%08x\n", what, (unsigned)result);
			return 0;
		}
	}
	/* (direct memory comes back holding what it held before) */
	memset((void *)address, 0, size);
	say("%s: 0x%llx-0x%llx, %llu MB of direct memory at physical 0x%llx%s, zeroed, in %llu ms\n", what,
		(unsigned long long)address, (unsigned long long)(address + size), (unsigned long long)(size / MEGABYTE),
		(unsigned long long)physical, executable ? ", read, write and execute" : "",
		(unsigned long long)((now_us() - start) / 1000));
	return 1;
}

static int image_ready, window_ready, pool_ready;
static uint64_t window_base, pool_base, pool_next, pool_end;

static void map_guest_ranges(void)
{
	heading("the guest's ranges");
	image_ready = map_direct("image", HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE, 1);
	for (window_base = HALO_GUEST_WINDOW_BASE; window_base + HALO_GUEST_WINDOW_SIZE <= 0x100000000ULL;
		window_base += HALO_GUEST_WINDOW_ALIGNMENT)
		if ((window_ready = map_direct("Xbox window", window_base, HALO_GUEST_WINDOW_SIZE, 0)))
			break;
	if (!window_ready)
		window_base = 0;
	map_direct("Custom Edition's tag cache", HALO_GUEST_CUSTOM_EDITION_BASE,
		HALO_GUEST_CUSTOM_EDITION_END - HALO_GUEST_CUSTOM_EDITION_BASE, 0);
	pool_ready = map_direct("pool", POOL_BASE, POOL_SIZE, 0);
	if (pool_ready)
		pool_base = POOL_BASE;
	pool_next = pool_base;
	pool_end = pool_base + POOL_SIZE;
}

static uint64_t pool_take(uint64_t size)
{
	uint64_t taken;

	size = (size + DIRECT_UNIT - 1) & ~(DIRECT_UNIT - 1);
	if (!pool_ready || pool_end - pool_next < size)
		return 0;
	taken = pool_next;
	pool_next += size;
	return taken;
}

/* a thread whose stack is in the pool, as the guest's threads will be */
static int start_low_thread(pthread_t *thread, void *(*function)(void *), void *argument)
{
	pthread_attr_t attributes;
	uint64_t stack = pool_take(THREAD_STACK_SIZE);
	int result;

	if (!stack)
		return -1;
	pthread_attr_init(&attributes);
	result = pthread_attr_setstack(&attributes, (void *)stack, THREAD_STACK_SIZE);
	if (result == 0)
		result = pthread_create(thread, &attributes, function, argument);
	pthread_attr_destroy(&attributes);
	return result;
}

/* ---------- 1. futexes */

struct waiter
{
	volatile uint32_t *word;
	int operation;
	uint32_t value;
	void *size_or_null;
	void *timeout;
	volatile int ready;
	volatile int done;
	int result;
	int error;
	uint64_t returned_us;
	uint64_t stack_address;
};

static void *waiter_thread(void *argument)
{
	struct waiter *waiter = argument;
	int marker;

	waiter->stack_address = (uint64_t)(uintptr_t)&marker;
	waiter->ready = 1;
	errno = 0;
	waiter->result = _umtx_op((void *)waiter->word, waiter->operation, waiter->value, waiter->size_or_null,
		waiter->timeout);
	waiter->error = errno;
	waiter->returned_us = now_us();
	waiter->done = 1;
	return NULL;
}

static void wait_until_ready(struct waiter *waiters, int count)
{
	int index;

	for (index = 0; index < count; index++)
		while (!waiters[index].ready)
			usleep(100);
	/* (the flag is set just before the call: give it time to sleep) */
	usleep(20000);
}

static void futex_set(const char *label, int wait_operation, int wake_operation, volatile uint32_t *word)
{
	uint64_t latencies[10];
	int run, result, error, index;
	uint64_t start, took;

	say("-- %s (wait %d, wake %d)\n", label, wait_operation, wake_operation);

	/* the word already differs from the value */
	*word = 1;
	errno = 0;
	start = now_us();
	result = _umtx_op((void *)word, wait_operation, 0, NULL, NULL);
	error = errno;
	took = now_us() - start;
	say("wait for 0 on a word holding 1: returned %d, errno %d (%s), in %llu us\n", result, error,
		result ? strerror(error) : "-", (unsigned long long)took);

	/* one waiter, woken: the time from the wake to the waiter's return */
	for (run = 0; run < 10; run++)
	{
		struct waiter waiter;
		pthread_t thread;
		uint64_t woken_at;

		memset(&waiter, 0, sizeof(waiter));
		*word = 0;
		waiter.word = word;
		waiter.operation = wait_operation;
		waiter.value = 0;
		if (start_low_thread(&thread, waiter_thread, &waiter) != 0)
		{
			say("a waiter thread could not start\n");
			return;
		}
		wait_until_ready(&waiter, 1);
		if (waiter.done)
			say("run %d: the waiter returned before the wake (%d, errno %d)\n", run, waiter.result, waiter.error);
		*word = 1;
		woken_at = now_us();
		result = _umtx_op((void *)word, wake_operation, 1, NULL, NULL);
		pthread_join(thread, NULL);
		latencies[run] = waiter.returned_us > woken_at ? waiter.returned_us - woken_at : 0;
		if (run == 0)
			say("one waiter (its stack at 0x%llx): the wake returned %d; the waiter returned %d, errno %d\n",
				(unsigned long long)waiter.stack_address, result, waiter.result, waiter.error);
	}
	qsort(latencies, 10, sizeof(latencies[0]), compare_u64);
	say("wake to the waiter's return, 10 runs: min %llu us, median %llu us, max %llu us\n",
		(unsigned long long)latencies[0], (unsigned long long)latencies[5], (unsigned long long)latencies[9]);

	/* a timeout, relative, as a struct timespec */
	{
		struct timespec timeout = { 0, 100000000 };

		*word = 0;
		errno = 0;
		start = now_us();
		result = _umtx_op((void *)word, wait_operation, 0, (void *)sizeof(timeout), &timeout);
		error = errno;
		say("wait with a relative struct timespec of 100 ms: returned %d, errno %d (%s), after %llu us\n", result,
			error, result ? strerror(error) : "-", (unsigned long long)(now_us() - start));
	}
	/* struct _umtx_time, relative, then absolute on CLOCK_MONOTONIC */
	{
		struct _umtx_time timeout;

		memset(&timeout, 0, sizeof(timeout));
		timeout._timeout.tv_nsec = 100000000;
		timeout._clockid = CLOCK_MONOTONIC;
		errno = 0;
		start = now_us();
		result = _umtx_op((void *)word, wait_operation, 0, (void *)sizeof(timeout), &timeout);
		error = errno;
		say("wait with a relative struct _umtx_time of 100 ms (CLOCK_MONOTONIC): returned %d, errno %d (%s), "
			"after %llu us\n",
			result, error, result ? strerror(error) : "-", (unsigned long long)(now_us() - start));
		clock_gettime(CLOCK_MONOTONIC, &timeout._timeout);
		timeout._timeout.tv_nsec += 100000000;
		if (timeout._timeout.tv_nsec >= 1000000000)
		{
			timeout._timeout.tv_sec++;
			timeout._timeout.tv_nsec -= 1000000000;
		}
		timeout._flags = UMTX_ABSTIME;
		errno = 0;
		start = now_us();
		result = _umtx_op((void *)word, wait_operation, 0, (void *)sizeof(timeout), &timeout);
		error = errno;
		say("wait with struct _umtx_time, UMTX_ABSTIME, now + 100 ms on CLOCK_MONOTONIC: returned %d, errno %d "
			"(%s), after %llu us\n",
			result, error, result ? strerror(error) : "-", (unsigned long long)(now_us() - start));
	}

	/* four waiters, all woken */
	{
		struct waiter waiters[4];
		pthread_t threads[4];
		int returned = 0;

		memset(waiters, 0, sizeof(waiters));
		*word = 0;
		for (index = 0; index < 4; index++)
		{
			waiters[index].word = word;
			waiters[index].operation = wait_operation;
			if (start_low_thread(&threads[index], waiter_thread, &waiters[index]) != 0)
			{
				say("waiter %d could not start\n", index);
				return;
			}
		}
		wait_until_ready(waiters, 4);
		*word = 1;
		result = _umtx_op((void *)word, wake_operation, INT_MAX, NULL, NULL);
		error = errno;
		usleep(50000);
		for (index = 0; index < 4; index++)
			returned += waiters[index].done;
		say("four waiters, wake INT_MAX: the wake returned %d (errno %d); %d of 4 had returned 50 ms later\n",
			result, result ? error : 0, returned);
		if (returned < 4)
		{
			/* wake the rest, so the threads can be joined */
			_umtx_op((void *)word, wake_operation, INT_MAX, NULL, NULL);
		}
		for (index = 0; index < 4; index++)
			pthread_join(threads[index], NULL);
	}
}

static void probe_futexes(void)
{
	volatile uint32_t *word;

	heading("1. futexes (_umtx_op)");
	if (!pool_ready)
	{
		say("skipped: no pool\n");
		return;
	}
	if (!guard_begin("1. futexes"))
		return;
	word = (volatile uint32_t *)(uintptr_t)pool_take(DIRECT_UNIT);
	say("the word at %p (in the pool); _umtx_op is linked, so libkernel exports it\n", (void *)word);
	futex_set("UMTX_OP_WAIT_UINT, UMTX_OP_WAKE", UMTX_OP_WAIT_UINT, UMTX_OP_WAKE, word);
	futex_set("UMTX_OP_WAIT_UINT_PRIVATE, UMTX_OP_WAKE_PRIVATE", UMTX_OP_WAIT_UINT_PRIVATE, UMTX_OP_WAKE_PRIVATE,
		word);

	/* the 64-bit compare on a 32-bit word whose neighbour is not zero */
	{
		struct timespec timeout = { 0, 100000000 };
		uint64_t start;
		int result, error;

		say("-- UMTX_OP_WAIT (a long, 64 bits) on a 32-bit word\n");
		word[0] = 0;
		word[1] = 1;
		errno = 0;
		start = now_us();
		result = _umtx_op((void *)word, UMTX_OP_WAIT, 0, (void *)sizeof(timeout), &timeout);
		error = errno;
		say("wait for 0 on a word holding 0 whose next word holds 1, 100 ms timeout: returned %d, errno %d (%s), "
			"after %llu us\n",
			result, error, result ? strerror(error) : "-", (unsigned long long)(now_us() - start));
		say("(it compares the 64 bits from the word's address, 0x100000000 here, so a waiter whose 32-bit word "
			"matches does not sleep when the next word is not zero: not to be used on the guest's words)\n");
		word[1] = 0;
	}
	guard_end();
}

/* ---------- 2. faults */

enum fault_fix
{
	FIX_SCE_KERNEL_MPROTECT,
	FIX_MPROTECT,
	FIX_JUMP,
};

struct fault_record
{
	int signal_number;
	int code;
	uint64_t address;
	uint64_t rip, rsp, rbp, rax, rdi, err, trapno, addr, flags;
	uint64_t handler_stack;
	int on_alternate_stack;
	int fix_result;
};

static volatile enum fault_fix fault_fix;
static volatile uint64_t fault_page;
static volatile int fault_count;
static struct fault_record fault_record;
static sigjmp_buf fault_jump;
static uint64_t alternate_low, alternate_high;
static uint64_t page_size;

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
	const mcontext_t *m = &((const ucontext_t *)context)->uc_mcontext;
	int marker;

	fault_count++;
	if (fault_count == 1)
	{
		fault_record.signal_number = signal_number;
		fault_record.code = info->si_code;
		fault_record.address = (uint64_t)(uintptr_t)info->si_addr;
		fault_record.rip = (uint64_t)m->mc_rip;
		fault_record.rsp = (uint64_t)m->mc_rsp;
		fault_record.rbp = (uint64_t)m->mc_rbp;
		fault_record.rax = (uint64_t)m->mc_rax;
		fault_record.rdi = (uint64_t)m->mc_rdi;
		fault_record.err = (uint64_t)m->mc_err;
		fault_record.trapno = (uint64_t)m->mc_trapno;
		fault_record.addr = (uint64_t)m->mc_addr;
		fault_record.flags = (uint64_t)m->mc_flags;
		fault_record.handler_stack = (uint64_t)(uintptr_t)&marker;
		fault_record.on_alternate_stack = fault_record.handler_stack >= alternate_low &&
			fault_record.handler_stack < alternate_high;
	}
	switch (fault_fix)
	{
	case FIX_SCE_KERNEL_MPROTECT:
		fault_record.fix_result = sceKernelMprotect((void *)fault_page, page_size, PROT_CPU_RW);
		break;
	case FIX_MPROTECT:
		fault_record.fix_result = mprotect((void *)fault_page, page_size, PROT_READ | PROT_WRITE);
		break;
	case FIX_JUMP:
		siglongjmp(fault_jump, 1);
	}
	if (fault_record.fix_result != 0 && fault_count > 3)
	{
		/* the unprotect failed, so the write faults again: stop it
		repeating forever */
		signal(signal_number, SIG_DFL);
	}
}

static const char *signal_name(int signal_number)
{
	return signal_number == SIGSEGV ? "SIGSEGV" : signal_number == SIGBUS ? "SIGBUS" : "another signal";
}

static void say_fault_record(void)
{
	const struct fault_record *r = &fault_record;

	say("  signal %d (%s), si_code %d, si_addr 0x%llx\n", r->signal_number, signal_name(r->signal_number), r->code,
		(unsigned long long)r->address);
	say("  mc_rip 0x%llx mc_rsp 0x%llx mc_rbp 0x%llx mc_rax 0x%llx mc_rdi 0x%llx\n", (unsigned long long)r->rip,
		(unsigned long long)r->rsp, (unsigned long long)r->rbp, (unsigned long long)r->rax, (unsigned long long)r->rdi);
	say("  mc_err 0x%llx mc_trapno %llu mc_addr 0x%llx mc_flags 0x%llx\n", (unsigned long long)r->err,
		(unsigned long long)r->trapno, (unsigned long long)r->addr, (unsigned long long)r->flags);
	say("  the handler ran on 0x%llx: %s the alternate stack (0x%llx-0x%llx); the interrupted stack is %s 4 GB\n",
		(unsigned long long)r->handler_stack, r->on_alternate_stack ? "inside" : "NOT inside",
		(unsigned long long)alternate_low, (unsigned long long)alternate_high, r->rsp < 0x100000000ULL ? "below" : "above");
}

enum fault_test
{
	TEST_SCE_KERNEL_MPROTECT,
	TEST_MPROTECT,
	TEST_FLEXIBLE,
	TEST_CYCLES_SCE,
	TEST_CYCLES_MPROTECT,
	TEST_UNMAPPED,
};

struct fault_job
{
	enum fault_test test;
	uint64_t page;
};

static void install_alternate_stack(void)
{
	stack_t alternate;

	alternate.ss_size = 64 * 1024;
	alternate.ss_sp = malloc(alternate.ss_size);
	alternate.ss_flags = 0;
	if (!alternate.ss_sp || sigaltstack(&alternate, NULL) != 0)
	{
		say("sigaltstack: failed (%s)\n", strerror(errno));
		return;
	}
	alternate_low = (uint64_t)(uintptr_t)alternate.ss_sp;
	alternate_high = alternate_low + alternate.ss_size;
}

static int protect_read_only(enum fault_test test, uint64_t page)
{
	if (test == TEST_SCE_KERNEL_MPROTECT || test == TEST_CYCLES_SCE)
		return sceKernelMprotect((void *)page, page_size, PS5_KERNEL_PROT_CPU_READ);
	return mprotect((void *)page, page_size, PROT_READ);
}

static void *fault_thread(void *argument)
{
	struct fault_job *job = argument;
	volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)(job->page + 0x100);
	int marker;
	int result;

	install_alternate_stack();
	say("  the faulting thread's stack is at %p\n", (void *)&marker);
	memset(&fault_record, 0, sizeof(fault_record));
	fault_count = 0;
	fault_page = job->page;
	switch (job->test)
	{
	case TEST_SCE_KERNEL_MPROTECT:
	case TEST_MPROTECT:
	case TEST_FLEXIBLE:
		fault_fix = job->test == TEST_SCE_KERNEL_MPROTECT ? FIX_SCE_KERNEL_MPROTECT : FIX_MPROTECT;
		*word = 0x11111111;
		result = protect_read_only(job->test, job->page);
		say("  %s(0x%llx, %llu bytes, read only): %d (0x%08x)%s%s\n",
			job->test == TEST_SCE_KERNEL_MPROTECT ? "sceKernelMprotect" : "mprotect", (unsigned long long)job->page,
			(unsigned long long)page_size, result, (unsigned)result, result == -1 ? ", " : "",
			result == -1 ? strerror(errno) : "");
		*word = 0x22222222;
		say("  the write: %d fault%s; then the value reads 0x%08x (%s)\n", fault_count, fault_count == 1 ? "" : "s",
			*word, *word == 0x22222222 ? "the write completed" : "WRONG");
		if (fault_count)
		{
			say_fault_record();
			say("  the handler's %s(read, write): %d\n",
				fault_fix == FIX_SCE_KERNEL_MPROTECT ? "sceKernelMprotect" : "mprotect", fault_record.fix_result);
		}
		break;
	case TEST_CYCLES_SCE:
	case TEST_CYCLES_MPROTECT:
	{
		uint64_t start, protect_total = 0;
		int cycle, failures = 0;

		fault_fix = job->test == TEST_CYCLES_SCE ? FIX_SCE_KERNEL_MPROTECT : FIX_MPROTECT;
		start = now_us();
		for (cycle = 0; cycle < 1000; cycle++)
		{
			uint64_t before = now_us();

			if (protect_read_only(job->test, job->page) != 0)
				failures++;
			protect_total += now_us() - before;
			*word = (uint32_t)cycle;
		}
		say("  1000 cycles of %s read only, write, fault, the handler's unprotect: %llu us, %.2f us a cycle "
			"(the protect alone %.2f us); %d faults, %d failed protects; the value reads %u\n",
			job->test == TEST_CYCLES_SCE ? "sceKernelMprotect" : "mprotect", (unsigned long long)(now_us() - start),
			(now_us() - start) / 1000.0, protect_total / 1000.0, fault_count, failures, *word);
		break;
	}
	case TEST_UNMAPPED:
		fault_fix = FIX_JUMP;
		if (sigsetjmp(fault_jump, 1) == 0)
		{
			volatile uint32_t value = *(volatile uint32_t *)(uintptr_t)job->page;

			say("  the read of 0x%llx returned 0x%08x, no fault\n", (unsigned long long)job->page, value);
		}
		else
		{
			say("  the read of 0x%llx faulted:\n", (unsigned long long)job->page);
			say_fault_record();
		}
		break;
	}
	return NULL;
}

static void run_fault_test(const char *section, enum fault_test test, uint64_t page)
{
	struct fault_job job;
	pthread_t thread;
	int result;

	say("-- %s\n", section);
	if (!guard_begin(section))
		return;
	job.test = test;
	job.page = page;
	result = start_low_thread(&thread, fault_thread, &job);
	if (result != 0)
		say("the thread could not start: %d\n", result);
	else
		pthread_join(thread, NULL);
	guard_end();
}

static void probe_faults(void)
{
	struct sigaction action;
	void *flexible;

	heading("2. faults");
	page_size = (uint64_t)sysconf(_SC_PAGESIZE);
	say("sysconf(_SC_PAGESIZE): %llu\n", (unsigned long long)page_size);
	if (page_size == 0 || page_size > 0x10000)
		page_size = 0x4000;
	if (!window_ready || !pool_ready)
	{
		say("skipped: no window or no pool\n");
		return;
	}
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fault_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	say("sigaction(SIGSEGV): %d; sigaction(SIGBUS): %d\n", sigaction(SIGSEGV, &action, NULL),
		sigaction(SIGBUS, &action, NULL));

	run_fault_test("2. faults: sceKernelMprotect on the window", TEST_SCE_KERNEL_MPROTECT, window_base + 0x100000);
	run_fault_test("2. faults: mprotect on the window", TEST_MPROTECT, window_base + 0x200000);
	flexible = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (flexible == MAP_FAILED)
		say("an anonymous mmap of flexible memory: failed (%s)\n", strerror(errno));
	else
	{
		say("an anonymous mmap of flexible memory: %p\n", flexible);
		run_fault_test("2. faults: mprotect on flexible memory", TEST_FLEXIBLE, (uint64_t)(uintptr_t)flexible);
	}
	run_fault_test("2. faults: 1000 cycles, sceKernelMprotect", TEST_CYCLES_SCE, window_base + 0x300000);
	run_fault_test("2. faults: 1000 cycles, mprotect", TEST_CYCLES_MPROTECT, window_base + 0x400000);
	run_fault_test("2. faults: a read of unmapped 0x08000000", TEST_UNMAPPED, 0x08000000ULL);
	if (flexible != MAP_FAILED)
		munmap(flexible, page_size);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
}

/* ---------- 3. the data folder */

#define CHUNK MEGABYTE

static void fill_chunk(uint8_t *buffer, uint64_t chunk)
{
	uint64_t index;

	for (index = 0; index < CHUNK; index++)
		buffer[index] = (uint8_t)(chunk * 31 + index * 7 + (index >> 12));
}

static double megabytes_per_second(uint64_t bytes, uint64_t microseconds)
{
	return microseconds ? (double)bytes / MEGABYTE / (microseconds / 1e6) : 0.0;
}

static int write_file(const char *path, uint64_t chunks, uint8_t *buffer)
{
	FILE *file;
	uint64_t chunk, start, took;
	int result;

	file = fopen(path, "wb");
	if (!file)
	{
		say("fopen(%s, wb): %s\n", path, strerror(errno));
		return 0;
	}
	start = now_us();
	for (chunk = 0; chunk < chunks; chunk++)
	{
		fill_chunk(buffer, chunk);
		if (fwrite(buffer, 1, CHUNK, file) != CHUNK)
		{
			say("fwrite of chunk %llu: %s\n", (unsigned long long)chunk, strerror(errno));
			fclose(file);
			return 0;
		}
	}
	fflush(file);
	result = fsync(fileno(file));
	took = now_us() - start;
	fclose(file);
	say("wrote %s: %llu MB in 1 MB fwrites, fflush, fsync (%d): %.3f s, %.1f MB/s\n", path,
		(unsigned long long)chunks, result, took / 1e6, megabytes_per_second(chunks * CHUNK, took));
	result = chmod(path, 0666);
	say("chmod 0666: %d%s%s\n", result, result ? ", " : "", result ? strerror(errno) : "");
	return 1;
}

static void check_ends(const char *how, const uint8_t *first, const uint8_t *last, uint64_t chunks)
{
	static uint8_t expected[CHUNK];
	uint8_t want_first, want_last;

	fill_chunk(expected, 0);
	want_first = expected[0];
	fill_chunk(expected, chunks - 1);
	want_last = expected[CHUNK - 1];
	say("  %s: the first byte 0x%02x (0x%02x written), the last 0x%02x (0x%02x written): %s\n", how, *first,
		want_first, *last, want_last, *first == want_first && *last == want_last ? "right" : "WRONG");
}

static void read_file_three_ways(const char *path, uint64_t chunks)
{
	uint64_t size = chunks * CHUNK;
	uint8_t *whole = malloc(size);
	int file;
	uint64_t start, took, done;
	int calls;
	FILE *stream;
	static uint8_t buffer[64 * 1024];
	static uint8_t stream_buffer[64 * 1024];
	uint8_t first = 0, last = 0;

	if (!whole)
	{
		say("malloc(%llu MB) for the read: NULL\n", (unsigned long long)chunks);
		return;
	}
	/* one read */
	file = open(path, O_RDONLY);
	if (file < 0)
	{
		say("open(%s): %s\n", path, strerror(errno));
		free(whole);
		return;
	}
	start = now_us();
	done = 0;
	calls = 0;
	while (done < size)
	{
		ssize_t got = read(file, whole + done, size - done);

		calls++;
		if (got <= 0)
			break;
		done += (uint64_t)got;
	}
	took = now_us() - start;
	close(file);
	say("read %llu MB in one read after open: %llu bytes in %d call%s, %.3f s, %.1f MB/s\n",
		(unsigned long long)chunks, (unsigned long long)done, calls, calls == 1 ? "" : "s", took / 1e6,
		megabytes_per_second(done, took));
	if (done == size)
		check_ends("one read", &whole[0], &whole[size - 1], chunks);

	/* 64 KB reads */
	file = open(path, O_RDONLY);
	if (file >= 0)
	{
		start = now_us();
		done = 0;
		for (;;)
		{
			ssize_t got = read(file, buffer, sizeof(buffer));

			if (got <= 0)
				break;
			if (done == 0)
				first = buffer[0];
			last = buffer[got - 1];
			done += (uint64_t)got;
		}
		took = now_us() - start;
		close(file);
		say("read it in 64 KB reads: %llu bytes, %.3f s, %.1f MB/s\n", (unsigned long long)done, took / 1e6,
			megabytes_per_second(done, took));
		if (done == size)
			check_ends("64 KB reads", &first, &last, chunks);
	}

	/* fread with a 64 KB buffer */
	stream = fopen(path, "rb");
	if (stream)
	{
		setvbuf(stream, (char *)stream_buffer, _IOFBF, sizeof(stream_buffer));
		start = now_us();
		done = 0;
		for (;;)
		{
			size_t got = fread(buffer, 1, sizeof(buffer), stream);

			if (got == 0)
				break;
			if (done == 0)
				first = buffer[0];
			last = buffer[got - 1];
			done += got;
		}
		took = now_us() - start;
		fclose(stream);
		say("read it through fread, a 64 KB buffer: %llu bytes, %.3f s, %.1f MB/s\n", (unsigned long long)done,
			took / 1e6, megabytes_per_second(done, took));
		if (done == size)
			check_ends("fread", &first, &last, chunks);
	}
	free(whole);
}

static void say_access(const char *path)
{
	int result;

	errno = 0;
	result = access(path, F_OK | R_OK | W_OK);
	say("access(%s, F_OK | R_OK | W_OK): %d%s%s\n", path, result, result ? ", " : "", result ? strerror(errno) : "");
}

static void probe_folder(const char *folder)
{
	static uint8_t buffer[CHUNK];
	char probe[256], small[256], large[256], renamed[256], missing[256], resolved[PATH_MAX];
	struct stat info;
	DIR *directory;
	int result;

	say("-- %s\n", folder);
	if (stat(folder, &info) != 0)
	{
		say("stat(%s): %s: the folder is not visible to the title\n", folder, strerror(errno));
		return;
	}
	say("stat(%s): mode 0%o, uid %u, gid %u\n", folder, (unsigned)info.st_mode, (unsigned)info.st_uid,
		(unsigned)info.st_gid);
	directory = opendir(folder);
	if (!directory)
		say("opendir(%s): %s\n", folder, strerror(errno));
	else
	{
		struct dirent *entry;
		int count = 0;

		while ((entry = readdir(directory)) != NULL)
		{
			if (count++ < 40)
				say("  entry \"%s\", d_type %u\n", entry->d_name, (unsigned)entry->d_type);
		}
		closedir(directory);
		say("readdir: %d entries\n", count);
	}
	snprintf(probe, sizeof(probe), "%s/probe", folder);
	snprintf(small, sizeof(small), "%s/probe/16mb.bin", folder);
	snprintf(large, sizeof(large), "%s/probe/100mb.bin", folder);
	snprintf(renamed, sizeof(renamed), "%s/probe/renamed.bin", folder);
	snprintf(missing, sizeof(missing), "%s/probe/missing.bin", folder);
	result = mkdir(probe, 0777);
	say("mkdir(%s): %d%s%s\n", probe, result, result ? ", " : "", result ? strerror(errno) : "");
	result = chmod(probe, 0777);
	say("chmod(%s, 0777): %d%s%s\n", probe, result, result ? ", " : "", result ? strerror(errno) : "");

	if (write_file(small, 16, buffer) && write_file(large, 100, buffer))
	{
		time_t now = time(NULL);

		read_file_three_ways(large, 100);
		if (stat(large, &info) == 0)
			say("stat(%s): st_size %lld, st_mode 0%o, st_mtime %lld (the clock: %lld, %+lld s), st_blksize %lld\n",
				large, (long long)info.st_size, (unsigned)info.st_mode, (long long)info.st_mtime, (long long)now,
				(long long)(info.st_mtime - now), (long long)info.st_blksize);
		else
			say("stat(%s): %s\n", large, strerror(errno));
		say_access(folder);
		say_access(large);
		say_access(missing);
		if (realpath(probe, resolved))
			say("realpath(%s): %s\n", probe, resolved);
		else
			say("realpath(%s): %s\n", probe, strerror(errno));
		result = rename(large, renamed);
		say("rename(100mb.bin, renamed.bin): %d%s%s\n", result, result ? ", " : "", result ? strerror(errno) : "");
		result = unlink(result == 0 ? renamed : large);
		say("unlink of it: %d%s%s\n", result, result ? ", " : "", result ? strerror(errno) : "");
	}
	unlink(small);
	unlink(large);
	unlink(renamed);
	result = rmdir(probe);
	say("rmdir(%s): %d%s%s\n", probe, result, result ? ", " : "", result ? strerror(errno) : "");
}

static void probe_data_folder(void)
{
	struct stat info;

	heading("3. the data folder");
	if (guard_begin("3. the data folder"))
	{
		probe_folder("/data/halo");
		probe_folder("/app0");
		guard_end();
	}
	if (!guard_begin("3. /download0"))
		return;
	if (stat("/download0", &info) != 0)
		say("stat(/download0): %s\n", strerror(errno));
	else
	{
		FILE *file;

		say("stat(/download0): mode 0%o\n", (unsigned)info.st_mode);
		file = fopen("/download0/halo-probe.txt", "w");
		if (!file)
			say("/download0: a file cannot be made (%s)\n", strerror(errno));
		else
		{
			int written = fputs("probe\n", file) >= 0;

			fclose(file);
			say("/download0: a file was made and %s; unlink: %d\n", written ? "written" : "NOT written",
				unlink("/download0/halo-probe.txt"));
		}
	}
	guard_end();
}

/* ---------- 4. the host's memory and Vulkan */

static struct
{
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkPhysicalDeviceMemoryProperties memory;
	int has_display;
	int ready;
} vk;

#define INSTANCE_FUNCTIONS(X) \
	X(EnumerateInstanceExtensionProperties) \
	X(CreateInstance)
#define PHYSICAL_FUNCTIONS(X) \
	X(EnumeratePhysicalDevices) \
	X(GetPhysicalDeviceProperties) \
	X(GetPhysicalDeviceMemoryProperties) \
	X(GetPhysicalDeviceQueueFamilyProperties) \
	X(CreateDevice) \
	X(GetDeviceProcAddr) \
	X(GetPhysicalDeviceDisplayPropertiesKHR) \
	X(GetDisplayModePropertiesKHR) \
	X(GetPhysicalDeviceDisplayPlanePropertiesKHR) \
	X(GetDisplayPlaneSupportedDisplaysKHR) \
	X(CreateDisplayModeKHR) \
	X(GetDisplayPlaneCapabilitiesKHR) \
	X(CreateDisplayPlaneSurfaceKHR) \
	X(DestroySurfaceKHR) \
	X(GetPhysicalDeviceSurfaceSupportKHR) \
	X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(GetPhysicalDeviceSurfaceFormatsKHR) \
	X(GetPhysicalDeviceSurfacePresentModesKHR)
#define DEVICE_FUNCTIONS(X) \
	X(GetDeviceQueue) \
	X(AllocateMemory) \
	X(FreeMemory) \
	X(MapMemory) \
	X(UnmapMemory) \
	X(CreateCommandPool) \
	X(AllocateCommandBuffers) \
	X(BeginCommandBuffer) \
	X(EndCommandBuffer) \
	X(ResetCommandBuffer) \
	X(CmdPipelineBarrier) \
	X(CmdClearColorImage) \
	X(QueueSubmit) \
	X(CreateFence) \
	X(DestroyFence) \
	X(WaitForFences) \
	X(ResetFences) \
	X(CreateSemaphore) \
	X(DestroySemaphore) \
	X(DeviceWaitIdle) \
	X(CreateSwapchainKHR) \
	X(DestroySwapchainKHR) \
	X(GetSwapchainImagesKHR) \
	X(AcquireNextImageKHR) \
	X(QueuePresentKHR)

#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_FUNCTIONS(DECLARE)
PHYSICAL_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)

static void say_memory_flags(char *text, size_t size, VkMemoryPropertyFlags flags)
{
	snprintf(text, size, "%s%s%s%s%s%s", flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device-local" : "",
		flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "",
		flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " host-coherent" : "",
		flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " host-cached" : "",
		flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " lazily-allocated" : "",
		flags & VK_MEMORY_PROPERTY_PROTECTED_BIT ? " protected" : "");
}

static int vk_create(void)
{
	const char *instance_extensions[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
	const char *device_extensions[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkExtensionProperties extensions[64];
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDevice physicals[4];
	VkPhysicalDeviceProperties properties;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	uint32_t count, index;
	float priority = 1.0f;
	VkResult result;
	int surface = 0, display = 0;
	uint64_t start;

#define LOAD_GLOBAL(name) vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name);
	INSTANCE_FUNCTIONS(LOAD_GLOBAL)
	if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties)
	{
		say("vk_icdGetInstanceProcAddr gives no vkCreateInstance\n");
		return 0;
	}
	count = 64;
	result = vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
	say("instance extensions (%d):", result);
	for (index = 0; index < count; index++)
	{
		say(" %s", extensions[index].extensionName);
		surface |= !strcmp(extensions[index].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
		display |= !strcmp(extensions[index].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME);
	}
	say("\n");
	vk.has_display = surface && display;

	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "Halo PS5 host probe";
	application.apiVersion = VK_API_VERSION_1_1;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	instance_info.enabledExtensionCount = vk.has_display ? 2 : 0;
	instance_info.ppEnabledExtensionNames = instance_extensions;
	start = now_us();
	result = vkCreateInstance(&instance_info, NULL, &vk.instance);
	say("vkCreateInstance (VK_KHR_surface, VK_KHR_display %s): %d in %llu ms\n",
		vk.has_display ? "enabled" : "NOT reported", result, (unsigned long long)((now_us() - start) / 1000));
	if (result != VK_SUCCESS)
		return 0;
#define LOAD_INSTANCE(name) vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(vk.instance, "vk" #name);
	PHYSICAL_FUNCTIONS(LOAD_INSTANCE)

	count = 4;
	result = vkEnumeratePhysicalDevices(vk.instance, &count, physicals);
	say("vkEnumeratePhysicalDevices: %d, %u device%s\n", result, count, count == 1 ? "" : "s");
	if (count == 0)
		return 0;
	vk.physical = physicals[0];
	vkGetPhysicalDeviceProperties(vk.physical, &properties);
	say("device \"%s\", Vulkan %u.%u.%u, driver version 0x%08x, vendor 0x%04x, device 0x%04x\n", properties.deviceName,
		VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
		VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion, properties.vendorID,
		properties.deviceID);
	vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
	for (index = 0; index < vk.memory.memoryHeapCount; index++)
		say("memory heap %u: %.1f MB, flags 0x%x%s\n", index, (double)vk.memory.memoryHeaps[index].size / MEGABYTE,
			(unsigned)vk.memory.memoryHeaps[index].flags,
			vk.memory.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "");
	for (index = 0; index < vk.memory.memoryTypeCount; index++)
	{
		char flags[160];

		say_memory_flags(flags, sizeof(flags), vk.memory.memoryTypes[index].propertyFlags);
		say("memory type %u: heap %u, flags 0x%x%s\n", index, vk.memory.memoryTypes[index].heapIndex,
			(unsigned)vk.memory.memoryTypes[index].propertyFlags, flags);
	}

	count = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, families);
	vk.queue_family = UINT32_MAX;
	for (index = 0; index < count; index++)
	{
		say("queue family %u: flags 0x%x, %u queues\n", index, (unsigned)families[index].queueFlags,
			families[index].queueCount);
		if (vk.queue_family == UINT32_MAX && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			vk.queue_family = index;
	}
	if (vk.queue_family == UINT32_MAX)
	{
		say("no graphics queue family\n");
		return 0;
	}
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = vk.queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = vk.has_display ? 1 : 0;
	device_info.ppEnabledExtensionNames = device_extensions;
	start = now_us();
	result = vkCreateDevice(vk.physical, &device_info, NULL, &vk.device);
	say("vkCreateDevice (one graphics queue of family %u%s): %d in %llu ms\n", vk.queue_family,
		vk.has_display ? ", VK_KHR_swapchain" : "", result, (unsigned long long)((now_us() - start) / 1000));
	if (result != VK_SUCCESS)
		return 0;
#define LOAD_DEVICE(name) vk##name = (PFN_vk##name)vkGetDeviceProcAddr(vk.device, "vk" #name);
	DEVICE_FUNCTIONS(LOAD_DEVICE)
	vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
	return 1;
}

static uint32_t find_memory_type(VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < vk.memory.memoryTypeCount; index++)
		if ((vk.memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			return index;
	return UINT32_MAX;
}

static VkDeviceMemory allocate_and_say(const char *what, VkDeviceSize size, VkMemoryPropertyFlags wanted)
{
	VkMemoryAllocateInfo info;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t type = find_memory_type(wanted);
	VkResult result;
	uint64_t start;
	char when[96];

	if (type == UINT32_MAX)
	{
		say("no memory type is %s\n", what);
		return VK_NULL_HANDLE;
	}
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = size;
	info.memoryTypeIndex = type;
	start = now_us();
	result = vkAllocateMemory(vk.device, &info, NULL, &memory);
	say("vkAllocateMemory(%llu MB, type %u, %s): %d in %llu us\n", (unsigned long long)(size / MEGABYTE), type, what,
		result, (unsigned long long)(now_us() - start));
	snprintf(when, sizeof(when), "after %llu MB of %s", (unsigned long long)(size / MEGABYTE), what);
	say_memory(when);
	return result == VK_SUCCESS ? memory : VK_NULL_HANDLE;
}

static void probe_host_memory(void)
{
	enum
	{
		BLOCK = 16 * MEGABYTE,
		BLOCK_LIMIT = 512,
	};
	static void *blocks[BLOCK_LIMIT];
	int count = 0, index;
	uint64_t start;

	heading("4. the host's memory");
	if (guard_begin("4. malloc"))
	{
		start = now_us();
		while (count < BLOCK_LIMIT)
		{
			uint8_t *block = malloc(BLOCK);
			uint64_t offset;

			if (!block)
				break;
			for (offset = 0; offset < BLOCK; offset += 0x4000)
				block[offset] = (uint8_t)offset;
			blocks[count++] = block;
			if (count == 1 || count % 64 == 0)
				say("malloc: %d blocks, %llu MB; the latest at %p\n", count,
					(unsigned long long)(count * (BLOCK / MEGABYTE)), (void *)block);
		}
		say("malloc of 16 MB blocks, every page written: %d blocks, %llu MB, in %.3f s; %s\n", count,
			(unsigned long long)(count * (BLOCK / MEGABYTE)), (now_us() - start) / 1e6,
			count == BLOCK_LIMIT ? "stopped at 8 GB" : "then NULL");
		say_memory("with that taken");
		for (index = 0; index < count; index++)
			free(blocks[index]);
		say_memory("after it is freed");
		guard_end();
	}

	if (!guard_begin("4. Vulkan"))
		return;
	vk.ready = vk_create();
	say_memory("with the instance and the device");
	if (vk.ready)
	{
		VkDeviceMemory local = allocate_and_say("device-local", 256 * MEGABYTE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory visible = allocate_and_say("host-visible and host-coherent", 64 * MEGABYTE,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

		if (visible)
		{
			void *mapped = NULL;
			VkResult result = vkMapMemory(vk.device, visible, 0, VK_WHOLE_SIZE, 0, &mapped);

			say("vkMapMemory of the 64 MB: %d, at %p\n", result, mapped);
			if (result == VK_SUCCESS)
			{
				uint32_t *words = mapped;
				size_t index2, wrong = 0;

				for (index2 = 0; index2 < 64 * MEGABYTE / 4; index2++)
					words[index2] = (uint32_t)(index2 * 2654435761u);
				for (index2 = 0; index2 < 64 * MEGABYTE / 4; index2++)
					wrong += words[index2] != (uint32_t)(index2 * 2654435761u);
				say("written and read back: %zu words wrong\n", wrong);
				vkUnmapMemory(vk.device, visible);
			}
		}
		if (local)
			vkFreeMemory(vk.device, local, NULL);
		if (visible)
			vkFreeMemory(vk.device, visible, NULL);
		say_memory("after both are freed");
	}
	guard_end();
}

/* ---------- 5. the display */

#define MAX_IMAGES 8
#define IN_FLIGHT 2

struct swapchain
{
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkExtent2D extent;
	VkFormat format;
	uint32_t image_count;
	VkImage images[MAX_IMAGES];
	VkSemaphore rendered[MAX_IMAGES];
	VkSemaphore acquired[IN_FLIGHT];
	VkFence fences[IN_FLIGHT];
	VkCommandBuffer commands[IN_FLIGHT];
	uint64_t frame;
	int out_of_date;
};

static VkCommandPool command_pool;

static int make_surface(struct swapchain *chain, VkDisplayModeKHR mode, VkExtent2D extent, uint32_t plane,
	uint32_t stack_index)
{
	VkDisplayPlaneCapabilitiesKHR capabilities;
	VkDisplaySurfaceCreateInfoKHR info;
	VkBool32 supported = VK_FALSE;
	VkResult result;

	memset(&capabilities, 0, sizeof(capabilities));
	vkGetDisplayPlaneCapabilitiesKHR(vk.physical, mode, plane, &capabilities);
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	info.displayMode = mode;
	info.planeIndex = plane;
	info.planeStackIndex = stack_index;
	info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.globalAlpha = 1.0f;
	info.alphaMode = (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) ?
		VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR :
		(VkDisplayPlaneAlphaFlagBitsKHR)(capabilities.supportedAlpha & -capabilities.supportedAlpha);
	info.imageExtent = extent;
	result = vkCreateDisplayPlaneSurfaceKHR(vk.instance, &info, NULL, &chain->surface);
	say("vkCreateDisplayPlaneSurfaceKHR(%ux%u, plane %u, stack %u, alpha 0x%x): %d\n", extent.width, extent.height,
		plane, stack_index, (unsigned)info.alphaMode, result);
	if (result != VK_SUCCESS)
		return 0;
	result = vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, vk.queue_family, chain->surface, &supported);
	say("vkGetPhysicalDeviceSurfaceSupportKHR(family %u): %d, %s\n", vk.queue_family, result,
		supported ? "supported" : "NOT supported");
	return 1;
}

static void say_surface(struct swapchain *chain)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[32];
	VkPresentModeKHR modes[8];
	uint32_t count, index;
	VkResult result;

	result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, chain->surface, &capabilities);
	say("surface capabilities (%d): images %u to %u, current %ux%u, min %ux%u, max %ux%u, layers %u, transforms "
		"0x%x (current 0x%x), composite alpha 0x%x, usage 0x%x\n",
		result, capabilities.minImageCount, capabilities.maxImageCount, capabilities.currentExtent.width,
		capabilities.currentExtent.height, capabilities.minImageExtent.width, capabilities.minImageExtent.height,
		capabilities.maxImageExtent.width, capabilities.maxImageExtent.height, capabilities.maxImageArrayLayers,
		(unsigned)capabilities.supportedTransforms, (unsigned)capabilities.currentTransform,
		(unsigned)capabilities.supportedCompositeAlpha, (unsigned)capabilities.supportedUsageFlags);
	count = 32;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, chain->surface, &count, formats);
	say("surface formats (%d):", result);
	for (index = 0; index < count; index++)
		say(" format %d colour space %d;", formats[index].format, formats[index].colorSpace);
	say("\n");
	count = 8;
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, chain->surface, &count, modes);
	say("present modes (%d):", result);
	for (index = 0; index < count; index++)
		say(" %d%s", modes[index],
			modes[index] == VK_PRESENT_MODE_FIFO_KHR ? " (FIFO)" :
			modes[index] == VK_PRESENT_MODE_MAILBOX_KHR ? " (mailbox)" :
			modes[index] == VK_PRESENT_MODE_IMMEDIATE_KHR ? " (immediate)" :
			modes[index] == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? " (FIFO relaxed)" : "");
	say("\n");
}

static int make_swapchain(struct swapchain *chain, VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[32];
	VkSwapchainCreateInfoKHR info;
	VkSemaphoreCreateInfo semaphore_info;
	VkFenceCreateInfo fence_info;
	VkCommandBufferAllocateInfo allocate_info;
	uint32_t count = 32, index;
	VkResult result;

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, chain->surface, &capabilities);
	vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, chain->surface, &count, formats);
	if (!count)
		return 0;
	chain->format = formats[0].format;
	for (index = 0; index < count; index++)
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM)
			chain->format = formats[index].format;
	chain->extent = capabilities.currentExtent;
	if (chain->extent.width == UINT32_MAX)
		chain->extent = capabilities.minImageExtent;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = chain->surface;
	info.minImageCount = capabilities.minImageCount < 3 ? 3 : capabilities.minImageCount;
	if (capabilities.maxImageCount && info.minImageCount > capabilities.maxImageCount)
		info.minImageCount = capabilities.maxImageCount;
	info.imageFormat = chain->format;
	info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	for (index = 0; index < count; index++)
		if (formats[index].format == chain->format)
			info.imageColorSpace = formats[index].colorSpace;
	info.imageExtent = chain->extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (capabilities.supportedUsageFlags &
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = capabilities.currentTransform;
	info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
		(VkCompositeAlphaFlagBitsKHR)(capabilities.supportedCompositeAlpha & -capabilities.supportedCompositeAlpha);
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	result = vkCreateSwapchainKHR(vk.device, &info, NULL, &chain->swapchain);
	say("vkCreateSwapchainKHR(FIFO, %ux%u, format %d, %u images asked%s): %d\n", chain->extent.width,
		chain->extent.height, chain->format, info.minImageCount, old ? ", the old one retired" : "", result);
	if (result != VK_SUCCESS)
		return 0;
	chain->image_count = MAX_IMAGES;
	vkGetSwapchainImagesKHR(vk.device, chain->swapchain, &chain->image_count, chain->images);
	say("%u swapchain images\n", chain->image_count);

	memset(&semaphore_info, 0, sizeof(semaphore_info));
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (index = 0; index < chain->image_count; index++)
		vkCreateSemaphore(vk.device, &semaphore_info, NULL, &chain->rendered[index]);
	for (index = 0; index < IN_FLIGHT; index++)
	{
		vkCreateSemaphore(vk.device, &semaphore_info, NULL, &chain->acquired[index]);
		vkCreateFence(vk.device, &fence_info, NULL, &chain->fences[index]);
	}
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = command_pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = IN_FLIGHT;
	vkAllocateCommandBuffers(vk.device, &allocate_info, chain->commands);
	chain->frame = 0;
	chain->out_of_date = 0;
	return 1;
}

/* the swapchain's own objects, not its surface */
static void destroy_swapchain(struct swapchain *chain)
{
	uint32_t index;

	vkDeviceWaitIdle(vk.device);
	for (index = 0; index < chain->image_count; index++)
		vkDestroySemaphore(vk.device, chain->rendered[index], NULL);
	for (index = 0; index < IN_FLIGHT; index++)
	{
		vkDestroySemaphore(vk.device, chain->acquired[index], NULL);
		vkDestroyFence(vk.device, chain->fences[index], NULL);
	}
	if (chain->swapchain)
		vkDestroySwapchainKHR(vk.device, chain->swapchain, NULL);
	chain->swapchain = VK_NULL_HANDLE;
	chain->image_count = 0;
}

static void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
	VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* one frame cleared to gray (0 to 1) and presented: VK_SUCCESS, or the
first result of the acquire or the present that was not (*where says
which) */
static VkResult present_frame(struct swapchain *chain, float gray, const char **where)
{
	uint32_t slot = (uint32_t)(chain->frame % IN_FLIGHT);
	VkCommandBuffer command = chain->commands[slot];
	VkCommandBufferBeginInfo begin;
	VkClearColorValue colour;
	VkImageSubresourceRange range;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	uint32_t image = 0;
	VkResult result, present_result;

	*where = "";
	vkWaitForFences(vk.device, 1, &chain->fences[slot], VK_TRUE, UINT64_MAX);
	result = vkAcquireNextImageKHR(vk.device, chain->swapchain, 2000000000ULL, chain->acquired[slot], VK_NULL_HANDLE,
		&image);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		*where = "acquire";
		if (result == VK_ERROR_OUT_OF_DATE_KHR)
			chain->out_of_date = 1;
		return result;
	}
	if (result == VK_SUBOPTIMAL_KHR)
		*where = "acquire";
	vkResetFences(vk.device, 1, &chain->fences[slot]);
	vkResetCommandBuffer(command, 0);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command, &begin);
	image_barrier(command, chain->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	colour.float32[0] = colour.float32[1] = colour.float32[2] = gray;
	colour.float32[3] = 1.0f;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage(command, chain->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	image_barrier(command, chain->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	vkEndCommandBuffer(command);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &chain->acquired[slot];
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &chain->rendered[image];
	result = vkQueueSubmit(vk.queue, 1, &submit, chain->fences[slot]);
	if (result != VK_SUCCESS)
	{
		*where = "submit";
		return result;
	}
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &chain->rendered[image];
	present.swapchainCount = 1;
	present.pSwapchains = &chain->swapchain;
	present.pImageIndices = &image;
	present_result = vkQueuePresentKHR(vk.queue, &present);
	chain->frame++;
	if (present_result != VK_SUCCESS)
	{
		*where = "present";
		if (present_result == VK_ERROR_OUT_OF_DATE_KHR)
			chain->out_of_date = 1;
		return present_result;
	}
	return **where ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

/* 60 frames of changing gray, the last 50 timed */
static void present_sixty(struct swapchain *chain, const char *what)
{
	uint64_t times[61];
	uint64_t intervals[60];
	int frame, failures = 0, good = 0;

	times[0] = now_us();
	for (frame = 0; frame < 60; frame++)
	{
		const char *where;
		VkResult result = present_frame(chain, (float)(frame % 30) / 29.0f, &where);

		times[frame + 1] = now_us();
		if (result != VK_SUCCESS && failures++ < 5)
			say("frame %d: %s returned %d\n", frame, where, result);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
			good++;
	}
	vkDeviceWaitIdle(vk.device);
	for (frame = 11; frame <= 60; frame++)
		intervals[frame - 11] = times[frame] - times[frame - 1];
	qsort(intervals, 50, sizeof(intervals[0]), compare_u64);
	say("%s: 60 frames, %d presented; the last 50 took %.3f s (50 refreshes at 59.94 Hz: 0.834 s); intervals min "
		"%.2f ms, median %.2f ms, max %.2f ms\n",
		what, good, (times[60] - times[10]) / 1e6, intervals[0] / 1e3, intervals[25] / 1e3, intervals[49] / 1e3);
}

struct mode_entry
{
	VkDisplayModeKHR mode;
	VkExtent2D extent;
	uint32_t refresh;
	int created;
};

static struct swapchain native_chain;
static int native_ready;
static VkDisplayModeKHR native_mode;
static VkExtent2D native_extent;
static uint32_t native_plane, native_stack;

static void probe_display(void)
{
	static const struct
	{
		uint32_t width, height, refresh;
	} wanted[] = {
		{ 1920, 1080, 59940 },
		{ 1920, 1080, 60000 },
		{ 3840, 2160, 60000 },
		{ 1280, 720, 59940 },
	};
	VkDisplayPropertiesKHR displays[4];
	VkDisplayModePropertiesKHR modes[16];
	VkDisplayPlanePropertiesKHR planes[8];
	struct mode_entry entries[24];
	uint32_t display_count = 4, mode_count = 16, plane_count = 8, entry_count = 0, index, inner;
	VkCommandPoolCreateInfo pool_info;
	VkDisplayKHR display;
	VkResult result;
	int have_1080 = -1;

	heading("5. the display");
	if (!vk.ready || !vk.has_display)
	{
		say("skipped: no Vulkan device or no VK_KHR_display\n");
		return;
	}
	if (!guard_begin("5. the display"))
		return;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = vk.queue_family;
	vkCreateCommandPool(vk.device, &pool_info, NULL, &command_pool);

	result = vkGetPhysicalDeviceDisplayPropertiesKHR(vk.physical, &display_count, displays);
	say("vkGetPhysicalDeviceDisplayPropertiesKHR: %d, %u display%s\n", result, display_count,
		display_count == 1 ? "" : "s");
	for (index = 0; index < display_count; index++)
		say("display %u: \"%s\", %ux%u mm, %ux%u, transforms 0x%x, plane reorder %u, persistent content %u\n", index,
			displays[index].displayName ? displays[index].displayName : "(no name)",
			displays[index].physicalDimensions.width, displays[index].physicalDimensions.height,
			displays[index].physicalResolution.width, displays[index].physicalResolution.height,
			(unsigned)displays[index].supportedTransforms, displays[index].planeReorderPossible,
			displays[index].persistentContent);
	if (display_count == 0)
	{
		guard_end();
		return;
	}
	display = displays[0].display;
	result = vkGetDisplayModePropertiesKHR(vk.physical, display, &mode_count, modes);
	say("vkGetDisplayModePropertiesKHR: %d, %u mode%s\n", result, mode_count, mode_count == 1 ? "" : "s");
	for (index = 0; index < mode_count && entry_count < 24; index++)
	{
		say("mode %u: %ux%u at %u mHz\n", index, modes[index].parameters.visibleRegion.width,
			modes[index].parameters.visibleRegion.height, modes[index].parameters.refreshRate);
		entries[entry_count].mode = modes[index].displayMode;
		entries[entry_count].extent = modes[index].parameters.visibleRegion;
		entries[entry_count].refresh = modes[index].parameters.refreshRate;
		entries[entry_count].created = 0;
		entry_count++;
	}
	result = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(vk.physical, &plane_count, planes);
	say("vkGetPhysicalDeviceDisplayPlanePropertiesKHR: %d, %u plane%s\n", result, plane_count,
		plane_count == 1 ? "" : "s");
	native_plane = UINT32_MAX;
	for (index = 0; index < plane_count; index++)
	{
		VkDisplayKHR supported[4];
		uint32_t supported_count = 4;

		result = vkGetDisplayPlaneSupportedDisplaysKHR(vk.physical, index, &supported_count, supported);
		say("plane %u: current display %s, stack index %u; supported displays (%d): %u%s\n", index,
			planes[index].currentDisplay == VK_NULL_HANDLE ? "none" :
			planes[index].currentDisplay == display ? "display 0" : "another",
			planes[index].currentStackIndex, result, supported_count,
			supported_count && supported[0] == display ? " (display 0 first)" : "");
		for (inner = 0; inner < supported_count; inner++)
			if (supported[inner] == display && native_plane == UINT32_MAX)
			{
				native_plane = index;
				native_stack = planes[index].currentStackIndex;
			}
	}
	if (native_plane == UINT32_MAX)
	{
		say("no plane supports display 0\n");
		guard_end();
		return;
	}

	for (index = 0; index < sizeof(wanted) / sizeof(wanted[0]); index++)
	{
		VkDisplayModeCreateInfoKHR info;
		VkDisplayModeKHR mode = VK_NULL_HANDLE;

		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR;
		info.parameters.visibleRegion.width = wanted[index].width;
		info.parameters.visibleRegion.height = wanted[index].height;
		info.parameters.refreshRate = wanted[index].refresh;
		result = vkCreateDisplayModeKHR(vk.physical, display, &info, NULL, &mode);
		say("vkCreateDisplayModeKHR(%ux%u at %u mHz): %d\n", wanted[index].width, wanted[index].height,
			wanted[index].refresh, result);
		if (result == VK_SUCCESS && entry_count < 24)
		{
			entries[entry_count].mode = mode;
			entries[entry_count].extent = info.parameters.visibleRegion;
			entries[entry_count].refresh = wanted[index].refresh;
			entries[entry_count].created = 1;
			entry_count++;
		}
	}
	for (index = 0; index < entry_count; index++)
	{
		VkDisplayPlaneCapabilitiesKHR capabilities;

		memset(&capabilities, 0, sizeof(capabilities));
		result = vkGetDisplayPlaneCapabilitiesKHR(vk.physical, entries[index].mode, native_plane, &capabilities);
		say("plane %u capabilities on %ux%u at %u mHz (%s): %d; alpha 0x%x, source position %d,%d to %d,%d, source "
			"extent %ux%u to %ux%u, destination position %d,%d to %d,%d, destination extent %ux%u to %ux%u\n",
			native_plane, entries[index].extent.width, entries[index].extent.height, entries[index].refresh,
			entries[index].created ? "created" : "the display's", result, (unsigned)capabilities.supportedAlpha,
			capabilities.minSrcPosition.x, capabilities.minSrcPosition.y, capabilities.maxSrcPosition.x,
			capabilities.maxSrcPosition.y, capabilities.minSrcExtent.width, capabilities.minSrcExtent.height,
			capabilities.maxSrcExtent.width, capabilities.maxSrcExtent.height, capabilities.minDstPosition.x,
			capabilities.minDstPosition.y, capabilities.maxDstPosition.x, capabilities.maxDstPosition.y,
			capabilities.minDstExtent.width, capabilities.minDstExtent.height, capabilities.maxDstExtent.width,
			capabilities.maxDstExtent.height);
		if (have_1080 < 0 && entries[index].extent.width == 1920 && entries[index].extent.height == 1080)
			have_1080 = (int)index;
	}

	/* the native mode: the display's first */
	native_mode = entries[0].mode;
	native_extent = entries[0].extent;
	say("-- the native mode, %ux%u\n", native_extent.width, native_extent.height);
	notify("Halo PS5 host probe: the screen cycles through grays for a second");
	if (make_surface(&native_chain, native_mode, native_extent, native_plane, native_stack))
	{
		say_surface(&native_chain);
		if (make_swapchain(&native_chain, VK_NULL_HANDLE))
		{
			present_sixty(&native_chain, "the native mode");
			native_ready = 1;
		}
	}
	guard_end();

	if (have_1080 >= 0 && native_ready && guard_begin("5. the display at 1080p"))
	{
		struct swapchain chain;

		memset(&chain, 0, sizeof(chain));
		say("-- 1920x1080 at %u mHz (%s); the native swapchain and surface are destroyed first, as one plane "
			"shows one surface\n",
			entries[have_1080].refresh, entries[have_1080].created ? "created" : "the display's");
		destroy_swapchain(&native_chain);
		vkDestroySurfaceKHR(vk.instance, native_chain.surface, NULL);
		native_chain.surface = VK_NULL_HANDLE;
		native_ready = 0;
		notify("Halo PS5 host probe: grays again, in a 1080p mode: do they fill the whole screen?");
		if (make_surface(&chain, entries[have_1080].mode, entries[have_1080].extent, native_plane, native_stack))
		{
			say_surface(&chain);
			if (make_swapchain(&chain, VK_NULL_HANDLE))
			{
				present_sixty(&chain, "1080p");
				destroy_swapchain(&chain);
			}
			vkDestroySurfaceKHR(vk.instance, chain.surface, NULL);
		}
		say("-- the native mode again, for section 6\n");
		memset(&native_chain, 0, sizeof(native_chain));
		if (make_surface(&native_chain, native_mode, native_extent, native_plane, native_stack) &&
			make_swapchain(&native_chain, VK_NULL_HANDLE))
			native_ready = 1;
		guard_end();
	}
	else if (have_1080 < 0)
		say("no 1920x1080 mode exists or was created: the renderer scales to the native mode itself\n");
}

/* ---------- 6. leaving and returning */

static void probe_leaving(void)
{
	static struct pad_sample samples[64];
	static uint8_t event[16384] __attribute__((aligned(16)));
	static uint8_t status[16384] __attribute__((aligned(16)));
	uint64_t intervals[2048];
	int32_t user = -1, pad = -1;
	int32_t result;
	uint64_t start, second_start, last_present = 0;
	unsigned frames_total = 0, frames_second = 0, interval_count = 0;
	int last_connected = -2;
	uint32_t last_intercepted = 2;
	int32_t last_event_result = 1, last_status_result = 1;
	uint8_t last_status[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	int index;

	heading("6. leaving and returning");
	if (!native_ready)
	{
		say("skipped: no swapchain on the native mode\n");
		return;
	}
	if (!guard_begin("6. leaving and returning"))
		return;
	say("sceUserServiceInitialize: 0x%08x\n", (unsigned)sceUserServiceInitialize(NULL));
	result = sceUserServiceGetInitialUser(&user);
	say("sceUserServiceGetInitialUser: 0x%08x, user %d\n", (unsigned)result, user);
	say("scePadInit: 0x%08x\n", (unsigned)scePadInit());
	pad = scePadOpen(user, 0, 0, NULL);
	if (pad < 0)
		pad = scePadGetHandle(user, 0, 0);
	say("the pad: handle 0x%08x\n", (unsigned)pad);
	say("sceSystemServiceReceiveEvent and sceSystemServiceGetStatus are declared by hand (OpenOrbis's PS4 "
		"declarations), each given a 16 KB buffer; only the event's type and the status's first 8 bytes are read\n");

	notify("Halo PS5 host probe: watch the grays 10 s; press PS, stay 10 s in the menu, come back; hold PS for the "
		"Quick Menu, close it; then wait (90 s in all)");
	say("for 90 s: watch the grays for 10 s; press the PS button, stay in the system's menu for 10 s, return to "
		"the title; hold the PS button for the Quick Menu, close it; wait\n");
	start = now_us();
	second_start = start;
	while (now_us() - start < 90000000ULL)
	{
		const char *where;
		VkResult present_result;
		uint64_t now;

		present_result = present_frame(&native_chain, (float)(frames_total % 120) / 119.0f, &where);
		now = now_us();
		if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR)
		{
			if (last_present && interval_count < 2048)
				intervals[interval_count++] = now - last_present;
			last_present = now;
			frames_total++;
			frames_second++;
		}
		if (present_result != VK_SUCCESS)
		{
			say("%7.3f s: %s returned %d\n", (now - start) / 1e6, where, present_result);
			if (present_result != VK_SUBOPTIMAL_KHR)
				usleep(16000);
		}

		if (pad >= 0)
		{
			int32_t count = scePadRead(pad, samples, 64);

			if (count > 0)
			{
				const struct pad_sample *sample = &samples[count - 1];
				uint32_t intercepted = (sample->buttons & PAD_INTERCEPTED) ? 1 : 0;

				if (sample->connected != last_connected || intercepted != last_intercepted)
				{
					say("%7.3f s: the pad: connected %d, intercepted %u (buttons 0x%08x)\n", (now - start) / 1e6,
						sample->connected, intercepted, (unsigned)sample->buttons);
					last_connected = sample->connected;
					last_intercepted = intercepted;
				}
			}
			else if (count < 0 && last_connected != -1)
			{
				say("%7.3f s: scePadRead: 0x%08x\n", (now - start) / 1e6, (unsigned)count);
				last_connected = -1;
			}
		}

		memset(event, 0, 64);
		result = sceSystemServiceReceiveEvent(event);
		if (result == 0)
			say("%7.3f s: sceSystemServiceReceiveEvent: an event, type 0x%08x\n", (now - start) / 1e6,
				(unsigned)*(const int32_t *)event);
		else if (result != last_event_result)
			say("%7.3f s: sceSystemServiceReceiveEvent: 0x%08x\n", (now - start) / 1e6, (unsigned)result);
		last_event_result = result;
		memset(status, 0, 64);
		result = sceSystemServiceGetStatus(status);
		if (result != last_status_result || memcmp(status, last_status, 8) != 0)
		{
			say("%7.3f s: sceSystemServiceGetStatus: 0x%08x; events waiting %d, then bytes %02x %02x %02x %02x "
				"(isSystemUiOverlaid first)\n",
				(now - start) / 1e6, (unsigned)result, *(const int32_t *)status, status[4], status[5], status[6],
				status[7]);
			last_status_result = result;
			memcpy(last_status, status, 8);
		}

		if (now - second_start >= 1000000ULL)
		{
			struct timespec monotonic, realtime;

			clock_gettime(CLOCK_MONOTONIC, &monotonic);
			clock_gettime(CLOCK_REALTIME, &realtime);
			if (interval_count)
			{
				qsort(intervals, interval_count, sizeof(intervals[0]), compare_u64);
				say("%7.3f s: %u frames in %.3f s, intervals min %.2f ms, median %.2f ms, max %.2f ms; monotonic "
					"%lld.%03ld, realtime %lld.%03ld\n",
					(now - start) / 1e6, frames_second, (now - second_start) / 1e6, intervals[0] / 1e3,
					intervals[interval_count / 2] / 1e3, intervals[interval_count - 1] / 1e3,
					(long long)monotonic.tv_sec, monotonic.tv_nsec / 1000000, (long long)realtime.tv_sec,
					realtime.tv_nsec / 1000000);
			}
			else
				say("%7.3f s: %u frames in %.3f s; monotonic %lld.%03ld, realtime %lld.%03ld\n", (now - start) / 1e6,
					frames_second, (now - second_start) / 1e6, (long long)monotonic.tv_sec,
					monotonic.tv_nsec / 1000000, (long long)realtime.tv_sec, realtime.tv_nsec / 1000000);
			frames_second = 0;
			interval_count = 0;
			second_start = now;
		}
	}
	say("90 s: %u frames presented in all; the swapchain %s out of date\n", frames_total,
		native_chain.out_of_date ? "reported" : "never reported");

	{
		int good = 0;

		for (index = 0; index < 10; index++)
		{
			const char *where;
			VkResult present_result = present_frame(&native_chain, 0.5f, &where);

			if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR)
				good++;
			else
				say("after: frame %d: %s returned %d\n", index, where, present_result);
		}
		say("after the 90 s: %d of 10 frames presented\n", good);
		if (native_chain.out_of_date || good < 10)
		{
			struct swapchain fresh;
			VkSwapchainKHR old = native_chain.swapchain;

			memset(&fresh, 0, sizeof(fresh));
			fresh.surface = native_chain.surface;
			vkDeviceWaitIdle(vk.device);
			if (make_swapchain(&fresh, old))
			{
				good = 0;
				for (index = 0; index < 10; index++)
				{
					const char *where;
					VkResult present_result = present_frame(&fresh, 0.25f, &where);

					if (present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR)
						good++;
					else
						say("a new swapchain: frame %d: %s returned %d\n", index, where, present_result);
				}
				say("a new swapchain on the same surface: %d of 10 frames presented\n", good);
				destroy_swapchain(&native_chain);
				native_chain = fresh;
			}
		}
	}
	if (pad >= 0)
		say("scePadClose: 0x%08x\n", (unsigned)scePadClose(pad));
	guard_end();
}

int main(void)
{
	ps5_fp_ieee();
	report_open("host-probe.txt");
	notify("Halo PS5 host probe starts");
	say("RADV's standard error to the kernel log: %d\n", ps5_klog_capture_stderr("[host-probe:stderr] "));
	guard_load();
	heading("4. the host's memory, before anything");
	say_memory("before anything");
	map_guest_ranges();
	say_memory("after the guest's ranges");
	probe_futexes();
	probe_faults();
	probe_data_folder();
	probe_host_memory();
	probe_display();
	probe_leaving();
	/* last: the second run's getcwd took the title down (an export that
	jumps to an unmapped address), so it goes after everything else */
	heading("3. getcwd (last)");
	if (guard_begin("3. getcwd"))
	{
		char cwd[PATH_MAX];

		if (getcwd(cwd, sizeof(cwd)))
			say("getcwd: %s\n", cwd);
		else
			say("getcwd: %s\n", strerror(errno));
		guard_end();
	}
	heading("done");
	if (native_ready)
	{
		destroy_swapchain(&native_chain);
		vkDestroySurfaceKHR(vk.instance, native_chain.surface, NULL);
	}
	notify("Halo PS5 host probe is done: read host-probe.txt over FTP");
	report_close();
	/* (RADV's threads may still be running) */
	_exit(0);
}
