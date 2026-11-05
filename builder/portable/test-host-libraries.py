#!/usr/bin/env python3
"""Exercise host-library ABI and private-zlib failure boundaries in Docker."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import host_libraries as host


class HostLibraryTests(unittest.TestCase):
    def test_package_providers_cover_every_allowed_library(self):
        for profile, kind in [('glibc', 'deb'), ('musl', 'apk')]:
            names = []
            for item in host.components(profile):
                self.assertTrue(item[kind])
                names += item['sonames']
            self.assertEqual(len(names), len(set(names)))
        self.assertIn('libcrypt1', host.package_dependencies('deb'))
        self.assertIn('libbsd', host.package_dependencies('apk'))
        self.assertNotIn('libz.so.1', host.sonames('glibc'))

    def test_compiler_floor_rejects_newer_and_private_versions(self):
        for version in ['GLIBCXX_3.4.29', 'CXXABI_1.3.13', 'GCC_3.0', 'GCC_4.5.0']:
            host.check_compiler_versions([{'symbol': 'fixture', 'version': version}])
        for version in ['GLIBCXX_3.4.30', 'CXXABI_1.3.14', 'GCC_7.0.0', 'GLIBCXX_PRIVATE']:
            with self.assertRaisesRegex(RuntimeError, 'exceeds portable ABI'):
                host.check_compiler_versions([{'symbol': 'fixture', 'version': version}])

    def test_converted_archives_rejected(self):
        for name in ['libpng16.a', 'libglib-2.0.a', 'libbsd.a', 'libintl.a',
                     'libpcre2-8.a', 'libunwind-aarch64.a', 'libstdc++.a', 'libgcc_eh.a']:
            with self.assertRaisesRegex(RuntimeError, 'embeds a host library'):
                host.reject_static_host_library(Path('/sdk') / name)
        for name in ['libz.a', 'libwebp.a', 'libyaml.a', 'libstdc++_nonshared.a', 'libgcc.a']:
            host.reject_static_host_library(Path('/sdk') / name)

    def test_static_codec_arithmetic_exception_does_not_relax_other_imports(self):
        imported = {'symbol': '__divmodti4', 'version': 'GCC_7.0.0'}
        with patch.dict(os.environ, {'KASMVNC_FFMPEG_MODE': 'static'}):
            host.check_compiler_versions([imported])
            with self.assertRaisesRegex(RuntimeError, 'exceeds portable ABI'):
                host.check_compiler_versions([dict(imported, symbol='another_symbol')])
            with self.assertRaisesRegex(RuntimeError, 'exceeds portable ABI'):
                host.check_compiler_versions([dict(imported, version='GCC_8.0.0')])
        with patch.dict(os.environ, {'KASMVNC_FFMPEG_MODE': 'adapters'}):
            with self.assertRaisesRegex(RuntimeError, 'exceeds portable ABI'):
                host.check_compiler_versions([imported])

    def test_real_elf_exposed_private_zlib_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / 'z.c'
            source.write_text('int deflateInit_(void){return 0;} int main(void){return deflateInit_();}\n')
            binary = base / 'fixture'
            subprocess.run(['cc', '-pie', '-Wl,--export-dynamic', str(source), '-o', str(binary)], check=True)
            with self.assertRaisesRegex(RuntimeError, 'Private zlib symbols exported'):
                host.audit_host_libraries(binary, [], 'glibc')
            # KasmVNC exports this configuration global; it is not a zlib API.
            source.write_text('int zlibLevel=5; __attribute__((visibility("hidden"))) int deflateInit_(void){return 0;} int main(void){return deflateInit_();}\n')
            subprocess.run(['cc', '-pie', '-Wl,--export-dynamic', str(source), '-o', str(binary)], check=True)
            host.audit_host_libraries(binary, [], 'glibc')

    def test_required_library_cannot_silently_be_embedded(self):
        with patch.object(host, 'symbol_imports', return_value=[]):
            with self.assertRaisesRegex(RuntimeError, 'Missing required host libraries'):
                host.audit_host_libraries(Path('Xkasmvnc'), ['libstdc++.so.6'], 'glibc')
            with self.assertRaisesRegex(RuntimeError, 'zlib must remain private'):
                host.audit_host_libraries(Path('fixture'), ['libz.so.1'], 'musl')

    def test_tar_rpm_capabilities_cover_catalog(self):
        script = Path(__file__).resolve().parents[1] / 'portable-tests/prerequisites.sh'
        # Execute only the declaration section, never package installation.
        declarations = script.read_text().split('if command -v apk', 1)[0]
        for mode in ('static', 'adapters'):
            text = subprocess.check_output(['bash', '-c', declarations +
                                            '\nprintf "%s\\n" "${tar_host_sonames[@]}"',
                                            'prerequisites', 'tar', 'all', mode], text=True)
            with patch.dict(os.environ, {'KASMVNC_FFMPEG_MODE': mode}):
                self.assertTrue(host.sonames('glibc') <= set(text.split()))


if __name__ == '__main__':
    unittest.main()
