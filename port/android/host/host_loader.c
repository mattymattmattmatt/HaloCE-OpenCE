/*
HOST_LOADER.C

Loads the guest image: a statically linked ELF executable of ILP32 code
(see tools/android_build.py) whose segments are copied to the addresses it
was linked at, below 4 GB. On AArch64 hosts it is a 64-bit AArch64 ELF file
(arm64_32 code); on x86-64 hosts a 32-bit x86-64 one (x32). The image starts
with a struct halo_guest_header naming its import table, which is filled
with the host functions of the same names.

The file is checked before anything is mapped (host_image_validate), and
its header, import table and names before they are used
(host_image_validate_imports): every offset, size and address must lie in
the file and in the range the host reserves for the image.

Where the Java runtime already holds those addresses, the AArch64 image is
copied somewhere free instead, by a whole number of 64 KB. Its code needs
nothing for that: branches and the ADRP pairs that form addresses are
relative to the instruction. Its 32-bit pointers (data, the header) are
listed in halo_guest.relocs (tools/guest_relocations.py), and each has the
distance added before anything reads them. (An x32 image is static code at
a fixed address, and is never moved.)
*/

#include "host.h"

#include <elf.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* more than the guest has (its own, the POSIX and the GL imports) */
#define IMPORTS_MAXIMUM 16384

struct host_guest_image host_image;

static int fail(const char *format, ...) __attribute__((format(printf, 1, 2)));

static int fail(const char *format, ...)
{
	char text[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_ERROR, "the guest image %s", text);
	return -1;
}

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

/* guest globals host-side diagnostics want to read; the addresses come from
the ELF rather than being written down here, because a rebuild moves them */
static const struct
{
	const char *name;
	size_t offset;
} host_image_symbols[] = {
	{ "cache_file_globals", offsetof(struct host_guest_image, cache_file_globals) },
	{ "global_tag_instances", offsetof(struct host_guest_image, global_tag_instances) },
};

/* one symbol of either class's table */
static void read_symbol(const unsigned char *entry, int elf_class, uint32_t *name, uint64_t *value,
	uint16_t *section, unsigned char *bind)
{
	if (elf_class == ELFCLASS64)
	{
		Elf64_Sym symbol;

		memcpy(&symbol, entry, sizeof(symbol));
		*name = symbol.st_name;
		*value = symbol.st_value;
		*section = symbol.st_shndx;
		*bind = ELF64_ST_BIND(symbol.st_info);
	}
	else
	{
		Elf32_Sym symbol;

		memcpy(&symbol, entry, sizeof(symbol));
		*name = symbol.st_name;
		*value = symbol.st_value;
		*section = symbol.st_shndx;
		*bind = ELF32_ST_BIND(symbol.st_info);
	}
}

/* finds a named symbol's address in the ELF file (of the host's class); 0 if
it has none. A name can appear more than once (a tentative definition and
the real one), so a definition in a section is preferred. */
static uint32_t host_image_find_symbol(const void *file, size_t size, const char *wanted)
{
	const unsigned char *bytes = file;
	int elf_class = HOST_IMAGE_CLASS;
	uint64_t section_offset;
	uint32_t section_count, section_size, symbol_size, index;
	size_t wanted_size = strlen(wanted) + 1;

	if (size < EI_NIDENT || bytes[EI_CLASS] != elf_class)
		return 0;
	if (elf_class == ELFCLASS64)
	{
		Elf64_Ehdr elf;

		if (size < sizeof(elf))
			return 0;
		memcpy(&elf, file, sizeof(elf));
		section_offset = elf.e_shoff;
		section_count = elf.e_shnum;
		section_size = sizeof(Elf64_Shdr);
		symbol_size = sizeof(Elf64_Sym);
	}
	else
	{
		Elf32_Ehdr elf;

		if (size < sizeof(elf))
			return 0;
		memcpy(&elf, file, sizeof(elf));
		section_offset = elf.e_shoff;
		section_count = elf.e_shnum;
		section_size = sizeof(Elf32_Shdr);
		symbol_size = sizeof(Elf32_Sym);
	}
	if (!section_offset || !section_count || section_offset > size ||
		(uint64_t)section_count * section_size > size - section_offset)
	{
		return 0;
	}
	for (index = 0; index < section_count; index++)
	{
		uint64_t offset, table_size, strings_offset, strings_size, symbol_index;
		uint32_t type, link;

		/* the table and its strings (sh_type, sh_link, sh_offset, sh_size) */
		if (elf_class == ELFCLASS64)
		{
			Elf64_Shdr table, strings;

			memcpy(&table, bytes + section_offset + (uint64_t)index * section_size, sizeof(table));
			type = table.sh_type;
			link = table.sh_link;
			offset = table.sh_offset;
			table_size = table.sh_size;
			if (type != SHT_SYMTAB || link >= section_count)
				continue;
			memcpy(&strings, bytes + section_offset + (uint64_t)link * section_size, sizeof(strings));
			strings_offset = strings.sh_offset;
			strings_size = strings.sh_size;
		}
		else
		{
			Elf32_Shdr table, strings;

			memcpy(&table, bytes + section_offset + (uint64_t)index * section_size, sizeof(table));
			type = table.sh_type;
			link = table.sh_link;
			offset = table.sh_offset;
			table_size = table.sh_size;
			if (type != SHT_SYMTAB || link >= section_count)
				continue;
			memcpy(&strings, bytes + section_offset + (uint64_t)link * section_size, sizeof(strings));
			strings_offset = strings.sh_offset;
			strings_size = strings.sh_size;
		}
		if (offset > size || table_size > size - offset || strings_offset > size || strings_size > size - strings_offset)
			continue;
		for (symbol_index = 0; symbol_index < table_size / symbol_size; symbol_index++)
		{
			uint32_t name;
			uint64_t value;
			uint16_t section;
			unsigned char bind;

			read_symbol(bytes + offset + symbol_index * symbol_size, elf_class, &name, &value, &section, &bind);
			if (!name || name >= strings_size || wanted_size > strings_size - name ||
				memcmp(bytes + strings_offset + name, wanted, wanted_size) || value >= 0x100000000ULL)
			{
				continue;
			}
			/* the game compiles with -fcommon, so a tentative definition and
			the definition that won can both be in the table; take the one that
			is actually in a section */
			if (section == SHN_UNDEF)
				continue;
			if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_LOCAL)
				continue;
			return (uint32_t)value;
		}
	}
	return 0;
}

#define RELOCATIONS_MAGIC 0x434c5248u /* 'HRLC' */

/* adds shift to each of the image's 32-bit pointers, at base; -1 if the
table is not this image's */
static int relocate(uint64_t base, uint64_t size, uint32_t shift, const void *relocations, size_t relocations_size)
{
	const uint32_t *table = relocations;
	uint32_t count, index;

	if (!table || relocations_size < 3 * sizeof(uint32_t) || table[0] != RELOCATIONS_MAGIC ||
		table[1] != HALO_GUEST_IMAGE_BASE)
	{
		return fail("relocation table is missing or not for this image");
	}
	count = table[2];
	if (relocations_size < (3 + (uint64_t)count) * sizeof(uint32_t))
		return fail("relocation table is cut short");
	for (index = 0; index < count; index++)
	{
		uint32_t offset = table[3 + index];

		if ((uint64_t)offset + sizeof(uint32_t) > size)
			return fail("relocation table names %08x, outside the image", offset);
		*(uint32_t *)(uintptr_t)(base + offset) += shift;
	}
	host_logf(HOST_LOG_INFO, "guest image moved by %08x: %u pointers", shift, count);
	return 0;
}

/* ---------- the ELF file */

/* the fields of the file header and of a program header the loader uses,
in either class */
struct file_header
{
	uint16_t type, machine;
	uint32_t version;
	uint64_t program_offset;
	uint16_t program_entry_size, program_count;
};

struct program_header
{
	uint32_t type, flags;
	uint64_t offset, address, file_size, memory_size;
};

static int read_file_header(const unsigned char *file, size_t size, int elf_class, struct file_header *header)
{
	if (elf_class == ELFCLASS64)
	{
		Elf64_Ehdr elf;

		if (size < sizeof(elf))
			return fail("is too short for an ELF header (%zu bytes)", size);
		memcpy(&elf, file, sizeof(elf));
		header->type = elf.e_type;
		header->machine = elf.e_machine;
		header->version = elf.e_version;
		header->program_offset = elf.e_phoff;
		header->program_entry_size = elf.e_phentsize;
		header->program_count = elf.e_phnum;
		return elf.e_phentsize == sizeof(Elf64_Phdr) ? 0 : fail("has program headers of %u bytes", elf.e_phentsize);
	}
	else
	{
		Elf32_Ehdr elf;

		if (size < sizeof(elf))
			return fail("is too short for an ELF header (%zu bytes)", size);
		memcpy(&elf, file, sizeof(elf));
		header->type = elf.e_type;
		header->machine = elf.e_machine;
		header->version = elf.e_version;
		header->program_offset = elf.e_phoff;
		header->program_entry_size = elf.e_phentsize;
		header->program_count = elf.e_phnum;
		return elf.e_phentsize == sizeof(Elf32_Phdr) ? 0 : fail("has program headers of %u bytes", elf.e_phentsize);
	}
}

static void read_program_header(const unsigned char *entry, int elf_class, struct program_header *program)
{
	if (elf_class == ELFCLASS64)
	{
		Elf64_Phdr segment;

		memcpy(&segment, entry, sizeof(segment));
		program->type = segment.p_type;
		program->flags = segment.p_flags;
		program->offset = segment.p_offset;
		program->address = segment.p_vaddr;
		program->file_size = segment.p_filesz;
		program->memory_size = segment.p_memsz;
	}
	else
	{
		Elf32_Phdr segment;

		memcpy(&segment, entry, sizeof(segment));
		program->type = segment.p_type;
		program->flags = segment.p_flags;
		program->offset = segment.p_offset;
		program->address = segment.p_vaddr;
		program->file_size = segment.p_filesz;
		program->memory_size = segment.p_memsz;
	}
}

int host_image_validate(const void *file, size_t size, int elf_class, int machine, struct host_image_layout *layout)
{
	const unsigned char *bytes = file;
	const uint64_t reserved_end = (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE;
	struct file_header header;
	uint64_t low = ~0ULL, high = 0;
	uint32_t index;

	memset(layout, 0, sizeof(*layout));
	if (!file || size < EI_NIDENT || memcmp(bytes, ELFMAG, SELFMAG))
		return fail("is not an ELF file");
	if (bytes[EI_CLASS] != elf_class)
		return fail("is a %s ELF file; this host needs a %s one", bytes[EI_CLASS] == ELFCLASS64 ? "64-bit" : "32-bit",
			elf_class == ELFCLASS64 ? "64-bit" : "32-bit");
	if (bytes[EI_DATA] != ELFDATA2LSB)
		return fail("is not little-endian");
	if (read_file_header(bytes, size, elf_class, &header) != 0)
		return -1;
	if (header.machine != machine)
		return fail("is for machine %u; this host needs machine %d", header.machine, machine);
	if (header.type != ET_EXEC)
		return fail("is not an executable (type %u)", header.type);
	if (header.version != EV_CURRENT)
		return fail("has ELF version %u", header.version);
	if (!header.program_count || header.program_count > 64)
		return fail("has %u program headers", header.program_count);
	if (header.program_offset > size ||
		(uint64_t)header.program_count * header.program_entry_size > size - header.program_offset)
	{
		return fail("has its program headers outside the file");
	}

	for (index = 0; index < header.program_count; index++)
	{
		struct program_header program;
		struct host_image_segment *segment;

		read_program_header(bytes + header.program_offset + (uint64_t)index * header.program_entry_size, elf_class,
			&program);
		if (program.type == PT_DYNAMIC || program.type == PT_INTERP)
			return fail("is not static: it has a %s segment", program.type == PT_DYNAMIC ? "PT_DYNAMIC" : "PT_INTERP");
		if (program.type != PT_LOAD)
			continue;
		if (program.file_size > program.memory_size)
			return fail("segment %u is larger in the file than in memory", index);
		if (program.offset > size || program.file_size > size - program.offset)
			return fail("segment %u lies outside the file", index);
		if (program.address < HALO_GUEST_IMAGE_BASE || program.address > reserved_end ||
			program.memory_size > reserved_end - program.address)
		{
			return fail("segment %u (%llx, %llx bytes) is outside its range %08x-%08llx", index,
				(unsigned long long)program.address, (unsigned long long)program.memory_size,
				HALO_GUEST_IMAGE_BASE, (unsigned long long)reserved_end);
		}
		if (layout->segment_count == HOST_IMAGE_SEGMENTS_MAXIMUM)
			return fail("has more than %d loadable segments", HOST_IMAGE_SEGMENTS_MAXIMUM);
		segment = &layout->segments[layout->segment_count++];
		segment->offset = program.offset;
		segment->address = program.address;
		segment->file_size = program.file_size;
		segment->memory_size = program.memory_size;
		segment->flags = program.flags;
		if (program.address < low)
			low = program.address;
		if (program.address + program.memory_size > high)
			high = program.address + program.memory_size;
	}
	if (!layout->segment_count)
		return fail("has no loadable segment");
	low &= ~0xfffULL;
	high = (high + 0xfff) & ~0xfffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high - low < sizeof(struct halo_guest_header))
		return fail("spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
	layout->low = low;
	layout->high = high;
	return 0;
}

/* ---------- the header and the import table */

static int inside(uint32_t base, uint32_t end, uint32_t address, uint64_t size)
{
	return address >= base && address <= end && size <= (uint64_t)(end - address);
}

static int executable(const struct host_image_layout *layout, uint32_t address)
{
	uint32_t index;

	for (index = 0; index < layout->segment_count; index++)
	{
		const struct host_image_segment *segment = &layout->segments[index];

		if ((segment->flags & PF_X) && address >= segment->address &&
			address - segment->address < segment->memory_size)
		{
			return 1;
		}
	}
	return 0;
}

int host_image_validate_imports(const uint8_t *memory, uint32_t base, uint32_t end,
	const struct host_image_layout *layout, uint32_t *count)
{
	struct halo_guest_header header;
	uint32_t names, index;

	if (end < base || end - base < sizeof(header))
		return fail("is too small for its header");
	memcpy(&header, memory, sizeof(header));
	if (header.magic != HALO_GUEST_MAGIC || header.abi_version != HALO_GUEST_ABI_VERSION)
		return fail("header does not match this host (ABI version %u, the host's %d)", header.abi_version,
			HALO_GUEST_ABI_VERSION);
	if (!executable(layout, header.start) || !executable(layout, header.thread_start) ||
		!executable(layout, header.thread_attach) || !executable(layout, header.thread_detach))
	{
		return fail("header names an entry point outside its code");
	}
	if (header.init_array_start != header.init_array_end && (header.init_array_start > header.init_array_end ||
		(header.init_array_start & 3) ||
		!inside(base, end, header.init_array_start, header.init_array_end - header.init_array_start)))
	{
		return fail("header's initializers (%08x-%08x) are outside the image", header.init_array_start,
			header.init_array_end);
	}
	if ((header.import_count & 3) || !inside(base, end, header.import_count, sizeof(uint32_t)))
		return fail("header's import count (at %08x) is outside the image", header.import_count);
	memcpy(count, memory + (header.import_count - base), sizeof(*count));
	if (*count > IMPORTS_MAXIMUM)
		return fail("has %u imports (at most %d)", *count, IMPORTS_MAXIMUM);
	if ((header.import_table & 7) || !inside(base, end, header.import_table, (uint64_t)*count * 8))
		return fail("import table (%u entries at %08x) is outside the image", *count, header.import_table);
	names = header.import_names;
	for (index = 0; index < *count; index++)
	{
		const uint8_t *terminator;

		if (!inside(base, end, names, 1))
			return fail("import name %u is outside the image", index);
		terminator = memchr(memory + (names - base), 0, end - names);
		if (!terminator)
			return fail("import name %u is not terminated", index);
		if (terminator == memory + (names - base))
			return fail("import name %u is empty", index);
		names += (uint32_t)(terminator - (memory + (names - base))) + 1;
	}
	return 0;
}

/* ---------- loading */

uint32_t host_image_span(const void *file, size_t size)
{
	struct host_image_layout layout;

	if (host_image_validate(file, size, HOST_IMAGE_CLASS, HOST_IMAGE_MACHINE, &layout) != 0)
		return 0;
	return (uint32_t)(layout.high - layout.low);
}

int host_load_image(const void *file, size_t size, const void *relocations, size_t relocations_size)
{
	struct host_image_layout layout;
	uint32_t base;

	if (host_image_validate(file, size, HOST_IMAGE_CLASS, HOST_IMAGE_MACHINE, &layout) != 0)
		return -1;
	if (host_memory_initialize((uint32_t)layout.low, (uint32_t)(layout.high - layout.low), &base) != 0)
		return -1;
	return host_load_image_reserved(file, size, relocations, relocations_size, base);
}

int host_load_image_reserved(const void *file, size_t size, const void *relocations, size_t relocations_size,
	uint32_t base)
{
	struct host_image_layout layout;
	const struct halo_guest_header *header;
	uint64_t low, high;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	uint32_t shift;
	int missing = 0;

	if (host_image_validate(file, size, HOST_IMAGE_CLASS, HOST_IMAGE_MACHINE, &layout) != 0)
		return -1;
	/* (unsigned arithmetic: a move down wraps, and adding it back to a
	32-bit pointer wraps the same way) */
	shift = base - (uint32_t)layout.low;
#if defined(__x86_64__)
	if (shift)
		return fail("cannot be moved (to %08x): x32 code is linked to its address", base);
#endif
	/* from here on the layout is where the image is, not where it was linked */
	for (index = 0; index < layout.segment_count; index++)
		layout.segments[index].address = (uint32_t)(layout.segments[index].address + shift);
	layout.low = base;
	layout.high = base + (layout.high - HALO_GUEST_IMAGE_BASE);
	low = layout.low;
	high = layout.high;
	if (mmap((void *)(uintptr_t)low, high - low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		-1, 0) != (void *)(uintptr_t)low)
	{
		return fail("range %llx-%llx cannot be mapped", (unsigned long long)low, (unsigned long long)high);
	}
	for (index = 0; index < layout.segment_count; index++)
	{
		const struct host_image_segment *segment = &layout.segments[index];

		memcpy((void *)(uintptr_t)segment->address, (const char *)file + segment->offset, segment->file_size);
	}
	if (shift && relocate(low, high - low, shift, relocations, relocations_size) != 0)
		return -1;

	if (host_image_validate_imports((const uint8_t *)(uintptr_t)low, (uint32_t)low, (uint32_t)high, &layout,
		&count) != 0)
	{
		return -1;
	}
	header = (const struct halo_guest_header *)(uintptr_t)low;
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	host_image.shift = shift;
	for (index = 0; index < sizeof(host_image_symbols) / sizeof(*host_image_symbols); index++)
	{
		uint32_t address = host_image_find_symbol(file, size, host_image_symbols[index].name);

		*(uint32_t *)((char *)&host_image + host_image_symbols[index].offset) = address ? address + shift : 0;
	}

	table = (uint64_t *)(uintptr_t)header->import_table;
	name = (const char *)(uintptr_t)header->import_names;
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
	for (index = 0; index < sizeof(host_image_symbols) / sizeof(*host_image_symbols); index++)
		host_logf(HOST_LOG_INFO, "  guest %s at %08x", host_image_symbols[index].name,
			*(const uint32_t *)((const char *)&host_image + host_image_symbols[index].offset));

	/* code becomes read-only and executable */
	for (index = 0; index < layout.segment_count; index++)
	{
		const struct host_image_segment *segment = &layout.segments[index];
		uint64_t start = segment->address & ~0xfffULL;
		uint64_t end = (segment->address + segment->memory_size + 0xfff) & ~0xfffULL;

		if (segment->flags & PF_X)
			mprotect((void *)(uintptr_t)start, end - start, PROT_READ | PROT_EXEC);
	}
	__builtin___clear_cache((char *)(uintptr_t)low, (char *)(uintptr_t)high);
	return 0;
}
