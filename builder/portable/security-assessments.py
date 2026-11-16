#!/usr/bin/env python3
"""Reconcile reviewed decisions with one successful scan; never discard matches.

The committed registry is review history. Its per-build snapshot is regenerated,
including inactive records. An absent match is NOT evidence of remediation.
"""
import argparse
from collections import Counter
import copy
import datetime
import hashlib
import json
from pathlib import Path
from urllib.parse import quote, parse_qsl, urlencode

STATES = {'not_affected', 'false_positive', 'resolved', 'in_triage', 'exploitable'}
JUSTIFICATIONS = {'code_not_present', 'code_not_reachable', 'requires_environment'}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def scoped_purl(purl, revision, arch, libc):
    # Grype's CycloneDX reader loses application-level product identity. Its
    # package-as-product VEX matching does preserve PURL qualifiers. Identify
    # our embedded variant without asserting anything about arbitrary upstream
    # copies of the same library/version, and preserve ecosystem/version fields.
    head, separator, subpath = purl.partition('#')
    base, _, query = head.partition('?')
    qualifiers = dict(parse_qsl(query))
    qualifiers.update(kasmvnc_build=revision, arch=arch, libc=libc)
    return base + '?' + urlencode(sorted(qualifiers.items())) + (separator + subpath if separator else '')


def validate_registry(registry):
    if registry.get('schema_version') != 1:
        raise ValueError('Unsupported assessment registry')
    seen = set()
    for r in registry['assessments']:
        key = (r['cve'], r['package'])
        if key in seen:
            raise ValueError('Duplicate assessment: ' + str(key))
        seen.add(key)
        if r['state'] not in STATES or not all(r.get(k) for k in
                ('versions', 'architectures', 'libcs', 'detail', 'references', 'reviewed_at', 'author', 'evidence')):
            raise ValueError('Incomplete assessment: ' + r['cve'])
        if r['state'] == 'not_affected' and r.get('justification') not in JUSTIFICATIONS:
            raise ValueError('Missing not-affected justification: ' + r['cve'])
        for path, expected in r['evidence'].items():
            if Path(path).is_absolute() or '..' in Path(path).parts or len(expected) != 64:
                raise ValueError('Invalid evidence path/hash')
        datetime.datetime.fromisoformat(r['reviewed_at'].replace('Z', '+00:00'))


def reconcile(bom, raw, registry, root, now):
    validate_registry(registry)
    if raw.get('descriptor', {}).get('db', {}).get('status', {}).get('valid') is not True or not isinstance(raw.get('matches'), list):
        raise ValueError('Cannot reconcile an incomplete/failed scan')
    product = bom['metadata']['component']
    props = {p['name']: p['value'] for p in product.get('properties', [])}
    arch = props['kasmvnc:architecture']
    libc = props.get('kasmvnc:libc')
    if libc is None:  # Existing 1.6 inventories, before explicit libc metadata.
        libc = 'musl' if 'musl >=' in props.get('kasmvnc:host-requirements', '') else 'glibc'
    revision = props['kasmvnc:revision']
    cdx = copy.deepcopy(bom)
    for c in cdx['components']:
        if c.get('purl', '').startswith('pkg:generic/'):
            original = c['purl']
            c['purl'] = scoped_purl(original, revision, arch, libc)
            if not any(p['name'] == 'kasmvnc:upstream-purl' for p in c.get('properties', [])):
                c.setdefault('properties', []).append({'name': 'kasmvnc:upstream-purl', 'value': original})
    components = {(c['name'], c.get('version')): c for c in cdx['components']}
    rules = {(r['cve'], r['package']): r for r in registry['assessments']}
    observed = set()
    annotated = copy.deepcopy(raw)
    annotated['matches'] = []
    cdx['vulnerabilities'] = []
    records = []
    evidence_cache = {}
    product_id = 'pkg:generic/KasmVNC@' + quote(revision, safe='') + '?arch=' + quote(arch, safe='') + '&libc=' + quote(libc, safe='')
    cdx['metadata']['component']['purl'] = product_id
    vex = {'@context': 'https://openvex.dev/ns/v0.2.0',
           '@id': 'urn:sha256:' + hashlib.sha256((product_id + now + json.dumps(raw, sort_keys=True)).encode()).hexdigest(),
           'author': 'Kasm Technologies', 'timestamp': now, 'version': 1, 'statements': []}
    for match in raw['matches']:
        artifact = match['artifact']; vuln = match['vulnerability']
        component = components.get((artifact['name'], artifact['version']))
        if component is None:
            raise ValueError('Scan artifact missing from SBOM: ' + artifact['name'])
        ids = {vuln['id'], *(v['id'] for v in match.get('relatedVulnerabilities') or [])}
        candidates = [rules[(i, artifact['name'])] for i in ids if (i, artifact['name']) in rules]
        if len(candidates) > 1:
            raise ValueError('Conflicting advisory aliases')
        r = candidates[0] if candidates else None
        a = {'state': 'in_triage', 'lifecycle': 'unreviewed', 'detail': 'No applicable reviewed assessment.',
             'cve': vuln['id'], 'package': artifact['name'], 'version': artifact['version'],
             'architecture': arch, 'libc': libc, 'revision': revision, 'observed_at': now}
        if r:
            observed.add((r['cve'], r['package']))
            errors = []
            if artifact['version'] not in r['versions'] or arch not in r['architectures'] or libc not in r['libcs']:
                errors.append('version/architecture/libc outside reviewed scope')
            for name, expected in r['evidence'].items():
                if name not in evidence_cache:
                    path = root / name
                    evidence_cache[name] = digest(path) if path.is_file() else None
                if evidence_cache[name] != expected:
                    errors.append('changed evidence: ' + name)
            # Caller absence must include new files, not just hashes of old ones.
            if r.get('check') == 'no_cjson_compare_callers':
                for directory in ('common', 'unix'):
                    for path in (root / directory).rglob('*'):
                        if path.suffix in ('.c', '.cc', '.cpp', '.cxx', '.h') and path.name not in ('cJSON.c', 'cJSON.h'):
                            if 'cJSON_Compare' in path.read_text(errors='replace'):
                                errors.append('cJSON_Compare reference: ' + str(path.relative_to(root)))
            a.update(cve=r['cve'], reviewed_at=r['reviewed_at'], author=r['author'], references=r['references'],
                     evidence=r['evidence'], reviewed_state=r['state'])
            if errors:
                a.update(lifecycle='needs_review', detail='Assessment invalidated: ' + '; '.join(errors))
            else:
                a.update(state=r['state'], lifecycle='active', detail=r['detail'])
                if r.get('justification'):
                    a['justification'] = r['justification']
        records.append(a)
        item = copy.deepcopy(match); item['kasmvncAssessment'] = a
        if component.get('purl'):
            item['artifact']['purl'] = component['purl']
        annotated['matches'].append(item)
        analysis = {'state': a['state'], 'detail': a['detail'], 'lastUpdated': now}
        if 'justification' in a:
            analysis['justification'] = a['justification']
        cdx['vulnerabilities'].append({'id': vuln['id'], 'source': {'name': 'Grype'},
            'affects': [{'ref': component['bom-ref']}], 'analysis': analysis,
            'properties': [{'name': 'kasmvnc:assessment', 'value': json.dumps(a, sort_keys=True)}]})
        status = {'resolved': 'fixed', 'false_positive': 'not_affected', 'not_affected': 'not_affected',
                  'in_triage': 'under_investigation', 'exploitable': 'affected'}[a['state']]
        statement = {'vulnerability': {'name': vuln['id']}, 'timestamp': now,
                     'products': [{'@id': component['purl']}] if component.get('purl', '').startswith('pkg:generic/') else
                                 [{'@id': product_id, 'subcomponents': [{'@id': component.get('purl', 'pkg:generic/' + component['bom-ref'])}]}],
                     'status': status, 'status_notes': a['detail'] + '\nAssessment: ' + json.dumps(a, sort_keys=True)}
        if status == 'not_affected':
            statement['impact_statement'] = a['detail']
            if a.get('justification') in ('code_not_present', 'code_not_reachable'):
                statement['justification'] = {'code_not_present': 'vulnerable_code_not_present',
                    'code_not_reachable': 'vulnerable_code_not_in_execute_path'}[a['justification']]
        if status == 'affected':
            statement['action_statement'] = a['detail']
        vex['statements'].append(statement)
    for r in registry['assessments']:
        if arch not in r['architectures'] or libc not in r['libcs']:
            continue  # This decision never applied to this build.
        if (r['cve'], r['package']) not in observed:
            records.append({**copy.deepcopy(r), 'lifecycle': 'not_detected',
                'state': 'in_triage', 'reviewed_state': r['state'], 'observed_at': now,
                'detail': 'Not detected in this successful scan; previous decision is inactive. Absence does not prove a fix.',
                'previous_detail': r['detail'], 'architecture': arch, 'libc': libc, 'revision': revision})
            vex['statements'].append({'vulnerability': {'name': r['cve']}, 'timestamp': now,
                'products': [{'@id': product_id}], 'status': 'under_investigation',
                'status_notes': 'not_detected: previous assessment is inactive; absence does not prove a fix. Previous review: ' + json.dumps(r, sort_keys=True)})
    snapshot = {'schema_version': 1, 'product': product_id, 'generated_at': now,
                'assessments': records, 'counts': dict(Counter(a['lifecycle'] for a in records))}
    # Carry inactive history in SBOM metadata, without inventing current findings.
    cdx['metadata']['properties'] = [p for p in cdx['metadata'].get('properties', [])
                                   if p['name'] != 'kasmvnc:security-assessments']
    cdx['metadata'].setdefault('properties', []).append({'name': 'kasmvnc:security-assessments',
                                                       'value': json.dumps(snapshot, sort_keys=True)})
    annotated['kasmvncAssessmentSummary'] = snapshot
    return cdx, annotated, vex, snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('basename', type=Path)
    parser.add_argument('--registry', type=Path, default=Path(__file__).with_name('security-assessments.json'))
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(); base = str(args.basename)
    raw_path = Path(base + '.grype.raw.json'); bom_path = Path(base + '.cdx.json')
    now = datetime.datetime.now(datetime.timezone.utc).isoformat()
    cdx, annotated, vex, snapshot = reconcile(json.loads(bom_path.read_text()), json.loads(raw_path.read_text()),
        json.loads(args.registry.read_text()), args.source_root, now)
    snapshot['registry_sha256'] = digest(args.registry)
    snapshot['raw_scan_sha256'] = digest(raw_path)
    snapshot['scanned_sbom_sha256'] = digest(bom_path)
    write(base + '.assessments.json', snapshot)
    write(base + '.openvex.json', vex)
    write(base + '.vulnerabilities.json', annotated)
    write(bom_path, cdx)
    summary = {'policy': 'advisory; every match retained with its scoped assessment',
               'match_counts_by_severity': dict(Counter(m['vulnerability']['severity'] for m in annotated['matches'])),
               'assessment_counts_by_state': dict(Counter(m['kasmvncAssessment']['state'] for m in annotated['matches'])),
               'reconciliation': snapshot,
               'published_sbom_sha256': digest(bom_path), 'scanner': annotated['descriptor']}
    write(base + '.vulnerability-summary.json', summary)
    print(json.dumps({'security_assessments': summary['assessment_counts_by_state'], 'lifecycle': snapshot['counts']}))


if __name__ == '__main__':
    main()
