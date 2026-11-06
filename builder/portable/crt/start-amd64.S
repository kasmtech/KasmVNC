/* SPDX-License-Identifier: GPL-2.0-or-later
 * KasmVNC dynamic-PIE process entry for the Linux AMD64 ABI.
 * The initial stack contains argc/argv/envp; %rdx carries the loader's
 * finalizer. This source implements that interface, not a C runtime.
 */
#if !defined(__x86_64__) || !defined(__LP64__)
#error "This entry point requires the Linux AMD64 LP64 ABI"
#endif

.section .text
.globl _start
.type _start,@function
.hidden kasm_crt_start
_start:
    .cfi_startproc
    .cfi_undefined %rip
    endbr64
    xorl %ebp, %ebp
    movq %rsp, %rdi
    movq %rdx, %rsi
    andq $-16, %rsp
    call kasm_crt_start
    ud2
    .cfi_endproc
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
/* GNU_PROPERTY_X86_FEATURE_1_AND: this non-returning entry supports IBT
 * and shadow stacks. Preserve the corresponding compiler-object notes.
 */
.section .note.gnu.property,"a",@note
.p2align 3
.long 4, 16, 5
.asciz "GNU"
.long 0xc0000002, 4, 3, 0
