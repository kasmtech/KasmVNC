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

    def test_real_musl_elf_audit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'probe.c'
            source.write_text('int main(void){return 0;}\n')
            for name in ['one','two','three','four','five']:
                subprocess.run(['gcc','-g','-fPIE','-pie',str(source),'-o',str(root/name)],check=True)
            arch = {'x86_64':'amd64', 'aarch64':'arm64'}[platform.machine()]
            wrong = 'arm64' if arch == 'amd64' else 'amd64'
            self.assertEqual(len(audit.audit(root, arch)),5)
            with self.assertRaises(RuntimeError): audit.audit(root, wrong)
            subprocess.run(['gcc','-static',str(source),'-o',str(root/'one')],check=True)
            with self.assertRaises(RuntimeError): audit.audit(root, arch)

if __name__ == '__main__':
    unittest.main()
