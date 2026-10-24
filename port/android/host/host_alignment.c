/*
HOST_ALIGNMENT.C

Misaligned memory accesses of the 32-bit app's guest (armeabi-v7a only).

The game's data is not always 4-aligned (its scripts' stacks hold 16-bit
cells; Xbox structures in its files and saved games keep the x86 layout).
ARMv7 loads and stores single words and halfwords at any address, but
LDRD/STRD, LDM/STM and the VFP's VLDR/VSTR/VLDM/VSTM need a multiple of 4,
and a 64-bit kernel running 32-bit code delivers SIGBUS rather than fixing
them up (the 64-bit app never meets them: AArch64 has no such limit). The
guest is compiled not to pair loads and stores (tools/android_build.py), so
what remains is mostly floating-point loads and stores of a misaligned
float (and NEON loads and stores the compiler gave an alignment). The
SIGBUS handler (host_memory.c) passes such a fault here, which performs
the access a byte at a time on the signal's saved registers, as 32-bit ARM
Linux's alignment trap does, and resumes after the instruction.

Only Thumb-2 code of the guest image is emulated; anything else is
reported and ends the process as before.
*/

#include "host.h"

#ifdef __arm__

#include <signal.h>
#include <string.h>
#include <ucontext.h>

#define VFP_MAGIC 0x56465001u
#define PSR_T_BIT 0x00000020u
#define PSR_IT_MASK 0x0600fc00u

/* the kernel's VFP frame in uc_regspace */
struct vfp_frame
{
	uint32_t magic;
	uint32_t size;
	uint64_t registers[32];
	uint32_t fpscr;
};

static struct vfp_frame *vfp_frame_of(ucontext_t *ucontext)
{
	unsigned char *space = (unsigned char *)ucontext->uc_regspace;
	size_t offset = 0;

	/* (the VFP frame is the first one on ARMv7, but walk the list as the
	kernel lays it out: magic, size, contents) */
	while (offset + 8 <= sizeof(ucontext->uc_regspace))
	{
		uint32_t magic, size;

		memcpy(&magic, space + offset, 4);
		memcpy(&size, space + offset + 4, 4);
		if (magic == VFP_MAGIC)
			return (struct vfp_frame *)(space + offset);
		if (!magic || size < 8)
			break;
		offset += size;
	}
	return NULL;
}

static uint32_t *core_register(struct sigcontext *registers, unsigned index)
{
	/* arm_r0 .. arm_r10, arm_fp, arm_ip, arm_sp, arm_lr, arm_pc are
	consecutive in struct sigcontext */
	return (uint32_t *)&registers->arm_r0 + index;
}

static uint32_t read_core(struct sigcontext *registers, unsigned index)
{
	/* (as an address, the PC reads as the instruction's address + 4,
	word-aligned, in Thumb) */
	if (index == 15)
		return (registers->arm_pc + 4) & ~3u;
	return *core_register(registers, index);
}

static unsigned long it_advance(unsigned long cpsr)
{
	if ((cpsr & 0x06000400u) == 0)
	{
		cpsr &= ~PSR_IT_MASK;
	}
	else
	{
		const unsigned long mask = 0x06001c00u;
		unsigned long it = cpsr & mask;

		it <<= 1;
		it |= it >> (27 - 10);
		it &= mask;
		cpsr &= ~mask;
		cpsr |= it;
	}
	return cpsr;
}

static uint64_t fixups;

/* emulates the misaligned access of the Thumb-2 instruction at the signal's
PC; returns 1 if it was done (the registers then resume after it) */
int host_alignment_fixup(void *context)
{
	ucontext_t *ucontext = context;
	struct sigcontext *registers = &ucontext->uc_mcontext;
	uint32_t pc = registers->arm_pc;
	uint16_t first, second;
	uint32_t instruction;
	unsigned rn, size = 4;

	if (!(registers->arm_cpsr & PSR_T_BIT) || pc < host_image.base || pc + 4 > host_image.end)
		return 0;
	memcpy(&first, (const void *)(uintptr_t)pc, 2);
	if ((first >> 11) < 0x1d)
	{
		/* 16-bit LDMIA/STMIA Rn!, {list} */
		if ((first & 0xf000) == 0xc000)
		{
			int load = (first >> 11) & 1;
			unsigned list = first & 0xff, index;
			uint32_t address;

			rn = (first >> 8) & 7;
			address = read_core(registers, rn);
			for (index = 0; index < 8; index++)
			{
				if (!(list & (1u << index)))
					continue;
				if (load)
					memcpy(core_register(registers, index), (const void *)(uintptr_t)address, 4);
				else
					memcpy((void *)(uintptr_t)address, core_register(registers, index), 4);
				address += 4;
			}
			/* (writeback unless a load's list holds the base) */
			if (!load || !(list & (1u << rn)))
				*core_register(registers, rn) = address;
			size = 2;
			goto done;
		}
		return 0;
	}
	memcpy(&second, (const void *)(uintptr_t)(pc + 2), 2);
	instruction = ((uint32_t)first << 16) | second;
	rn = (instruction >> 16) & 15;

	/* LDRD/STRD (immediate): 1110 100P U1WL Rn Rt Rt2 imm8 */
	if ((instruction & 0xfe400000u) == 0xe8400000u && (instruction & 0x01200000u))
	{
		int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, writeback = (instruction >> 21) & 1;
		int load = (instruction >> 20) & 1;
		unsigned rt = (instruction >> 12) & 15, rt2 = (instruction >> 8) & 15;
		uint32_t offset = (instruction & 0xff) << 2;
		uint32_t base = read_core(registers, rn);
		uint32_t target = up ? base + offset : base - offset;
		uint32_t address = pre ? target : base;

		if (load)
		{
			uint32_t low, high;

			memcpy(&low, (const void *)(uintptr_t)address, 4);
			memcpy(&high, (const void *)(uintptr_t)(address + 4), 4);
			*core_register(registers, rt) = low;
			*core_register(registers, rt2) = high;
		}
		else
		{
			uint32_t low = read_core(registers, rt), high = read_core(registers, rt2);

			memcpy((void *)(uintptr_t)address, &low, 4);
			memcpy((void *)(uintptr_t)(address + 4), &high, 4);
		}
		if (writeback)
			*core_register(registers, rn) = target;
		goto done;
	}
	/* LDM/STM (IA, DB): 1110 100 op(2) 0 W L Rn (P M) 0 list */
	if ((instruction & 0xfe400000u) == 0xe8000000u && ((instruction >> 23) & 3) != 0 &&
		((instruction >> 23) & 3) != 3)
	{
		int increment = ((instruction >> 23) & 3) == 1, writeback = (instruction >> 21) & 1;
		int load = (instruction >> 20) & 1;
		unsigned list = instruction & 0xffff, count = (unsigned)__builtin_popcount(list), index;
		uint32_t base = read_core(registers, rn);
		uint32_t address = increment ? base : base - 4 * count;
		uint32_t pc_value = 0;
		int loads_pc = 0;

		if (rn == 15 || (list & (1u << 13)))
			return 0;
		for (index = 0; index < 16; index++)
		{
			if (!(list & (1u << index)))
				continue;
			if (load)
			{
				uint32_t value;

				memcpy(&value, (const void *)(uintptr_t)address, 4);
				if (index == 15)
				{
					pc_value = value;
					loads_pc = 1;
				}
				else
				{
					*core_register(registers, index) = value;
				}
			}
			else
			{
				uint32_t value = read_core(registers, index);

				memcpy((void *)(uintptr_t)address, &value, 4);
			}
			address += 4;
		}
		if (writeback && !(load && (list & (1u << rn))))
			*core_register(registers, rn) = increment ? base + 4 * count : base - 4 * count;
		if (loads_pc)
		{
			/* (an interworking return: the T bit from the address) */
			if (pc_value & 1)
				registers->arm_cpsr |= PSR_T_BIT;
			else
				registers->arm_cpsr &= ~PSR_T_BIT;
			registers->arm_pc = pc_value & ~1u;
			registers->arm_cpsr &= ~PSR_IT_MASK;
			fixups++;
			return 1;
		}
		goto done;
	}
	/* VLDR/VSTR: 1110 1101 UD0L Rn Vd 101s imm8; VLDM/VSTM (and VPUSH,
	VPOP): 1110 110P UDWL Rn Vd 101s imm8 */
	if ((instruction & 0xfe000e00u) == 0xec000a00u && (instruction & 0x01200000u))
	{
		struct vfp_frame *vfp = vfp_frame_of(ucontext);
		int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, writeback = (instruction >> 21) & 1;
		int load = (instruction >> 20) & 1, is_double = (instruction >> 8) & 1;
		unsigned d = (instruction >> 22) & 1, vd = (instruction >> 12) & 15;
		uint32_t imm8 = instruction & 0xff;
		uint32_t base = read_core(registers, rn);
		unsigned first_register, count, index;
		uint32_t address;

		if (!vfp)
			return 0;
		if (pre && !writeback)
		{
			/* VLDR/VSTR: one register at base +/- imm8 * 4 */
			address = up ? base + imm8 * 4 : base - imm8 * 4;
			count = 1;
		}
		else
		{
			/* VLDM/VSTM: increment after (P=0, U=1) or decrement before
			(P=1, U=0, W=1) */
			if (pre == up)
				return 0;
			count = is_double ? imm8 / 2 : imm8;
			address = up ? base : base - imm8 * 4;
		}
		first_register = is_double ? (d << 4) | vd : (vd << 1) | d;
		for (index = 0; index < count; index++)
		{
			unsigned number = first_register + index;

			if (is_double)
			{
				if (number >= 32)
					return 0;
				if (load)
					memcpy(&vfp->registers[number], (const void *)(uintptr_t)address, 8);
				else
					memcpy((void *)(uintptr_t)address, &vfp->registers[number], 8);
				address += 8;
			}
			else
			{
				uint32_t *single;

				if (number >= 32)
					return 0;
				single = (uint32_t *)&vfp->registers[number >> 1] + (number & 1);
				if (load)
					memcpy(single, (const void *)(uintptr_t)address, 4);
				else
					memcpy((void *)(uintptr_t)address, single, 4);
				address += 4;
			}
		}
		if (!(pre && !writeback) && writeback)
			*core_register(registers, rn) = up ? base + imm8 * 4 : base - imm8 * 4;
		goto done;
	}
	/* VLD1/VST1 (multiple single elements) with an alignment qualifier:
	1111 1001 0D L0 Rn Vd type size align Rm (the bytes in order, little-endian) */
	if ((instruction & 0xff900000u) == 0xf9000000u)
	{
		struct vfp_frame *vfp = vfp_frame_of(ucontext);
		int load = (instruction >> 21) & 1;
		unsigned first_register = (((instruction >> 22) & 1) << 4) | ((instruction >> 12) & 15);
		unsigned type = (instruction >> 8) & 15, rm = instruction & 15, count, index;
		uint32_t base = read_core(registers, rn), address = base;

		switch (type)
		{
		case 7: count = 1; break;
		case 10: count = 2; break;
		case 6: count = 3; break;
		case 2: count = 4; break;
		default: return 0;
		}
		if (!vfp || rn == 15 || first_register + count > 32)
			return 0;
		for (index = 0; index < count; index++)
		{
			if (load)
				memcpy(&vfp->registers[first_register + index], (const void *)(uintptr_t)address, 8);
			else
				memcpy((void *)(uintptr_t)address, &vfp->registers[first_register + index], 8);
			address += 8;
		}
		if (rm == 13)
			*core_register(registers, rn) = base + 8 * count;
		else if (rm != 15)
			*core_register(registers, rn) = base + read_core(registers, rm);
		goto done;
	}
	return 0;

done:
	registers->arm_pc = pc + size;
	if (registers->arm_cpsr & PSR_IT_MASK)
		registers->arm_cpsr = it_advance(registers->arm_cpsr);
	fixups++;
	return 1;
}

uint64_t host_alignment_fixups(void)
{
	return fixups;
}

#endif
