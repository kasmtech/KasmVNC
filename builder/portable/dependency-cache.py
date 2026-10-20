#!/usr/bin/env python3
"""Content key and authenticated, fail-closed registry lookup (no writes)."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import tarfile
import urllib.error
import urllib.parse
import urllib.request


def canonical_mode(path):
    # Git tracks the owner's executable bit, not checkout umask/write bits.
    return 0o755 if path.is_dir() or path.stat().st_mode & 0o100 else 0o644


def context_entries(context):
    for path in sorted(context.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'Symlinks forbidden in dependency context: {path}')
        if not path.is_file() and not path.is_dir():
            raise ValueError(f'Unsupported dependency context entry: {path}')
        yield path


def context_archive(context, output):
    if context.resolve() in output.resolve().parents:
        raise ValueError('Archive output must be outside the dependency context')
    with tarfile.open(output, 'w', format=tarfile.PAX_FORMAT) as archive:
        for path in context_entries(context):
            info = archive.gettarinfo(str(path), arcname=path.relative_to(context).as_posix())
            info.mode = canonical_mode(path)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ''
            if path.is_dir():
                archive.addfile(info)
            else:
                with path.open('rb') as stream:
                    archive.addfile(info, stream)


def context_key(context, architecture):
    if architecture not in ('amd64', 'arm64'):
        raise ValueError('Unsupported architecture')
    digest = hashlib.sha256(b'kasmvnc-dependency-context-v2\0' + architecture.encode() + b'\0')
    for path in context_entries(context):
        name = path.relative_to(context).as_posix().encode()
        content = b'' if path.is_dir() else path.read_bytes()
        kind = b'directory' if path.is_dir() else b'file'
        for item in (kind, name, oct(canonical_mode(path)).encode(), content):
            digest.update(len(item).to_bytes(8, 'big'))
            digest.update(item)
    return digest.hexdigest()


def registry_digest(reference, opener=urllib.request.urlopen):
    host, rest = reference.split('/', 1)
    repository, tag = rest.rsplit(':', 1)
    url = f'https://{host}/v2/{repository}/manifests/{tag}'
    headers = {'Accept': ', '.join(('application/vnd.oci.image.manifest.v1+json',
                'application/vnd.docker.distribution.manifest.v2+json',
                'application/vnd.oci.image.index.v1+json',
                'application/vnd.docker.distribution.manifest.list.v2+json'))}
    # Obtain the registry challenge first; never treat auth/network failures as misses.
    try:
        response = opener(urllib.request.Request(f'https://{host}/v2/'), timeout=30)
        response.close()
    except urllib.error.HTTPError as error:
        if error.code != 401:
            raise
        challenge = error.headers.get('WWW-Authenticate', '')
        if not challenge.startswith('Bearer '):
            raise RuntimeError('Expected Bearer registry authentication') from error
        fields = dict(re.findall(r'(\w+)="([^"]*)"', challenge))
        realm = fields['realm']
        parsed = urllib.parse.urlparse(realm)
        if parsed.scheme != 'https' or parsed.hostname != host:
            # GitLab.com registry delegates to GitLab.com, not arbitrary hosts.
            if not (host == 'registry.gitlab.com' and parsed.scheme == 'https' and parsed.hostname == 'gitlab.com'):
                raise RuntimeError('Untrusted registry authentication realm')
        query = urllib.parse.urlencode({'service': fields.get('service', ''), 'scope': f'repository:{repository}:pull'})
        credentials = base64.b64encode((os.environ['CI_REGISTRY_USER'] + ':' + os.environ['CI_REGISTRY_PASSWORD']).encode()).decode()
        request = urllib.request.Request(realm + ('&' if '?' in realm else '?') + query,
                                         headers={'Authorization': 'Basic ' + credentials})
        with opener(request, timeout=30) as response:
            token = json.load(response)['token']
        headers['Authorization'] = 'Bearer ' + token
    try:
        with opener(urllib.request.Request(url, headers=headers, method='HEAD'), timeout=30) as response:
            digest = response.headers.get('Docker-Content-Digest', '')
            if not re.fullmatch(r'sha256:[a-f0-9]{64}', digest):
                raise RuntimeError('Registry omitted valid immutable manifest digest')
            return reference.rsplit(':', 1)[0] + '@' + digest
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    key = sub.add_parser('key')
    key.add_argument('context', type=Path)
    key.add_argument('architecture')
    archive = sub.add_parser('archive')
    archive.add_argument('context', type=Path)
    archive.add_argument('output', type=Path)
    lookup = sub.add_parser('lookup')
    lookup.add_argument('reference')
    args = parser.parse_args()
    if args.command == 'key':
        print(context_key(args.context, args.architecture))
    elif args.command == 'archive':
        context_archive(args.context, args.output)
    else:
        result = registry_digest(args.reference)
        if result is None:
            raise SystemExit(3)  # Only a manifest HTTP 404 permits a build.
        print(result)
