"""Exercise freshness against real local Git remotes, including stale gitlinks."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'freshness', Path(__file__).resolve().parents[1] / 'check-submodule-freshness.py')
freshness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(freshness)


def git(*args):
    return subprocess.check_output(['git', *args], text=True, stderr=subprocess.DEVNULL).strip()


class FreshnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        previous = os.getcwd()
        self.addCleanup(os.chdir, previous)
        os.chdir(self.temp.name)
        self.remote = str(Path('remote').resolve())
        git('init', '-b', 'main', self.remote)
        git('-C', self.remote, '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
            'commit', '--allow-empty', '-m', 'initial')
        self.initial = git('-C', self.remote, 'rev-parse', 'HEAD')
        git('init', '-b', 'master', 'parent')
        os.chdir('parent')
        for path in freshness.SUBMODULES:
            git('config', '-f', '.gitmodules', f'submodule.{path}.path', path)
            git('config', '-f', '.gitmodules', f'submodule.{path}.url', self.remote)
            git('update-index', '--add', '--cacheinfo', f'160000,{self.initial},{path}')
        git('add', '.gitmodules')
        git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-m', 'pins')

    def check(self, path='kasmvnc-unit-tests', target='master'):
        with contextlib.redirect_stdout(io.StringIO()):
            return freshness.check_submodule(path, target)

    def test_default_branch_is_discovered_and_checkout_is_not_required(self):
        self.assertTrue(self.check())
        self.assertFalse(Path('kasmvnc-unit-tests').exists())

    def test_stale_pin_fails_without_modifying_parent(self):
        git('-C', self.remote, '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
            'commit', '--allow-empty', '-m', 'advance')
        before = git('status', '--porcelain')
        self.assertFalse(self.check())
        self.assertEqual(git('status', '--porcelain'), before)

    def test_reads_committed_pin_and_config_not_index(self):
        git('update-index', '--force-remove', 'kasmvnc-unit-tests')
        Path('.gitmodules').write_text('invalid working tree config')
        self.assertTrue(self.check())

    def test_test_repository_requires_exact_release_name(self):
        git('-C', self.remote, 'branch', 'release/1.2')
        self.assertTrue(self.check(target='release/1.2'))
        with self.assertRaisesRegex(freshness.CheckError, 'Missing remote branch'):
            self.check(target='release/1.3')

    def test_novnc_accepts_different_release_version_tip(self):
        git('-C', self.remote, 'branch', 'release/9.0')
        self.assertTrue(self.check('kasmweb', 'release/1.2'))

    def test_novnc_does_not_accept_default_or_feature_branch(self):
        git('-C', self.remote, 'branch', 'feature/release/1.2')
        with self.assertRaisesRegex(freshness.CheckError, 'No remote release'):
            self.check('kasmweb', 'release/1.2')

    def test_novnc_release_requires_tip_not_ancestor(self):
        git('-C', self.remote, '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
            'commit', '--allow-empty', '-m', 'advance')
        git('-C', self.remote, 'branch', 'release/9.0')
        self.assertFalse(self.check('kasmweb', 'release/1.2'))

    def test_lookup_failure_is_not_stale_result(self):
        with patch.object(freshness, 'remote_refs', side_effect=freshness.CheckError('unavailable')):
            with self.assertRaisesRegex(freshness.CheckError, 'unavailable'):
                self.check()


class TargetTests(unittest.TestCase):
    def test_mr_target_precedes_source_branch(self):
        env = {'CI_MERGE_REQUEST_TARGET_BRANCH_NAME': 'release/1.2', 'CI_COMMIT_BRANCH': 'feature/a'}
        self.assertEqual(freshness.target_branch(env), 'release/1.2')

    def test_branch_pipeline_looks_up_target(self):
        with patch.object(freshness, 'mr_targets', return_value={'release/1.2'}):
            self.assertEqual(freshness.target_branch({'CI_COMMIT_BRANCH': 'feature/a'}), 'release/1.2')

    def test_no_mr_fallback_and_ambiguous_targets(self):
        with patch.object(freshness, 'mr_targets', return_value=set()):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(freshness.target_branch({'CI_COMMIT_BRANCH': 'feature/a'}), 'master')
                self.assertEqual(freshness.target_branch({'CI_COMMIT_BRANCH': 'release/1.2'}), 'release/1.2')
        with patch.object(freshness, 'mr_targets', return_value={'master', 'release/1.2'}):
            with self.assertRaisesRegex(freshness.CheckError, 'Multiple MR targets'):
                freshness.target_branch({'CI_COMMIT_BRANCH': 'feature/a'})

    def test_paginated_mrs_ignore_forks(self):
        first = io.BytesIO(json.dumps([
            {'source_project_id': 99, 'target_branch': 'master'}]).encode())
        first.headers = {'X-Next-Page': '2'}
        second = io.BytesIO(json.dumps([
            {'source_project_id': 42, 'target_branch': 'release/1.2'}]).encode())
        second.headers = {}
        env = {'CI_COMMIT_BRANCH': 'feature/a', 'CI_PROJECT_ID': '42',
               'CI_API_V4_URL': 'https://gitlab.example/api/v4', 'CI_JOB_TOKEN': 'test'}
        with patch.object(freshness, 'urlopen', side_effect=[first, second]) as request:
            self.assertEqual(freshness.mr_targets(env), {'release/1.2'})
            self.assertIn('page=2', request.call_args.args[0].full_url)

    def test_api_failure_cannot_silently_fall_back_to_master(self):
        with patch.object(freshness, 'mr_targets', side_effect=freshness.CheckError('API failed')):
            with self.assertRaisesRegex(freshness.CheckError, 'API failed'):
                freshness.target_branch({'CI_COMMIT_BRANCH': 'feature/a'})

    def test_ci_token_is_scoped_to_gitlab_and_never_sent_to_github(self):
        env = {'CI_SERVER_HOST': 'gitlab.example', 'CI_SERVER_URL': 'https://gitlab.example',
               'CI_JOB_TOKEN': 'test-token'}
        with patch.dict(os.environ, env, clear=True), patch.object(freshness, 'git', return_value='') as run:
            freshness.remote_refs('git@gitlab.example:group/tests.git')
            self.assertEqual(run.call_args.args[2], 'https://gitlab.example/group/tests.git')
            self.assertEqual(run.call_args.kwargs['env']['GIT_CONFIG_KEY_0'],
                             'http.https://gitlab.example/.extraHeader')
            freshness.remote_refs('https://github.com/kasmtech/noVNC.git')
            self.assertNotIn('GIT_CONFIG_COUNT', run.call_args.kwargs['env'])


if __name__ == '__main__':
    unittest.main()
