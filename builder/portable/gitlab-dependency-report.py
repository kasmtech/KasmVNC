#!/usr/bin/env python3
"""Translate the existing Grype scan; no rescanning or finding suppression."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re

SCHEMA_VERSION = '15.2.3'


def identifier(vulnerability):
    value = vulnerability['id']
    if not isinstance(value, str) or not value:
        raise ValueError('Missing vulnerability identifier')
    kind = 'cve' if value.startswith('CVE-') else 'ghsa' if value.startswith('GHSA-') else 'grype'
    result = {'type': kind, 'name': value, 'value': value}
    url = vulnerability.get('dataSource')
    if isinstance(url, str) and re.match(r'^https?://.+', url):
        result['url'] = url
    return result


def convert(report, start, end):
    descriptor = report['descriptor']
    if descriptor.get('db', {}).get('status', {}).get('valid') is not True:
        raise ValueError('Grype database is not valid')
    if not isinstance(report.get('matches'), list) or not descriptor.get('version'):
        raise ValueError('Incomplete Grype report')
    for timestamp in (start, end):
        datetime.datetime.strptime(timestamp, '%Y-%m-%dT%H:%M:%S')
    if end < start:
        raise ValueError('Scan end precedes start')
    findings = {}
    for match in report['matches']:
        vuln, package = match['vulnerability'], match['artifact']
        name, version = package['name'], package['version']
        if not isinstance(name, str) or not name or not isinstance(version, str) or not version:
            raise ValueError('Missing package name/version')
        identifiers = [identifier(vuln)]
        for related in match.get('relatedVulnerabilities') or []:
            item = identifier(related)
            if not any(i['value'] == item['value'] for i in identifiers):
                identifiers.append(item)
        # Keep architecture, revision, scan time and temporary build paths out
        # of identity. Distinct packages/versions/advisories remain distinct.
        identity = json.dumps([identifiers[0]['value'], package.get('type'), name, version])
        key = hashlib.sha256(identity.encode()).hexdigest()
        severity = vuln.get('severity', 'Unknown')
        severity = {'Negligible': 'Info', 'Moderate': 'Medium'}.get(severity, severity)
        if severity not in ('Info', 'Unknown', 'Low', 'Medium', 'High', 'Critical'):
            severity = 'Unknown'
        finding = {
            'id': key, 'name': vuln['id'][:255], 'severity': severity,
            'description': (vuln.get('description') or 'Grype dependency vulnerability match.')[:1048576],
            'identifiers': identifiers,
            'location': {'file': 'gl-sbom.cdx.json',
                         'dependency': {'package': {'name': name}, 'version': version}},
        }
        links = sorted({url for url in [vuln.get('dataSource'), *(vuln.get('urls') or [])]
                        if isinstance(url, str) and re.match(r'^https?://.+', url)})
        if links:
            finding['links'] = [{'url': url} for url in links]
        fix = vuln.get('fix') or {}
        versions = fix.get('versions') or []
        if versions:
            finding['solution'] = ('Fixed versions reported by Grype: ' + ', '.join(versions))[:7000]
        else:
            finding['solution'] = 'No fixed version reported by Grype (state: ' + (fix.get('state') or 'unknown') + ').'
        # Repeated identical match records are harmless; do not silently discard
        # conflicting metadata for an identical identity.
        if key in findings and findings[key] != finding:
            raise ValueError('Conflicting Grype matches for ' + identity)
        findings[key] = finding
    return {
        'version': SCHEMA_VERSION,
        'vulnerabilities': [findings[key] for key in sorted(findings)],
        'scan': {
            'analyzer': {'id': 'kasmvnc-grype', 'name': 'KasmVNC Grype report adapter',
                         'version': '1.0.0', 'vendor': {'name': 'Kasm Technologies'}},
            'scanner': {'id': 'grype', 'name': 'Grype', 'version': descriptor['version'],
                        'vendor': {'name': 'Anchore'}, 'url': 'https://github.com/anchore/grype'},
            'type': 'dependency_scanning', 'start_time': start, 'end_time': end,
            'status': 'success',
            'messages': [{'level': 'info', 'value': 'Advisory scan of embedded native and web dependencies. Host packages and drivers are excluded. Matches require review for backports, enabled features and reachability.'}],
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--start', required=True)
    parser.add_argument('--end', required=True)
    args = parser.parse_args()
    report = convert(json.loads(args.input.read_text()), args.start, args.end)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
