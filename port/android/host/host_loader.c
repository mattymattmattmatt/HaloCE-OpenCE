/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code, see tools/android_build.py) whose segments are copied to
the addresses it was linked at, below 4 GB. The 32-bit app's host
(armeabi-v7a) loads the same code built as a 32-bit ARM executable, and
moves it when the range it was linked at is taken. The image starts with a
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

#ifdef __arm__
/* the table of the places that hold the image's addresses
(tools/android_rebase.py): the section .halo_rebase */
static const uint32_t *rebase_table(const void *file, size_t size, uint32_t *entries)
{
	const Elf32_Ehdr *elf = file;
	const Elf32_Shdr *sections, *names;
	uint32_t index;

	*entries = 0;
	if (!elf->e_shoff || elf->e_shentsize != sizeof(Elf32_Shdr) || elf->e_shstrndx >= elf->e_shnum ||
		elf->e_shoff + (uint64_t)elf->e_shnum * sizeof(Elf32_Shdr) > size)
		return NULL;
	sections = (const Elf32_Shdr *)((const char *)file + elf->e_shoff);
	names = &sections[elf->e_shstrndx];
	for (index = 0; index < elf->e_shnum; index++)
	{
		const char *name = (const char *)file + names->sh_offset + sections[index].sh_name;

		if (names->sh_offset + sections[index].sh_name + sizeof(".halo_rebase") > size ||
			strcmp(name, ".halo_rebase"))
			continue;
		if (sections[index].sh_offset + (uint64_t)sections[index].sh_size > size || sections[index].sh_size < 16 ||
			sections[index].sh_size % 4)
			return NULL;
		*entries = sections[index].sh_size / 4;
		return (const uint32_t *)((const char *)file + sections[index].sh_offset);
	}
	return NULL;
}

static uint32_t read_word(const void *address)
{
	uint32_t value;

	memcpy(&value, address, 4);
	return value;
}

/* moves the image copied at base, linked at link_base, by base - link_base
(a multiple of 64 KB): the words holding its addresses, and the upper
halves of the MOVW/MOVT pairs building them */
static int rebase_image(const uint32_t *table, uint32_t entries, uint32_t link_base, uint32_t base, uint32_t end)
{
	uint32_t delta = base - link_base, words, thumb_movts, arm_movts, index;
	const uint32_t *place;

	if (!table || entries < 4)
		return -1;
	words = read_word(&table[1]);
	thumb_movts = read_word(&table[2]);
	arm_movts = read_word(&table[3]);
	if (read_word(&table[0]) != link_base || (delta & 0xffff) ||
		(uint64_t)words + thumb_movts + arm_movts + 4 > entries)
		return -1;
	place = table + 4;
	for (index = 0; index < words + thumb_movts + arm_movts; index++)
	{
		uint32_t address = read_word(&place[index]) - link_base + base;
		uint8_t *at = (uint8_t *)(uintptr_t)address;

		if (address < base || address + 4 > end)
			return -1;
		if (index < words)
		{
			uint32_t value = read_word(at) + delta;

			memcpy(at, &value, 4);
		}
		else if (index < words + thumb_movts)
		{
			/* MOVT (T1): 11110 i 101100 imm4 | 0 imm3 Rd imm8 */
			uint16_t first, second;
			uint32_t immediate;

			memcpy(&first, at, 2);
			memcpy(&second, at + 2, 2);
			immediate = ((first & 0xfu) << 12) | (((first >> 10) & 1u) << 11) | (((second >> 12) & 7u) << 8) |
				(second & 0xffu);
			immediate = (immediate + (delta >> 16)) & 0xffff;
			first = (uint16_t)((first & ~0x040fu) | ((immediate >> 12) & 0xf) | (((immediate >> 11) & 1) << 10));
			second = (uint16_t)((second & ~0x70ffu) | (((immediate >> 8) & 7) << 12) | (immediate & 0xff));
			memcpy(at, &first, 2);
			memcpy(at + 2, &second, 2);
		}
		else
		{
			/* MOVT (A1): cond 00110100 imm4 Rd imm12 */
			uint32_t instruction = read_word(at);
			uint32_t immediate = ((instruction >> 4) & 0xf000u) | (instruction & 0xfffu);

			immediate = (immediate + (delta >> 16)) & 0xffff;
			instruction = (instruction & ~0x000f0fffu) | ((immediate & 0xf000u) << 4) | (immediate & 0xfffu);
			memcpy(at, &instruction, 4);
		}
	}
	return 0;
}

/* the 32-bit app's (armeabi-v7a): the image goes at the address it was
linked at if that range is free, else wherever host_memory_place_image
finds room, and is moved there with its rebase table */
int host_load_image(const void *file, size_t size)
{
	const GUEST_ELF_HEADER *elf = file;
	const GUEST_ELF_SEGMENT *segments;
	uint64_t low = ~0ULL, high = 0;
	uint32_t base, end, table_entries;
	const uint32_t *table;
	const struct halo_guest_header *header;
	uint64_t *imports;
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
	table = rebase_table(file, size, &table_entries);
	base = host_memory_place_image((uint32_t)low, (uint32_t)(high - low));
	if (!base)
		return -1;
	if (base != low && !table)
	{
		host_logf(HOST_LOG_ERROR, "the guest image has no rebase table, and its range is taken");
		return -1;
	}
	end = base + (uint32_t)(high - low);
	if (mmap((void *)(uintptr_t)base, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
			0) != (void *)(uintptr_t)base)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const GUEST_ELF_SEGMENT *segment = &segments[index];

		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size)
			return -1;
		memcpy((void *)(uintptr_t)(segment->p_vaddr - low + base), (const char *)file + segment->p_offset,
			segment->p_filesz);
	}
	if (base != low && rebase_image(table, table_entries, (uint32_t)low, base, end) != 0)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's rebase table does not fit it");
		return -1;
	}

	header = (const struct halo_guest_header *)(uintptr_t)base;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = base;
	host_image.end = end;
	host_image.link_base = (uint32_t)low;

	imports = (uint64_t *)(uintptr_t)header->import_table;
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
		imports[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08x-%08x (linked at %08llx), %u imports (%d unavailable)", base, end,
		(unsigned long long)low, count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < elf->e_phnum; index++)
	{
		const GUEST_ELF_SEGMENT *segment = &segments[index];
		uint64_t start = (segment->p_vaddr - low + base) & ~0xfffULL;
		uint64_t stop = (segment->p_vaddr - low + base + segment->p_memsz + 0xfff) & ~0xfffULL;

		if (segment->p_type == PT_LOAD && (segment->p_flags & PF_X))
			mprotect((void *)(uintptr_t)start, stop - start, PROT_READ | PROT_EXEC);
	}
	__builtin___clear_cache((char *)(uintptr_t)base, (char *)(uintptr_t)end);
	return 0;
}
#else
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
#endif
