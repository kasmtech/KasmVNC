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
    def test_runtime_system_closure_with_and_without_dl(self):
        for libraries in ('dl;rt', ''):
            with self.subTest(libraries=libraries), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                prefix = root / 'runtime'
                (prefix / 'lib').mkdir(parents=True)
                for name in ('ssl', 'crypto', 'png16', 'z', 'webp', 'sharpyuv'):
                    (prefix / 'lib' / ('lib' + name + '.a')).touch()
                (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(profile NONE)
set(CMAKE_DL_LIBS "{libraries}")
set(KASMVNC_STATIC_RUNTIME_PREFIX "{prefix}" CACHE PATH "" FORCE)
include("{ROOT}/cmake/StaticRuntime.cmake")
get_target_property(closure kasmvnc_static_crypto INTERFACE_LINK_LIBRARIES)
file(WRITE "${{CMAKE_BINARY_DIR}}/closure.txt" "${{closure}}")
''')
                build = root / 'build'
                run('cmake', '-S', str(root), '-B', str(build))
                expected = (libraries.split(';') if libraries else []) + ['-pthread']
                self.assertEqual((build / 'closure.txt').read_text().split(';'), expected)
                line = (build / 'kasmvnc-static-runtime.mk').read_text().splitlines()[0]
                flags = shlex.split(line.split('=', 1)[1])[2:]
                self.assertEqual(flags, ['-l' + x if not x.startswith('-') else x for x in expected])

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
                expected = ['--prefix', desktop, '--runtime-prefix=' + runtime,
                            '--source-root', str(root), '--']
                for row in rows:
                    self.assertEqual(row[:len(expected)], expected)
                run(str(build / 'probe'))
                run(str(build / 'manual'))


if __name__ == '__main__':
    unittest.main()
