#!/usr/bin/env python3
"""Exercise the spec-cache registry boundary without network or Docker."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Tests(unittest.TestCase):
    def resolve(self, status=0, label='key'):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('Dockerfile', 'repository-refresh.txt'):
                dest = root/'builder/spec-dependencies'/name
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT/'builder/spec-dependencies'/name, dest)
            for name in ('Pipfile','Pipfile.lock'):
                shutil.copyfile(ROOT/name, root/name)
            binpath = root/'bin'; binpath.mkdir()
            scripts = {
                'python3': '''#!/bin/sh
if [ "$2" = key ]; then echo key; exit 0; fi
if [ -f "$CASE/looked" ]; then echo 'registry/image@sha256:digest'; exit 0; fi
touch "$CASE/looked"
if [ "$STATUS" = 0 ]; then echo 'registry/image@sha256:digest'; fi
exit "$STATUS"
''',
                'docker': '''#!/bin/sh
echo "$*" >> "$CASE/calls"
if [ "$1" = login ]; then cat >/dev/null; fi
if [ "$1 $2" = 'image inspect' ]; then echo "$LABEL"; fi
'''}
            for name,text in scripts.items():
                path=binpath/name;path.write_text(text);path.chmod(0o755)
            env=dict(os.environ,CASE=tmp,STATUS=str(status),LABEL=label,
                CI_REGISTRY_IMAGE='registry/project',CI_REGISTRY='registry',
                CI_REGISTRY_USER='user',CI_REGISTRY_PASSWORD='not-a-secret',
                PATH=str(binpath)+':'+os.environ['PATH'])
            result=subprocess.run(['bash',str(ROOT/'builder/prepare-spec-image')],cwd=root,env=env,capture_output=True,text=True)
            return result,(root/'calls').read_text()

    def test_hit_is_immutable_and_does_not_build(self):
        result,calls=self.resolve();self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.strip(),'registry/image@sha256:digest')
        self.assertNotIn('build ',calls);self.assertNotIn('push ',calls)
        self.assertIn('pull registry/image@sha256:digest',calls)
    def test_only_missing_image_builds(self):
        result,calls=self.resolve(3);self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('build --pull',calls);self.assertIn('push registry/project/',calls)
    def test_registry_error_does_not_build(self):
        result,calls=self.resolve(1);self.assertNotEqual(result.returncode,0)
        self.assertNotIn('build ',calls);self.assertNotIn('push ',calls)
    def test_wrong_recipe_is_rejected(self):
        result,_=self.resolve(label='wrong');self.assertNotEqual(result.returncode,0)

if __name__=='__main__': unittest.main()
