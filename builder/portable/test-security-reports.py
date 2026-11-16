#!/usr/bin/env python3
"""Validate all published assessment projections against pinned public schemas."""
import hashlib
import json
from pathlib import Path
import sys
import urllib.request

import jsonschema

SOURCES = {
 'cdx': ('https://raw.githubusercontent.com/CycloneDX/specification/1.6/schema/bom-1.6.schema.json', '3e92dddbc30cf7f6a02b80f0942b1a4cfd4fb1c26f1dfc4310afa9d613cafb93'),
 'gitlab': ('https://gitlab.com/gitlab-org/security-products/security-report-schemas/-/raw/v15.2.3/dist/dependency-scanning-report-format.json', '4228b68f6951f6b0a2c76b85d6f93d822cd8fc7b265c4225f65a1ddfe28d1fb9'),
 'vex': ('https://raw.githubusercontent.com/openvex/spec/main/openvex_json_schema.json', '9373597734ed1d3ea5161a8b46d3866c4a8cfe76fd632fdd16aef01fb34b3238')}


def read(path):
    return json.loads(Path(path).read_text())


def main():
    validators = {}
    for name, (url, expected) in SOURCES.items():
        data = urllib.request.urlopen(url, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Public schema changed: ' + name)
        schema = json.loads(data)
        cls = jsonschema.validators.validator_for(schema)
        validators[name] = cls(schema)
    directories = [Path(p) for p in sys.argv[1:]] or sorted(Path('builder/build').glob('portable_*'))
    count = 0
    for directory in directories:
        for path in directory.glob('*.assessments.json'):
            base = str(path).removesuffix('.assessments.json')
            snap = read(path); bom = read(base + '.cdx.json'); report = read(base + '.vulnerabilities.json')
            raw = read(base + '.grype.raw.json'); vex = read(base + '.openvex.json')
            applied = read(base + '.vex-applied.json')
            gl = read(directory / 'gl-dependency-scanning-report.json')
            for key, document in [('cdx', bom), ('gitlab', gl), ('vex', vex)]:
                validators[key].validate(document)
            assert bom == read(directory / 'gl-sbom.cdx.json'), 'GitLab SBOM differs'
            assert snap['raw_scan_sha256'] == hashlib.sha256(Path(base + '.grype.raw.json').read_bytes()).hexdigest()
            assert len(raw['matches']) == len(report['matches']) == len(gl['vulnerabilities']) == len(bom['vulnerabilities'])
            def key(m):
                return (m['vulnerability']['id'], m['artifact']['name'], m['artifact']['version'])
            ignored = {key(m) for m in applied.get('ignoredMatches', [])}
            remaining = {key(m) for m in applied['matches']}
            expected_ignored = {key(m) for m in report['matches'] if m['kasmvncAssessment']['state'] in ('not_affected', 'false_positive', 'resolved')}
            assert ignored == expected_ignored, 'Grype did not apply exactly the reviewed exceptions'
            assert not ignored & remaining
            assert ignored | remaining == {key(m) for m in raw['matches']}, 'VEX consumer lost/added findings'
            for m in report['matches']:
                a = m['kasmvncAssessment']; cve = m['vulnerability']['id']; package = m['artifact']
                assert a in snap['assessments']
                c = next(v for v in bom['vulnerabilities'] if v['id'] == cve and v['affects'][0]['ref'] == package['id'])
                assert c['analysis']['state'] == a['state']
                f = next(v for v in gl['vulnerabilities'] if v['name'] == cve and v['location']['dependency']['package']['name'] == package['name'])
                assert json.loads(f['details']['kasmvnc_assessment']['value']) == a
                v = next(v for v in vex['statements'] if v['vulnerability']['name'] == cve and 'not_detected:' not in v['status_notes'])
                expected = {'resolved': 'fixed', 'false_positive': 'not_affected', 'not_affected': 'not_affected', 'in_triage': 'under_investigation', 'exploitable': 'affected'}[a['state']]
                assert v['status'] == expected
            print(path, 'schemas and cross-report assessments PASS', snap['counts'])
            count += 1
    if not count:
        raise SystemExit('No assessment artifacts found')


if __name__ == '__main__':
    main()
