/* SPDX-License-Identifier: GPL-2.0-or-later
 * Dynamic glibc >= 2.28 only. The host libc and loader retain ownership of
 * security initialization, preinitializers, TLS, threading and exit.
 * This bridge invokes executable constructors for the older startup ABI.
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
typedef void (*initializer)(int, char **, char **);
extern initializer __init_array_start[] __attribute__((visibility("hidden")));
extern initializer __init_array_end[] __attribute__((visibility("hidden")));

static void initialize(int argc, char **argv, char **envp)
{
    for (initializer *next = __init_array_start; next != __init_array_end; ++next)
        (*next)(argc, argv, envp);
}
extern int main(int, char **, char **);
extern int kasm_libc_start_main(int (*)(int, char **, char **), int, char **,
                               initializer, finalizer, finalizer, void *);
/* The dynamic loader executes preinit and owns finalization. Unlike the
 * 2.34 entry ABI, these public compatibility symbols require a nonnull init.
 * No legacy .init/.fini sections are supported (enforced by the ELF audit).
 */
#if defined(__x86_64__)
__asm__(".symver kasm_libc_start_main,__libc_start_main@GLIBC_2.2.5");
#else
__asm__(".symver kasm_libc_start_main,__libc_start_main@GLIBC_2.17");
#endif

__attribute__((noreturn, visibility("hidden")))
void kasm_crt_start(stack_word *initial_stack, finalizer loader_finalize)
{
    /* The dynamic loader already established stack-end state. The final
     * argument remains the initial stack bound; static libc is unsupported.
     */
    kasm_libc_start_main(main, (int)initial_stack[0],
                        (char **)(initial_stack + 1), initialize, 0,
                        loader_finalize, initial_stack);
    __builtin_trap();
}
