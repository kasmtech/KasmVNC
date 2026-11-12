"""Keep development pipelines independent of merge-request state."""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class PipelinePolicy(unittest.TestCase):
    def test_one_branch_workflow_without_mr_overrides(self):
        root = yaml.safe_load((ROOT / '.gitlab-ci.yml').read_text())
        self.assertEqual(root['include'], [{'local': '.ci/build-pipeline.yml'}])
        self.assertEqual(root['workflow']['rules'], [
            {'if': '$CI_COMMIT_TAG', 'when': 'never'},
            {'if': '$CI_PIPELINE_SOURCE == "merge_request_event"', 'when': 'never'},
            {'if': '$CI_COMMIT_BRANCH'},
        ])

    def test_all_native_profiles_and_required_minimal_are_unconditional(self):
        ci = yaml.safe_load((ROOT / '.ci/build-pipeline.yml').read_text())
        for name in ('.build_portable', '.test_portable', 'functional_test'):
            self.assertEqual(ci[name]['rules'], [{'when': 'on_success'}])
            self.assertFalse(ci[name]['allow_failure'])
        groups = set()
        for arch, tag in [('amd64', 'oci-fixed-amd'), ('arm64', 'oci-fixed-arm')]:
            for libc in ('', 'musl_'):
                job = ci['build_portable_' + libc + arch]
                self.assertEqual(job['tags'], [tag])
                groups.add(job['resource_group'])
        self.assertEqual(len(groups), 4)
        self.assertEqual(ci['functional_test']['needs'], ['build_portable_amd64'])

    def test_standard_requires_manual_action_and_is_nonblocking(self):
        ci = yaml.safe_load((ROOT / '.ci/build-pipeline.yml').read_text())
        self.assertEqual(ci['functional_test_standard']['rules'], [
            {'when': 'manual', 'allow_failure': True}])


if __name__ == '__main__':
    unittest.main()
