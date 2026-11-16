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


class NoRegistryRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Neither Basic nor Bearer credentials may leave the validated endpoint.
        # Reject even same-origin redirects: a redirected 404 is not a cache miss.
        raise RuntimeError('Registry redirects are not supported')


def endpoint(url):
    try:
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != 'https' or not parsed.hostname or parsed.username is not None
                or parsed.password is not None or parsed.fragment):
            raise ValueError('invalid endpoint')
        port = parsed.port if parsed.port is not None else 443
        if port < 1:
            raise ValueError('invalid port')
        return parsed.hostname.lower(), port
    except ValueError as error:
        raise RuntimeError('Untrusted registry authentication realm') from error


def registry_digest(reference, opener=None):
    if opener is None:
        opener = urllib.request.build_opener(NoRegistryRedirects()).open
    host, rest = reference.split('/', 1)
    repository, tag = rest.rsplit(':', 1)
    registry_endpoint = endpoint('https://' + host)
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
        realm = fields.get('realm')
        if not realm:
            raise RuntimeError('Registry challenge has no realm')
        trusted = {registry_endpoint}
        if registry_endpoint == ('registry.gitlab.com', 443):
            trusted.add(('gitlab.com', 443))
        if endpoint(realm) not in trusted:
            raise RuntimeError('Untrusted registry authentication realm')
        for variable in ('CI_REGISTRY_USER', 'CI_REGISTRY_PASSWORD'):
            if not os.environ.get(variable):
                raise RuntimeError('Missing registry credential: ' + variable)
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
        try:
            result = registry_digest(args.reference)
        except (RuntimeError, ValueError, KeyError, urllib.error.URLError, OSError) as error:
            # Only the original manifest's HTTP 404 below may return miss code 3.
            # Avoid exception URLs/headers, which may contain authentication data.
            message = str(error) if isinstance(error, RuntimeError) else type(error).__name__
            raise SystemExit('Registry lookup failed: ' + message)
        if result is None:
            raise SystemExit(3)  # Only a manifest HTTP 404 permits a build.
        print(result)
