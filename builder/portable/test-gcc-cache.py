#!/usr/bin/env python3
"""Independent toolchain keys and actual shell resolver failure boundaries."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('cache', ROOT/'builder/portable/dependency-cache.py')
cache = importlib.util.module_from_spec(spec); spec.loader.exec_module(cache)


class Tests(unittest.TestCase):
    def test_toolchain_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copytree(ROOT/'builder/scripts/portable-gcc-runtime', root/'gcc')
            shutil.copytree(ROOT/'builder/scripts/portable-dependencies', root/'deps')
            before = cache.context_key(root/'gcc', 'amd64')
            old_child = cache.context_key(root/'deps', 'amd64')
            with (root/'deps/build-static-runtime').open('a') as f: f.write('\n# changed libyaml\n')
            self.assertEqual(before, cache.context_key(root/'gcc', 'amd64'))
            self.assertNotEqual(old_child, cache.context_key(root/'deps', 'amd64'))
            self.assertNotEqual(before, cache.context_key(root/'gcc', 'arm64'))
            for filename in ('Dockerfile', 'build-static-gcc-runtime', 'repository-refresh.txt', 'build'):
                path = root/'gcc'/filename; original = path.read_bytes()
                path.write_bytes(original + b'\n# changed\n')
                self.assertNotEqual(before, cache.context_key(root/'gcc', 'amd64'))
                path.write_bytes(original)
            parent = root/'deps/gcc-runtime-image.txt'
            parent.write_text('registry/image@sha256:' + 'a'*64)
            child = cache.context_key(root/'deps', 'amd64')
            parent.write_text('registry/image@sha256:' + 'b'*64)
            self.assertNotEqual(child, cache.context_key(root/'deps', 'amd64'))

    def resolve(self, status, label='key', local=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'archive').write_bytes(b'context')
            # Execute the production shell function against controlled registry
            # and Docker executables, without network or privileged Docker access.
            source = (ROOT/'builder/build-portable').read_text()
            function = source[source.index('prepare_dependency_image() {'):source.index('# git archive omits')]
            scripts = {
                'python3': '''#!/bin/sh
if [ -f "$TMP_CASE/looked" ]; then echo 'registry/image@sha256:digest'; exit 0; fi
touch "$TMP_CASE/looked"
if [ "$LOOKUP_STATUS" = 0 ]; then echo 'registry/image@sha256:digest'; fi
exit "$LOOKUP_STATUS"
''',
                'docker': '''#!/bin/sh
echo "$*" >> "$TMP_CASE/calls"
if [ "$1 $2" = 'image inspect' ]; then
 case "$4" in *Labels*) echo "$LABEL";; *) echo 'sha256:local';; esac
fi
if [ "$1" = build ]; then cat >/dev/null; fi
'''}
            for name, text in scripts.items():
                path = root/name; path.write_text(text); path.chmod(0o755)
            script = '''set -euo pipefail
log="$TMP_CASE/log"
cache_repository=portable-gcc-runtime
arch=amd64
key=key
context_archive="$TMP_CASE/archive"
extra_build_args=()
run_logged() { shift; "$@"; }
'''+function+'\nprepare_dependency_image\n'
            env = dict(os.environ, TMP_CASE=str(root), LOOKUP_STATUS=str(status), LABEL=label,
                       PATH=str(root)+':'+os.environ['PATH'], CI_REGISTRY_IMAGE='' if local else 'registry/project')
            result = subprocess.run(['bash','-c',script],env=env,capture_output=True,text=True)
            calls = (root/'calls').read_text() if (root/'calls').exists() else ''
            return result, calls

    def test_hit_does_not_build(self):
        result,calls = self.resolve(0); self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn('build ',calls); self.assertNotIn('push ',calls);self.assertIn('pull registry/image@',calls)

    def test_only_missing_manifest_builds(self):
        result,calls = self.resolve(3);self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('build --pull',calls);self.assertIn('push registry/project/portable-gcc-runtime:',calls)

    def test_registry_error_never_builds(self):
        result,calls = self.resolve(1);self.assertNotEqual(result.returncode,0);self.assertNotIn('build ',calls)

    def test_wrong_label_rejected(self):
        result,_ = self.resolve(0,label='wrong');self.assertNotEqual(result.returncode,0)

    def test_local_never_publishes(self):
        result,calls = self.resolve(0,local=True);self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('build ',calls);self.assertNotIn('push ',calls);self.assertNotIn('pull ',calls)


if __name__ == '__main__': unittest.main()
