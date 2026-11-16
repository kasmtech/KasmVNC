"""Exercise the parent CI handoff through the real functional preflight."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
REVISION = 'abcdef' + '1'*34
PUBLIC_KEY = b'-----BEGIN PUBLIC KEY-----\nreviewed-fixture\n-----END PUBLIC KEY-----\n'


class StandardCandidate(unittest.TestCase):
    def fixture(self, root):
        (root/'kasmvnc-functional-tests').mkdir()
        files = {}
        for family, formats in [('portable', ['deb', 'rpm']), ('portable_musl', ['apk', 'rsa.pub'])]:
            directory = root/'output'/f'{family}_amd64'
            directory.mkdir(parents=True)
            stem = f'kasmvncserver_{family}_1.5.3_feature_test_abcdef_adapters_amd64'
            contents = {stem+'.'+fmt: PUBLIC_KEY if fmt == 'rsa.pub' else ('fixture '+fmt).encode()
                        for fmt in formats}
            contents[stem+'.provenance.json'] = json.dumps({
                'revision': REVISION, 'version': '1.5.3', 'architecture': 'amd64',
                'ffmpeg_mode': 'adapters'}).encode()
            for name, data in contents.items():
                (directory/name).write_bytes(data)
                files[name] = directory/name
            (directory/(stem+'.sha256')).write_text(''.join(
                hashlib.sha256(data).hexdigest()+'  '+name+'\n' for name, data in contents.items()))
        return files

    def prepare(self, root):
        script = f'''source "{ROOT}/.ci/helpers.sh"
detect_release_version() {{ RELEASE_VERSION=1.5.3; }}
prepare_standard_functional_tests
printf '%s\\n' "$KASMVNC_CANDIDATE_DIR" > selected-candidate.txt
printf '%s\\n' "$KASMVNC_CANDIDATE_SHA256" > selected-candidate.sha256
touch host-launched
'''
        return subprocess.run(['bash', '-eu', '-c', script], cwd=root, text=True,
            env={**os.environ, 'CI': '1', 'CI_COMMIT_SHA': REVISION,
                 'CI_COMMIT_REF_NAME': 'feature/test',
                 'PYTHONPATH': str(ROOT/'kasmvnc-functional-tests'),
                 'KASMVNC_CANDIDATE_DIR': '/stale/candidate',
                 'KASMVNC_ALPINE_KEY_SHA256': hashlib.sha256(PUBLIC_KEY).hexdigest()},
            capture_output=True)

    def test_all_three_same_pipeline_packages_are_frozen_before_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); files = self.fixture(root)
            result = self.prepare(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            functional = root/'kasmvnc-functional-tests'
            candidate = functional/'candidate'
            self.assertTrue((functional/'host-launched').is_file())
            self.assertEqual((functional/'selected-candidate.txt').read_text().strip(), str(candidate))
            self.assertEqual((functional/'selected-candidate.sha256').read_text().strip(),
                             hashlib.sha256((candidate/'candidate.json').read_bytes()).hexdigest())
            self.assertEqual((functional/'report/candidate.json').read_bytes(), (candidate/'candidate.json').read_bytes())
            descriptor = json.loads((candidate/'candidate.json').read_text())
            self.assertEqual(descriptor['commit'], REVISION)
            self.assertEqual({entry['name'].rsplit('.', 1)[1] for entry in descriptor['artifacts']}, {'deb', 'rpm', 'apk'})
            for entry in descriptor['artifacts'] + [descriptor['signing_key']]:
                self.assertEqual((candidate/'artifacts'/entry['name']).read_bytes(), files[entry['name']].read_bytes())

    def test_invalid_pipeline_inputs_fail_before_host_launch(self):
        for corruption in ['missing-musl', 'modified-apk', 'wrong-full-revision']:
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); files = self.fixture(root)
                if corruption == 'missing-musl':
                    directory = root/'output/portable_musl_amd64'
                    for path in directory.iterdir(): path.unlink()
                    directory.rmdir()
                elif corruption == 'modified-apk':
                    next(path for name, path in files.items() if name.endswith('.apk')).write_bytes(b'corrupt')
                else:
                    # Keep the short filename prefix unchanged: full provenance must bind identity.
                    provenance = next(path for name, path in files.items()
                                      if name.endswith('.provenance.json') and 'portable_musl' not in name)
                    prior = provenance.read_bytes()
                    data = json.loads(prior); data['revision'] = REVISION[:6] + '0'*34
                    provenance.write_text(json.dumps(data))
                    manifest = provenance.with_name(provenance.name.replace('.provenance.json', '.sha256'))
                    manifest.write_text(manifest.read_text().replace(hashlib.sha256(prior).hexdigest(),
                                        hashlib.sha256(provenance.read_bytes()).hexdigest()))
                result = self.prepare(root)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                functional = root/'kasmvnc-functional-tests'
                self.assertFalse((functional/'host-launched').exists())
                self.assertFalse((functional/'report/candidate.json').exists())

    def test_standard_job_consumes_both_producer_artifacts_and_frozen_selection(self):
        job = yaml.safe_load((ROOT/'.ci/build-pipeline.yml').read_text())['functional_test_standard']
        self.assertEqual({item['job'] for item in job['needs']},
                         {'build_portable_amd64', 'build_portable_musl_amd64'})
        self.assertTrue(all(item.get('artifacts') is True for item in job['needs']))
        launch = next(command for command in job['script'] if 'virtualization.matrix' in command)
        self.assertNotIn('--package-deb', launch)
        self.assertNotIn('--package-rpm', launch)
        self.assertLess(job['script'].index('prepare_standard_functional_tests'), job['script'].index(launch))
