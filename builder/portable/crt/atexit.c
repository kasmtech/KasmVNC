/* SPDX-License-Identifier: GPL-2.0-or-later
 * Register C exit handlers with the host's existing DSO-aware machinery.
 * Both supported LP64 ABIs permit this callback to ignore its first argument.
 * The function-pointer adaptation is deliberately a platform ABI operation,
 * not a claim of portable ISO C semantics for differing function types.
 */
#if !defined(__linux__) || !defined(__LP64__) || \
    (!defined(__x86_64__) && !defined(__aarch64__))
#error "The portable atexit bridge supports Linux AMD64/AArch64 LP64 only"
#endif
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void *__dso_handle; /* Supplied by the compiler's crtbeginS.o. */

/* Hidden binding associates each linked object's callbacks with its own DSO;
 * a plugin must not interpose the executable's bridge or handle.
 */
__attribute__((visibility("hidden"))) int atexit(void (*callback)(void))
{
    return __cxa_atexit((void (*)(void *))callback, 0, __dso_handle);
}
