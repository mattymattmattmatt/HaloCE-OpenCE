/*
GUEST_ARM.LD

Layout of the 32-bit ARM app's guest image (armeabi-v7a,
tools/android_build.py), as guest.ld lays out the arm64_32 one: linked to
run at HALO_GUEST_IMAGE_BASE (port/android/include/halo_android_abi.h), with
the image header first. Its compiler is an ELF one, whose constructors are
in .init_array; they become the image's .guest_init.
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0x88000000;
	.guest_header : { KEEP(*(.guest_header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) *(.data.rel.ro .data.rel.ro.*) }
	.guest_init : {
		__guest_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x1000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.fini_array .fini_array.*) *(.ARM.exidx*) *(.ARM.extab*) *(.ARM.attributes) }
}
