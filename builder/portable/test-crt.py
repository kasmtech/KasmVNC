#!/usr/bin/env python3
"""Build and execute portable CRT lifecycle fixtures inside a test container.

CC/CXX select native or cross compilers; CRT_RUNNER can contain e.g.
"qemu-aarch64 -L /usr/aarch64-linux-gnu". CRT_TEST_OUTPUT retains evidence.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parent / 'crt'
ROOT = Path(__file__).resolve().parents[2]
EXPECTED = '''preinit
attribute:constructor
global:constructor
main:arguments-environment
thread:tls
tls:destructor
exception:caught
signal:handled
plugin:constructor
plugin:call
plugin:attribute-destructor
plugin:destructor
plugin:atexit
main:return
tls:destructor
atexit:second
atexit:first
global:destructor
attribute:destructor
'''


class PortableCRTTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shlex.split(os.environ.get('CC', 'gcc'))
        cls.cxx = shlex.split(os.environ.get('CXX', 'g++'))
        cls.runner = shlex.split(os.environ.get('CRT_RUNNER', ''))
        cls.target = cls.run_command([*cls.cc, '-dumpmachine']).strip()
        if cls.target.startswith('x86_64'):
            cls.architecture, flags = 'amd64', ['-fcf-protection=full']
        elif cls.target.startswith('aarch64'):
            cls.architecture, flags = 'arm64', ['-mbranch-protection=standard']
        else:
            raise RuntimeError('Unsupported CRT test architecture: ' + cls.target)
        cls.temp = tempfile.TemporaryDirectory(prefix='kasm-crt-test-')
        cls.directory = Path(os.environ.get('CRT_TEST_OUTPUT', cls.temp.name)).resolve()
        cls.directory.mkdir(parents=True, exist_ok=True)
        cls.flags = ['-O2', '-fPIE', '-fno-stack-protector', '-fno-sanitize=all', *flags]
        cls.begin = cls.run_command([*cls.cc, '-print-file-name=crtbeginS.o']).strip()
        cls.end = cls.run_command([*cls.cc, '-print-file-name=crtendS.o']).strip()
        # AArch64's loader owns __stack_chk_guard. The normal libc linker
        # script adds it; spell out that shared input when bypassing the script.
        cls.loader = ['-l:ld-linux-aarch64.so.1'] if cls.architecture == 'arm64' else []
        for source, obj in [('start-' + cls.architecture + '.S', 'kasm-start.o'),
                            ('bootstrap.c', 'kasm-bootstrap.o'), ('atexit.c', 'kasm-atexit.o'),
                            ('stat.c', 'kasm-stat.o'), ('metadata.S', 'kasm-metadata.o')]:
            cls.run_command([*cls.cc, *cls.flags, '-c', str(SOURCE / source), '-o', str(cls.directory / obj)])
        cls.run_command([*cls.cxx, '-O2', '-fPIE', '-UNDEBUG', *flags, '-c',
                         str(SOURCE / 'lifecycle-main.cxx'), '-o', str(cls.directory / 'main.o')])
        cls.run_command([*cls.cxx, '-O2', '-fPIC', '-UNDEBUG', *flags, '-c',
                         str(SOURCE / 'lifecycle-plugin.cxx'), '-o', str(cls.directory / 'plugin.o')])
        d = cls.directory
        cls.run_command([*cls.cxx, '-shared', str(d / 'plugin.o'), '-o', str(d / 'plugin-default.so')])
        cls.run_command([*cls.cxx, '-shared', '-nostdlib', cls.begin, str(d / 'plugin.o'),
                         str(d / 'kasm-atexit.o'), '-lstdc++', '-lgcc_s', '-l:libc.so.6',
                         *cls.loader, cls.end, '-o', str(d / 'plugin-owned.so')])
        cls.run_command([*cls.cxx, '-pie', str(d / 'main.o'), '-pthread', '-ldl',
                         '-Wl,-z,relro,-z,now', '-o', str(d / 'baseline')])
        cls.run_command([*cls.cxx, '-pie', '-nostdlib', '-Wl,-z,relro,-z,now',
                         '-Wl,-Map,' + str(d / 'replacement.map'), str(d / 'kasm-start.o'),
                         str(d / 'kasm-metadata.o'), cls.begin, str(d / 'kasm-bootstrap.o'), str(d / 'main.o'),
                         str(d / 'kasm-atexit.o'), str(d / 'kasm-stat.o'), '-lstdc++', '-lm', '-lpthread', '-ldl', '-lgcc_s', '-l:libc.so.6',
                         *cls.loader, cls.end, '-o', str(d / 'replacement')])
        # Exercise the real compiler driver/specs integration as well as the
        # independent minimal link. Its default libraries must retain the same
        # lifecycle semantics, including compiler-supplied nonshared helpers.
        cls.run_command([sys.executable, str(ROOT / 'builder/scripts/link-static-desktop'),
                         '--prefix', '', '--source-root', str(ROOT), '--host-shared',
                         '--owned-glibc-crt', '--runtime-only', '--', *cls.cxx,
                         str(d / 'main.o'), '-pthread', '-ldl', '-o', str(d / 'replacement-driver')])
        cls.elf = cls.run_command(['readelf', '-aW', str(d / 'replacement')])
        (d / 'replacement.readelf.txt').write_text(cls.elf)
        cls.results = []

    @classmethod
    def run_command(cls, args):
        result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError('Command failed: ' + shlex.join(args) + '\n' + result.stderr)
        return result.stdout

    @classmethod
    def tearDownClass(cls):
        (cls.directory / 'summary.json').write_text(json.dumps({
            'architecture': cls.architecture, 'compiler_target': cls.target,
            'runner': cls.runner, 'lifecycle_runs': cls.results,
        }, indent=2) + '\n')
        cls.temp.cleanup()

    def test_lifecycle_matches_toolchain_startup(self):
        for plugin in ('plugin-default.so', 'plugin-owned.so'):
            for binary in ('baseline', 'replacement', 'replacement-driver'):
                with self.subTest(plugin=plugin, binary=binary):
                    result = subprocess.run([
                        *self.runner, str(self.directory / binary), 'first-argument',
                        'second-argument', str(self.directory / plugin)],
                        env={**os.environ, 'KASM_CRT_TEST_ENV': 'environment-value'},
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.results.append({'binary': binary, 'plugin': plugin,
                                         'status': result.returncode, 'stdout': result.stdout,
                                         'stderr': result.stderr})
                    self.assertEqual(result.returncode, 17, result.stderr)
                    self.assertEqual(result.stdout, EXPECTED)
                    self.assertEqual(result.stderr, '')

    def test_no_glibc_development_objects_are_loaded(self):
        link_map = (self.directory / 'replacement.map').read_text()
        for name in ('Scrt1.o', 'crti.o', 'crtn.o', 'libc_nonshared.a'):
            self.assertNotIn(name, link_map)
        self.assertIn('crtbeginS.o', link_map)
        self.assertIn('crtendS.o', link_map)

    def test_production_driver_audit(self):
        sys.path.insert(0, str(ROOT / 'builder/portable'))
        import glibc_crt
        result = glibc_crt.audit_link(self.directory / 'replacement-driver')
        self.assertEqual(result['mode'], 'owned-glibc-2.28')

    def test_dynamic_pie_and_host_runtime_abi(self):
        self.assertRegex(self.elf, r'Type:\s+DYN')
        self.assertIn('Requesting program interpreter:', self.elf)
        self.assertIn('Shared library: [libc.so.6]', self.elf)
        self.assertIn('__libc_start_main@GLIBC_' + ('2.2.5' if self.architecture == 'amd64' else '2.17'), self.elf)
        self.assertIn('__cxa_atexit@GLIBC_', self.elf)
        self.assertRegex(self.elf, r'FUNC\s+LOCAL\s+\S+\s+\S+\s+atexit\b')
        for marker in ('__data_start', 'data_start', '_IO_stdin_used'):
            self.assertIn(marker, self.elf)
        self.assertIn('OS: Linux, ABI: 3.' + ('2' if self.architecture == 'amd64' else '7') + '.0', self.elf)
        self.assertRegex(self.elf, r'\.note\.ABI-tag\s+NOTE\b')

    def test_hardening_and_array_lifecycle(self):
        self.assertRegex(self.elf, r'GNU_STACK\s+.*\sRW\s')
        self.assertIn('GNU_RELRO', self.elf)
        self.assertRegex(self.elf, r'\(BIND_NOW\)|\(FLAGS(?:_1)?\).*\b(?:BIND_NOW|NOW)\b')
        self.assertIn('(PREINIT_ARRAY)', self.elf)
        self.assertIn('(INIT_ARRAY)', self.elf)
        self.assertIn('(FINI_ARRAY)', self.elf)
        self.assertNotRegex(self.elf, r'\((?:INIT|FINI)\)')
        if self.architecture == 'amd64':
            self.assertRegex(self.elf, r'\.note\.gnu\.property\s+NOTE\b')
            self.assertIn('x86 feature: IBT, SHSTK', self.elf)
            self.assertRegex(self.elf, r'x86-64-baseline|0xc0008002 data: 01 00 00 00')
        # Some GCC AArch64 crtbegin/end objects lack property notes. Verify the
        # owned entry's claim without falsely claiming the full ELF has BTI/PAC.
        else:
            entry = self.run_command(['readelf', '-aW', str(self.directory / 'kasm-start.o')])
            self.assertRegex(entry, r'\.note\.gnu\.property\s+NOTE\b')
            self.assertIn('AArch64 feature: BTI, PAC', entry)

    def test_bootstrap_requires_pie(self):
        result = subprocess.run([*self.cc, '-fno-pie', '-c', str(SOURCE / 'bootstrap.c'),
                                 '-o', str(self.directory / 'invalid.o')],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must be compiled for PIE', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
