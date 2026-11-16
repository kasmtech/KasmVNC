#!/usr/bin/env python3
"""Exercise the real compiler wrapper and its startup rejection gates in Docker."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import glibc_crt

ROOT = Path(__file__).resolve().parents[2]


class StartupLink(unittest.TestCase):
    def test_cmake_covers_runtime_only_and_desktop(self):
        with tempfile.TemporaryDirectory(prefix='kasm crt cmake ') as tmp:
            root = Path(tmp)
            # Copy the real startup sources so this test can change one and
            # verify both build systems actually relink existing executables.
            scripts, portable = root / 'builder/scripts', root / 'builder/portable'
            scripts.mkdir(parents=True)
            portable.mkdir(parents=True)
            shutil.copy2(ROOT / 'builder/scripts/link-static-desktop', scripts)
            shutil.copy2(ROOT / 'builder/portable/glibc_crt.py', portable)
            shutil.copytree(ROOT / 'builder/portable/crt', portable / 'crt')
            (root / 'main.c').write_text('int main(void) { return 0; }\n')
            (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(startup C)
set(KASMVNC_DYNAMIC_DESKTOP ON CACHE BOOL "" FORCE)
set(KASMVNC_OWN_GLIBC_CRT ON CACHE BOOL "" FORCE)
include("{ROOT}/cmake/StaticDesktop.cmake")
add_executable(runtime main.c)
kasmvnc_static_desktop_target(runtime RUNTIME_ONLY)
add_executable(desktop main.c)
kasmvnc_static_desktop_target(desktop)
''')
            build = root / 'build'
            for command in (['cmake', '-S', str(root), '-B', str(build)],
                            ['cmake', '--build', str(build)]):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ('runtime', 'desktop'):
                glibc_crt.audit_link(build / name)
                subprocess.run([str(build / name)], check=True)
            shim = root / 'libtool'
            shim.write_text('#!/bin/sh\nshift 2\nexec "$@"\n')
            shim.chmod(0o755)
            # Match real Automake ordering: the generated executable rule
            # appears before the included KasmVNC fragment.
            (build / 'manual.mk').write_text('''LIBTOOL = ../libtool
CXX = gcc
CXXFLAGS = -fPIE
Xvnc: ../main.c
\t$(Xvnc_LINK) "../main.c"
include kasmvnc-static-desktop.mk
''')
            make = ['make', '-f', 'manual.mk', 'Xvnc']
            result = subprocess.run(make, cwd=build, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            targets = [build / name for name in ('runtime', 'desktop', 'Xvnc')]
            before = {path: path.stat().st_mtime_ns for path in targets}
            commands = [(['cmake', '--build', str(build)], None), (make, build)]
            for command, cwd in commands:
                result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(before, {path: path.stat().st_mtime_ns for path in targets})
            bootstrap = portable / 'crt/bootstrap.c'
            bootstrap.write_text(bootstrap.read_text() + '\n/* Incremental build regression. */\n')
            for command, cwd in commands:
                result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for path in targets:
                self.assertGreater(path.stat().st_mtime_ns, before[path])
                glibc_crt.audit_link(path)

    def test_wrapper_and_failure_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='kasm crt link ') as tmp:
            root = Path(tmp)
            # Exercise the launcher branch without requiring a cache daemon.
            launcher = root / 'ccache'
            launcher.write_text('#!/bin/sh\nexec "$@"\n')
            launcher.chmod(0o755)
            wrapper = ['python3', str(ROOT / 'builder/scripts/link-static-desktop'),
                       '--prefix', '', '--source-root', str(ROOT), '--host-shared',
                       '--owned-glibc-crt', '--runtime-only', '--', str(launcher)]
            for compiler, extension, source in [
                (os.environ.get('CC', 'gcc'), 'c',
                 '#include <stdlib.h>\n#include <stdio.h>\n'
                 'static void end(void) { puts("exit"); }\n'
                 'int main(void) { atexit(end); puts("main"); return 0; }\n'),
                (os.environ.get('CXX', 'g++'), 'cxx',
                 '#include <cstdio>\n#include <stdexcept>\n'
                 'struct global { global() { puts("init"); } ~global() { puts("exit"); } } g;\n'
                 'int main() { try { throw 7; } catch (int) { puts("main"); } }\n')]:
                with self.subTest(language=extension):
                    src, binary = root / ('main.' + extension), root / ('owned ' + extension)
                    src.write_text(source)
                    result = subprocess.run(wrapper + [compiler, '-fPIE', str(src), '-o', str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, ('init\n' if extension == 'cxx' else '') + 'main\nexit\n')
                    glibc_crt.audit_link(binary)
                    meta = json.loads(Path(str(binary) + '.static-link.json').read_text())
                    self.assertEqual(meta['startup_mode'], glibc_crt.MODE)
                    # A genuine compiler-default link must not be represented
                    # as using the owned policy, even with forged metadata.
                    baseline = root / ('default ' + extension)
                    subprocess.run([compiler, '-fPIE', str(src), '-pie', '-Wl,-Map=' + str(baseline) + '.link-map',
                                    '-o', str(baseline)], check=True)
                    Path(str(baseline) + '.static-link.json').write_text(json.dumps(meta))
                    with self.assertRaisesRegex(RuntimeError, 'Embedded glibc'):
                        glibc_crt.audit_link(baseline)
            # No implicit discard of a newly introduced legacy initializer.
            legacy = root / 'legacy.S'
            legacy.write_text('.section .init,"ax",@progbits\n.byte 0\n.section .note.GNU-stack,"",@progbits\n')
            result = subprocess.run(wrapper + [os.environ.get('CC', 'gcc'), '-fPIE', str(root / 'main.c'),
                                               str(legacy), '-o', str(root / 'legacy')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Unsupported legacy', result.stderr)
            for flag, error in [('-Wl,-e,main', 'entry does not use owned _start'),
                                ('-Wl,-z,lazy', 'Missing immediate binding')]:
                result = subprocess.run(wrapper + [os.environ.get('CC', 'gcc'), '-fPIE', str(root / 'main.c'),
                                                   flag, '-o', str(root / 'bypass')], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)

    def test_owned_stat_wrappers(self):
        # Force out-of-line calls: optimized EL8 headers normally inline them,
        # masking the libc_nonshared fallback selected by real application code.
        with tempfile.TemporaryDirectory(prefix='kasm-stat-') as tmp:
            root = Path(tmp)
            source = root / 'stat.c'
            source.write_text(r"""
#define _GNU_SOURCE 1
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
int main(int argc, char **argv) {
    struct stat s; struct stat64 s64;
    int fd = open(argv[1], O_CREAT|O_RDWR, 0600); assert(fd >= 0);
    assert(write(fd, "abc", 3) == 3);
    assert(symlink(argv[1], argv[2]) == 0);
    assert(stat(argv[2], &s) == 0 && S_ISREG(s.st_mode) && s.st_size == 3);
    assert(lstat(argv[2], &s) == 0 && S_ISLNK(s.st_mode));
    assert(fstat(fd, &s) == 0 && s.st_size == 3);
    assert(fstatat(AT_FDCWD, argv[2], &s, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(s.st_mode));
    assert(stat64(argv[2], &s64) == 0 && s64.st_size == 3);
    assert(lstat64(argv[2], &s64) == 0 && S_ISLNK(s64.st_mode));
    assert(fstat64(fd, &s64) == 0 && s64.st_size == 3);
    assert(fstatat64(AT_FDCWD, argv[2], &s64, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(s64.st_mode));
    errno = 0; assert(fstat(-1, &s) == -1 && errno == EBADF);
    assert(unlink(argv[1]) == 0);
    errno = 0; assert(stat(argv[1], &s) == -1 && errno == ENOENT);
    assert(unlink(argv[2]) == 0 && close(fd) == 0);
    return 0;
}
""")
            binary = root / 'stat-test'
            subprocess.run(['python3', str(ROOT / 'builder/scripts/link-static-desktop'),
                            '--prefix', '', '--source-root', str(ROOT), '--host-shared',
                            '--owned-glibc-crt', '--runtime-only', '--', os.environ.get('CC', 'gcc'),
                            '-O0', '-fPIE', str(source), '-o', str(binary)], check=True)
            glibc_crt.audit_link(binary)
            subprocess.run([str(binary), str(root / 'file'), str(root / 'symlink')], check=True)

    def test_selected_archive_boundary(self):
        # A libc linker script can name the unused archive. That does not
        # warrant suppressing it when any member actually enters the binary.
        glibc_crt.reject_glibc_inputs('Discarded input sections\nLOAD /usr/lib64/libc_nonshared.a\n')
        for name in ('libc_nonshared.a', 'libc.a'):
            with self.assertRaisesRegex(RuntimeError, 'Embedded glibc'):
                glibc_crt.reject_glibc_inputs('/usr/lib64/' + name + '(atexit.oS)\nDiscarded input sections\n')

    def test_profile_rejects_musl(self):
        previous = os.environ.get('KASMVNC_LIBC')
        try:
            os.environ['KASMVNC_LIBC'] = 'musl'
            with self.assertRaisesRegex(RuntimeError, 'cannot be used with musl'):
                glibc_crt.prepare(['gcc'], [], Path('unused'), ROOT)
        finally:
            if previous is None:
                os.environ.pop('KASMVNC_LIBC')
            else:
                os.environ['KASMVNC_LIBC'] = previous


if __name__ == '__main__':
    unittest.main()
