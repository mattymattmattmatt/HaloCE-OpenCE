/*
PS5 MEM PROBE

Answers the question PS5_HANDOFF.md asks first: can a PS5 homebrew process
keep memory below 4 GB at fixed addresses and run code there? The Android
and Switch ports run the game as an ILP32 guest whose pointers are 32 bits,
so everything the guest touches (its image, its heap, its stacks, the Xbox
window) has to be below 4 GB. The PS5 port would build that guest for x32
(64-bit x86 instructions, 32-bit pointers), so this probe asks:

1. What is already mapped in this process, below 4 GB and above it
   (sysctl KERN_PROC_VMMAP)?
2. Where below 4 GB will mmap(MAP_FIXED) place a page? MAP_EXCL is
   ignored here (the first run found it mapping over a page), so each
   fixed mapping first reads the map and goes only where nothing is. The
   probe walks the whole range a megabyte at a time and reports the runs,
   so the answer is where usable memory starts and ends, not a yes or no
   about a few chosen addresses (the Switch's probe asked the latter and
   took three more runs to find where its memory began).
3. Can the ranges the guest is built for be mapped whole: the image at
   0x3c000000, Custom Edition's tag cache at 0x40440000, the Xbox window at
   0x80000000 (128 MB, and the 512 MB it can grow to)?
4. Can memory there be made executable, and by which route: mmap with
   PROT_EXEC, mprotect after writing, a shared memory object mapped twice
   (writable above, executable below), or the jailbreak's kernel_mprotect?
   For each that works: is executable memory still readable, does code
   there run, load through a 32-bit pointer, jump to the host's code above
   4 GB, and run on a stack below 4 GB?
5. Will a thread start on a stack below 4 GB?

Build: make -C port/ps5/probe/mem   (needs PS5_PAYLOAD_SDK)
Run:   make -C port/ps5/probe/mem test PS5_HOST=<console> [PS5_PORT=9021]

The report goes to stdout, to the kernel log (klogsrv, port 3232) and to
/data/halo-ps5-mem-probe.txt (read it back over FTP). Each line is written
as it is made, so a crash leaves everything up to it. Every call is
reported whether it succeeded or not; "no" answers the question as well as
"yes" does.
*/

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
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define REPORT_PATH "/data/halo-ps5-mem-probe.txt"

#define PAGE 0x1000ULL
#define MEGABYTE 0x100000ULL
#define LOW_LIMIT 0x100000000ULL

/* the guest's layout (port/android/include/halo_android_abi.h) */
#define GUEST_IMAGE_BASE 0x3c000000ULL
#define GUEST_IMAGE_RESERVE 0x01000000ULL
#define GUEST_CUSTOM_EDITION_BASE 0x40440000ULL
#define GUEST_CUSTOM_EDITION_END 0x41b40000ULL
#define GUEST_WINDOW_BASE 0x80000000ULL
#define GUEST_WINDOW_SIZE 0x08000000ULL
#define GUEST_WINDOW_MAXIMUM 0x20000000ULL

/* the execution tests' areas, from a base below 4 GB: the code, then (a
megabyte on) a data page with a stack above it, then a thread's stack */
#define CODE_SIZE 0x10000ULL
#define DATA_OFFSET (1 * MEGABYTE)
#define DATA_SIZE 0x10000ULL
#define THREAD_STACK_OFFSET (2 * MEGABYTE)
#define THREAD_STACK_SIZE 0x40000ULL

#define DATA_MAGIC 0x4f4c4148u /* 'HALO' */

extern const unsigned char guest_code_start[];
extern const unsigned char guest_code_end[];
extern const unsigned char guest_add[];
extern const unsigned char guest_load32[];
extern const unsigned char guest_call_host[];
extern const unsigned char guest_stack_check[];
long run_on_stack(long (*function)(long), long argument, void *stack_top);

typedef struct notify_request
{
	char useless1[45];
	char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static FILE *report_file;

static void say(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	klog_printf("%s", line);
	/* the ELF loader may pass stdout back over the socket that sent the
	payload, when the kernel log and FTP are not running */
	fputs(line, stdout);
	fflush(stdout);
	if (report_file)
	{
		fputs(line, report_file);
		fflush(report_file);
	}
}

static void heading(const char *text)
{
	say("\n=== %s ===\n", text);
}

static void notify(const char *message)
{
	notify_request_t request;

	memset(&request, 0, sizeof(request));
	strncpy(request.message, message, sizeof(request.message) - 1);
	sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

/* ---------- faults

A probe that touches memory it is unsure of has to survive the touch. The
handler runs on its own stack, since a fault can come from code running on
a stack the probe made itself. */

static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_armed;
static volatile int fault_signal;
static volatile uintptr_t fault_address;

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
	(void)context;
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

static void install_fault_handler(void)
{
	static const int signals[] = {SIGSEGV, SIGBUS, SIGILL};
	struct sigaction action;
	stack_t alternate;
	size_t index;

	alternate.ss_sp = malloc(0x10000);
	alternate.ss_size = 0x10000;
	alternate.ss_flags = 0;
	if (!alternate.ss_sp || sigaltstack(&alternate, NULL) != 0)
		say("sigaltstack: failed (%s); faults on a bad stack will not be caught\n",
			strerror(errno));
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
	}
	return "signal";
}

static void say_fault(const char *what)
{
	say("%s: FAULT %s at 0x%lx\n", what, signal_name(fault_signal),
		(unsigned long)fault_address);
}

static long touch_thunk(void *argument)
{
	volatile uint32_t *word = argument;

	*word = DATA_MAGIC;
	return *word;
}

static long read_thunk(void *argument)
{
	return *(volatile const unsigned char *)argument;
}

/* The process's map, read with sysctl KERN_PROC_VMMAP. MAP_EXCL is ignored
on the PS5 (the first run mapped over a page with it), so this is what keeps
the probe from mapping over the loader, the libraries or itself: it is read
again before every fixed mapping. */

#define MAP_ENTRIES_MAXIMUM 512

struct map_entry
{
	uint64_t start;
	uint64_t end;
	int protection;
	int type;
};

static struct map_entry map_entries[MAP_ENTRIES_MAXIMUM];
static int map_entry_count;
static char map_buffer[MAP_ENTRIES_MAXIMUM * sizeof(struct kinfo_vmentry)];

/* Returns 0, or errno. */
static int read_vm_map(void)
{
	int mib[4];
	size_t size;
	char *cursor;
	char *end;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_VMMAP;
	mib[3] = getpid();
	size = sizeof(map_buffer);
	if (sysctl(mib, 4, map_buffer, &size, NULL, 0) != 0)
		return errno ? errno : -2;
	map_entry_count = 0;
	end = map_buffer + size;
	for (cursor = map_buffer; cursor < end && map_entry_count < MAP_ENTRIES_MAXIMUM;)
	{
		struct kinfo_vmentry *entry = (struct kinfo_vmentry *)cursor;
		struct map_entry *copy = &map_entries[map_entry_count++];

		if (entry->kve_structsize <= 0)
			break;
		copy->start = entry->kve_start;
		copy->end = entry->kve_end;
		copy->protection = entry->kve_protection;
		copy->type = entry->kve_type;
		cursor += entry->kve_structsize;
	}
	return 0;
}

/* Returns 0 if nothing is mapped in [address, address + size), -3 if
something is, or errno if the map could not be read. */
static int range_free(uint64_t address, uint64_t size)
{
	int error;
	int index;

	error = read_vm_map();
	if (error)
		return error;
	for (index = 0; index < map_entry_count; index++)
	{
		if (map_entries[index].start < address + size && map_entries[index].end > address)
			return -3;
	}
	return 0;
}

/* Maps size bytes at exactly address, only where nothing is mapped yet.
Returns 0, or errno; -1 when the system placed it elsewhere, -3 when
something is mapped there already. */
static int map_fixed(uint64_t address, uint64_t size, int protection)
{
	void *result;
	int error;

	error = range_free(address, size);
	if (error)
		return error;
	result = mmap((void *)(uintptr_t)address, size, protection,
		MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0);
	if (result == MAP_FAILED)
		return errno ? errno : -2;
	if ((uintptr_t)result != address)
	{
		munmap(result, size);
		return -1;
	}
	return 0;
}

static const char *map_error(int error)
{
	if (error == -1)
		return "placed elsewhere";
	if (error == -2)
		return "failed without errno";
	if (error == -3)
		return "already mapped";
	if (error == -4)
		return "maps, but faults when touched";
	return strerror(error);
}

/* ---------- 0: where the probe itself is */

static void probe_self(void)
{
	int local;
	void *heap;

	heading("0: the process");
	say("firmware: 0x%08x\n", kernel_get_fw_version());
	say("pid: %d\n", (int)getpid());
	heap = malloc(64);
	say("code (probe_self): %p\n", (void *)probe_self);
	say("data (report_file): %p\n", (void *)&report_file);
	say("stack: %p\n", (void *)&local);
	say("heap (malloc 64): %p\n", heap);
	say("libc (printf): %p\n", (void *)printf);
	free(heap);
}

/* ---------- 1: what is mapped */

static void probe_vm_map(void)
{
	int above = 0;
	int error;
	int index;

	heading("1: the process's map (KERN_PROC_VMMAP)");
	error = read_vm_map();
	if (error)
	{
		say("sysctl: failed (%s)\n", map_error(error));
		return;
	}
	for (index = 0; index < map_entry_count; index++)
	{
		struct map_entry *entry = &map_entries[index];

		if (entry->start >= LOW_LIMIT)
			above++;
		say("%s 0x%011lx-0x%011lx %c%c%c type %d\n",
			entry->start < LOW_LIMIT ? "LOW " : "    ",
			(unsigned long)entry->start, (unsigned long)entry->end,
			entry->protection & KVME_PROT_READ ? 'r' : '-',
			entry->protection & KVME_PROT_WRITE ? 'w' : '-',
			entry->protection & KVME_PROT_EXEC ? 'x' : '-',
			entry->type);
	}
	say("%d entries at or above 4 GB\n", above);
}

/* ---------- 2: where below 4 GB a page can go */

/* map_fixed has to refuse to map over what is there. The first run found
MAP_EXCL ignored, so this checks the map-reading guard instead, and reports
MAP_EXCL again in case a firmware or loader changes it. */
static int map_guard_works(void)
{
	void *page;
	void *again;
	int error;

	page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (page == MAP_FAILED)
	{
		say("guard check: mmap failed (%s)\n", strerror(errno));
		return 0;
	}
	again = mmap(page, PAGE, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0);
	say("MAP_EXCL over %p: %s\n", page,
		again == MAP_FAILED ? "refused" : "mapped over it (ignored)");
	error = map_fixed((uintptr_t)page, PAGE, PROT_READ | PROT_WRITE);
	munmap(page, PAGE);
	if (error != -3)
	{
		say("guard check: map_fixed over %p gave \"%s\", not \"already mapped\"; "
			"the walk and the big mappings are skipped\n", page, map_error(error));
		return 0;
	}
	say("guard check: map_fixed over %p refused (already mapped), as it should be\n", page);
	return 1;
}

/* outcome of one address: 0 mapped and usable, > 0 errno, or map_error's
negative codes (-4 mapped but faulted when touched) */
static int walk_one(uint64_t address)
{
	int error;
	long value;

	error = map_fixed(address, PAGE, PROT_READ | PROT_WRITE);
	if (error)
		return error;
	if (!guarded(touch_thunk, (void *)(uintptr_t)address, &value) || value != DATA_MAGIC)
		error = -4;
	munmap((void *)(uintptr_t)address, PAGE);
	return error;
}

static void say_run(uint64_t start, uint64_t end, int outcome)
{
	const char *text;

	text = outcome == 0 ? "maps" : map_error(outcome);
	say("0x%09lx-0x%09lx  %s\n", (unsigned long)start, (unsigned long)end, text);
}

static uint64_t walk_low_memory(void)
{
	uint64_t address;
	uint64_t run_start;
	uint64_t lowest = 0;
	uint64_t usable = 0;
	int run_outcome;
	int outcome;

	heading("2: one page every megabyte below 4 GB");
	say("first, the pages below 1 MB at 64 KB:\n");
	for (address = 0x10000; address < MEGABYTE; address += 0x10000)
	{
		outcome = walk_one(address);
		if (outcome == 0)
		{
			say("0x%09lx  maps\n", (unsigned long)address);
			if (!lowest)
				lowest = address;
		}
	}
	say("then every megabyte (runs of the same outcome):\n");
	run_start = MEGABYTE;
	run_outcome = walk_one(MEGABYTE);
	for (address = MEGABYTE; address < LOW_LIMIT; address += MEGABYTE)
	{
		outcome = address == MEGABYTE ? run_outcome : walk_one(address);
		if (outcome == 0)
		{
			usable += MEGABYTE;
			if (!lowest)
				lowest = address;
		}
		if (outcome != run_outcome)
		{
			say_run(run_start, address, run_outcome);
			run_start = address;
			run_outcome = outcome;
		}
	}
	say_run(run_start, LOW_LIMIT, run_outcome);
	say("lowest address that maps: 0x%lx\n", (unsigned long)lowest);
	say("megabytes that map: %lu of 4095\n", (unsigned long)(usable / MEGABYTE));
	return lowest;
}

static void probe_map_32bit(void)
{
	void *result;

	result = mmap(NULL, MEGABYTE, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE | MAP_32BIT, -1, 0);
	if (result == MAP_FAILED)
	{
		say("MAP_32BIT (1 MB, no address): failed (%s)\n", strerror(errno));
		return;
	}
	say("MAP_32BIT (1 MB, no address): placed at %p\n", result);
	munmap(result, MEGABYTE);
}

/* ---------- 3: the guest's own ranges, whole */

static void probe_range(const char *name, uint64_t address, uint64_t size)
{
	int error;
	long value;
	uint64_t last;

	error = map_fixed(address, size, PROT_READ | PROT_WRITE);
	if (error)
	{
		say("%s 0x%lx + 0x%lx: refused (%s)\n", name, (unsigned long)address,
			(unsigned long)size, map_error(error));
		return;
	}
	last = address + size - PAGE;
	if (!guarded(touch_thunk, (void *)(uintptr_t)address, &value))
		say_fault("first page");
	else if (!guarded(touch_thunk, (void *)(uintptr_t)last, &value))
		say_fault("last page");
	else
		say("%s 0x%lx + 0x%lx: maps, first and last pages written and read\n",
			name, (unsigned long)address, (unsigned long)size);
	munmap((void *)(uintptr_t)address, size);
}

/* mmap with an address but without MAP_FIXED: does the system take the
hint below 4 GB, or place the memory where it likes? */
static void probe_hint(uint64_t address)
{
	void *result;

	if (range_free(address, MEGABYTE) != 0)
	{
		say("hint 0x%lx: something is mapped there; skipped\n", (unsigned long)address);
		return;
	}
	result = mmap((void *)(uintptr_t)address, MEGABYTE, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (result == MAP_FAILED)
	{
		say("hint 0x%lx: failed (%s)\n", (unsigned long)address, strerror(errno));
		return;
	}
	say("hint 0x%lx: placed at %p (%s)\n", (unsigned long)address, result,
		(uintptr_t)result == address ? "taken" : "ignored");
	munmap(result, MEGABYTE);
}

static void probe_guest_ranges(void)
{
	heading("3: the guest's ranges");
	probe_hint(GUEST_IMAGE_BASE);
	probe_hint(GUEST_WINDOW_BASE);
	probe_range("image", GUEST_IMAGE_BASE, GUEST_IMAGE_RESERVE);
	probe_range("custom edition tag cache", GUEST_CUSTOM_EDITION_BASE,
		GUEST_CUSTOM_EDITION_END - GUEST_CUSTOM_EDITION_BASE);
	probe_range("xbox window", GUEST_WINDOW_BASE, GUEST_WINDOW_SIZE);
	probe_range("xbox window at its largest", GUEST_WINDOW_BASE, GUEST_WINDOW_MAXIMUM);
	probe_range("a 256 MB pool", 0x10000000ULL, 0x10000000ULL);
	probe_range("1 GB from 0x40000000", 0x40000000ULL, 0x40000000ULL);
}

/* ---------- 4: running code below 4 GB */

struct code_test
{
	uintptr_t code; /* the executable view of the code */
	uintptr_t data; /* a writable page below 4 GB */
	uintptr_t stack_top; /* the top of a stack below 4 GB */
};

static long host_triple(int value)
{
	return (long)value * 3;
}

#define GUEST_FUNCTION(test, name) ((test)->code + (uintptr_t)((name) - guest_code_start))

static long add_thunk(void *argument)
{
	struct code_test *test = argument;
	int (*function)(int, int) = (int (*)(int, int))GUEST_FUNCTION(test, guest_add);

	return function(40, 2);
}

static long load32_thunk(void *argument)
{
	struct code_test *test = argument;
	int (*function)(uint32_t) = (int (*)(uint32_t))GUEST_FUNCTION(test, guest_load32);

	return (uint32_t)function((uint32_t)test->data);
}

static long call_host_thunk(void *argument)
{
	struct code_test *test = argument;
	int (*function)(int, int, long (*)(int)) =
		(int (*)(int, int, long (*)(int)))GUEST_FUNCTION(test, guest_call_host);

	return function(40, 2, host_triple);
}

static long low_stack_thunk(void *argument)
{
	struct code_test *test = argument;
	long (*function)(long) = (long (*)(long))GUEST_FUNCTION(test, guest_stack_check);

	return run_on_stack(function, 0x12345678, (void *)test->stack_top);
}

static void run_code_tests(struct code_test *test)
{
	long value;

	if (guarded(read_thunk, (void *)test->code, &value))
		say("  readable: yes\n");
	else
		say_fault("  readable: no");
	if (!guarded(add_thunk, test, &value))
	{
		say_fault("  runs");
		return;
	}
	say("  runs: %s (40 + 2 = %ld)\n", value == 42 ? "yes" : "WRONG RESULT", value);
	if (guarded(load32_thunk, test, &value))
		say("  loads through a 32-bit pointer: %s (0x%lx)\n",
			value == DATA_MAGIC ? "yes" : "WRONG RESULT", (unsigned long)value);
	else
		say_fault("  loads through a 32-bit pointer");
	if (guarded(call_host_thunk, test, &value))
		say("  jumps to the host above 4 GB: %s (%ld)\n",
			value == 126 ? "yes" : "WRONG RESULT", value);
	else
		say_fault("  jumps to the host above 4 GB");
	if (guarded(low_stack_thunk, test, &value))
		say("  runs on a stack below 4 GB: %s (0x%lx)\n",
			value == 0x12345678 ? "yes" : "WRONG RESULT", (unsigned long)value);
	else
		say_fault("  runs on a stack below 4 GB");
}

/* the size of the test code payload.S assembles */
static size_t code_bytes(void)
{
	return (size_t)(guest_code_end - guest_code_start);
}

static void exec_mmap_rwx(struct code_test *test)
{
	int error;

	say("A: mmap PROT_READ | PROT_WRITE | PROT_EXEC\n");
	error = map_fixed(test->code, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
	if (error)
	{
		say("  refused (%s)\n", map_error(error));
		return;
	}
	memcpy((void *)test->code, guest_code_start, code_bytes());
	run_code_tests(test);
	munmap((void *)test->code, CODE_SIZE);
}

static void exec_mprotect(struct code_test *test)
{
	int error;

	say("B: mmap read-write, then mprotect PROT_READ | PROT_EXEC\n");
	error = map_fixed(test->code, CODE_SIZE, PROT_READ | PROT_WRITE);
	if (error)
	{
		say("  refused (%s)\n", map_error(error));
		return;
	}
	memcpy((void *)test->code, guest_code_start, code_bytes());
	if (mprotect((void *)test->code, CODE_SIZE, PROT_READ | PROT_EXEC) != 0)
		say("  mprotect: refused (%s)\n", strerror(errno));
	else
		run_code_tests(test);
	munmap((void *)test->code, CODE_SIZE);
}

static void exec_shared_alias(struct code_test *test)
{
	int descriptor;
	void *writable;
	void *executable;

	say("C: a shared memory object, mapped writable anywhere and "
		"PROT_READ | PROT_EXEC below 4 GB\n");
	descriptor = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
	if (descriptor < 0)
	{
		say("  shm_open: failed (%s)\n", strerror(errno));
		return;
	}
	if (ftruncate(descriptor, CODE_SIZE) != 0)
	{
		say("  ftruncate: failed (%s)\n", strerror(errno));
		close(descriptor);
		return;
	}
	writable = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (writable == MAP_FAILED)
	{
		say("  writable view: failed (%s)\n", strerror(errno));
		close(descriptor);
		return;
	}
	say("  writable view at %p\n", writable);
	if (range_free(test->code, CODE_SIZE) != 0)
	{
		say("  0x%lx is not free; skipped\n", (unsigned long)test->code);
		munmap(writable, CODE_SIZE);
		close(descriptor);
		return;
	}
	executable = mmap((void *)test->code, CODE_SIZE, PROT_READ | PROT_EXEC,
		MAP_SHARED | MAP_FIXED | MAP_EXCL, descriptor, 0);
	if (executable == MAP_FAILED)
		say("  executable view: refused (%s)\n", strerror(errno));
	else if ((uintptr_t)executable != test->code)
	{
		say("  executable view: placed at %p, not 0x%lx\n", executable,
			(unsigned long)test->code);
		munmap(executable, CODE_SIZE);
	}
	else
	{
		/* written through the other view after mapping, so a run
		shows the two are one memory */
		memcpy(writable, guest_code_start, code_bytes());
		run_code_tests(test);
		munmap(executable, CODE_SIZE);
	}
	munmap(writable, CODE_SIZE);
	close(descriptor);
}

static void exec_kernel_mprotect(struct code_test *test)
{
	int error;
	int result;

	say("D: mmap read-write, then the SDK's kernel_mprotect to read-execute\n");
	error = map_fixed(test->code, CODE_SIZE, PROT_READ | PROT_WRITE);
	if (error)
	{
		say("  refused (%s)\n", map_error(error));
		return;
	}
	memcpy((void *)test->code, guest_code_start, code_bytes());
	result = kernel_mprotect(getpid(), (intptr_t)test->code, CODE_SIZE,
		PROT_READ | PROT_EXEC);
	if (result != 0)
		say("  kernel_mprotect: returned %d\n", result);
	else
	{
		say("  kernel_get_vmem_protection now: 0x%x\n",
			kernel_get_vmem_protection(getpid(), (intptr_t)test->code, CODE_SIZE));
		run_code_tests(test);
	}
	munmap((void *)test->code, CODE_SIZE);
}

/* The base for the execution tests: the image's own address if it maps,
or else the first of a few others below 4 GB that does. */
static uint64_t choose_test_base(uint64_t lowest)
{
	uint64_t candidates[5];
	size_t count = 0;
	size_t index;

	candidates[count++] = GUEST_IMAGE_BASE;
	candidates[count++] = 0x10000000ULL;
	candidates[count++] = 0x20000000ULL;
	candidates[count++] = 0x60000000ULL;
	if (lowest)
		candidates[count++] = (lowest + MEGABYTE - 1) & ~(MEGABYTE - 1);
	for (index = 0; index < count; index++)
	{
		uint64_t size = THREAD_STACK_OFFSET + THREAD_STACK_SIZE;

		if (map_fixed(candidates[index], size, PROT_READ | PROT_WRITE) == 0)
		{
			munmap((void *)(uintptr_t)candidates[index], size);
			return candidates[index];
		}
	}
	return 0;
}

static void probe_execution(uint64_t base)
{
	struct code_test test;
	int error;

	heading("4: running code below 4 GB");
	if (!base)
	{
		say("no base below 4 GB could hold the tests' 2.25 MB; skipped\n");
		return;
	}
	say("test base: 0x%lx (code there, data at +1 MB, stack above the data)\n",
		(unsigned long)base);
	say("test code: %zu bytes from %p\n", code_bytes(), (void *)guest_code_start);
	test.code = base;
	test.data = base + DATA_OFFSET;
	test.stack_top = test.data + DATA_SIZE;
	error = map_fixed(test.data, DATA_SIZE, PROT_READ | PROT_WRITE);
	if (error)
	{
		say("data page: refused (%s); skipped\n", map_error(error));
		return;
	}
	*(volatile uint32_t *)test.data = DATA_MAGIC;
	exec_mmap_rwx(&test);
	exec_mprotect(&test);
	exec_shared_alias(&test);
	exec_kernel_mprotect(&test);
	munmap((void *)test.data, DATA_SIZE);
}

/* ---------- 5: a thread on a stack below 4 GB */

static void *thread_main(void *argument)
{
	volatile int local = 0;

	*(uintptr_t *)argument = (uintptr_t)&local;
	return NULL;
}

static void probe_thread_stack(uint64_t base)
{
	uint64_t stack;
	pthread_attr_t attributes;
	pthread_t thread;
	uintptr_t local_address = 0;
	int error;

	heading("5: a thread on a stack below 4 GB");
	if (!base)
	{
		say("no base; skipped\n");
		return;
	}
	stack = base + THREAD_STACK_OFFSET;
	error = map_fixed(stack, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE);
	if (error)
	{
		say("stack at 0x%lx: refused (%s)\n", (unsigned long)stack, map_error(error));
		return;
	}
	pthread_attr_init(&attributes);
	error = pthread_attr_setstack(&attributes, (void *)(uintptr_t)stack, THREAD_STACK_SIZE);
	if (error)
		say("pthread_attr_setstack: failed (%s)\n", strerror(error));
	else
	{
		say("starting a thread on 0x%lx-0x%lx (if nothing follows, it crashed)\n",
			(unsigned long)stack, (unsigned long)(stack + THREAD_STACK_SIZE));
		error = pthread_create(&thread, &attributes, thread_main, &local_address);
		if (error)
			say("pthread_create: failed (%s)\n", strerror(error));
		else
		{
			pthread_join(thread, NULL);
			say("the thread ran; its local was at 0x%lx (%s)\n",
				(unsigned long)local_address,
				local_address >= stack && local_address < stack + THREAD_STACK_SIZE
					? "on the stack given" : "NOT on the stack given");
		}
	}
	pthread_attr_destroy(&attributes);
	munmap((void *)(uintptr_t)stack, THREAD_STACK_SIZE);
}

int main(void)
{
	uint64_t lowest = 0;
	uint64_t base;
	int guard;

	report_file = fopen(REPORT_PATH, "w");
	say("Halo PS5 memory probe\n");
	if (!report_file)
		say("could not open %s (%s); the report is in the kernel log only\n",
			REPORT_PATH, strerror(errno));
	install_fault_handler();
	probe_self();
	probe_vm_map();
	heading("2: the guard against mapping over the process");
	guard = map_guard_works();
	probe_map_32bit();
	if (guard)
	{
		lowest = walk_low_memory();
		probe_guest_ranges();
		base = choose_test_base(lowest);
		probe_execution(base);
		probe_thread_stack(base);
	}
	heading("done");
	if (report_file)
		fclose(report_file);
	notify("Halo memory probe finished: " REPORT_PATH);
	return 0;
}
