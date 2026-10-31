/*
HOST_ALLOC_TRACK_PS5.C

Where the host's heap goes, by caller: a diagnostic for the heap's growth in
a match with a second player (port/ps5/PS5.md, phase 6: 16 MB a minute with
one other player, 32 with split screen, none hosting alone).

With PS5_ALLOC_TRACK set to 1, this file answers the link's wrapped
allocator names itself (--wrap=malloc and its family, PS5_Vulkan's
radv-link.sh), so the platform layer's own wrappers (its heap_wrap.c) are
not linked; each call goes to the platform's heap (ps5platform/heap.h,
libc's allocator when that refuses), as theirs does. Each block is recorded
in a side table with its size and the return address of the call that made
it, and every minute (the heartbeat's memory line, host_sdl_ps5.c) the log
lists the callers whose live bytes grew the most:

  alloc: caller 0x0000000000a1b2c3: +3.2 MB in the minute, 41.5 MB live in 81234 blocks (802345 made)

The addresses are the host's: build/ps5/host/eboot.elf.map, or
llvm-symbolizer -e build/ps5/host/eboot.elf. A caller that is the C++
runtime's operator new stands for every C++ allocation.

With PS5_ALLOC_TRACK at 0 (as the title ships) nothing here is compiled
but an empty report, and the platform's wrappers serve the allocator.
*/

#define PS5_ALLOC_TRACK 0

#include "ps5_host.h"

#if !PS5_ALLOC_TRACK

void ps5_alloc_report(void)
{
}

void ps5_alloc_start(void)
{
}

#else

#include "host.h"

#include <ps5platform/heap.h>

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
__attribute__((weak)) size_t __real_malloc_usable_size(const void *pointer);

/* ---------- the tables: blocks by address (linear probing, deletion by
moving the run back), callers by return address */

#define BLOCK_BITS 21
#define BLOCKS (1u << BLOCK_BITS)
#define CALLERS 8192
#define REPORTED 12

struct block
{
	uintptr_t address; /* 0: empty */
	uint32_t size;
	uint32_t caller;
};

struct caller
{
	uintptr_t address; /* 0: empty */
	int64_t live_bytes, live_blocks;
	uint64_t made;
	int64_t reported_bytes;
};

static struct block *blocks;
static struct caller callers[CALLERS];
static uint32_t block_count;
static uint64_t untracked;
static int lock, started, failed;
/* set by host_main.c once main runs: what is allocated before (libc's and the
system's start-up) goes to the heap untracked */
static int armed;
/* HALO_PS5_ALLOC_DEEPER's callers (runtime addresses, comma-separated):
blocks made there are put down to the caller one frame further up, for an
allocator's own wrapper that keeps a frame pointer (SDL2's SDL_malloc_REAL),
whose callers are the ones wanted. Only those named: a frame without a
frame pointer cannot be walked safely. */
#define DEEPER 4
static uintptr_t deeper[DEEPER];
static int deeper_count;

static void take(void)
{
	while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
		sched_yield();
}

static void give(void)
{
	__atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

static uint32_t hash_of(uintptr_t value, uint32_t bits)
{
	return (uint32_t)(((uint64_t)value * 0x9e3779b97f4a7c15ull) >> (64 - bits));
}

/* the lock is held */
static uint32_t caller_index(uintptr_t address)
{
	uint32_t index = hash_of(address, 13) % CALLERS, probes;

	for (probes = 0; probes < CALLERS; probes++, index = (index + 1) % CALLERS)
	{
		if (callers[index].address == address)
			return index;
		if (!callers[index].address)
		{
			callers[index].address = address;
			return index;
		}
	}
	/* (full: the last slot stands for every other caller) */
	return CALLERS - 1;
}

/* the lock is held; the table is made at the first call */
static int ready(void)
{
	if (blocks)
		return 1;
	if (failed)
		return 0;
	blocks = ps5_heap_calloc(BLOCKS, sizeof(*blocks));
	failed = !blocks;
	return blocks != NULL;
}

static void record(void *pointer, size_t size, uintptr_t caller)
{
	uint32_t index;
	struct caller *who;

	if (!pointer || !__atomic_load_n(&armed, __ATOMIC_ACQUIRE))
		return;
	take();
	if (!ready() || block_count >= BLOCKS - BLOCKS / 8)
	{
		untracked++;
		give();
		return;
	}
	index = hash_of((uintptr_t)pointer, BLOCK_BITS);
	while (blocks[index].address && blocks[index].address != (uintptr_t)pointer)
		index = (index + 1) & (BLOCKS - 1);
	if (!blocks[index].address)
		block_count++;
	else
	{
		/* (a block the table missed being freed: take it off its caller) */
		who = &callers[blocks[index].caller];
		who->live_bytes -= blocks[index].size;
		who->live_blocks--;
	}
	blocks[index].address = (uintptr_t)pointer;
	blocks[index].size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
	blocks[index].caller = caller_index(caller);
	who = &callers[blocks[index].caller];
	who->live_bytes += blocks[index].size;
	who->live_blocks++;
	who->made++;
	started = 1;
	give();
}

static void forget(void *pointer)
{
	uint32_t index, next;

	if (!pointer || !__atomic_load_n(&started, __ATOMIC_ACQUIRE))
		return;
	take();
	index = hash_of((uintptr_t)pointer, BLOCK_BITS);
	while (blocks[index].address && blocks[index].address != (uintptr_t)pointer)
		index = (index + 1) & (BLOCKS - 1);
	if (blocks[index].address)
	{
		struct caller *who = &callers[blocks[index].caller];

		who->live_bytes -= blocks[index].size;
		who->live_blocks--;
		block_count--;
		blocks[index].address = 0;
		/* the run after it moves back over the hole, each entry that may */
		for (next = (index + 1) & (BLOCKS - 1); blocks[next].address; next = (next + 1) & (BLOCKS - 1))
		{
			uint32_t home = hash_of(blocks[next].address, BLOCK_BITS);

			if (((next - home) & (BLOCKS - 1)) >= ((next - index) & (BLOCKS - 1)))
			{
				blocks[index] = blocks[next];
				blocks[next].address = 0;
				index = next;
			}
		}
	}
	give();
}

static inline uintptr_t is_deeper(uintptr_t caller)
{
	int index;

	for (index = 0; index < deeper_count; index++)
	{
		if (deeper[index] == caller)
			return 1;
	}
	return 0;
}

/* (a frame further up is read only for the callers named, whose frames are
known to keep a frame pointer, as the wrappers here do) */
#pragma clang diagnostic ignored "-Wframe-address"

/* (a macro: the return addresses are the wrapper's own, which it is used in) */
#define CALLER (is_deeper((uintptr_t)__builtin_return_address(0)) ? (uintptr_t)__builtin_return_address(1) : \
	(uintptr_t)__builtin_return_address(0))

/* ---------- the allocator's names, each to the platform's heap first */

static void *heap_or_libc(size_t bytes)
{
	void *pointer = ps5_heap_malloc(bytes);

	return pointer ? pointer : __real_malloc(bytes);
}

static void release(void *pointer)
{
	if (ps5_heap_owns(pointer))
		ps5_heap_free(pointer);
	else
		__real_free(pointer);
}

static size_t usable(const void *pointer)
{
	if (ps5_heap_owns(pointer))
		return ps5_heap_usable_size(pointer);
	return __real_malloc_usable_size ? __real_malloc_usable_size(pointer) : 0;
}

/* a block moved to bytes, from wherever it is; NULL (the block kept) if
there is no room */
static void *resize(void *pointer, size_t bytes)
{
	void *moved;
	size_t kept;

	if (ps5_heap_owns(pointer))
	{
		moved = ps5_heap_realloc(pointer, bytes);
		if (moved)
			return moved;
		moved = __real_malloc(bytes);
	}
	else
	{
		moved = __real_realloc(pointer, bytes);
		if (moved || !__real_malloc_usable_size)
			return moved;
		moved = ps5_heap_malloc(bytes);
	}
	if (!moved)
		return NULL;
	kept = usable(pointer);
	memcpy(moved, pointer, kept < bytes ? kept : bytes);
	release(pointer);
	return moved;
}

void *__wrap_malloc(size_t bytes)
{
	void *pointer = heap_or_libc(bytes);

	record(pointer, bytes, CALLER);
	return pointer;
}

void *__wrap_calloc(size_t count, size_t bytes)
{
	void *pointer = ps5_heap_calloc(count, bytes);

	if (!pointer)
		pointer = __real_calloc(count, bytes);
	record(pointer, count * bytes, CALLER);
	return pointer;
}

void __wrap_free(void *pointer)
{
	if (!pointer)
		return;
	forget(pointer);
	release(pointer);
}

size_t __wrap_malloc_usable_size(const void *pointer)
{
	return pointer ? usable(pointer) : 0;
}

static void *reallocate(void *pointer, size_t bytes, uintptr_t caller)
{
	void *moved;

	if (!pointer)
	{
		moved = heap_or_libc(bytes);
		record(moved, bytes, caller);
		return moved;
	}
	/* (FreeBSD's realloc gives a request for nothing the smallest block) */
	if (!bytes)
		bytes = 1;
	/* (off the table before it moves: once it is freed another thread may be
	given its address) */
	forget(pointer);
	moved = resize(pointer, bytes);
	if (moved)
		record(moved, bytes, caller);
	else
		record(pointer, usable(pointer), caller);
	return moved;
}

void *__wrap_realloc(void *pointer, size_t bytes)
{
	return reallocate(pointer, bytes, CALLER);
}

void *__wrap_reallocf(void *pointer, size_t bytes)
{
	void *moved = reallocate(pointer, bytes, CALLER);

	if (!moved && pointer)
		__wrap_free(pointer);
	return moved;
}

void *__wrap_reallocarray(void *pointer, size_t count, size_t bytes)
{
	if (bytes && count > SIZE_MAX / bytes)
	{
		errno = ENOMEM;
		return NULL;
	}
	return reallocate(pointer, count * bytes, CALLER);
}

static int aligned(void **out, size_t alignment, size_t bytes, uintptr_t caller)
{
	void *pointer = ps5_heap_memalign(alignment, bytes);
	int result = 0;

	if (!pointer)
		result = __real_posix_memalign(&pointer, alignment, bytes);
	if (result)
		return result;
	record(pointer, bytes, caller);
	*out = pointer;
	return 0;
}

int __wrap_posix_memalign(void **out, size_t alignment, size_t bytes)
{
	if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *))
		return EINVAL;
	return aligned(out, alignment, bytes, CALLER);
}

void *__wrap_aligned_alloc(size_t alignment, size_t bytes)
{
	void *pointer = NULL;
	int result;

	if (!alignment || (alignment & (alignment - 1)))
	{
		errno = EINVAL;
		return NULL;
	}
	result = aligned(&pointer, alignment < sizeof(void *) ? sizeof(void *) : alignment, bytes, CALLER);
	if (result)
	{
		errno = result;
		return NULL;
	}
	return pointer;
}

void *__wrap_memalign(size_t alignment, size_t bytes)
{
	return __wrap_aligned_alloc(alignment, bytes);
}

/* libc's own would grow the caller's buffer with libc's realloc, which must
not be given the heap's blocks */
ssize_t __wrap_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream)
{
	size_t used = 0;
	int c;

	if (!line || !capacity || !stream)
	{
		errno = EINVAL;
		return -1;
	}
	if (!*line || !*capacity)
	{
		char *made = reallocate(*line, 128, CALLER);

		if (!made)
		{
			errno = ENOMEM;
			return -1;
		}
		*line = made;
		*capacity = 128;
	}
	while ((c = getc(stream)) != EOF)
	{
		if (used + 2 > *capacity)
		{
			char *grown = reallocate(*line, *capacity * 2, CALLER);

			if (!grown)
			{
				errno = ENOMEM;
				return -1;
			}
			*line = grown;
			*capacity *= 2;
		}
		(*line)[used++] = (char)c;
		if (c == delimiter)
			break;
	}
	(*line)[used] = 0;
	return used ? (ssize_t)used : -1;
}

ssize_t __wrap_getline(char **line, size_t *capacity, FILE *stream)
{
	return __wrap_getdelim(line, capacity, '\n', stream);
}

/* ---------- the report, every minute */

void ps5_alloc_report(void)
{
	struct caller top[REPORTED];
	int64_t total = 0, growth;
	uint64_t blocks_now, missed;
	struct ps5_heap_stats heap;
	int count = 0, index, slot;

	if (!started)
		return;
	memset(top, 0, sizeof(top));
	take();
	for (index = 0; index < CALLERS; index++)
	{
		struct caller *who = &callers[index];

		if (!who->address)
			continue;
		total += who->live_bytes;
		growth = who->live_bytes - who->reported_bytes;
		/* (the REPORTED largest growths, in order) */
		for (slot = count < REPORTED ? count : REPORTED - 1; slot >= 0; slot--)
		{
			if (slot < count && top[slot].live_bytes - top[slot].reported_bytes >= growth)
				break;
		}
		slot++;
		if (slot < REPORTED)
		{
			memmove(&top[slot + 1], &top[slot], sizeof(top[0]) * (size_t)(REPORTED - 1 - slot));
			top[slot] = *who;
			if (count < REPORTED)
				count++;
		}
		who->reported_bytes = who->live_bytes;
	}
	blocks_now = block_count;
	missed = untracked;
	give();
	memset(&heap, 0, sizeof(heap));
	ps5_heap_stats(&heap);
	ps5_log("alloc: %.1f MB live in %llu blocks (%llu not tracked; the table itself 32 MB); the heap %zu MB in %u "
		"segments, %u arenas, %llu sent to libc; ps5_alloc_report is at %p (to place the callers in eboot.elf)",
		total / 1048576.0, (unsigned long long)blocks_now, (unsigned long long)missed, heap.mapped_bytes >> 20,
		heap.segments, heap.arenas, heap.libc_fallbacks, (void *)ps5_alloc_report);
	for (index = 0; index < count; index++)
	{
		growth = top[index].live_bytes - top[index].reported_bytes;
		ps5_log("alloc: caller 0x%016llx: %+.2f MB in the minute, %.2f MB live in %lld blocks (%llu made)",
			(unsigned long long)top[index].address, growth / 1048576.0, top[index].live_bytes / 1048576.0,
			(long long)top[index].live_blocks, (unsigned long long)top[index].made);
	}
}

/* (nothing the tracking calls allocates through these names: the table comes
from ps5_heap_calloc, the log writes from the stack) */
void ps5_alloc_start(void)
{
	const char *text = getenv("HALO_PS5_ALLOC_DEEPER");

	while (text && *text && deeper_count < DEEPER)
	{
		char *end;
		unsigned long long address = strtoull(text, &end, 16);

		if (end == text)
			break;
		deeper[deeper_count++] = (uintptr_t)address;
		ps5_log("alloc: blocks made at 0x%llx are put down to their caller's caller", address);
		text = *end == ',' ? end + 1 : end;
	}
	__atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
}

#endif
