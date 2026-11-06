/* SPDX-License-Identifier: GPL-2.0-or-later
 * ELF compatibility metadata retained when replacing the toolchain startup.
 * These declarations contain no library implementation. Kernel ABI tags
 * retain the builder's existing minimum; they are not a tested-kernel claim.
 */
#if defined(__x86_64__) && defined(__LP64__)
#define OBJECT_TYPE @object
#define PROGBITS_TYPE @progbits
#define NOTE_TYPE @note
#define KERNEL_MINOR 2
#elif defined(__aarch64__) && defined(__LP64__)
#define OBJECT_TYPE %object
#define PROGBITS_TYPE %progbits
#define NOTE_TYPE %note
#define KERNEL_MINOR 7
#else
#error "Unsupported portable CRT metadata architecture"
#endif

.section .data
.p2align 2
.globl __data_start
.type __data_start,OBJECT_TYPE
__data_start:
.long 0
.size __data_start,4
.weak data_start
.set data_start,__data_start

/* The GNU stdio ABI marker identifies the modern FILE interface. */
.section .rodata
.p2align 2
.globl _IO_stdin_used
.type _IO_stdin_used,OBJECT_TYPE
_IO_stdin_used:
.long 0x20001
.size _IO_stdin_used,4

.section .note.ABI-tag,"a",NOTE_TYPE
.p2align 2
.long 4,16,1
.asciz "GNU"
.long 0,3,KERNEL_MINOR,0

.section .note.gnu.property,"a",NOTE_TYPE
.p2align 3
#ifdef __x86_64__
.long 4,32,5
.asciz "GNU"
/* Compatible with IBT/SHSTK; require the portable glibc x86-64-v2 floor. */
.long 0xc0000002,4,3,0
.long 0xc0008002,4,2,0
#else
.long 4,16,5
.asciz "GNU"
/* This data-only object is compatible with BTI/PAC. */
.long 0xc0000000,4,3,0
#endif
.section .note.GNU-stack,"",PROGBITS_TYPE
