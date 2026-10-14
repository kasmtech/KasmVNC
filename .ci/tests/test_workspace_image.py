"""Run in Debian: python3 -m unittest discover -s .ci/tests -v."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('workspace_image', Path(__file__).parents[1] / 'build-workspace-image.py')
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def package(self, arch='amd64'):
        src = self.root / 'pkg'
        (src / 'DEBIAN').mkdir(parents=True)
        (src / 'DEBIAN/control').write_text(
            f'Package: kasmvncserver\nVersion: 1.0\nArchitecture: {arch}\n'
            'Maintainer: Test <test@example.invalid>\nDescription: fixture\n')
        package = self.root / f'kasmvncserver_1.0_{arch}.deb'
        subprocess.run(['dpkg-deb', '--build', str(src), str(package)], check=True, stdout=subprocess.DEVNULL)
        checksum = hashlib.sha256(package.read_bytes()).hexdigest()
        (self.root / 'workspace-package.txt').write_text(f'123\ncommit\n456\n{arch}\n{checksum}\n')
        return package

    def test_native_packages(self):
        for arch in ('amd64', 'arm64'):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as tmp:
                self.root = Path(tmp)
                package = self.package(arch)
                self.assertEqual(image.select_package(self.root, arch, '123', 'commit')[0], package)

    def test_rejects_wrong_architecture(self):
        self.package()
        with self.assertRaisesRegex(ValueError, 'architecture'):
            image.select_package(self.root, 'arm64', '123', 'commit')

    def test_rejects_stale_pipeline_and_changed_package(self):
        package = self.package()
        with self.assertRaisesRegex(ValueError, 'provenance'):
            image.select_package(self.root, 'amd64', '124', 'commit')
        with self.assertRaisesRegex(ValueError, 'provenance'):
            image.select_package(self.root, 'amd64', '123', 'other-commit')
        marker = self.root / 'workspace-package.txt'
        marker.write_text(marker.read_text().replace(hashlib.sha256(package.read_bytes()).hexdigest(), 'wrong'))
        with self.assertRaisesRegex(ValueError, 'provenance'):
            image.select_package(self.root, 'amd64', '123', 'commit')

    def test_rejects_missing_and_ambiguous_package(self):
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            image.select_package(self.root, 'amd64', '123', 'commit')
        package = self.package()
        (self.root / 'kasmvncserver_2_amd64.deb').write_bytes(package.read_bytes())
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            image.select_package(self.root, 'amd64', '123', 'commit')

    def test_producer_records_checksum_and_identity(self):
        package = self.package()
        directory = self.root / 'builder/build/ubuntu_noble'
        directory.mkdir(parents=True)
        (directory / package.name).write_bytes(package.read_bytes())
        recorder = Path(__file__).resolve().parents[1] / 'record-workspace-package'
        env = dict(os.environ, DISTRO_SLUG='ubuntu_noble', CI_PIPELINE_ID='123',
                   CI_COMMIT_SHA='commit', CI_JOB_ID='456')
        subprocess.run(['bash', str(recorder)], cwd=self.root, env=env, check=True)
        self.assertEqual(image.select_package(directory, 'amd64', '123', 'commit')[2], '456')

    def test_patch_preserves_installer_and_rejects_drift(self):
        installer = self.root / 'src/ubuntu/install/kasm_vnc/install_kasm_vnc.sh'
        installer.parent.mkdir(parents=True)
        installer.write_text('before\n    download_kasmvnc_deb kasmvncserver.deb\nafter\n')
        (self.root / 'dockerfile-kasm-core').write_text(
            'COPY ./src/ubuntu/install/kasm_vnc $INST_SCRIPTS/kasm_vnc/\n'
            'RUN bash $INST_SCRIPTS/kasm_vnc/install_kasm_vnc.sh\n')
        image.prepare_core(self.root, self.package())
        self.assertIn('before\n    cp ', installer.read_text())
        self.assertTrue(installer.read_text().endswith('after\n'))
        with self.assertRaisesRegex(ValueError, 'patch anchor'):
            image.prepare_core(self.root, next(self.root.glob('*.deb')))


class PreflightTests(unittest.TestCase):
    def test_access_failure_has_remediation(self):
        with patch.dict(os.environ, {'CI_JOB_TOKEN': 'test-token'}), \
                patch.object(image, 'repository_git', side_effect=subprocess.CalledProcessError(128, 'git')):
            with self.assertRaisesRegex(ValueError, 'allowlist.*user starting'):
                image.preflight_access('workspaces-images', 'develop')

    def test_missing_job_token_fails_before_git(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(image, 'repository_git') as git:
            with self.assertRaisesRegex(ValueError, 'CI_JOB_TOKEN is required'):
                image.preflight_access('workspaces-images', 'develop')
            git.assert_not_called()

    def test_replaced_entrypoint_rejected(self):
        with patch.object(image, 'run', return_value='["/bin/sh"]'):
            with self.assertRaisesRegex(ValueError, 'Unexpected Workspaces entrypoint'):
                image.verify_entrypoint('desktop', 'core')

    def test_modified_entrypoint_contents_rejected(self):
        expected = __import__('json').dumps(['/dockerstartup/kasm_default_profile.sh',
                    '/dockerstartup/vnc_startup.sh', '/dockerstartup/kasm_startup.sh'])
        with patch.object(image, 'run', side_effect=[expected, expected, None, 'original', 'changed']):
            with self.assertRaisesRegex(ValueError, 'changed the core'):
                image.verify_entrypoint('desktop', 'core')


class PublishingTests(unittest.TestCase):
    def check_alias(self, previous, current, expected_pushes):
        labels = {'com.kasm.workspace.branch': 'feature/a', 'com.kasm.workspace.pipeline': current}
        existing = dict(labels, **{'com.kasm.workspace.pipeline': previous})
        calls = []
        def run(*args, **kwargs):
            calls.append(args)
            if 'inspect' in args:
                return '["repo@sha256:digest"]' if '.RepoDigests' in args[4] else __import__('json').dumps(existing)
        with patch.dict(os.environ, {'CI_REGISTRY': 'registry', 'CI_REGISTRY_USER': 'user', 'CI_REGISTRY_PASSWORD': 'secret'}), \
                patch.object(image.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')), \
                patch.object(image, 'run', side_effect=run):
            updated, _ = image.publish('immutable', 'alias', labels)
        self.assertEqual([c[2] for c in calls if c[:2] == ('docker', 'push')], expected_pushes)
        self.assertEqual(updated, int(previous) <= int(current))

    def test_new_pipeline_updates_alias(self):
        self.check_alias('10', '11', ['immutable', 'alias'])

    def test_old_pipeline_preserves_newer_alias(self):
        self.check_alias('11', '10', ['immutable'])

    def test_collision_fails_closed(self):
        with patch.dict(os.environ, {'CI_REGISTRY': 'registry', 'CI_REGISTRY_USER': 'user', 'CI_REGISTRY_PASSWORD': 'secret'}), \
                patch.object(image.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')), \
                patch.object(image, 'run', return_value='{"com.kasm.workspace.branch":"other"}'):
            with self.assertRaisesRegex(ValueError, 'collision'):
                image.publish('immutable', 'alias', {'com.kasm.workspace.branch': 'branch'})


if __name__ == '__main__':
    unittest.main()
