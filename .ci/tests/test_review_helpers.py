import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ReviewHelpers(unittest.TestCase):
    def shell(self, script, root):
        return subprocess.run(['bash', '-eu', '-c', script], cwd=root, text=True,
            env={**os.environ, 'CI': '1', 'CI_COMMIT_SHA': 'test', 'S3_BUCKET': 'unused'},
            capture_output=True)

    def test_legacy_package_selection_excludes_symbols(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'functional').mkdir()
            packages = root/'output/ubuntu_jammy'; packages.mkdir(parents=True)
            for name in ['kasmvncserver_1_amd64.deb', 'kasmvncserver-dbgsym_1_amd64.deb']:
                (packages/name).touch()
            result = self.shell(f'source "{ROOT}/.ci/helpers.sh"; BUILD_PORTABLE=false; select_functional_test_package; echo "$FUNCTIONAL_TEST_PACKAGE"', root/'functional')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(packages/'kasmvncserver_1_amd64.deb'))

    def test_only_runtime_artifacts_get_statuses(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); packages = root/'builder/build/portable_amd64'; packages.mkdir(parents=True)
            base = 'kasmvncserver_portable_1.5.1_feature_test_abcdef_amd64'
            suffixes = ['deb', 'rpm', 'tar.gz', 'dbgsym.deb', 'debuginfo.rpm', 'debug.tar.gz', 'sources.tar.gz', 'cdx.json', 'sha256']
            for suffix in suffixes: (packages/(base+'.'+suffix)).touch()
            (packages/(base+'.debug.json')).write_text(json.dumps([{'binary':'/usr/bin/Xkasmvnc','md5':'a'*32}]))
            result = self.shell(f'''source "{ROOT}/.ci/helpers.sh"
prepare_to_run_scripts_and_s3_uploads() {{ :; }}
upload_to_s3() {{ UPLOADED_FILE_S3_URL=unused; }}
show_file_link_in_ci() {{ echo "STATUS:$1"; }}
upload_portable_packages
''', root)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = sorted(line[7:] for line in result.stdout.splitlines() if line.startswith('STATUS:'))
            self.assertEqual(actual, sorted(base+'.'+suffix for suffix in ['deb','rpm','tar.gz']))
            self.assertEqual(len(list((root/'output/uploaded_s3_paths').iterdir())), len(suffixes)+1)

    def test_failed_certificate_generation_does_not_publish_partial_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cert = root/'private/server.pem'; cert.parent.mkdir()
            script = (ROOT/'builder/portable/initialize-tls').read_text().replace('/etc/pki/tls/private/kasmvnc.pem',str(cert)).replace('/etc/pki/tls/private',str(cert.parent))
            runner=root/'initialize'; runner.write_text(script)
            binpath=root/'bin'; binpath.mkdir()
            for name in ['getent','chgrp']:
                p=binpath/name;p.write_text('#!/bin/sh\nexit 0\n');p.chmod(0o755)
            p=binpath/'openssl';p.write_text('''#!/bin/sh
if [ "$1" = req ]; then
  while [ "$1" != -keyout ]; do shift; done
  echo certificate > "$2"
  exit "${FAIL_GENERATION:-0}"
fi
exit 0
''');p.chmod(0o755)
            env={**os.environ,'PATH':str(binpath)+':'+os.environ['PATH'],'FAIL_GENERATION':'1'}
            failed=subprocess.run(['sh',str(runner)],env=env,capture_output=True)
            self.assertNotEqual(failed.returncode,0)
            self.assertFalse(cert.exists());self.assertEqual(list(cert.parent.iterdir()),[])
            env['FAIL_GENERATION']='0'
            self.assertEqual(subprocess.run(['sh',str(runner)],env=env,capture_output=True).returncode,0)
            self.assertEqual(cert.read_text(),'certificate\n')
            self.assertEqual(cert.stat().st_mode & 0o777,0o640)
