"""Keep the declared portable userspace coverage aligned with required CI jobs."""
import json
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class PortableMatrix(unittest.TestCase):
    def test_only_four_portable_producers_have_package_runtime_consumers(self):
        pipeline = yaml.safe_load((ROOT / '.ci/build-pipeline.yml').read_text())
        expected = {'build_portable_amd64', 'build_portable_arm64',
                    'build_portable_musl_amd64', 'build_portable_musl_arm64'}
        producers = {name for name in pipeline if name.startswith('build_') and name != 'build_www'}
        self.assertEqual(producers, expected)
        self.assertFalse(any(name.startswith('run_test_') for name in pipeline))
        for switch in ('BUILD_WORKSPACE_LEGACY', 'BUILD_FULL_LEGACY', 'BUILD_PORTABLE'):
            self.assertNotIn(switch, pipeline['variables'])
        for job in pipeline.values():
            if not isinstance(job, dict):
                continue
            for dependency in job.get('needs', []):
                name = dependency if isinstance(dependency, str) else dependency['job']
                self.assertIn(name, pipeline, 'Dangling CI dependency')

    def test_each_eligible_userspace_has_a_required_native_architecture_job(self):
        pipeline = yaml.safe_load((ROOT / '.ci/build-pipeline.yml').read_text())
        cases = json.loads((ROOT / 'builder/portable-tests/matrix.json').read_text())['cases']
        self.assertFalse(pipeline['.test_portable']['allow_failure'])
        for arch in ('amd64', 'arm64'):
            for musl in (False, True):
                with self.subTest(arch=arch, musl=musl):
                    expected = {case['name'] for case in cases
                                if not case['exclude_reason'] and arch in case['architectures']
                                and (case['format'] == 'apk') == musl}
                    job = pipeline['test_portable_' + ('musl_' if musl else '') + arch]
                    matrix = job.get('parallel', pipeline[job['extends']].get('parallel'))
                    actual = [name for row in matrix['matrix'] for name in row['PORTABLE_CASE']]
                    self.assertEqual(len(actual), len(set(actual)), 'Duplicate runtime case')
                    self.assertEqual(set(actual), expected)
                    self.assertEqual(job['variables']['PORTABLE_ARCH'], arch)
                    self.assertEqual(job['tags'], ['oci-fixed-amd' if arch == 'amd64' else 'oci-fixed-arm'])


if __name__ == '__main__':
    unittest.main()
