#!/usr/bin/env python3
"""Retain signed sources for installed static archives and startup objects."""
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    cache = Path('/opt/kasm-rpm-sources')
    cache.mkdir(exist_ok=True)
    owners = set()
    for path in run('rpm', '-qal').splitlines():
        if path.endswith('.a') or re.fullmatch(r'[a-zA-Z]*crt[^/]*\.o', Path(path).name):
            owners.add(run('rpm', '-qf', '--qf', '%{SOURCERPM}\n', path))
    records = {}
    for srpm in sorted(owners):
        if not re.fullmatch(r'[a-zA-Z0-9_.+~-]+\.src\.rpm', srpm):
            raise RuntimeError('Invalid source RPM name: ' + srpm)
        target = cache / srpm
        for repo in ['baseos/latest', 'appstream', 'codeready/builder', 'developer/EPEL']:
            url = f'https://yum.oracle.com/repo/OracleLinux/OL8/{repo}/{platform.machine()}/getPackageSource/{srpm}'
            result = subprocess.run(['curl', '-fLsS', '--retry', '3', '--connect-timeout', '30',
                                     '--max-time', '600', url, '-o', str(target)])
            if result.returncode == 0:
                break
        else:
            raise RuntimeError('Exact corresponding SRPM unavailable: ' + srpm)
        identity = run('rpm', '-qp', '--qf', '%{NAME}-%{VERSION}-%{RELEASE}.src.rpm', str(target))
        if identity != srpm:
            raise RuntimeError('Source RPM identity mismatch: ' + srpm)
        signature = run('rpmkeys', '--checksig', str(target))
        if 'signatures OK' not in signature:
            raise RuntimeError('Source RPM signature invalid: ' + srpm)
        digest = hashlib.sha256()
        with target.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
        records[srpm] = {'sha256': digest.hexdigest(), 'url': url}
        print('Retained signed dependency source: ' + srpm, flush=True)
    (cache / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
