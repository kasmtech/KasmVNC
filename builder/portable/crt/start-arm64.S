/* SPDX-License-Identifier: GPL-2.0-or-later
 * KasmVNC dynamic-PIE process entry for the Linux AArch64 LP64 ABI.
 * SP addresses argc/argv/envp and is already 16-byte aligned; x0 is the
 * loader's finalizer. The outermost frame has no caller or return address.
 */
#if !defined(__aarch64__) || !defined(__LP64__)
#error "This entry point requires the Linux AArch64 LP64 ABI"
#endif

.section .text
.p2align 2
.globl _start
.type _start,%function
.hidden kasm_crt_start
_start:
    .cfi_startproc
    .cfi_undefined 30
    hint #34                       /* BTI c; compatible with Armv8.0. */
    mov x1, x0
    mov x0, sp
    mov x29, xzr
    mov x30, xzr
    bl kasm_crt_start
    brk #0
    .cfi_endproc
.size _start, .-_start

.section .note.GNU-stack,"",%progbits
/* GNU_PROPERTY_AARCH64_FEATURE_1_AND: BTI and PAC compatible. This entry
 * never returns, so it has no saved return address requiring signing.
 */
.section .note.gnu.property,"a",%note
.p2align 3
.long 4, 16, 5
.asciz "GNU"
.long 0xc0000000, 4, 3, 0
