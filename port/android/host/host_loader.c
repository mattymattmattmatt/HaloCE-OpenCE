/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code, see tools/android_build.py) whose segments are copied to
the addresses it was linked at, below 4 GB. The 32-bit app's host
(armeabi-v7a) loads the same code built as a 32-bit ARM executable. The image starts with a
struct halo_guest_header naming its import table, which is filled with the
host functions of the same names.
*/

#include "host.h"

#include <elf.h>
#include <string.h>
#include <sys/mman.h>

#ifdef __arm__
#define GUEST_ELF_HEADER Elf32_Ehdr
#define GUEST_ELF_SEGMENT Elf32_Phdr
#define GUEST_ELF_CLASS ELFCLASS32
#define GUEST_ELF_MACHINE EM_ARM
#define GUEST_ELF_DESCRIPTION "a 32-bit ARM executable"
#else
#define GUEST_ELF_HEADER Elf64_Ehdr
#define GUEST_ELF_SEGMENT Elf64_Phdr
#define GUEST_ELF_CLASS ELFCLASS64
#define GUEST_ELF_MACHINE EM_AARCH64
#define GUEST_ELF_DESCRIPTION "an AArch64 executable"
#endif

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

int host_load_image(const void *file, size_t size)
{
	const GUEST_ELF_HEADER *elf = file;
	const GUEST_ELF_SEGMENT *segments;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, ELFMAG, SELFMAG) || elf->e_ident[EI_CLASS] != GUEST_ELF_CLASS ||
		elf->e_machine != GUEST_ELF_MACHINE || elf->e_type != ET_EXEC)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not " GUEST_ELF_DESCRIPTION);
		return -1;
	}
	segments = (const GUEST_ELF_SEGMENT *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		if (segments[index].p_type != PT_LOAD)
			continue;
		if (segments[index].p_vaddr < low)
			low = segments[index].p_vaddr;
		if (segments[index].p_vaddr + segments[index].p_memsz > high)
			high = segments[index].p_vaddr + segments[index].p_memsz;
	}
	low &= ~0xfffULL;
	high = (high + 0xfff) & ~0xfffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;
	if (mmap((void *)(uintptr_t)low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
		(void *)(uintptr_t)low)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const GUEST_ELF_SEGMENT *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size)
			return -1;
		memcpy((void *)(uintptr_t)segment->p_vaddr, (const char *)file + segment->p_offset, segment->p_filesz);
	}

	header = (const struct halo_guest_header *)(uintptr_t)low;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	table = (uint64_t *)(uintptr_t)header->import_table;
	name = (const char *)(uintptr_t)header->import_names;
	count = *(const uint32_t *)(uintptr_t)header->import_count;
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < elf->e_phnum; index++)
	{
		const GUEST_ELF_SEGMENT *segment = &segments[index];
		uint64_t start = segment->p_vaddr & ~0xfffULL;
		uint64_t end = (segment->p_vaddr + segment->p_memsz + 0xfff) & ~0xfffULL;

		if (segment->p_type == PT_LOAD && (segment->p_flags & PF_X))
			mprotect((void *)(uintptr_t)start, end - start, PROT_READ | PROT_EXEC);
	}
	__builtin___clear_cache((char *)(uintptr_t)low, (char *)(uintptr_t)high);
	return 0;
}
