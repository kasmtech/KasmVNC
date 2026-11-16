#!/usr/bin/env python3
"""Exact installed APK ownership and checked corresponding-source cache.

Cache creation executes the official APKBUILD's fetch/verify in the disposable
builder. Consumers never execute downloaded code and fail on any missing hash.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def installed(database=Path('/lib/apk/db/installed')):
    packages = []
    for block in database.read_text().split('\n\n'):
        package, files, directory = {}, [], ''
        for line in block.splitlines():
            if len(line) < 2 or line[1] != ':':
                continue
            k, v = line[0], line[2:]
            if k == 'F':
                directory = v
            elif k == 'R':
                files.append('/' + directory + '/' + v)
            elif k in ('P', 'V', 'A', 'L', 'o', 'c', 'C'):
                package[k] = v
        if package:
            package['files'] = files
            packages.append(package)
    return packages


def owner(path, packages):
    candidates = {str(path), str(Path(path).resolve())}
    matches = [p for p in packages if candidates.intersection(p['files'])]
    if len(matches) > 1:
        raise RuntimeError('Ambiguous APK owner: ' + str(path))
    return matches[0] if matches else None


def checked_source(cache, package):
    records = json.loads((cache / 'manifest.json').read_text())
    key = package['P'] + '-' + package['V']
    record = records.get(key)
    if not record or any(record.get(k) != package[k] for k in ('P', 'V', 'A', 'o', 'c', 'C')):
        raise RuntimeError('Missing/mismatched exact APK source: ' + key)
    filename = record['file']
    if Path(filename).name != filename or not filename.endswith('.tar.gz'):
        raise RuntimeError('Invalid cached APK source path')
    archive = cache / filename
    if not archive.is_file() or archive.is_symlink() or sha(archive) != record['sha256']:
        raise RuntimeError('Cached APK source missing or hash mismatch: ' + key)
    return archive, record


def create_cache(cache=Path('/opt/kasm-apk-sources')):
    cache.mkdir(parents=True, exist_ok=True)
    packages = installed()
    selected = [p for p in packages if any(f.endswith('.a') or
                (Path(f).name.endswith('.o') and 'crt' in Path(f).name) for f in p['files'])]
    records, origins = {}, {}
    with tempfile.TemporaryDirectory(prefix='kasm-aports-') as tmp:
        work = Path(tmp)
        for p in selected:
            origin, commit = p.get('o', ''), p.get('c', '')
            if not re.fullmatch(r'[a-zA-Z0-9_.+-]+', origin) or not re.fullmatch(r'[0-9a-f]{40}', commit):
                raise RuntimeError('Missing exact source identity for ' + p['P'])
            identity = origin + '-' + commit
            if identity not in origins:
                snapshot = work / (commit + '.tar.gz')
                url = 'https://github.com/alpinelinux/aports/archive/' + commit + '.tar.gz'
                if not snapshot.exists():
                    subprocess.run(['curl', '-fLsS', '--retry', '3', '--connect-timeout', '30', '--max-time', '600', url, '-o', str(snapshot)], check=True)
                stage = work / identity
                stage.mkdir()
                with tarfile.open(snapshot) as tf:
                    candidates = [m for m in tf.getmembers() if re.fullmatch(r'aports-[0-9a-f]+/(main|community|testing)/' + re.escape(origin) + '/APKBUILD', m.name)]
                    if len(candidates) != 1:
                        raise RuntimeError('Ambiguous/missing APKBUILD: ' + identity)
                    prefix = str(Path(candidates[0].name).parent) + '/'
                    for m in tf.getmembers():
                        if m.name.startswith(prefix):
                            m.name = m.name[len(prefix):]
                            if m.name:
                                tf.extract(m, stage, filter='data')
                # pkgver is shared by all subpackages, and APKBUILD corresponds
                # to the exact commit recorded in the signed installed package.
                version = subprocess.check_output(['sh', '-c', '. ./APKBUILD; printf "%s-r%s" "$pkgver" "$pkgrel"'], cwd=stage, text=True)
                if version != p['V']:
                    raise RuntimeError('APKBUILD version mismatch: ' + identity + ': ' + version + ' != ' + p['V'])
                sources = stage / 'upstream'
                sources.mkdir()
                subprocess.run(['abuild', '-F', '-d', '-s', str(sources), 'fetch', 'verify'], cwd=stage, check=True, timeout=1800)
                # Keep original recipe/patches and all checksum-verified inputs;
                # remove only abuild's transient symlink workspace if present.
                shutil.rmtree(stage / 'src', ignore_errors=True)
                manifest = {str(f.relative_to(stage)): sha(f) for f in stage.rglob('*') if f.is_file() and not f.is_symlink()}
                (stage / 'source-inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
                filename = identity + '.tar.gz'
                subprocess.run(['tar', '--sort=name', '--owner=0', '--group=0', '--numeric-owner', '-czf', str(cache / filename), '-C', str(stage), '.'], check=True)
                origins[identity] = {'file': filename, 'sha256': sha(cache / filename), 'url': url,
                                     'aports_commit': commit, 'checksums': 'abuild fetch verify against exact APKBUILD'}
            records[p['P'] + '-' + p['V']] = {k: p[k] for k in ('P', 'V', 'A', 'L', 'o', 'c', 'C')}
            records[p['P'] + '-' + p['V']].update(origins[identity])
    (cache / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    create_cache()
