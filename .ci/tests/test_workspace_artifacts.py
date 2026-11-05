"""Artifact contract used by workspaces-core-images at f9ac90812d99.

Run in the dedicated packaging-test container (Python, PyYAML, Bash and Ruby).
Keep native names/layouts until that consumer migrates; aliases are not tested
or accepted as a substitute for native producer jobs.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
CONSUMERS = {
    'ubuntu_jammy': 'deb', 'ubuntu_noble': 'deb', 'ubuntu_resolute': 'deb',
    'debian_bookworm': 'deb', 'debian_trixie': 'deb', 'kali_kali-rolling': 'deb',
    'oracle_8': 'rpm', 'oracle_9': 'rpm', 'opensuse_16': 'rpm',
    'fedora_42': 'rpm', 'fedora_43': 'rpm',
    'alpine_321': 'apk', 'alpine_322': 'apk', 'alpine_323': 'apk',
}


class WorkspaceArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pipeline = yaml.safe_load((ROOT / '.ci/build-pipeline.yml').read_text())
        cls.rows = cls.pipeline['.parallel_matrix']['matrix']

    def selected(self, job, row, **overrides):
        env = {**self.pipeline['variables'], **row, **overrides}
        # Evaluate only the admission guards; later rules choose failure policy.
        for rule in self.pipeline[job]['rules']:
            if rule.get('when') != 'never':
                break
            if self.matches(rule['if'], env):
                return False
        return True

    def matches(self, expression, env):
        alternatives = []
        for alternative in expression.split(' || '):
            terms = []
            for term in alternative.split(' && '):
                match = re.fullmatch(r'\$(\w+) (==|!=|=~) (?:"([^"]*)"|\$(\w+)|/(.*)/)', term)
                self.assertIsNotNone(match, 'Review changed CI expression')
                key, operator, literal, other_key, pattern = match.groups()
                value = str(env.get(key, ''))
                if operator == '=~':
                    terms.append(bool(re.search(pattern.replace(r'\/', '/'), value)))
                else:
                    expected = literal if literal is not None else str(env.get(other_key, ''))
                    terms.append(value == expected if operator == '==' else value != expected)
            alternatives.append(all(terms))
        return any(alternatives)

    def failure_allowed(self, job, row, **overrides):
        env = {**self.pipeline['variables'], **row, 'CI_COMMIT_BRANCH': 'feature/test',
               'CI_DEFAULT_BRANCH': 'master', **overrides}
        for rule in self.pipeline[job]['rules']:
            if 'if' not in rule or self.matches(rule['if'], env):
                self.assertNotEqual(rule.get('when'), 'never', 'Expected a selected job')
                return rule.get('allow_failure', False)
        self.fail('Expected matching job rule')

    def test_default_has_native_producers_and_tests_for_every_consumer(self):
        self.assertEqual(self.pipeline['variables']['BUILD_PORTABLE'], 'true')
        self.assertEqual(self.pipeline['variables'].get('BUILD_WORKSPACE_LEGACY'), 'true')
        by_slug = {row['DISTRO_SLUG']: row for row in self.rows}
        self.assertTrue(CONSUMERS.keys() <= by_slug.keys())
        self.assertNotIn('ubuntu_focal', by_slug)
        for slug, fmt in CONSUMERS.items():
            with self.subTest(slug=slug):
                self.assertTrue((ROOT / f'builder/dockerfile.{slug}.build').is_file())
                self.assertTrue((ROOT / f'builder/dockerfile.{slug}.{fmt}.build').is_file())
                for job in ['build_amd64', 'build_arm64', 'run_test_amd64', 'run_test_arm64']:
                    self.assertTrue(self.selected(job, by_slug[slug]))

    def test_opt_out_full_legacy_and_no_build_modes(self):
        for job in ['build_amd64', 'build_arm64', 'run_test_amd64', 'run_test_arm64']:
            for row in self.rows:
                with self.subTest(job=job, distro=row['DISTRO_SLUG']):
                    self.assertEqual(self.selected(job, row, BUILD_WORKSPACE_LEGACY='false'),
                                     row['DISTRO_SLUG'] in {'oracle_8', 'opensuse_15'})
                    self.assertTrue(self.selected(job, row, BUILD_WORKSPACE_LEGACY='false', BUILD_FULL_LEGACY='true'))
                    self.assertTrue(self.selected(job, row, BUILD_WORKSPACE_LEGACY='false', BUILD_PORTABLE='false'))
                    self.assertFalse(self.selected(job, row, BUILD_DISTROS_REGEX='none'))

    def test_compatibility_jobs_cannot_hide_failures(self):
        for row in self.rows:
            for job in ['build_amd64', 'build_arm64', 'run_test_amd64', 'run_test_arm64']:
                with self.subTest(job=job, distro=row['DISTRO_SLUG']):
                    self.assertFalse(self.failure_allowed(job, row, ALLOW_RUN_TESTS_TO_FAIL='true'))
                    # Explicit opt-out retains the prior optional feature-build
                    # and runtime-test override behavior in full legacy mode.
                    self.assertTrue(self.failure_allowed(job, row, BUILD_WORKSPACE_LEGACY='false',
                        BUILD_FULL_LEGACY='true', ALLOW_RUN_TESTS_TO_FAIL='true'))

    def test_shell_filter_skips_excluded_rows_but_preserves_selected_failures(self):
        for job in ['build_amd64', 'build_arm64', 'run_test_amd64', 'run_test_arm64']:
            for regex, code, expected in [('jammy', 17, 0), ('noble', 17, 17), ('noble', 0, 0)]:
                with self.subTest(job=job, regex=regex, code=code), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    for name in ['builder/build-package', 'builder/test-barebones', '.ci/record-workspace-package']:
                        path = root/name; path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text('#!/bin/bash\nprintf called >> "$CALLS"\nexit "$FIXTURE_STATUS"\n')
                        path.chmod(0o755)
                    script = '\n'.join(self.pipeline[job]['script'])
                    result = subprocess.run(['bash', '-e', '-c',
                        f'source "{ROOT}/.ci/helpers.sh"\nupload_packages() {{ :; }}\n' + script],
                        cwd=root, text=True, capture_output=True,
                        env={**os.environ, 'CI': '1', 'CI_COMMIT_SHA': 'fixture', 'S3_BUCKET': 'unused',
                             'DISTRO': 'ubuntu noble', 'DISTRO_SLUG': 'ubuntu_noble',
                             'BUILD_DISTROS_REGEX': regex, 'CALLS': str(root/'calls'), 'FIXTURE_STATUS': str(code)})
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assertEqual((root/'calls').exists(), regex == 'noble')

    def test_artifact_collection_uses_only_current_native_distro(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for slug in ['ubuntu_noble', 'ubuntu_jammy']:
                directory = root/'builder/build'/slug; directory.mkdir(parents=True)
                (directory/'package.deb').write_text(slug)
            script = '\n'.join(self.pipeline['.prepare_artfacts'])
            for slug in ['alpine_321', 'ubuntu_noble']:
                result = subprocess.run(['bash', '-euc', script], cwd=root, text=True, capture_output=True,
                                        env={**os.environ, 'DISTRO_SLUG': slug})
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted(p.name for p in (root/'output').iterdir()), ['ubuntu_noble'])
            self.assertEqual((root/'output/ubuntu_noble/package.deb').read_text(), 'ubuntu_noble')

    def test_real_publisher_matches_downstream_native_names(self):
        for slug, fmt in CONSUMERS.items():
            for deb_arch, native_arch in [('amd64', 'x86_64'), ('arm64', 'aarch64')]:
                arch = deb_arch if fmt == 'deb' else native_arch
                package = {'deb': f'kasmvncserver_1.5.3-1_{arch}.deb',
                           'rpm': f'kasmvncserver-1.5.3-1.{arch}.rpm',
                           'apk': f'kasmvncserver-1.5.3-r0_{arch}.apk'}[fmt]
                for branch, version in [('feature/test', '1.5.3_feature_test_abcdef'),
                                        ('release/1.5.3', '1.5.3')]:
                    with self.subTest(slug=slug, arch=arch, branch=branch):
                        result = subprocess.run(['bash', '-eu', '-c', '''
source .ci/helpers.sh
detect_release_version() { RELEASE_VERSION=1.5.3; }
prepare_package_s3_name "$1"
printf '%s\n' "$package_s3_name"
''', '--', f'builder/build/{slug}/{package}'], cwd=ROOT,
                            env={**os.environ, 'CI': '1', 'S3_BUCKET': 'unused', 'RELEASE_BRANCH': '',
                                 'CI_COMMIT_SHA': 'abcdef1234567890', 'CI_COMMIT_REF_NAME': branch},
                            text=True, capture_output=True)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), f'kasmvncserver_{slug}_{version}_{arch}.{fmt}')


if __name__ == '__main__':
    unittest.main()
