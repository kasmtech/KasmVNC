#!/usr/bin/env python3
"""Exercise generated CMake/Make link commands in a disposable build container."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


class Profiles(unittest.TestCase):
    def test_runtime_images_do_not_require_static_openssl(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prefix = root / 'runtime'
            (prefix / 'lib').mkdir(parents=True)
            for name in ('png16', 'z', 'webp', 'sharpyuv'):
                (prefix / 'lib' / ('lib' + name + '.a')).touch()
            (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(profile NONE)
set(KASMVNC_STATIC_RUNTIME_PREFIX "{prefix}" CACHE PATH "" FORCE)
include("{ROOT}/cmake/StaticRuntime.cmake")
''')
            build = root / 'build'
            run('cmake', '-S', str(root), '-B', str(build))
            flags = (build / 'kasmvnc-static-runtime.mk').read_text()
            self.assertNotIn('libssl.a', flags)
            self.assertNotIn('libcrypto.a', flags)
            (prefix / 'lib/libcrypto.a').touch()
            result = subprocess.run(['cmake', '-S', str(root), '-B', str(build)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Obsolete static OpenSSL', result.stderr)

    def test_openssl_sdk_headers_require_30(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prefix = root / 'sdk'
            (prefix / 'lib').mkdir(parents=True)
            (prefix / 'include/openssl').mkdir(parents=True)
            for name in ('ssl', 'crypto'):
                (prefix / 'lib' / ('lib' + name + '.so.3')).touch()
            header = prefix / 'include/openssl/opensslv.h'
            (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(profile NONE)
set(KASMVNC_OPENSSL_PREFIX "{prefix}" CACHE PATH "" FORCE)
include("{ROOT}/cmake/StaticRuntime.cmake")
if(NOT CMAKE_SKIP_RPATH)
  message(FATAL_ERROR "SDK must not become a runtime search path")
endif()
''')
            for major, minor, valid in [(3, 0, True), (3, 5, False), (4, 0, False)]:
                header.write_text(f'# define OPENSSL_VERSION_MAJOR  {major}\n# define OPENSSL_VERSION_MINOR  {minor}\n')
                result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, valid, result.stderr)

    def test_both_wrapper_consumers_preserve_spaces_and_empty_prefix(self):
        for runtime in ('', 'runtime with spaces'):
            with self.subTest(runtime=runtime), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'source with spaces'
                scripts = root / 'builder/scripts'
                scripts.mkdir(parents=True)
                log = root / 'invocations.jsonl'
                (scripts / 'link-static-desktop').write_text(f'''import json, os, sys
with open({str(log)!r}, 'a') as out: out.write(json.dumps(sys.argv[1:]) + "\\n")
args = sys.argv[sys.argv.index('--') + 1:]
os.execvp(args[0], args)
''')
                desktop = str(root / 'desktop with spaces')
                (root / 'main.cxx').write_text('int main() { return 0; }\n')
                (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(profile CXX)
set(KASMVNC_STATIC_DESKTOP_PREFIX "{desktop}" CACHE PATH "" FORCE)
set(KASMVNC_STATIC_RUNTIME_PREFIX "{runtime}" CACHE PATH "" FORCE)
include("{ROOT}/cmake/StaticDesktop.cmake")
add_executable(probe main.cxx)
kasmvnc_static_desktop_target(probe)
''')
                build = root / 'build'
                run('cmake', '-S', str(root), '-B', str(build))
                run('cmake', '--build', str(build))
                # Exercise the generated Xorg make command with a libtool shim;
                # the real compiler still performs the link after wrapper parsing.
                shim = root / 'libtool'
                shim.write_text('#!/bin/sh\nshift 2\nexec "$@"\n')
                shim.chmod(0o755)
                (build / 'manual.mk').write_text('''include kasmvnc-static-desktop.mk
LIBTOOL = ../libtool
CXX = c++
all: manual
manual:
\t$(Xvnc_LINK) "../main.cxx"
''')
                run('make', '-f', 'manual.mk', cwd=build)
                rows = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(len(rows), 2)
                expected = ['--prefix', desktop, '--runtime-prefix=' + runtime, '--openssl-prefix=',
                            '--source-root', str(root), '--']
                for row in rows:
                    self.assertEqual(row[:len(expected)], expected)
                run(str(build / 'probe'))
                run(str(build / 'manual'))


if __name__ == '__main__':
    unittest.main()
