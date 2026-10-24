#!/usr/bin/env python3
"""The rebase table of the 32-bit ARM guest image (armeabi-v7a).

The 32-bit app shares its address space with the zygote's mappings, so the
range the image is linked at (HALO_GUEST_IMAGE_BASE) may be taken; the host
then loads it elsewhere (port/android/host/host_loader.c). The image is
linked with --emit-relocs, and this script turns the relocations that hold
an absolute address of the image into a table of the places to adjust:

- R_ARM_ABS32 and R_ARM_TARGET1: a word holding an address, and each entry
  of the GOT holding one;
- R_ARM_THM_MOVT_ABS (and R_ARM_MOVT_ABS in ARM code): the upper half of an
  address built by MOVW/MOVT. The host moves the image by a multiple of
  64 KB, so the lower halves (R_ARM_*MOVW_ABS_NC) never change.

Relocations relative to the place (branches, PC-relative loads) need
nothing, nor those of absolute symbols (constants, not addresses). Any other
type stops the build: the host could not move code that uses it. So does a
thunk the linker made with an absolute address in it, which no relocation
records: the image is linked with --pic-veneer so that its thunks are
relative.

The table (section .halo_rebase, not loaded) is: the link base, the counts
of words, Thumb MOVTs and ARM MOVTs, then the addresses of each kind.

usage: android_rebase.py linked.elf table.bin
"""

import struct
import sys

SHT_SYMTAB, SHT_REL, SHT_RELA = 2, 9, 4
SHF_ALLOC = 2
SHN_ABS = 0xFFF1
PT_LOAD = 1

WORD_TYPES = {2, 38}  # R_ARM_ABS32, R_ARM_TARGET1
THUMB_MOVT_TYPES = {48}  # R_ARM_THM_MOVT_ABS
ARM_MOVT_TYPES = {44}  # R_ARM_MOVT_ABS
LOW_HALF_TYPES = {43, 47}  # R_ARM_MOVW_ABS_NC, R_ARM_THM_MOVW_ABS_NC
RELATIVE_TYPES = {
    0,  # R_ARM_NONE
    3,  # R_ARM_REL32
    10, 11,  # R_ARM_THM_CALL, R_ARM_THM_PC8
    28, 29, 30,  # R_ARM_CALL, R_ARM_JUMP24, R_ARM_THM_JUMP24
    40, 42,  # R_ARM_V4BX, R_ARM_PREL31
    45, 46, 49, 50,  # R_ARM_MOVW_PREL_NC, R_ARM_MOVT_PREL, R_ARM_THM_MOVW_PREL_NC, R_ARM_THM_MOVT_PREL
    51, 52, 53, 54,  # R_ARM_THM_JUMP19, R_ARM_THM_JUMP6, R_ARM_THM_ALU_PREL_11_0, R_ARM_THM_PC12
    102, 103,  # R_ARM_THM_JUMP11, R_ARM_THM_JUMP8
    96,  # R_ARM_GOT_PREL: the GOT entry it reaches holds the address (below)
}


def main() -> int:
    source, table_path = sys.argv[1], sys.argv[2]
    data = open(source, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{source}: not a little-endian ELF32 file")
    (e_phoff, e_shoff, _flags, _ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<IIIHHHHHH", data, 28)
    sections = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + index * e_shentsize) for index in range(e_shnum)]
    names = sections[e_shstrndx]

    def name_of(section):
        start = names[4] + section[0]
        return data[start:data.index(b"\0", start)].decode()

    loads = [struct.unpack_from("<IIIIIIII", data, e_phoff + index * e_phentsize) for index in range(e_phnum)]
    base = min(segment[2] for segment in loads if segment[0] == PT_LOAD) & ~0xFFF
    end = max(segment[2] + segment[5] for segment in loads if segment[0] == PT_LOAD)

    # the linker's thunks (range extension, ARM/Thumb interworking) carry
    # their targets without a relocation: only the relative kind can move
    for section in sections:
        if section[1] != SHT_SYMTAB:
            continue
        strings = sections[section[6]]
        for offset in range(section[4], section[4] + section[5], 16):
            st_name = struct.unpack_from("<I", data, offset)[0]
            start = strings[4] + st_name
            name = data[start:data.index(b"\0", start)]
            if b"ABS" in name and b"Thunk" in name:
                sys.exit(f"{source}: the linker made an absolute thunk ({name.decode()}); link with --pic-veneer")

    words, thumb_movts, arm_movts = [], [], []
    for section in sections:
        if section[1] == SHT_RELA:
            sys.exit(f"{source}: RELA relocations in {name_of(section)}")
        if section[1] != SHT_REL:
            continue
        target = sections[section[7]]
        if not target[2] & SHF_ALLOC:
            continue
        symbols = sections[section[6]]
        for offset in range(section[4], section[4] + section[5], 8):
            r_offset, r_info = struct.unpack_from("<II", data, offset)
            kind, symbol_index = r_info & 0xFF, r_info >> 8
            if symbol_index:
                st_shndx = struct.unpack_from("<H", data, symbols[4] + symbol_index * 16 + 14)[0]
                if st_shndx == SHN_ABS:
                    continue
            if kind in WORD_TYPES:
                words.append(r_offset)
            elif kind in THUMB_MOVT_TYPES:
                thumb_movts.append(r_offset)
            elif kind in ARM_MOVT_TYPES:
                arm_movts.append(r_offset)
            elif kind in LOW_HALF_TYPES or kind in RELATIVE_TYPES:
                continue
            else:
                sys.exit(f"{source}: relocation type {kind} at {r_offset:#x} ({name_of(target)}): "
                         "the image could not be moved")
    for address in words + thumb_movts + arm_movts:
        if not base <= address < end:
            sys.exit(f"{source}: a relocated place {address:#x} is outside the image")
    # the GOT (clang's code for Linux is position-independent: it reaches
    # globals through it) holds addresses that no relocation is left for:
    # each entry within the image is one; zero (an undefined weak symbol)
    # and constants stay
    for section in sections:
        if name_of(section) == ".got.plt" and section[5]:
            sys.exit(f"{source}: the image has a .got.plt; it could not be moved")
        if name_of(section) == ".got":
            for place in range(section[3], section[3] + section[5], 4):
                value = struct.unpack_from("<I", data, section[4] + place - section[3])[0]
                if base <= value < end:
                    words.append(place)
    with open(table_path, "wb") as table:
        table.write(struct.pack("<IIII", base, len(words), len(thumb_movts), len(arm_movts)))
        for group in (words, thumb_movts, arm_movts):
            table.write(struct.pack(f"<{len(group)}I", *sorted(group)))
    print(f"{table_path}: {len(words)} words, {len(thumb_movts)} Thumb and {len(arm_movts)} ARM MOVTs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
