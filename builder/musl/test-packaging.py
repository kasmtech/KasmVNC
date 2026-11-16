#!/usr/bin/env python3
"""Run in the Alpine packaging test container, never on the developer host."""
import importlib.util
import json
import os
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

sources = load('sources', HERE / 'dependencies/cache-apk-sources.py')
keys = load('keys', HERE / 'signing-keys.py')
audit = load('audit', HERE / 'audit.py')

# Actual --dyn-syms --wide output from the native Alpine 3.21 ARM64 Xvnc.
ARM_GCC_ALIASES = (
    '   240: 0000000000000000     0 FUNC    WEAK   DEFAULT  UND __register_frame_info@GLIBC_2.0 (7)\n'
    '   433: 0000000000000000     0 FUNC    WEAK   DEFAULT  UND __deregister_frame_info@GLIBC_2.0 (7)\n'
)
ARM_GCC_VERSION_NEEDS = (
    "Version needs section '.gnu.version_r' contains 1 entry:\n"
    '  000000: Version: 1  File: libgcc_s.so.1  Cnt: 2\n'
    '  0x0010:   Name: GCC_3.0  Flags: none  Version: 6\n'
    '  0x0020:   Name: GLIBC_2.0  Flags: none  Version: 7\n'
)

class PackagingTests(unittest.TestCase):
    def test_exact_apk_ownership_and_cache_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            db = root / 'installed'
            db.write_text('P:musl-dev\nV:1.2.5-r11\nA:x86_64\no:musl\nc:' + 'a'*40 + '\nC:Q1signature\nL:MIT\nF:usr/lib\nR:crt1.o\n\n')
            package = sources.installed(db)[0]
            self.assertEqual(sources.owner(Path('/usr/lib/crt1.o'), [package]), package)
            self.assertIsNone(sources.owner(Path('/usr/lib/other.a'), [package]))
            archive = root / 'source.tar.gz'
            archive.write_bytes(b'fixture source')
            record = {k: package[k] for k in ('P','V','A','o','c','C')}
            record.update(file=archive.name, sha256=sources.sha(archive))
            def save():
                (root / 'manifest.json').write_text(json.dumps({'musl-dev-1.2.5-r11': record}))
            save()
            self.assertEqual(sources.checked_source(root, package)[0], archive)
            for field, value in [('file', '../source.tar.gz'), ('sha256', '0'*64), ('c', 'b'*40)]:
                original = record[field]
                record[field] = value
                save()
                with self.assertRaises(RuntimeError): sources.checked_source(root, package)
                record[field] = original
            save()
            archive.unlink()
            with self.assertRaises(RuntimeError): sources.checked_source(root, package)

    def test_key_policy_and_matching(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with patch.dict(os.environ, {}, clear=True):
                with self.assertRaises(RuntimeError): keys.materialize(root / 'missing')
            with patch.dict(os.environ, {'CI':'true', 'KASMVNC_APK_ALLOW_TEST_KEY':'1'}, clear=True):
                with self.assertRaises(RuntimeError): keys.materialize(root / 'ci')
            with patch.dict(os.environ, {'KASMVNC_APK_ALLOW_TEST_KEY':'1'}, clear=True):
                keys.materialize(root / 'local')
                keys.materialize(root / 'other')
            private = root / 'local/kasmvnc_signing_key.rsa'
            self.assertEqual(private.stat().st_mode & 0o777, 0o600)
            with patch.dict(os.environ, {'KASMVNC_ALPINE_PRIVATE_KEY':private.read_text(),
                    'KASMVNC_ALPINE_PUBLIC_KEY':(root / 'other/kasmvnc_signing_key.rsa.pub').read_text()}, clear=True):
                with self.assertRaises(RuntimeError): keys.materialize(root / 'mismatch')

    def test_audit_rejects_unqualified_baseline(self):
        for version in ('1.2.4', '1.2.6', 'unknown'):
            result = subprocess.CompletedProcess([], 1, '', 'musl libc\nVersion ' + version + '\n')
            with patch.object(audit.subprocess, 'run', return_value=result):
                with self.assertRaisesRegex(RuntimeError, 'requires musl 1.2.5'):
                    audit.require_baseline('/lib/ld-musl-test.so.1')
        with patch.object(audit.subprocess, 'run', side_effect=FileNotFoundError):
            with self.assertRaisesRegex(RuntimeError, 'unavailable'):
                audit.require_baseline('/missing')

    def test_arm_gcc_compatibility_alias_boundary(self):
        needed = ['libc.musl-aarch64.so.1', 'libgcc_s.so.1']
        aliases = audit.audit_compiler_compatibility_aliases(
            ARM_GCC_ALIASES, 'arm64', needed, ARM_GCC_VERSION_NEEDS)
        self.assertEqual(aliases, [
            {'symbol': symbol, 'version': 'GLIBC_2.0', 'binding': 'WEAK', 'provider': 'libgcc_s.so.1'}
            for symbol in ('__register_frame_info', '__deregister_frame_info')])
        self.assertEqual(audit.audit_compiler_compatibility_aliases(
            '1: 0000000000000000 0 FUNC GLOBAL DEFAULT UND malloc\n', 'amd64', [], ''), [])
        # Every line must satisfy the boundary: a valid first alias must not
        # conceal a strong/defined/unknown/version-mismatched second import.
        valid = ARM_GCC_ALIASES.splitlines()[0]
        invalid = {
            'strong binding': valid.replace('WEAK', 'GLOBAL'),
            'defined symbol': valid.replace('UND', '12'),
            'default-version definition': valid.replace('@GLIBC_', '@@GLIBC_'),
            'other symbol': valid.replace('__register_frame_info', 'malloc'),
            'other frame API': valid.replace('__register_frame_info', '__register_frame_info_bases'),
            'other version': valid.replace('GLIBC_2.0', 'GLIBC_2.1'),
            'version suffix': valid.replace('GLIBC_2.0', 'GLIBC_2.0_extra'),
            'wrong version index': valid.replace('(7)', '(6)'),
            'missing version index': valid.replace(' (7)', ''),
            'other symbol type': valid.replace('FUNC', 'OBJECT'),
            'other visibility': valid.replace('DEFAULT', 'HIDDEN'),
            'nonzero value': valid.replace('0000000000000000', '0000000000000001'),
            'unparsed line': 'GLIBC_2.0',
        }
        for description, line in invalid.items():
            with self.subTest(description=description):
                with self.assertRaisesRegex(RuntimeError, 'Nonportable glibc symbol'):
                    audit.audit_compiler_compatibility_aliases(
                        valid + '\n' + line, 'arm64', needed, ARM_GCC_VERSION_NEEDS)
        for arch, providers in [('amd64', needed), ('arm64', ['libc.musl-aarch64.so.1'])]:
            with self.subTest(arch=arch, providers=providers):
                with self.assertRaisesRegex(RuntimeError, 'Nonportable glibc symbol'):
                    audit.audit_compiler_compatibility_aliases(
                        ARM_GCC_ALIASES, arch, providers, ARM_GCC_VERSION_NEEDS)
        invalid_needs = {
            'wrong provider with libgcc still needed': ARM_GCC_VERSION_NEEDS.replace('libgcc_s.so.1', 'libc.musl-aarch64.so.1'),
            'missing provider': ARM_GCC_VERSION_NEEDS.replace('File: libgcc_s.so.1', 'File:'),
            'version definition instead of need': ARM_GCC_VERSION_NEEDS.replace('Version needs section', 'Version definition section'),
            'wrong version name': ARM_GCC_VERSION_NEEDS.replace('Name: GLIBC_2.0', 'Name: GCC_3.0'),
            'unknown version index': ARM_GCC_VERSION_NEEDS.replace('Version: 7', 'Version: 8'),
            'duplicate version index': ARM_GCC_VERSION_NEEDS + '  0x0030: Name: GCC_3.0 Flags: none Version: 7\n',
            'absent version needs': '',
        }
        for description, version_info in invalid_needs.items():
            with self.subTest(description=description):
                with self.assertRaisesRegex(RuntimeError, 'Nonportable glibc symbol'):
                    audit.audit_compiler_compatibility_aliases(ARM_GCC_ALIASES, 'arm64', needed, version_info)

    def test_arm_aliases_in_complete_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ['one', 'two', 'three', 'four', 'five']:
                (root / name).write_bytes(b'\x7fELF')
            reports = {
                '-hW': '  Type: DYN (Position-Independent Executable file)\n  Machine: AArch64\n',
                '-lW': '[Requesting program interpreter: /lib/ld-musl-aarch64.so.1]\n',
                '-dW': '(NEEDED) Shared library: [libc.musl-aarch64.so.1]\n'
                       '(NEEDED) Shared library: [libgcc_s.so.1]\n',
                '-sDW': ARM_GCC_ALIASES,
                '-VW': ARM_GCC_VERSION_NEEDS,
            }
            with patch.object(audit, 'require_baseline'), patch.object(
                    audit, 'read_sdk', return_value={}), patch.object(
                    audit, 'sdk_exports', return_value={}), patch.object(
                    audit, 'audit_openssl', return_value={}), patch.object(
                    audit, 'audit_host_libraries', return_value={}), patch.object(
                    audit.subprocess, 'check_output', side_effect=lambda command, **kw: reports[command[1]]):
                records = audit.audit(root, 'arm64')
                self.assertEqual(len(records), 5)
                for record in records:
                    self.assertEqual(len(record['compiler_compatibility_aliases']), 2)
                with self.assertRaisesRegex(RuntimeError, 'Expected native PIE'):
                    audit.audit(root, 'amd64')
                reports['-sDW'] += '1: 0000000000000000 0 FUNC GLOBAL DEFAULT UND malloc@GLIBC_2.17 (8)\n'
                with self.assertRaisesRegex(RuntimeError, 'Nonportable glibc symbol'):
                    audit.audit(root, 'arm64')

    def test_real_musl_elf_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'probe.c'
            source.write_text('int main(void){return 0;}\n')
            for name in ['one','two','three','four','five']:
                subprocess.run(['gcc','-g','-fPIE','-pie',str(source),'-o',str(root/name)],check=True)
            arch = {'x86_64':'amd64', 'aarch64':'arm64'}[platform.machine()]
            wrong = 'arm64' if arch == 'amd64' else 'amd64'
            # This test isolates the musl/architecture boundary. The shared
            # test-openssl-boundary.py compiles actual OpenSSL-shaped DSOs and
            # exercises SDK integrity, ABI floors and all five executables.
            with patch.object(audit, 'read_sdk', return_value={}), patch.object(
                    audit, 'sdk_exports', return_value={'libssl.so.3': set(), 'libcrypto.so.3': set()}):
                records = audit.audit(root, arch)
                self.assertEqual(len(records),5)
                if arch == 'amd64':
                    self.assertTrue(all(not record['compiler_compatibility_aliases'] for record in records))
                with self.assertRaises(RuntimeError): audit.audit(root, wrong)
                subprocess.run(['gcc','-static',str(source),'-o',str(root/'one')],check=True)
                with self.assertRaises(RuntimeError): audit.audit(root, arch)

if __name__ == '__main__':
    unittest.main()
