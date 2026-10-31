/*
PS5 GUEST PROBE

The memory probe (port/ps5/probe/mem) answered for a payload, which runs in
the ELF loader's process. The game will be a title: a process of its own
with a budget of flexible memory for anonymous mappings (448 MB on the
console PS5_PayloadSDK's platform/docs/PROBE.md measured), direct memory for
everything larger (12 GB), and execute permission only by sceKernelMprotect
on direct memory. So this asks again, as a title, and then runs the game:

1. The process: system software, flexible and direct memory, and where
   this program, its stack, malloc and an anonymous mmap landed.
2. Its map (sceKernelVirtualQuery, every region from 0 up).
3. Where below 4 GB address space is free: sceKernelReserveVirtualRange at
   each megabyte, which reserves there when the range is free and places
   the reservation elsewhere (or refuses) when not; reported as runs.
4. The guest's ranges in direct memory, each reserved where it must be and
   mapped there: the image (0x70000000, 64 MB, made read, write and
   execute), the Xbox window (512 MB at 0x80000000, else on the next whole
   256 MB that is free: a title has PS5_Vulkan's libc.prx at 0x80000000),
   Custom Edition's tag cache (0x40440000) and a pool for the guest's own
   mappings. And whether
   an anonymous mmap below 4 GB works too, and with execute.
5. Code in the image's range: does it run, load through a 32-bit pointer,
   call the host above 4 GB, use a stack below 4 GB? And a thread whose
   stack is below 4 GB.
6. The game: /app0/halo_guest.elf (the x32 image of `ninja linux_x86_64`,
   PR #359, configured with --linux-x86-64-movable-window so that it takes
   the window where it is put) is loaded where it is linked, its header checked, and every host
   import pointed at a thunk that reports the call. The probe answers the
   few imports a real host answers trivially (the thread pointer, logging,
   the system calls the C library makes at start: memory from the pool,
   clocks, standard output; files are not found), and stops at the first
   import it does not answer: that is how far the game runs on the PS5
   before it needs the real host. A fault in the game is reported with its
   registers. The run stops after 60 seconds whatever happens.

Build: ninja build/linux_x86_64/halo_guest.elf, then
       port/ps5/probe/title/build.sh port/ps5/probe/guest
Run:   copy build/ps5/probe/PPSA99102 to /data/homebrew/PPSA99102 over FTP
       and start "Halo PS5 guest probe" from the home screen.

The report goes to the kernel log and /app0/guest-probe.txt (over FTP,
/data/homebrew/PPSA99102/guest-probe.txt).
*/

#include "probe_report.h"

#include "guest_syscall_names.h"
#include "halo_android_abi.h"
#include "halo_guest_syscall.h"

#include <ps5platform/context.h>
#include <ps5platform/kernel.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MEGABYTE 0x100000ULL
#define LOW_LIMIT 0x100000000ULL
#define DIRECT_UNIT 0x10000ULL
#define PROT_CPU_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define PROT_CPU_RWX (PROT_CPU_RW | PS5_KERNEL_PROT_CPU_EXEC)

/* the pool for the guest's own mappings (its malloc, its stacks), and the
probe's low stacks at its top */
#define POOL_SIZE (256 * MEGABYTE)
#define GUEST_STACK_SIZE (2 * MEGABYTE)
#define TEST_STACK_SIZE (256 * 1024ULL)

/* the thunks every import leads to, at the top of the image's range */
#define THUNK_SIZE 32
#define GUEST_SECONDS 60
#define CALL_LINES 400

int32_t sceKernelVirtualQuery(const void *address, int flags, void *info, size_t size);
#define VIRTUAL_QUERY_FIND_NEXT 1

/* the PS4's SceKernelVirtualQueryInfo */
struct virtual_query_info
{
	uint64_t start;
	uint64_t end;
	uint64_t offset;
	int32_t protection;
	int32_t memory_type;
	uint32_t flags; /* flexible, direct, stack, pooled, committed: bits 0 to 4 */
	char name[32];
};

void ps5_fp_ieee(void);

extern const unsigned char guest_code_start[];
extern const unsigned char guest_code_end[];
extern const unsigned char guest_add[];
extern const unsigned char guest_load32[];
extern const unsigned char guest_call_host[];
extern const unsigned char guest_stack_check[];
extern const unsigned char import_dispatch[];
long run_on_stack(long (*function)(long), long argument, void *stack_top);

/* ---------- faults */

static sigjmp_buf fault_jump;
static sigjmp_buf guest_jump;
static volatile sig_atomic_t fault_armed;
static volatile sig_atomic_t guest_running;
static volatile int fault_signal;
static volatile uintptr_t fault_address;

static const char *signal_name(int signal_number)
{
	switch (signal_number)
	{
	case SIGSEGV:
		return "SIGSEGV";
	case SIGBUS:
		return "SIGBUS";
	case SIGILL:
		return "SIGILL";
	case SIGFPE:
		return "SIGFPE";
	case SIGTRAP:
		return "SIGTRAP";
	}
	return "signal";
}

static void report_registers(const ucontext_t *context)
{
	const mcontext_t *m = &context->uc_mcontext;

	say("  rip %016lx rsp %016lx rbp %016lx rflags %08lx\n", (unsigned long)m->mc_rip,
		(unsigned long)m->mc_rsp, (unsigned long)m->mc_rbp, (unsigned long)m->mc_rflags);
	say("  rax %016lx rbx %016lx rcx %016lx rdx %016lx\n", (unsigned long)m->mc_rax, (unsigned long)m->mc_rbx,
		(unsigned long)m->mc_rcx, (unsigned long)m->mc_rdx);
	say("  rsi %016lx rdi %016lx r8  %016lx r9  %016lx\n", (unsigned long)m->mc_rsi, (unsigned long)m->mc_rdi,
		(unsigned long)m->mc_r8, (unsigned long)m->mc_r9);
	say("  r10 %016lx r11 %016lx r12 %016lx r13 %016lx\n", (unsigned long)m->mc_r10, (unsigned long)m->mc_r11,
		(unsigned long)m->mc_r12, (unsigned long)m->mc_r13);
	say("  r14 %016lx r15 %016lx\n", (unsigned long)m->mc_r14, (unsigned long)m->mc_r15);
}

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
	if (guest_running)
	{
		guest_running = 0;
		say("the game faulted: %s at address 0x%lx\n", signal_name(signal_number), (unsigned long)info->si_addr);
		report_registers(context);
		if (((const ucontext_t *)context)->uc_mcontext.mc_rip >= HALO_GUEST_IMAGE_BASE &&
			((const ucontext_t *)context)->uc_mcontext.mc_rip < HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
			say("  (in the game's image: look the address up in build/linux_x86_64/halo_guest.elf.map)\n");
		siglongjmp(guest_jump, 2);
	}
	if (!fault_armed)
	{
		signal(signal_number, SIG_DFL);
		raise(signal_number);
		return;
	}
	fault_armed = 0;
	fault_signal = signal_number;
	fault_address = (uintptr_t)info->si_addr;
	siglongjmp(fault_jump, 1);
}

static int install_signal_stack(void)
{
	stack_t alternate;

	alternate.ss_sp = malloc(0x20000);
	alternate.ss_size = 0x20000;
	alternate.ss_flags = 0;
	if (!alternate.ss_sp || sigaltstack(&alternate, NULL) != 0)
	{
		say("sigaltstack: failed (%s); a fault on a bad stack will not be caught\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void install_fault_handler(void)
{
	static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
	struct sigaction action;
	size_t index;

	install_signal_stack();
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fault_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++)
	{
		if (sigaction(signals[index], &action, NULL) != 0)
			say("sigaction(%d): failed (%s)\n", signals[index], strerror(errno));
	}
}

/* Calls function(argument). Returns 1 and stores its result if it
returned, 0 if it faulted (fault_signal and fault_address say how). */
static int guarded(long (*function)(void *), void *argument, long *result)
{
	long value;

	fault_signal = 0;
	fault_address = 0;
	fault_armed = 1;
	if (sigsetjmp(fault_jump, 1) == 0)
	{
		value = function(argument);
		fault_armed = 0;
		if (result)
			*result = value;
		return 1;
	}
	return 0;
}

static void say_fault(const char *what)
{
	say("%s: FAULT %s at 0x%lx\n", what, signal_name(fault_signal), (unsigned long)fault_address);
}

static long touch_thunk(void *argument)
{
	volatile uint32_t *word = argument;

	*word = HALO_GUEST_MAGIC;
	return *word;
}

/* ---------- 1. the process */

static int stack_marker;

static void probe_process(void)
{
	size_t flexible_configured = 0, flexible_available = 0;
	int64_t direct_start = 0;
	size_t direct_largest = 0;
	void *heap, *anonymous;
	int32_t result;

	heading("1. the process");
	result = sceKernelConfiguredFlexibleMemorySize(&flexible_configured);
	say("flexible memory configured: 0x%08x, %zu MB\n", (unsigned)result, (size_t)(flexible_configured / MEGABYTE));
	result = sceKernelAvailableFlexibleMemorySize(&flexible_available);
	say("flexible memory available: 0x%08x, %zu MB\n", (unsigned)result, (size_t)(flexible_available / MEGABYTE));
	say("direct memory: %lld MB\n", (long long)sceKernelGetDirectMemorySize() / (long long)MEGABYTE);
	result = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), DIRECT_UNIT, &direct_start,
		&direct_largest);
	say("direct memory's largest free block: 0x%08x, %zu MB at 0x%llx\n", (unsigned)result,
		(size_t)(direct_largest / MEGABYTE), (unsigned long long)direct_start);
	heap = malloc(MEGABYTE);
	anonymous = mmap(NULL, MEGABYTE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	say("this program's code %p, its data %p, the stack %p, malloc %p, anonymous mmap %p\n",
		(void *)probe_process, (void *)&probe_process, (void *)&stack_marker, heap, anonymous);
	free(heap);
	if (anonymous != MAP_FAILED)
		munmap(anonymous, MEGABYTE);
}

/* ---------- 2. the map */

static void probe_map(void)
{
	struct virtual_query_info info;
	uint64_t address = 0;
	int regions = 0;

	heading("2. the map (sceKernelVirtualQuery)");
	say("%-18s %-18s %8s prot type flags name\n", "start", "end", "MB");
	while (regions < 2000)
	{
		int32_t result;

		memset(&info, 0, sizeof(info));
		result = sceKernelVirtualQuery((const void *)address, VIRTUAL_QUERY_FIND_NEXT, &info, sizeof(info));
		if (result != 0)
		{
			if (regions == 0)
				say("sceKernelVirtualQuery(0x%llx): 0x%08x\n", (unsigned long long)address, (unsigned)result);
			break;
		}
		say("0x%016llx 0x%016llx %8llu %4x %4d %5x %.32s\n", (unsigned long long)info.start,
			(unsigned long long)info.end, (unsigned long long)((info.end - info.start) / MEGABYTE),
			(unsigned)info.protection, info.memory_type, (unsigned)info.flags, info.name);
		regions++;
		if (info.end <= address)
			break;
		address = info.end;
	}
	say("%d regions\n", regions);
}

/* ---------- 3. free space below 4 GB */

enum place
{
	PLACE_FREE,
	PLACE_TAKEN,
	PLACE_REFUSED,
};

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

static uint64_t largest_free_start, largest_free_size;

static void probe_free_space(void)
{
	uint64_t address;
	uint64_t run_start = MEGABYTE;
	enum place run = PLACE_FREE;
	int32_t run_result = 0;
	uint64_t free_total = 0;

	heading("3. free space below 4 GB (a reservation at each megabyte)");
	for (address = MEGABYTE; address <= LOW_LIMIT; address += MEGABYTE)
	{
		enum place place = run;
		int32_t result = 0;

		if (address < LOW_LIMIT)
		{
			result = reserve_exactly(address, MEGABYTE);
			if (result == 0)
			{
				sceKernelMunmap((void *)address, MEGABYTE);
				place = PLACE_FREE;
			}
			else
				place = result == -1 ? PLACE_TAKEN : PLACE_REFUSED;
		}
		if (address == LOW_LIMIT || place != run || (place == PLACE_REFUSED && result != run_result))
		{
			if (address > run_start)
			{
				say("0x%09llx-0x%09llx %5llu MB %s", (unsigned long long)run_start, (unsigned long long)address,
					(unsigned long long)((address - run_start) / MEGABYTE),
					run == PLACE_FREE ? "free" : run == PLACE_TAKEN ? "taken (placed elsewhere)" : "refused");
				if (run == PLACE_REFUSED)
					say(" (0x%08x)", (unsigned)run_result);
				say("\n");
				if (run == PLACE_FREE)
				{
					free_total += address - run_start;
					if (address - run_start > largest_free_size)
					{
						largest_free_size = address - run_start;
						largest_free_start = run_start;
					}
				}
			}
			run = place;
			run_result = result;
			run_start = address;
		}
	}
	say("free below 4 GB: %llu MB; the largest run is %llu MB at 0x%llx\n", (unsigned long long)(free_total / MEGABYTE),
		(unsigned long long)(largest_free_size / MEGABYTE), (unsigned long long)largest_free_start);
}

/* ---------- 4. the guest's ranges */

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
	say("%s: 0x%llx-0x%llx, %llu MB of direct memory%s, zeroed, in %llu ms\n", what, (unsigned long long)address,
		(unsigned long long)(address + size), (unsigned long long)(size / MEGABYTE),
		executable ? ", read, write and execute" : "", (unsigned long long)((now_us() - start) / 1000));
	return 1;
}

static int image_ready, window_ready, pool_ready;
static uint64_t pool_base, pool_next, pool_end;
/* where the Xbox window was placed */
static uint64_t window_base;

/* whether [address, address + size) lies inside one of the ranges mapped
for the guest */
static int guest_range_mapped(uint64_t address, uint64_t size)
{
	uint64_t ranges[][2] = {
		{ HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE },
		{ window_base, window_ready ? HALO_GUEST_WINDOW_SIZE : 0 },
		{ HALO_GUEST_CUSTOM_EDITION_BASE, HALO_GUEST_CUSTOM_EDITION_END - HALO_GUEST_CUSTOM_EDITION_BASE },
		{ pool_base, pool_ready ? POOL_SIZE : 0 },
	};
	size_t index;

	for (index = 0; index < sizeof(ranges) / sizeof(ranges[0]); index++)
		if (address >= ranges[index][0] && address + size <= ranges[index][0] + ranges[index][1])
			return 1;
	return 0;
}

static void probe_anonymous_low(void)
{
	uint64_t address = largest_free_start + largest_free_size - 16 * MEGABYTE;
	void *mapped;
	long value;

	if (largest_free_size < 32 * MEGABYTE)
		return;
	mapped = mmap((void *)address, MEGABYTE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (mapped == MAP_FAILED)
		say("anonymous mmap(MAP_FIXED) at 0x%llx: failed (%s)\n", (unsigned long long)address, strerror(errno));
	else
	{
		say("anonymous mmap(MAP_FIXED) at 0x%llx: placed at %p, ", (unsigned long long)address, mapped);
		if (guarded(touch_thunk, mapped, &value))
			say("written and read back\n");
		else
			say_fault("written");
		munmap(mapped, MEGABYTE);
	}
	mapped = mmap((void *)address, MEGABYTE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
		-1, 0);
	if (mapped == MAP_FAILED)
		say("anonymous mmap(MAP_FIXED, read, write, execute) at 0x%llx: failed (%s)\n", (unsigned long long)address,
			strerror(errno));
	else
	{
		say("anonymous mmap(MAP_FIXED, read, write, execute) at 0x%llx: placed at %p\n", (unsigned long long)address,
			mapped);
		munmap(mapped, MEGABYTE);
	}
}

static void probe_guest_ranges(void)
{
	heading("4. the guest's ranges in direct memory");
	/* (first: a fixed mapping replaces whatever is there) */
	probe_anonymous_low();
	image_ready = map_direct("image", HALO_GUEST_IMAGE_BASE, HALO_GUEST_IMAGE_RESERVE, 1);
	/* the window: at 0x80000000 if it is free, else on the next whole 256 MB
	below 4 GB (the guest is built to take it anywhere, HALO_MOVABLE_WINDOW:
	this console loads a system module at 0x80000000) */
	for (window_base = HALO_GUEST_WINDOW_BASE; window_base + HALO_GUEST_WINDOW_SIZE <= 0x100000000ULL;
		window_base += HALO_GUEST_WINDOW_ALIGNMENT)
		if ((window_ready = map_direct("Xbox window", window_base, HALO_GUEST_WINDOW_SIZE, 0)))
			break;
	if (!window_ready)
		window_base = 0;
	map_direct("Custom Edition's tag cache", HALO_GUEST_CUSTOM_EDITION_BASE,
		HALO_GUEST_CUSTOM_EDITION_END - HALO_GUEST_CUSTOM_EDITION_BASE, 0);
	/* the pool: on a whole 256 MB below 4 GB, clear of the ranges above
	(each candidate is reserved exactly first, so one in use is passed by) */
	{
		uint64_t candidate;

		for (candidate = 0x10000000ULL; candidate + POOL_SIZE <= 0x100000000ULL && !pool_ready;
			candidate += 0x10000000ULL)
		{
			if (candidate < HALO_GUEST_CUSTOM_EDITION_END && candidate + POOL_SIZE > HALO_GUEST_CUSTOM_EDITION_BASE)
				continue;
			if (candidate < HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE &&
				candidate + POOL_SIZE > HALO_GUEST_IMAGE_BASE)
				continue;
			if (window_ready && candidate < window_base + HALO_GUEST_WINDOW_SIZE &&
				candidate + POOL_SIZE > window_base)
				continue;
			pool_ready = map_direct("pool", candidate, POOL_SIZE, 0);
			if (pool_ready)
				pool_base = candidate;
		}
	}
	if (!pool_ready)
		say("pool: no room was found for %llu MB\n", (unsigned long long)(POOL_SIZE / MEGABYTE));
	pool_next = pool_base;
	pool_end = pool_base + POOL_SIZE;
}

/* ---------- 5. code below 4 GB */

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

/* takes stacks from the top of the pool, so the guest's own mappings,
which take from the bottom, do not run into them */
static uint64_t stack_take(uint64_t size)
{
	if (!pool_ready || pool_end - pool_next < size)
		return 0;
	pool_end -= size;
	return pool_end;
}

static long host_function(long value)
{
	return value * 2;
}

#define GUEST_FUNCTION(base, name) ((uint64_t)(base) + (uint64_t)((name) - guest_code_start))

struct code_test
{
	uint64_t code;
	uint64_t data;
	uint64_t stack_top;
};

static long run_add(void *argument)
{
	const struct code_test *test = argument;

	return ((int (*)(int, int))GUEST_FUNCTION(test->code, guest_add))(40, 2);
}

static long run_load32(void *argument)
{
	const struct code_test *test = argument;

	return ((int (*)(uint32_t))GUEST_FUNCTION(test->code, guest_load32))((uint32_t)test->data);
}

static long run_call_host(void *argument)
{
	const struct code_test *test = argument;

	return ((int (*)(int, int, long (*)(long)))GUEST_FUNCTION(test->code, guest_call_host))(20, 1, host_function);
}

static long run_stack_check(void *argument)
{
	const struct code_test *test = argument;

	return run_on_stack((long (*)(long))GUEST_FUNCTION(test->code, guest_stack_check), 0x12345678,
		(void *)test->stack_top);
}

static void check(const char *what, long (*function)(void *), struct code_test *test, long expected)
{
	long value = 0;

	if (!guarded(function, test, &value))
		say_fault(what);
	else
		say("%s: %ld, %s\n", what, value, value == expected ? "right" : "WRONG");
}

static void *thread_test(void *argument)
{
	struct code_test *test = argument;
	int marker;
	long value;

	value = ((int (*)(int, int))GUEST_FUNCTION(test->code, guest_add))(1, 2);
	say("thread: running on %p, below 4 GB: %s; low code returned %ld\n", (void *)&marker,
		(uint64_t)&marker < LOW_LIMIT ? "yes" : "NO", value);
	return NULL;
}

static void probe_code(void)
{
	struct code_test test;
	pthread_attr_t attributes;
	pthread_t thread;
	uint64_t stack;
	int result;

	heading("5. code below 4 GB");
	if (!image_ready || !window_ready || !pool_ready)
	{
		say("skipped: the image's range, the window or the pool is missing\n");
		return;
	}
	/* (the top of the image's reserve, which the game does not reach) */
	test.code = HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE - 2 * MEGABYTE;
	test.data = window_base + 0x1000;
	test.stack_top = stack_take(TEST_STACK_SIZE) + TEST_STACK_SIZE;
	memcpy((void *)test.code, guest_code_start, (size_t)(guest_code_end - guest_code_start));
	*(volatile uint32_t *)test.data = HALO_GUEST_MAGIC;
	check("low code runs (40 + 2)", run_add, &test, 42);
	check("a load through a 32-bit pointer into the window", run_load32, &test, HALO_GUEST_MAGIC);
	check("low code calls the host above 4 GB ((20 + 1) * 2)", run_call_host, &test, 42);
	check("a stack below 4 GB, through %esp", run_stack_check, &test, 0x12345678);

	stack = stack_take(TEST_STACK_SIZE);
	pthread_attr_init(&attributes);
	result = pthread_attr_setstack(&attributes, (void *)stack, TEST_STACK_SIZE);
	say("pthread_attr_setstack(0x%llx): %d\n", (unsigned long long)stack, result);
	result = pthread_create(&thread, &attributes, thread_test, &test);
	say("pthread_create with that stack: %d\n", result);
	if (result == 0)
		pthread_join(thread, NULL);
	pthread_attr_destroy(&attributes);
	memset((void *)test.code, 0, (size_t)(guest_code_end - guest_code_start));
}

/* ---------- 6. the game */

static const struct halo_guest_header *guest_header;
static const char *const *import_name_list;
static uint32_t import_count;
static uint32_t guest_tp;
static volatile uint64_t calls;
static volatile int guest_done;
static char guest_end[256];
/* (set by import_dispatch, guest_probe.S) */
unsigned int current_import;
static unsigned int syscall_counts[512];

static const char *syscall_name(long long number)
{
	if (number >= 0 && number < (long long)(sizeof(guest_syscall_names) / sizeof(guest_syscall_names[0])) &&
		guest_syscall_names[number])
		return guest_syscall_names[number];
	return "?";
}

static void finish(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
static void finish(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(guest_end, sizeof(guest_end), format, arguments);
	va_end(arguments);
	guest_running = 0;
	siglongjmp(guest_jump, 1);
}

static void say_call(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say_call(const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (calls > CALL_LINES)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	say("%6llu %s", (unsigned long long)calls, line);
}

/* a NUL-terminated string in guest memory, for the report */
static const char *guest_string(uint64_t address)
{
	if (!address || address >= LOW_LIMIT)
		return "(none)";
	return (const char *)address;
}

static int64_t guest_clock(long long clock, uint64_t timespec)
{
	struct timespec now;
	/* Linux's clock numbers: REALTIME 0, MONOTONIC 1, and the rest are
	treated as the monotonic clock */
	clockid_t host = clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;

	if (clock_gettime(host, &now) != 0)
		return -GUEST_EINVAL;
	/* x32's struct timespec: a 64-bit tv_sec, and tv_nsec in 8 bytes */
	((int64_t *)timespec)[0] = now.tv_sec;
	((int64_t *)timespec)[1] = now.tv_nsec;
	return 0;
}

static int64_t guest_system_call(long long number, long long a, long long b, long long c, long long d,
	long long e, long long f)
{
	if (number >= 0 && number < 512)
		syscall_counts[number]++;
	switch (number)
	{
	case GUEST_SYS_mmap:
	{
		uint64_t taken;

		(void)a, (void)c, (void)f;
		if ((int)e != -1)
		{
			say_call("  mmap of a file (%d): not served\n", (int)e);
			return -GUEST_ENOSYS;
		}
		if (d & (0x10 | 0x100000))
		{
			/* MAP_FIXED, or MAP_FIXED_NOREPLACE (the window's and Custom
			Edition's reservations), inside what is already mapped for the
			guest */
			if (!guest_range_mapped((uint64_t)a, (uint64_t)b))
			{
				say_call("  mmap(0x%llx, %lld bytes, fixed): outside the guest's ranges\n",
					(unsigned long long)a, b);
				return (d & 0x10) ? -GUEST_ENOMEM : -GUEST_EEXIST;
			}
			if (d & 0x10)
				memset((void *)(uintptr_t)a, 0, (size_t)b);
			return a;
		}
		taken = pool_take((uint64_t)b);
		if (!taken)
		{
			say_call("  mmap(%lld bytes): the pool is out of room\n", b);
			return -GUEST_ENOMEM;
		}
		memset((void *)taken, 0, (size_t)b);
		return (int64_t)taken;
	}
	case GUEST_SYS_munmap:
	case GUEST_SYS_mprotect:
	case GUEST_SYS_madvise:
	case GUEST_SYS_rt_sigprocmask:
	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_sigaltstack:
	case GUEST_SYS_sched_yield:
		return 0;
	case GUEST_SYS_brk:
		return 0;
	case GUEST_SYS_set_tid_address:
	case GUEST_SYS_gettid:
	case GUEST_SYS_getpid:
		return 1;
	case GUEST_SYS_getppid:
		return 0;
	case GUEST_SYS_getuid:
	case GUEST_SYS_geteuid:
	case GUEST_SYS_getgid:
	case GUEST_SYS_getegid:
		return 1000;
	case GUEST_SYS_clock_gettime:
		return guest_clock(a, (uint64_t)b);
	case GUEST_SYS_clock_getres:
		if (b)
		{
			((int64_t *)(uintptr_t)b)[0] = 0;
			((int64_t *)(uintptr_t)b)[1] = 1;
		}
		return 0;
	case GUEST_SYS_futex:
		/* one thread: a wait would never end, and a wake wakes no one */
		return (b & 0x7f) == 0 ? -GUEST_EAGAIN : 0;
	case GUEST_SYS_write:
		if (a == 1 || a == 2)
		{
			char text[400];
			size_t length = (size_t)c < sizeof(text) - 1 ? (size_t)c : sizeof(text) - 1;

			memcpy(text, (const void *)(uintptr_t)b, length);
			text[length] = 0;
			say_call("  the game writes to %s: %s%s", a == 1 ? "stdout" : "stderr", text,
				length && text[length - 1] == '\n' ? "" : "\n");
			return c;
		}
		break;
	case GUEST_SYS_writev:
		if (a == 1 || a == 2)
		{
			const uint32_t *vector = (const uint32_t *)(uintptr_t)b;
			long long total = 0;
			int index;

			for (index = 0; index < (int)c; index++)
			{
				char text[400];
				size_t length = vector[index * 2 + 1] < sizeof(text) - 1 ? vector[index * 2 + 1] : sizeof(text) - 1;

				memcpy(text, (const void *)(uintptr_t)vector[index * 2], length);
				text[length] = 0;
				if (length)
					say_call("  the game writes to %s: %s%s", a == 1 ? "stdout" : "stderr", text,
						text[length - 1] == '\n' ? "" : "\n");
				total += vector[index * 2 + 1];
			}
			return total;
		}
		break;
	case GUEST_SYS_getcwd:
		if (b > 6)
		{
			memcpy((void *)(uintptr_t)a, "/app0", 6);
			return 6;
		}
		return -GUEST_EINVAL;
	case GUEST_SYS_openat:
	case GUEST_SYS_newfstatat:
	case GUEST_SYS_faccessat:
	case GUEST_SYS_readlinkat:
	case GUEST_SYS_mkdirat:
		say_call("  %s(\"%s\"): not found (the probe serves no files)\n", syscall_name(number),
			guest_string((uint64_t)b));
		return -GUEST_ENOENT;
	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		finish("the game exited with %lld", a);
	}
	say_call("  %s (%lld): not served\n", syscall_name(number), number);
	return -GUEST_ENOSYS;
}

/* every import's thunk leads here (import_dispatch, guest_probe.S) */
int64_t import_called(long long a, long long b, long long c, long long d, long long e, long long f, long long g);
int64_t import_called(long long a, long long b, long long c, long long d, long long e, long long f, long long g)
{
	const char *name = current_import < import_count ? import_name_list[current_import] : "?";
	int64_t result;

	calls++;
	if (!strcmp(name, "host_get_tp"))
		return guest_tp;
	if (!strcmp(name, "host_set_tp"))
	{
		guest_tp = (uint32_t)a;
		say_call("host_set_tp(0x%08x)\n", guest_tp);
		return 0;
	}
	if (!strcmp(name, "host_errno"))
		return 0;
	if (!strcmp(name, "host_log"))
	{
		say_call("the game logs (%lld): %s\n", a, guest_string((uint64_t)b));
		return 0;
	}
	if (!strcmp(name, "host_abort"))
		finish("the game aborted: %s", guest_string((uint64_t)a));
	if (!strcmp(name, "host_exit"))
		finish("the game exited with %lld", a);
	if (!strcmp(name, "host_syscall"))
	{
		result = guest_system_call(a, b, c, d, e, f, g);
		if (a != GUEST_SYS_write && a != GUEST_SYS_writev)
			say_call("host_syscall %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = %lld\n", syscall_name(a), b, c, d, e,
				(long long)result);
		return result;
	}
	finish("the first import the probe does not answer: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", name, a, b, c, d);
}

static int load_guest(void)
{
	Elf32_Ehdr header;
	Elf32_Phdr segments[16];
	int file;
	int index;
	uint64_t image_end = HALO_GUEST_IMAGE_BASE;

	file = open("/app0/halo_guest.elf", O_RDONLY);
	if (file < 0)
	{
		say("/app0/halo_guest.elf: %s\n", strerror(errno));
		return 0;
	}
	if (pread(file, &header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
		memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS32 ||
		header.e_machine != EM_X86_64 || header.e_type != ET_EXEC || header.e_phnum > 16 ||
		header.e_phentsize != sizeof(Elf32_Phdr))
	{
		say("halo_guest.elf is not a 32-bit x86-64 executable (x32)\n");
		close(file);
		return 0;
	}
	if (pread(file, segments, header.e_phnum * sizeof(Elf32_Phdr), header.e_phoff) !=
		(ssize_t)(header.e_phnum * sizeof(Elf32_Phdr)))
	{
		say("halo_guest.elf: its program headers cannot be read\n");
		close(file);
		return 0;
	}
	for (index = 0; index < header.e_phnum; index++)
	{
		const Elf32_Phdr *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_vaddr < HALO_GUEST_IMAGE_BASE ||
			(uint64_t)segment->p_vaddr + segment->p_memsz > HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE - 4 * MEGABYTE)
		{
			say("segment %d (0x%08x, %u bytes) is outside the image's range\n", index, segment->p_vaddr,
				segment->p_memsz);
			close(file);
			return 0;
		}
		if (segment->p_filesz &&
			pread(file, (void *)(uintptr_t)segment->p_vaddr, segment->p_filesz, segment->p_offset) !=
				(ssize_t)segment->p_filesz)
		{
			say("segment %d cannot be read\n", index);
			close(file);
			return 0;
		}
		say("segment %d: 0x%08x-0x%08x, %u bytes from the file, flags %x\n", index, segment->p_vaddr,
			segment->p_vaddr + segment->p_memsz, segment->p_filesz, segment->p_flags);
		if (segment->p_vaddr + segment->p_memsz > image_end)
			image_end = segment->p_vaddr + segment->p_memsz;
	}
	close(file);

	guest_header = (const struct halo_guest_header *)(uintptr_t)HALO_GUEST_IMAGE_BASE;
	say("header: magic 0x%08x (%s), ABI %u (the probe's %u), image end 0x%08x (loaded to 0x%08llx)\n",
		guest_header->magic, guest_header->magic == HALO_GUEST_MAGIC ? "right" : "WRONG", guest_header->abi_version,
		HALO_GUEST_ABI_VERSION, guest_header->image_end, (unsigned long long)image_end);
	say("entry points: start 0x%08x, thread start 0x%08x, attach 0x%08x, detach 0x%08x, constructors 0x%08x-0x%08x\n",
		guest_header->start, guest_header->thread_start, guest_header->thread_attach, guest_header->thread_detach,
		guest_header->init_array_start, guest_header->init_array_end);
	return guest_header->magic == HALO_GUEST_MAGIC && guest_header->abi_version == HALO_GUEST_ABI_VERSION;
}

static int bind_imports(void)
{
	uint64_t *table = (uint64_t *)(uintptr_t)guest_header->import_table;
	const char *names = (const char *)(uintptr_t)guest_header->import_names;
	unsigned char *thunks = (unsigned char *)(uintptr_t)(HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE - MEGABYTE);
	static const char *list[1024];
	uint32_t index;

	import_count = *(const uint32_t *)(uintptr_t)guest_header->import_count;
	say("%u imports\n", import_count);
	if (import_count > 1024)
		return 0;
	for (index = 0; index < import_count; index++)
	{
		unsigned char *thunk = thunks + index * THUNK_SIZE;
		uint64_t dispatch = (uint64_t)import_dispatch;

		list[index] = names;
		names += strlen(names) + 1;
		/* mov $index, %r10d; movabs $import_dispatch, %r11; jmp *%r11 */
		thunk[0] = 0x41;
		thunk[1] = 0xba;
		memcpy(thunk + 2, &index, 4);
		thunk[6] = 0x49;
		thunk[7] = 0xbb;
		memcpy(thunk + 8, &dispatch, 8);
		thunk[16] = 0x41;
		thunk[17] = 0xff;
		thunk[18] = 0xe3;
		table[index] = (uint64_t)(uintptr_t)thunk;
	}
	import_name_list = list;
	return 1;
}

static uint64_t boot_address;

static void *guest_thread(void *argument)
{
	uint32_t mxcsr = 0;

	(void)argument;
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	say("the game's thread starts on %p; MXCSR 0x%08x", (void *)&mxcsr, mxcsr);
	ps5_fp_ieee();
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	say(", 0x%08x after ps5_fp_ieee\n", mxcsr);
	install_signal_stack();
	if (sigsetjmp(guest_jump, 1) == 0)
	{
		guest_running = 1;
		((void (*)(uint64_t))(uintptr_t)guest_header->start)(boot_address);
		guest_running = 0;
		snprintf(guest_end, sizeof(guest_end), "the game returned from __guest_start");
	}
	guest_done = 1;
	return NULL;
}

static void probe_game(void)
{
	struct halo_guest_boot *boot;
	uint32_t *pointers;
	char *strings;
	pthread_attr_t attributes;
	pthread_t thread;
	uint64_t stack, start;
	int result;
	int number;

	heading("6. the game");
	if (!image_ready || !window_ready || !pool_ready)
	{
		say("skipped: the image's range, the window or the pool is missing\n");
		return;
	}
	if (!load_guest() || !bind_imports())
		return;

	/* the boot structure, argv and the environment, in guest memory */
	boot = (struct halo_guest_boot *)(uintptr_t)pool_take(DIRECT_UNIT);
	pointers = (uint32_t *)(boot + 1);
	strings = (char *)(pointers + 8);
	strcpy(strings, "halo");
	pointers[0] = (uint32_t)(uintptr_t)strings;
	pointers[1] = 0;
	pointers[2] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)&pointers[0];
	boot->environment = (uint32_t)(uintptr_t)&pointers[2];
	boot->page_size = 0x4000;
	boot->contiguous_base = (uint32_t)window_base;
	boot->contiguous_size = HALO_GUEST_WINDOW_SIZE;
	boot->image_shift = 0;
	boot_address = (uint64_t)(uintptr_t)boot;

	stack = stack_take(GUEST_STACK_SIZE);
	pthread_attr_init(&attributes);
	pthread_attr_setstack(&attributes, (void *)stack, GUEST_STACK_SIZE);
	result = pthread_create(&thread, &attributes, guest_thread, NULL);
	pthread_attr_destroy(&attributes);
	if (result != 0)
	{
		say("pthread_create for the game: %d\n", result);
		return;
	}
	start = now_us();
	while (!guest_done && now_us() - start < GUEST_SECONDS * 1000000ULL)
		usleep(100000);
	if (!guest_done)
	{
		say("the game is still running after %d s, after %llu calls; the last was %s\n", GUEST_SECONDS,
			(unsigned long long)calls,
			current_import < import_count ? import_name_list[current_import] : "none");
		return;
	}
	pthread_join(thread, NULL);
	say("%s\n", guest_end);
	say("after %llu calls to the host in %.3f s, %llu MB of the pool taken\n", (unsigned long long)calls,
		(now_us() - start) / 1e6, (unsigned long long)((pool_next - pool_base) / MEGABYTE));
	say("system calls:");
	for (number = 0; number < 512; number++)
	{
		if (syscall_counts[number])
			say(" %s %u", syscall_name(number), syscall_counts[number]);
	}
	say("\n");
}

int main(void)
{
	report_open("guest-probe.txt");
	notify("Halo PS5 guest probe starts");
	install_fault_handler();
	probe_process();
	probe_map();
	probe_free_space();
	probe_guest_ranges();
	probe_code();
	probe_game();
	heading("done");
	notify("Halo PS5 guest probe is done: read guest-probe.txt over FTP");
	report_close();
	/* (the game's thread may still be running) */
	_exit(0);
}
