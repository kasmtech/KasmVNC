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


class PipelineTests(unittest.TestCase):
    def test_optional_jobs_use_same_pipeline_portable_artifacts(self):
        import yaml
        pipeline = yaml.safe_load((image.ROOT / '.ci/build-pipeline.yml').read_text())
        template = pipeline['.workspace_image']
        self.assertEqual(template['rules'], [{'when': 'manual'}])
        self.assertTrue(template['allow_failure'])
        self.assertIn(template['stage'], pipeline['stages'])
        for arch, runner in [('amd64', 'oci-fixed-amd'), ('arm64', 'oci-fixed-arm')]:
            job = pipeline['build_workspace_noble_' + arch]
            self.assertEqual(job['extends'], '.workspace_image')
            self.assertEqual(job['variables']['WORKSPACE_ARCH'], arch)
            self.assertEqual(job['tags'], [runner])
            self.assertEqual(job['needs'], [{'job': 'build_portable_' + arch, 'artifacts': True}])
        script = pipeline['.build_portable']['script']
        self.assertTrue(script[-1].startswith('.ci/record-workspace-package output/portable_'))


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

    def test_portable_debug_symbols_are_not_runtime_candidates(self):
        package = self.package()
        (self.root / 'kasmvncserver_portable_1.0_amd64.dbgsym.deb').write_bytes(package.read_bytes())
        recorder = Path(__file__).resolve().parents[1] / 'record-workspace-package'
        env = dict(os.environ, CI_PIPELINE_ID='123', CI_COMMIT_SHA='commit', CI_JOB_ID='456')
        subprocess.run([str(recorder), str(self.root)], env=env, check=True)
        self.assertEqual(image.select_package(self.root, 'amd64', '123', 'commit')[0], package)

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
        directory = self.root / 'output/portable_amd64'
        directory.mkdir(parents=True)
        (directory / package.name).write_bytes(package.read_bytes())
        recorder = Path(__file__).resolve().parents[1] / 'record-workspace-package'
        env = dict(os.environ, DISTRO_SLUG='ubuntu_noble', CI_PIPELINE_ID='123',
                   CI_COMMIT_SHA='commit', CI_JOB_ID='456')
        subprocess.run(['bash', str(recorder), str(directory)], cwd=self.root, env=env, check=True)
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


class CoreBuildTests(unittest.TestCase):
    def test_passes_selected_core_release_to_docker(self):
        with tempfile.TemporaryDirectory() as tmp:
            core = Path(tmp)
            (core / '.gitlab-ci.yml').write_text('variables: {KASM_RELEASE: "1.20.0"}')
            with patch.object(image, 'run') as run:
                self.assertEqual(image.build_core(core, 'core:test', 'amd64', {}), '1.20.0')
                self.assertIn('KASM_RELEASE=1.20.0', run.call_args.args)

    def test_missing_or_unresolved_release_fails_before_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            core = Path(tmp)
            for value in ('null', '""', '"$RELEASE"'):
                with self.subTest(value=value), patch.object(image, 'run') as run:
                    (core / '.gitlab-ci.yml').write_text('variables: {KASM_RELEASE: ' + value + '}')
                    with self.assertRaisesRegex(ValueError, 'KASM_RELEASE'):
                        image.build_core(core, 'core:test', 'amd64', {})
                    run.assert_not_called()


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



class ReviewRegressionTests(unittest.TestCase):
    def test_registry_overrides_rejected(self):
        valid = dict(CI_REGISTRY='registry.gitlab.com', CI_REGISTRY_USER='user',
                     CI_REGISTRY_PASSWORD='secret',
                     CI_REGISTRY_IMAGE='registry.gitlab.com/kasm-technologies/internal/kasmvnc')
        for key, value in [('CI_REGISTRY', 'attacker.invalid'),
                           ('CI_REGISTRY_IMAGE', 'attacker.invalid/image'),
                           ('CI_REGISTRY_PASSWORD', '')]:
            with self.subTest(key=key), patch.dict(os.environ, dict(valid, **{key: value}), clear=True):
                with self.assertRaisesRegex(ValueError, key):
                    image.registry_settings()

    def test_missing_and_wildcard_refs_rejected(self):
        with patch.dict(os.environ, CI_JOB_TOKEN='test'), patch.object(image, 'repository_git', return_value='') as git:
            with self.assertRaisesRegex(ValueError, 'does not exist'):
                image.preflight_access('workspaces-images', 'missing')
            git.assert_called_once()
            for ref in ['*', 'develop?', '[abc]', '--upload-pack=bad']:
                with self.assertRaisesRegex(ValueError, 'Invalid'):
                    image.preflight_access('workspaces-images', ref)
            self.assertEqual(git.call_count, 1)

    def test_git_error_redacts_token(self):
        with patch.dict(os.environ, CI_JOB_TOKEN='secret'):
            error = subprocess.CalledProcessError(1, 'git', stderr='remote: secret refused')
            self.assertEqual(image.git_error(error), 'remote: [redacted] refused')

    def test_gitlab_yaml_and_invalid_release_shapes(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(image, 'run'):
            root = Path(tmp)
            config = root / '.gitlab-ci.yml'
            config.write_text('variables:\n  KASM_RELEASE: "1.20.0"\njob:\n  script: !reference [.base, script]\n')
            self.assertEqual(image.build_core(root, 'core', 'amd64', {}), '1.20.0')
            for content in ['[]', 'null', 'variables: []', 'variables: {KASM_RELEASE: 1.20}',
                            'variables: {KASM_RELEASE: {value: "1.20.0"}}', '[broken']:
                config.write_text(content)
                with self.subTest(content=content), self.assertRaises(ValueError):
                    image.build_core(root, 'core', 'amd64', {})

    def test_alias_failure_preserves_successful_immutable_publication(self):
        report = {'published': False}
        calls = []
        def run(*args, **kwargs):
            calls.append(args)
            return '{}'
        with patch.object(image, 'run', side_effect=run), \
                patch.object(image.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
            with self.assertRaisesRegex(ValueError, 'labels'):
                image.publish('immutable', 'alias', {}, report)
        self.assertEqual(calls[0], ('docker', 'push', 'immutable'))
        self.assertTrue(report['published'])
        self.assertEqual(report['publish_state'], 'passed')

    def test_failed_immutable_push_is_not_reported_published(self):
        report = {'published': False}
        with patch.object(image, 'run', side_effect=subprocess.CalledProcessError(1, 'docker')):
            with self.assertRaises(subprocess.CalledProcessError):
                image.publish('immutable', 'alias', {}, report)
        self.assertFalse(report['published'])

    def test_annotated_tag_exports_commit_sha(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / 'repo'
            subprocess.run(['git', 'init', '-q', str(repo)], check=True)
            (repo / 'file').write_text('test')
            subprocess.run(['git', '-C', str(repo), 'add', 'file'], check=True)
            subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Test', '-c',
                            'user.email=test@example.invalid', '-c', 'commit.gpgsign=false',
                            'commit', '-qm', 'fixture'], check=True)
            subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Test', '-c',
                            'user.email=test@example.invalid', '-c', 'tag.gpgsign=false',
                            'tag', '-a', 'release', '-m', 'release'], check=True)
            expected = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            tag = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'release'], text=True).strip()
            self.assertNotEqual(tag, expected)
            def fetch(*args):
                args = list(args)
                args[-2] = str(repo)
                return image.run('git', *args, capture=True)
            with patch.object(image, 'repository_git', side_effect=fetch):
                self.assertEqual(image.export_source('unused', 'release', root / 'export'), expected)

    def test_smoke_requires_running_desktop_and_always_removes_container(self):
        for desktop_result in [0, 1]:
            with self.subTest(desktop_result=desktop_result), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                binary = root / 'package/usr/bin/Xkasmvnc'
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b'fixture')
                checksum = hashlib.sha256(b'fixture').hexdigest()
                calls = []
                def run(*args, **kwargs):
                    if 'sha256sum' in args:
                        return checksum + ' /usr/bin/Xkasmvnc'
                    if 'inspect' in args:
                        return 'false'
                def subprocess_run(args, **kwargs):
                    calls.append(args)
                    status = desktop_result if 'bash' in args else 0
                    return subprocess.CompletedProcess(args, status, 'log', '')
                with patch.object(image, 'verify_entrypoint'), patch.object(image, 'run', side_effect=run), \
                        patch.object(image.subprocess, 'run', side_effect=subprocess_run):
                    if desktop_result:
                        with self.assertRaisesRegex(ValueError, 'smoke check failed'):
                            image.smoke_test('image', 'package', root, 'core', 'smoke', root / 'missing-report')
                    else:
                        image.smoke_test('image', 'package', root, 'core', 'smoke', root)
                self.assertTrue(any('xfce4-session' in ' '.join(c) for c in calls))
                self.assertEqual(calls[-1], ['docker', 'rm', '-f', 'smoke'])


    def test_stage_failure_and_interrupt_reports_and_cleanup(self):
        env = dict(CI_PIPELINE_ID='123', CI_COMMIT_SHA='commit', CI_JOB_ID='456',
                   CI_COMMIT_REF_SLUG='branch', CI_COMMIT_REF_NAME='branch',
                   CI_REGISTRY='registry.gitlab.com', CI_REGISTRY_USER='user',
                   CI_REGISTRY_PASSWORD='secret',
                   CI_REGISTRY_IMAGE='registry.gitlab.com/kasm-technologies/internal/kasmvnc')
        for failure, state in [(ValueError('build failed'), 'failed'), (SystemExit(143), 'interrupted')]:
            with self.subTest(state=state), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                def run(*args, **kwargs):
                    return 'x86_64' if args[:2] == ('docker', 'info') else None
                with patch.dict(os.environ, env, clear=True), patch('sys.argv', ['helper', '--arch', 'amd64']), \
                        patch.object(image, 'ROOT', root), patch.object(image, 'select_package', return_value=('pkg', 'hash', '1')), \
                        patch.object(image, 'preflight_access'), patch.object(image, 'export_source', return_value='sha'), \
                        patch.object(image, 'prepare_core'), patch.object(image, 'build_core', side_effect=failure), \
                        patch.object(image, 'run', side_effect=run), patch.object(image.subprocess, 'run') as cleanup:
                    with self.assertRaises(type(failure)):
                        image.main()
                report = __import__('json').loads((root / 'workspace-image-report/image.json').read_text())
                self.assertEqual(report['build'], state)
                self.assertFalse(report['published'])
                self.assertEqual(report['smoke_test'], 'pending')
                self.assertTrue((root / 'workspace-image-report/README.txt').exists())
                self.assertTrue(any(c.args[0][:2] == ['docker', 'logout'] for c in cleanup.call_args_list))
                self.assertTrue(any(c.args[0][:3] == ['docker', 'rm', '-f'] for c in cleanup.call_args_list))

    def test_missing_alias_ordering_is_rejected(self):
        with patch.object(image, 'run', return_value='{"com.kasm.workspace.branch":"branch"}'), \
                patch.object(image.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
            with self.assertRaisesRegex(ValueError, 'cannot establish ordering'):
                image.publish('immutable', 'alias', {'com.kasm.workspace.branch': 'branch'})

    def test_recorder_stops_on_metadata_command_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'kasmvncserver_invalid.deb').write_bytes(b'invalid')
            recorder = image.ROOT / '.ci/record-workspace-package'
            result = subprocess.run(['bash', str(recorder), str(root)],
                                    env=dict(os.environ, CI_PIPELINE_ID='123', CI_COMMIT_SHA='sha', CI_JOB_ID='1'),
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / 'workspace-package.txt').exists())


if __name__ == '__main__':
    unittest.main()
