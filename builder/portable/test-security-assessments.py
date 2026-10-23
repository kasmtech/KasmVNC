#!/usr/bin/env python3
"""Assessment lifecycle and cross-report contracts; run in a container."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


engine = load('security-assessments')
adapter = load('gitlab-dependency-report')
NOW = '2026-10-23T18:40:00Z'


class Assessments(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); (self.root / 'recipe').write_text('feature disabled')
        self.rule = {'cve': 'CVE-2026-1234', 'package': 'example', 'versions': ['1.0'],
            'architectures': ['amd64', 'arm64'], 'libcs': ['glibc', 'musl'],
            'state': 'not_affected', 'justification': 'code_not_present',
            'detail': 'Vulnerable feature disabled.', 'references': ['https://example.org/fix'],
            'reviewed_at': NOW, 'author': 'reviewer', 'evidence': {'recipe': engine.digest(self.root / 'recipe')}}
        self.registry = {'schema_version': 1, 'assessments': [self.rule]}
        self.bom = {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
            'metadata': {'component': {'type': 'application', 'name': 'KasmVNC', 'version': '1.0',
                'properties': [{'name': 'kasmvnc:architecture', 'value': 'amd64'},
                    {'name': 'kasmvnc:libc', 'value': 'glibc'}, {'name': 'kasmvnc:revision', 'value': 'abc123'}]}},
            'components': [{'type': 'library', 'name': 'example', 'version': '1.0',
                            'bom-ref': 'example@1.0', 'purl': 'pkg:generic/example@1.0'}]}
        self.raw = {'descriptor': {'version': '0.120.1', 'db': {'status': {'valid': True}}},
            'matches': [{'vulnerability': {'id': 'CVE-2026-1234', 'severity': 'High'},
                         'artifact': {'name': 'example', 'version': '1.0', 'type': 'UnknownPackage'}}]}

    def reconcile(self):
        return engine.reconcile(self.bom, self.raw, self.registry, self.root, NOW)

    def test_consistent_active_exception_without_suppression(self):
        cdx, report, vex, snapshot = self.reconcile()
        gl = adapter.convert(report, NOW[:-1], NOW[:-1])
        self.assertEqual(cdx['vulnerabilities'][0]['analysis']['state'], 'not_affected')
        self.assertEqual(vex['statements'][0]['status'], 'not_affected')
        self.assertEqual(report['matches'][0]['kasmvncAssessment'], snapshot['assessments'][0])
        self.assertIn('not_affected', gl['vulnerabilities'][0]['description'])
        self.assertEqual(len(gl['vulnerabilities'][0]['flags']), 1)
        self.assertEqual(len(report['matches']), len(self.raw['matches']))
        self.assertNotIn('kasmvncAssessment', self.raw['matches'][0])

    def test_absent_findings_become_inactive_then_reactivate_only_in_scope(self):
        match = self.raw['matches'].pop()
        cdx, report, vex, snapshot = self.reconcile()
        self.assertEqual(snapshot['assessments'][0]['lifecycle'], 'not_detected')
        self.assertEqual(cdx['vulnerabilities'], [])
        self.assertEqual(vex['statements'][0]['status'], 'under_investigation')
        self.assertIn('not_detected', vex['statements'][0]['status_notes'])
        self.raw['matches'].append(match)
        self.assertEqual(self.reconcile()[3]['assessments'][0]['lifecycle'], 'active')

    def test_changed_version_cannot_reuse_exception(self):
        self.raw['matches'][0]['artifact']['version'] = '2.0'
        self.bom['components'][0].update(version='2.0', **{'bom-ref': 'example@2.0'})
        cdx, report, vex, snapshot = self.reconcile()
        self.assertEqual(snapshot['assessments'][0]['lifecycle'], 'needs_review')
        self.assertEqual(vex['statements'][0]['status'], 'under_investigation')
        self.assertNotIn('flags', adapter.convert(report, NOW[:-1], NOW[:-1])['vulnerabilities'][0])

    def test_changed_recipe_or_removed_evidence_invalidates(self):
        for content in ['feature enabled', None]:
            p = self.root / 'recipe'
            if content: p.write_text(content)
            else: p.unlink()
            self.assertEqual(self.reconcile()[3]['assessments'][0]['state'], 'in_triage')

    def test_new_cjson_caller_invalidates(self):
        self.rule['check'] = 'no_cjson_compare_callers'
        (self.root / 'common').mkdir(); (self.root / 'common/new.c').write_text('cJSON_Compare(a,b,1);')
        self.assertEqual(self.reconcile()[3]['assessments'][0]['lifecycle'], 'needs_review')

    def test_unknown_finding_never_inherits_other_exception(self):
        self.raw['matches'][0]['vulnerability']['id'] = 'CVE-2026-9999'
        self.assertEqual(self.reconcile()[3]['assessments'][0]['lifecycle'], 'unreviewed')

    def test_wrong_architecture_and_libc_invalidate(self):
        for field in ['architectures', 'libcs']:
            saved = self.rule[field]; self.rule[field] = ['other']
            self.assertEqual(self.reconcile()[3]['assessments'][0]['lifecycle'], 'needs_review')
            self.rule[field] = saved

    def test_backport_and_wrong_advisory_version(self):
        for state, vex_state in [('resolved', 'fixed'), ('false_positive', 'not_affected')]:
            self.rule['state'] = state; self.rule.pop('justification', None)
            self.assertEqual(self.reconcile()[2]['statements'][0]['status'], vex_state)

    def test_bad_registry_and_failed_scan_are_fatal(self):
        self.registry['assessments'].append(copy.deepcopy(self.rule))
        with self.assertRaises(ValueError): self.reconcile()
        self.registry['assessments'].pop(); self.raw['descriptor']['db']['status']['valid'] = False
        with self.assertRaises(ValueError): self.reconcile()

    def test_shipped_registry_valid(self):
        p = Path(__file__).resolve().parent
        registry = json.loads((p / 'security-assessments.json').read_text()); engine.validate_registry(registry)


if __name__ == '__main__':
    unittest.main()
