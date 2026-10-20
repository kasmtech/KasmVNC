#!/usr/bin/env python3
"""Run in a test container with jsonschema and the pinned GitLab schema."""
import copy
import importlib.util
import json
import os
import subprocess
import tempfile
from pathlib import Path
import sys
import unittest

import jsonschema

spec = importlib.util.spec_from_file_location('adapter', Path(__file__).with_name('gitlab-dependency-report.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
schema = json.loads(Path(sys.argv.pop(1)).read_text())
real_reports = sys.argv[1:]
sys.argv[1:] = []
START, END = '2026-10-20T10:00:00', '2026-10-20T10:00:03'


def fixture():
    return {'descriptor': {'version': '0.120.1', 'db': {'status': {'valid': True}}},
            'matches': [{'vulnerability': {'id': 'GHSA-aaaa-bbbb-cccc', 'severity': 'High',
                'dataSource': 'https://example.com/advisory', 'description': 'Example finding',
                'fix': {'versions': ['2.0'], 'state': 'fixed'}},
                'relatedVulnerabilities': [{'id': 'CVE-2026-1234'}],
                'artifact': {'name': 'example', 'version': '1.0', 'type': 'npm'}}]}


class Conversion(unittest.TestCase):
    def convert(self, data):
        report = adapter.convert(data, START, END)
        jsonschema.Draft7Validator(schema).validate(report)
        return report

    def test_mapping(self):
        f = self.convert(fixture())['vulnerabilities'][0]
        self.assertEqual(f['severity'], 'High')
        self.assertEqual([i['type'] for i in f['identifiers']], ['ghsa', 'cve'])
        self.assertIn('2.0', f['solution'])
        self.assertEqual(f['location']['dependency']['package']['name'], 'example')
        self.assertEqual(f['links'], [{'url': 'https://example.com/advisory'}])

    def test_empty_success(self):
        data = fixture(); data['matches'] = []
        self.assertEqual(self.convert(data)['vulnerabilities'], [])

    def test_severity_and_unfixed(self):
        for source, expected in [('Negligible', 'Info'), ('Moderate', 'Medium'), ('future', 'Unknown')]:
            data = fixture(); v = data['matches'][0]['vulnerability']
            v['severity'] = source; v['fix'] = {'state': 'not-fixed'}
            f = self.convert(data)['vulnerabilities'][0]
            self.assertEqual(f['severity'], expected)
            self.assertIn('not-fixed', f['solution'])

    def test_stable_identity_and_duplicates(self):
        data = fixture(); before = self.convert(data)['vulnerabilities'][0]['id']
        data['matches'][0]['artifact']['id'] = 'different-build-path'
        data['matches'].append(copy.deepcopy(data['matches'][0]))
        self.assertEqual(len(self.convert(data)['vulnerabilities']), 1)
        self.assertEqual(self.convert(data)['vulnerabilities'][0]['id'], before)
        data['matches'][1]['artifact']['version'] = '1.1'
        self.assertEqual(len(self.convert(data)['vulnerabilities']), 2)

    def test_invalid_inputs_fail_closed(self):
        for mutate in [lambda d: d['descriptor']['db']['status'].update(valid=False),
                       lambda d: d.update(matches=None),
                       lambda d: d['matches'][0]['artifact'].update(name='')]:
            data = fixture(); mutate(data)
            with self.assertRaises(ValueError): self.convert(data)
        with self.assertRaises(ValueError): adapter.convert(fixture(), END, START)

    def test_conflicting_duplicates(self):
        data = fixture(); data['matches'].append(copy.deepcopy(data['matches'][0]))
        data['matches'][1]['vulnerability']['severity'] = 'Critical'
        with self.assertRaises(ValueError): self.convert(data)

    def test_scan_integration_and_stale_report_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / 'kasmvncserver_commit_arch'
            Path(str(base) + '.cdx.json').write_text('{"bomFormat":"CycloneDX"}')
            source = root / 'fixture.json'; source.write_text(json.dumps(fixture()))
            grype = root / 'grype'
            grype.write_text('#!/bin/sh\ncp "$FIXTURE" "$5"\n')
            grype.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'], FIXTURE=str(source))
            scan = Path(__file__).with_name('scan')
            subprocess.run(['bash', str(scan), str(base)], env=env, check=True, capture_output=True)
            report_path = root / 'gl-dependency-scanning-report.json'
            jsonschema.Draft7Validator(schema).validate(json.loads(report_path.read_text()))
            self.assertEqual((root / 'gl-sbom.cdx.json').read_bytes(), Path(str(base) + '.cdx.json').read_bytes())
            grype.write_text('#!/bin/sh\nexit 7\n')
            result = subprocess.run(['bash', str(scan), str(base)], env=env, capture_output=True)
            self.assertEqual(result.returncode, 7)
            self.assertFalse(report_path.exists())
            self.assertFalse((root / 'gl-sbom.cdx.json').exists())

    def test_real_reports(self):
        for path in real_reports:
            with self.subTest(path=path):
                data = json.loads(Path(path).read_text())
                report = self.convert(data)
                self.assertEqual(len(report['vulnerabilities']), len(data['matches']))
                print(path, len(report['vulnerabilities']), 'findings validated')


unittest.main()
