/*
The guest probe's assembly.

The test code is copied below 4 GB and run there, as the memory probe's
(port/ps5/probe/mem/payload.S, which says why each function stands for
something the x32 guest does). It sits in .rodata, since the probe reads
these bytes to copy them.

import_dispatch is where every entry of the guest's import table leads: each
entry is a thunk that loads its own index into %r10 (which no call passes
arguments in) and jumps here. The guest reached the thunk with a call, so
its arguments are still in their registers and on the stack as the host's
calling convention has them, and a jump to C passes all of them on.
*/

	.section .rodata
	.p2align 4
	.globl guest_code_start, guest_code_end
	.globl guest_add, guest_load32, guest_call_host, guest_stack_check

guest_code_start:

/* int guest_add(int a, int b): a + b */
guest_add:
	leal (%rdi,%rsi), %eax
	ret

/* int guest_load32(uint32_t address): the 32-bit word at address */
guest_load32:
	movl (%edi), %eax
	ret

/* int guest_call_host(int a, int b, int (*host)(int)): host(a + b) */
guest_call_host:
	leal (%rdi,%rsi), %edi
	jmp *%rdx

/* int guest_stack_check(int value): pushes value, reads it back through
%esp, and returns it; on a stack above 4 GB %esp is not its address */
guest_stack_check:
	pushq %rdi
	movl %esp, %eax
	movl (%eax), %eax
	popq %rdi
	ret

guest_code_end:

	.text
	.p2align 4
	.globl run_on_stack, import_dispatch

/* long run_on_stack(long (*function)(long), long argument, void *stack_top):
calls function(argument) with the stack pointer at stack_top */
run_on_stack:
	pushq %rbp
	movq %rsp, %rbp
	movq %rdx, %rsp
	andq $-16, %rsp
	movq %rdi, %rax
	movq %rsi, %rdi
	callq *%rax
	movq %rbp, %rsp
	popq %rbp
	ret

/* the guest's thread is the only one that calls imports */
import_dispatch:
	movl %r10d, current_import(%rip)
	jmp import_called
