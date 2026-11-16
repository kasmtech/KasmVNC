#!/usr/bin/env python3
"""Prevent the static desktop resolver from re-embedding portable OpenSSL."""
from importlib.machinery import SourceFileLoader
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
loader = SourceFileLoader('static_link', str(ROOT / 'builder/scripts/link-static-desktop'))
spec = importlib.util.spec_from_loader(loader.name, loader)
link = importlib.util.module_from_spec(spec)
loader.exec_module(link)


class Tests(unittest.TestCase):
    def test_all_shared_spellings_use_sdk_and_archives_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'lib').mkdir()
            for name in ('ssl', 'crypto'):
                shared = root / 'lib' / ('lib' + name + '.so.3')
                shared.touch()
                for arg in ('-l' + name, '-l:lib' + name + '.so.3',
                            '/usr/lib/lib' + name + '.so', '/usr/lib/lib' + name + '.so.3'):
                    self.assertEqual(link.openssl_shared_input(arg, root), str(shared))
                for suffix in ('.a', '.so.1.1', '.so.4'):
                    for arg in ('/usr/lib/lib' + name + suffix, '-l:lib' + name + suffix):
                        with self.assertRaises(RuntimeError):
                            link.openssl_shared_input(arg, root)
            self.assertIsNone(link.openssl_shared_input('-lwebp', root))
            self.assertIsNone(link.openssl_shared_input('/usr/lib/libcrypto.a', ''))

    def test_missing_sdk_cannot_fall_back_to_host(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):
                link.openssl_shared_input('-lcrypto', directory)

    def test_imported_crypto_link_is_shared_without_rpath(self):
        # Exercise a real linker; fake archives/touched files cannot establish
        # whether CMake silently emits RPATH or embeds the crypto implementation.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / 'sdk'
            (sdk / 'lib').mkdir(parents=True)
            (sdk / 'include/openssl').mkdir(parents=True)
            (sdk / 'include/openssl/opensslv.h').write_text(
                '#define OPENSSL_VERSION_MAJOR 3\n#define OPENSSL_VERSION_MINOR 0\n')
            (root / 'crypto.c').write_text('int sdk_crypto_probe(void) { return 42; }\n')
            for name in ('ssl', 'crypto'):
                subprocess.run(['cc', '-fPIC', '-shared', '-Wl,-soname,lib' + name + '.so.3',
                                str(root / 'crypto.c'), '-o', str(sdk / 'lib' / ('lib' + name + '.so.3'))], check=True)
            (root / 'main.c').write_text('int sdk_crypto_probe(void); int main(void) { return sdk_crypto_probe() != 42; }\n')
            (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.10)
project(openssl_boundary C)
set(KASMVNC_OPENSSL_PREFIX "{sdk}" CACHE PATH "" FORCE)
include("{ROOT}/cmake/StaticRuntime.cmake")
add_executable(probe main.c)
target_link_libraries(probe ${{KASMVNC_CRYPTO_LIBRARY}})
''')
            subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')], check=True)
            subprocess.run(['cmake', '--build', str(root / 'build')], check=True)
            dynamic = subprocess.check_output(['readelf', '-dW', str(root / 'build/probe')], text=True)
            self.assertIn('[libcrypto.so.3]', dynamic)
            self.assertNotIn('RPATH', dynamic)
            self.assertNotIn('RUNPATH', dynamic)
            symbols = subprocess.check_output(['nm', '-D', str(root / 'build/probe')], text=True)
            self.assertIn('U sdk_crypto_probe', symbols)


if __name__ == '__main__':
    unittest.main()
