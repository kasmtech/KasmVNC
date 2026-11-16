"""Validate the pinned build-only Xorg protocol SDK and its source archive."""
import hashlib
import json
import os
from pathlib import Path

VERSION = '2024.1'
SOURCE_URL = 'https://www.x.org/releases/individual/proto/xorgproto-2024.1.tar.xz'
SOURCE_SHA256 = '372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59'


def sdk_path():
    return Path(os.environ.get('KASMVNC_XORGPROTO_PREFIX', '/opt/kasm-xorgproto'))


def read_sdk(root=None):
    root = Path(root) if root is not None else sdk_path()
    manifest = json.loads((root / 'share/kasmvnc/xorgproto-sdk.json').read_text())
    archive = root / 'share/kasmvnc/sources' / ('xorgproto-' + VERSION + '.tar.xz')
    if (manifest.get('version') != VERSION or manifest.get('source_url') != SOURCE_URL
            or manifest.get('source_sha256') != SOURCE_SHA256
            or Path(manifest.get('source_archive', '')).resolve() != archive.resolve()):
        raise RuntimeError('Xorg protocol SDK does not match the pinned source')
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise RuntimeError('Xorg protocol SDK source checksum mismatch')
    return manifest
