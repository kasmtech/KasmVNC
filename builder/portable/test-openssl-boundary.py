#!/usr/bin/env python3
"""Compile real ELF fixtures to exercise the portable OpenSSL boundary in Docker."""
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import openssl_boundary as boundary
import glibc_crt


class OpenSSLBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='openssl-boundary-test-')
        cls.base = Path(cls.tmp.name)
        cls.sdk = cls.base / 'sdk'
        cls.lib = cls.sdk / 'lib'
        cls.lib.mkdir(parents=True)
        cls.header = cls.sdk / 'include/openssl/ssl.h'
        cls.header.parent.mkdir(parents=True)
        cls.header.write_text('/* ABI fixture, not an OpenSSL implementation. */\n')
        cls.shared('crypto', 'int EVP_sha256(void){return 0;}\nint EVP_MD_CTX_dup(void){return 0;}\n',
                   'OPENSSL_3.0.0 { global: EVP_sha256; local: *; };\n'
                   'OPENSSL_3.1.0 { global: EVP_MD_CTX_dup; } OPENSSL_3.0.0;\n')
        cls.shared('ssl', 'int SSL_new(void){return 0;}\n',
                   'OPENSSL_3.0.0 { global: SSL_new; local: *; };\n')
        archive = cls.sdk / 'share/kasmvnc/sources/openssl-3.0.22.tar.gz'
        archive.parent.mkdir(parents=True)
        with tarfile.open(archive, 'w:gz') as tf:
            for library, text in [('libcrypto', 'EVP_sha256 1 3_0_0 EXIST::FUNCTION:\n'
                                  'EVP_MD_CTX_dup 2 3_1_0 EXIST::FUNCTION:\n'),
                                  ('libssl', 'SSL_new 1 3_0_0 EXIST::FUNCTION:\n')]:
                info = tarfile.TarInfo('openssl-3.0.22/util/' + library + '.num')
                info.size = len(text.encode())
                tf.addfile(info, io.BytesIO(text.encode()))
        cls.manifest = {'format': 'kasmvnc-openssl-sdk-v1', 'version': '3.0.22',
                        'source_archive': str(archive.relative_to(cls.sdk)),
                        'source_url': 'https://example.invalid/openssl-3.0.22.tar.gz',
                        'source_sha256': boundary.sha256(archive),
                        'sonames': sorted(boundary.SONAMES),
                        'symbol_version_maximum': boundary.SYMBOL_BASELINE,
                        'files': {str(p.relative_to(cls.sdk)): boundary.sha256(p)
                                  for p in [cls.header, cls.lib / 'libssl.so.3', cls.lib / 'libcrypto.so.3']}}
        cls.metadata = cls.sdk / 'share/kasmvnc/openssl-sdk.json'
        cls.metadata.write_text(json.dumps(cls.manifest))
        cls.catalog = cls.base / 'sources.json'
        cls.catalog.write_text(json.dumps({'version': cls.manifest['version'],
                                          'url': cls.manifest['source_url'],
                                          'sha256': cls.manifest['source_sha256']}))
        cls.exports = boundary.sdk_exports(cls.sdk, cls.manifest)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def shared(cls, name, source, version_script=None, soname=None, directory=None):
        directory = directory or cls.lib
        directory.mkdir(parents=True, exist_ok=True)
        code = directory / (name + '.c')
        code.write_text(source)
        soname = soname or 'lib' + name + '.so.3'
        args = ['gcc', '-fPIC', '-shared', str(code), '-Wl,-soname,' + soname,
                '-o', str(directory / soname)]
        if version_script:
            version = directory / (name + '.map')
            version.write_text(version_script)
            args.append('-Wl,--version-script=' + str(version))
        subprocess.run(args, check=True)
        return directory / soname

    def binary(self, name, functions, libraries, extra=(), owned_crt=False):
        directory = Path(tempfile.mkdtemp(dir=self.base))
        code = directory / 'probe.c'
        code.write_text(''.join('extern int ' + fn + '(void);\n' for fn in functions)
                        + 'int main(void){return ' + ('+'.join(fn + '()' for fn in functions) or '0') + ';}\n')
        binary = directory / name
        startup = []
        if owned_crt:
            flags, objects = glibc_crt.prepare(['gcc'], list(extra), binary,
                                             Path(__file__).resolve().parents[2])
            startup = flags + objects
        subprocess.run(['gcc', *startup, '-fPIE', '-pie', str(code), '-o', str(binary), *extra,
                        '-Wl,-Map,' + str(binary) + '.link-map',
                        '-Wl,--no-as-needed', *map(str, libraries)], check=True)
        return binary

    def audit(self, binary):
        dynamic = subprocess.check_output(['readelf', '-dW', str(binary)], text=True)
        needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
        return boundary.audit_openssl(binary, needed, self.exports)

    def test_baseline_tls_and_crypto_helpers(self):
        server = self.binary('Xkasmvnc', ['SSL_new', 'EVP_sha256'],
                             [self.lib / 'libssl.so.3', self.lib / 'libcrypto.so.3'])
        result = self.audit(server)
        self.assertEqual(result['sonames'], sorted(boundary.SONAMES))
        self.assertEqual({r['version'] for r in result['imports']}, {boundary.SYMBOL_BASELINE})
        helper = self.binary('kasmvncpasswd', ['EVP_sha256'], [self.lib / 'libcrypto.so.3'])
        self.assertEqual(self.audit(helper)['sonames'], ['libcrypto.so.3'])
        unused = self.binary('kasmvncconfig', [], [])
        self.assertEqual(self.audit(unused)['imports'], [])

    def test_required_dependencies_cannot_disappear(self):
        for name, functions, libraries in [('Xkasmvnc', ['EVP_sha256'], [self.lib / 'libcrypto.so.3']),
                                           ('kasmvncpasswd', [], [])]:
            with self.subTest(name=name):
                binary = self.binary(name, functions, libraries)
                with self.assertRaisesRegex(RuntimeError, 'Missing required host'):
                    self.audit(binary)

    def test_newer_symbol_version_is_rejected(self):
        binary = self.binary('kasmvncpasswd', ['EVP_MD_CTX_dup'], [self.lib / 'libcrypto.so.3'])
        with self.assertRaisesRegex(RuntimeError, 'exceeds 3.0.0 ABI'):
            self.audit(binary)

    def test_unversioned_exports_still_enforce_baseline(self):
        lib = self.shared('crypto', 'int EVP_sha256(void){return 0;}\nint EVP_MD_CTX_dup(void){return 0;}\n',
                          directory=self.base / 'unversioned')
        valid = self.binary('kasmvncpasswd', ['EVP_sha256'], [lib])
        self.assertIsNone(self.audit(valid)['imports'][0]['version'])
        newer = self.binary('kasmvncpasswd', ['EVP_MD_CTX_dup'], [lib])
        with self.assertRaisesRegex(RuntimeError, 'absent from 3.0.0 ABI'):
            self.audit(newer)

    def test_other_major_is_rejected(self):
        lib = self.shared('crypto', 'int EVP_sha256(void){return 0;}\n',
                          soname='libcrypto.so.4', directory=self.base / 'major4')
        binary = self.binary('kasmvncpasswd', ['EVP_sha256'], [lib])
        with self.assertRaisesRegex(RuntimeError, 'Unexpected OpenSSL SONAME'):
            self.audit(binary)

    def test_hidden_implementation_in_renamed_archive_is_rejected(self):
        source = self.base / 'hidden.c'
        source.write_text('int SSL_new(void){return 0;}\n')
        obj = self.base / 'hidden.o'
        archive = self.base / 'librenamed.a'
        subprocess.run(['gcc', '-fvisibility=hidden', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run(['ar', 'rcs', str(archive), str(obj)], check=True)
        binary = self.binary('Xkasmvnc', ['SSL_new', 'EVP_sha256'],
                             [self.lib / 'libssl.so.3', self.lib / 'libcrypto.so.3'], [str(archive)])
        with self.assertRaisesRegex(RuntimeError, 'Embedded OpenSSL definitions'):
            boundary.reject_embedded_symbols(binary, self.exports)

    def test_libcrypt_collisions_require_local_binding_and_linker_ownership(self):
        collisions = {'BF_set_key': 'libcrypt_la-crypt-bcrypt.o',
                      'SHA256_Transform': 'libcrypt_la-alg-sha256.o',
                      'SHA512_Transform': 'libcrypt_la-alg-sha512.o'}
        exports = {k: set(v) for k, v in self.exports.items()}
        exports['libcrypto.so.3'].update(collisions)
        for variant in ['valid', 'renamed-archive', 'wrong-member', 'global-hidden']:
            with self.subTest(variant=variant):
                directory = Path(tempfile.mkdtemp(dir=self.base))
                archive = directory / ('librenamed.a' if variant == 'renamed-archive' else 'libcrypt.a')
                functions = []
                for index, (name, member) in enumerate(collisions.items()):
                    code = directory / (name + '.c')
                    binding = '' if variant == 'global-hidden' else 'static '
                    fn = 'crypt_fixture_' + str(index)
                    functions.append(fn)
                    code.write_text(binding + '__attribute__((noinline)) int ' + name + '(void){return 1;}\n'
                                    'int ' + fn + '(void){return ' + name + '();}\n')
                    obj = directory / (('wrong-' + member) if variant == 'wrong-member' else member)
                    subprocess.run(['gcc', '-c', str(code), '-o', str(obj)], check=True)
                    subprocess.run(['ar', 'rcs', str(archive), str(obj)], check=True)
                binary = self.binary('collision-probe', functions, [],
                                     [str(archive), '-Wl,--exclude-libs,' + archive.name])
                if variant != 'valid':
                    with self.assertRaisesRegex(RuntimeError, 'Embedded OpenSSL definitions'):
                        boundary.reject_embedded_symbols(binary, exports, [archive])
                    continue
                boundary.reject_embedded_symbols(binary, exports, [archive])
                with self.assertRaisesRegex(RuntimeError, 'Embedded OpenSSL definitions'):
                    boundary.reject_embedded_symbols(binary, exports)
                link_map = Path(str(binary) + '.link-map')
                original = link_map.read_text()
                link_map.write_text(original.replace(str(archive), '/unrelated/libcrypt.a'))
                with self.assertRaisesRegex(RuntimeError, 'Embedded OpenSSL definitions'):
                    boundary.reject_embedded_symbols(binary, exports, [archive])
                link_map.write_text(original)

    def test_all_five_executables_and_search_path_gate(self):
        musl = Path('/lib/ld-musl-' + platform.machine() + '.so.1').exists()
        here = Path(__file__).resolve().parent
        script = here.parent / 'musl/audit.py' if musl else here / 'audit.py'
        spec = importlib.util.spec_from_file_location('openssl_full_audit', script)
        audit = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
        stage = Path(tempfile.mkdtemp(dir=self.base))
        for name in ['Xkasmvnc', 'kasmvncpasswd', 'kasmvncconfig', 'kasmvncserver', 'kasmxproxy']:
            functions = ['SSL_new', 'EVP_sha256'] if name == 'Xkasmvnc' else (
                ['EVP_sha256'] if name == 'kasmvncpasswd' else [])
            libraries = [self.lib / 'libssl.so.3', self.lib / 'libcrypto.so.3'] if functions else []
            shutil.copy2(self.binary(name, functions, libraries, owned_crt=not musl), stage / name)
        arch = {'x86_64': 'amd64', 'aarch64': 'arm64'}[platform.machine()]
        # These deliberately minimal ELF fixtures exercise the OpenSSL and
        # search-path gates. The independent host-library suite supplies its
        # own positive/negative fixtures; real packages exercise both gates.
        with patch.object(boundary, 'SDK_CATALOG', self.catalog), patch.dict(
                os.environ, {'KASMVNC_OPENSSL_PREFIX': str(self.sdk)}), patch.object(
                audit, 'audit_host_libraries', return_value={}) as host_audit:
            self.assertEqual(len(audit.audit(stage, arch)), 5)
            self.assertEqual(host_audit.call_count, 5)
            bad = self.binary('kasmvncconfig', [], [], ['-Wl,-rpath,/build/private/lib'])
            shutil.copy2(bad, stage / 'kasmvncconfig')
            with self.assertRaisesRegex(RuntimeError, 'search.path|library search path'):
                audit.audit(stage, arch)

    def test_no_openssl_archive_names_are_accepted(self):
        for name in ['libssl.a', 'libcrypto.a', 'libssl-private.a', 'libcrypto.hidden.a']:
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeError, 'static OpenSSL'):
                boundary.reject_static_openssl(Path(name))
        boundary.reject_static_openssl(Path('libcrypt.a'))

    def test_sdk_file_source_and_pin_integrity(self):
        with patch.object(boundary, 'SDK_CATALOG', self.catalog):
            self.assertEqual(boundary.read_sdk(self.sdk)['version'], '3.0.22')
            original = self.header.read_bytes()
            try:
                self.header.write_bytes(b'tampered')
                with self.assertRaisesRegex(RuntimeError, 'file hash mismatch'):
                    boundary.read_sdk(self.sdk)
            finally:
                self.header.write_bytes(original)
            source = self.sdk / self.manifest['source_archive']
            original = source.read_bytes()
            try:
                source.write_bytes(b'tampered')
                with self.assertRaisesRegex(RuntimeError, 'source hash mismatch'):
                    boundary.read_sdk(self.sdk)
            finally:
                source.write_bytes(original)
            changed = dict(self.manifest, source_url='https://example.invalid/other')
            self.metadata.write_text(json.dumps(changed))
            try:
                with self.assertRaisesRegex(RuntimeError, 'does not match pinned source'):
                    boundary.read_sdk(self.sdk)
            finally:
                self.metadata.write_text(json.dumps(self.manifest))
            extra = self.header.parent / 'extra.h'
            extra.write_text('unlisted')
            try:
                with self.assertRaisesRegex(RuntimeError, 'file set mismatch'):
                    boundary.read_sdk(self.sdk)
            finally:
                extra.unlink()
            with self.assertRaisesRegex(RuntimeError, 'Invalid OpenSSL SDK path'):
                boundary.sdk_file(self.sdk, '../../outside')


if __name__ == '__main__':
    unittest.main()
