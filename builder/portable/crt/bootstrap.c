/* SPDX-License-Identifier: GPL-2.0-or-later
 * Dynamic glibc >= 2.34 only. The host libc and loader retain ownership of
 * security initialization, constructor arrays, TLS, threading and exit.
 */
#if !defined(__linux__) || !defined(__LP64__) || \
    (!defined(__x86_64__) && !defined(__aarch64__))
#error "The portable startup supports Linux AMD64/AArch64 LP64 only"
#endif
#ifndef __PIE__
#error "The portable startup must be compiled for PIE"
#endif

typedef unsigned long stack_word;
typedef void (*finalizer)(void);
extern int main(int, char **, char **);
extern int kasm_libc_start_main(int (*)(int, char **, char **), int, char **,
                               finalizer, finalizer, finalizer, void *);
/* Null init is supported by this ABI version. Do not accidentally bind the
 * older compatibility symbol, whose caller must provide constructor glue.
 */
__asm__(".symver kasm_libc_start_main,__libc_start_main@GLIBC_2.34");

__attribute__((noreturn, visibility("hidden")))
void kasm_crt_start(stack_word *initial_stack, finalizer loader_finalize)
{
    /* The dynamic loader already established stack-end state. The final
     * argument remains the initial stack bound; static libc is unsupported.
     */
    kasm_libc_start_main(main, (int)initial_stack[0],
                        (char **)(initial_stack + 1), 0, 0,
                        loader_finalize, initial_stack);
    __builtin_trap();
}
