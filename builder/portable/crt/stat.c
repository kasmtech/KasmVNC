/* SPDX-License-Identifier: GPL-2.0-or-later
 * EL8's public stat functions otherwise select glibc's nonshared wrappers.
 * Keep the implementation in host libc via its versioned stat ABI instead.
 * Only Linux LP64 AMD64/AArch64 are supported; no structure translation.
 */
#define _GNU_SOURCE 1
#define __NO_INLINE__ 1
#include <sys/stat.h>
#if !defined(__linux__) || !defined(__LP64__)
#error "Unsupported stat ABI"
#endif
#if defined(__x86_64__)
#define KASM_STAT_VERSION 1
#elif defined(__aarch64__)
#define KASM_STAT_VERSION 0
#else
#error "Unsupported stat architecture"
#endif
#ifdef _STAT_VER
_Static_assert(_STAT_VER == KASM_STAT_VERSION, "Unexpected host stat ABI");
#endif
extern int __xstat(int, const char *, struct stat *);
extern int __lxstat(int, const char *, struct stat *);
extern int __fxstat(int, int, struct stat *);
extern int __fxstatat(int, int, const char *, struct stat *, int);
extern int __xstat64(int, const char *, struct stat64 *);
extern int __lxstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);
extern int __fxstatat64(int, int, const char *, struct stat64 *, int);
#define HIDDEN __attribute__((visibility("hidden")))
HIDDEN int stat(const char *p, struct stat *s) { return __xstat(KASM_STAT_VERSION, p, s); }
HIDDEN int lstat(const char *p, struct stat *s) { return __lxstat(KASM_STAT_VERSION, p, s); }
HIDDEN int fstat(int fd, struct stat *s) { return __fxstat(KASM_STAT_VERSION, fd, s); }
HIDDEN int fstatat(int fd, const char *p, struct stat *s, int flags)
{ return __fxstatat(KASM_STAT_VERSION, fd, p, s, flags); }
HIDDEN int stat64(const char *p, struct stat64 *s) { return __xstat64(KASM_STAT_VERSION, p, s); }
HIDDEN int lstat64(const char *p, struct stat64 *s) { return __lxstat64(KASM_STAT_VERSION, p, s); }
HIDDEN int fstat64(int fd, struct stat64 *s) { return __fxstat64(KASM_STAT_VERSION, fd, s); }
HIDDEN int fstatat64(int fd, const char *p, struct stat64 *s, int flags)
{ return __fxstatat64(KASM_STAT_VERSION, fd, p, s, flags); }
