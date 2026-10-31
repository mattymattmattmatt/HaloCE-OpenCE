/*
The code the memory probe copies below 4 GB and runs there, assembled rather
than hand-encoded (the Switch's probe got an immediate wrong by hand).

It is placed in .rodata, not .text: the PS5 may map a program's text
execute-only, and the probe has to read these bytes to copy them.

Each function stands for something the x32 guest will do:
- guest_add: run at all from a low address;
- guest_load32: load through a 32-bit pointer (addr32 prefix), as every
  guest access to its data will;
- guest_call_host: jump from low code to the host's code above 4 GB;
- guest_stack_check: use a stack below 4 GB through a 32-bit stack pointer.
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
	.globl run_on_stack

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
