/*
 * glibc _setjmp / longjmp inside glibc's jmp_buf (bionic's is smaller).
 *
 * i386 (156-byte jmp_buf): glibc's __jmp_buf is int[6] = ebx esi edi ebp esp eip;
 * __mask_was_saved (int) at 24 is set to 0. cdecl: env at 4(%esp), val at 8(%esp).
 * Same rules as x86_64 below otherwise.
 *
 * x86_64 (200-byte jmp_buf): layout of the first 64 bytes as glibc's __jmp_buf:
 * rbx rbp r12 r13 r14 r15 rsp rip; __mask_was_saved (int) at 64 is set to 0.
 * _setjmp never saves the signal mask, so longjmp never restores one.
 * Both functions are only ever reached by a tail jump from a front, so the
 * return address on the stack is the original caller's.
 */
#include "glibc_abi.h"

#if defined(__i386__)
	.text
	.globl	nvg__setjmp
	.type	nvg__setjmp,@function
	.p2align 4
nvg__setjmp:
	movl	4(%esp), %eax		/* env */
	movl	%ebx, 0(%eax)
	movl	%esi, 4(%eax)
	movl	%edi, 8(%eax)
	movl	%ebp, 12(%eax)
	leal	4(%esp), %ecx		/* caller's esp after our return */
	movl	%ecx, 16(%eax)
	movl	(%esp), %ecx		/* return address */
	movl	%ecx, 20(%eax)
	movl	$0, NVG_GLIBC_OFFSETOF_JMP_BUF_MASK_WAS_SAVED(%eax)
	xorl	%eax, %eax
	ret
	.size	nvg__setjmp, .-nvg__setjmp

	.globl	nvg_longjmp
	.type	nvg_longjmp,@function
	.p2align 4
nvg_longjmp:
	movl	4(%esp), %ecx		/* env */
	movl	8(%esp), %eax		/* val */
	testl	%eax, %eax
	jnz	1f
	incl	%eax			/* longjmp(env, 0) returns 1 */
1:
	movl	0(%ecx), %ebx
	movl	4(%ecx), %esi
	movl	8(%ecx), %edi
	movl	12(%ecx), %ebp
	movl	16(%ecx), %esp
	jmpl	*20(%ecx)
	.size	nvg_longjmp, .-nvg_longjmp
#else
	.text
	.globl	nvg__setjmp
	.type	nvg__setjmp,@function
	.p2align 4
nvg__setjmp:
	movq	%rbx, 0(%rdi)
	movq	%rbp, 8(%rdi)
	movq	%r12, 16(%rdi)
	movq	%r13, 24(%rdi)
	movq	%r14, 32(%rdi)
	movq	%r15, 40(%rdi)
	leaq	8(%rsp), %rdx		/* caller's rsp after our return */
	movq	%rdx, 48(%rdi)
	movq	(%rsp), %rdx		/* return address */
	movq	%rdx, 56(%rdi)
	movl	$0, NVG_GLIBC_OFFSETOF_JMP_BUF_MASK_WAS_SAVED(%rdi)
	xorl	%eax, %eax
	ret
	.size	nvg__setjmp, .-nvg__setjmp

	.globl	nvg_longjmp
	.type	nvg_longjmp,@function
	.p2align 4
nvg_longjmp:
	movl	%esi, %eax
	testl	%eax, %eax
	jnz	1f
	incl	%eax			/* longjmp(env, 0) returns 1 */
1:
	movq	0(%rdi), %rbx
	movq	8(%rdi), %rbp
	movq	16(%rdi), %r12
	movq	24(%rdi), %r13
	movq	32(%rdi), %r14
	movq	40(%rdi), %r15
	movq	48(%rdi), %rsp
	jmpq	*56(%rdi)
	.size	nvg_longjmp, .-nvg_longjmp
#endif

	.section .note.GNU-stack,"",@progbits
