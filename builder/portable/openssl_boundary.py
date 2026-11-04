"""Verify the build-only OpenSSL SDK and the packaged host-library boundary."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile


SONAMES = {'libssl.so.3', 'libcrypto.so.3'}
SYMBOL_BASELINE = 'OPENSSL_3.0.0'
SDK_CATALOG = Path(__file__).resolve().parents[1] / 'openssl3/sources.json'
# Names supplement symbol versions on platforms that omit version metadata.
# Exact SDK exports cover the rest of the OpenSSL namespace.
OPENSSL_NAME = re.compile(
    r'^(?:SSL_|TLS_|DTLS_|BIO_|EVP_|OPENSSL_|OpenSSL_|CRYPTO_|X509|ASN1_|'
    r'BN_|EC_|ECDSA_|ECDH_|RSA_|DSA_|DH_|RAND_|SHA(?:1|224|256|384|512)|'
    r'MD[245]_|HMAC_|AES_|DES_|RC[245]_|OSSL_|PEM_|d2i_|i2d_|ERR_|OBJ_|'
    r'PKCS[0-9]|OCSP_|CMS_|SMIME_|UI_|CONF_|NCONF_|COMP_|CT_|TS_|ENGINE_|'
    r'ASYNC_|SCT_|SRP_|SRTP_|NETSCAPE_|ESS_)')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def sdk_path():
    return Path(os.environ.get('KASMVNC_OPENSSL_PREFIX', '/opt/kasm-openssl3'))


def sdk_file(root, relative):
    name = Path(relative)
    if name.is_absolute() or '..' in name.parts or not name.parts:
        raise RuntimeError('Invalid OpenSSL SDK path: ' + relative)
    path = root / name
    if not path.is_file() or path.is_symlink() or root.resolve() not in path.resolve().parents:
        raise RuntimeError('Missing or unsafe OpenSSL SDK file: ' + relative)
    return path


def read_sdk(root=None):
    root = Path(root) if root is not None else sdk_path()
    manifest = json.loads((root / 'share/kasmvnc/openssl-sdk.json').read_text())
    if (manifest.get('format') != 'kasmvnc-openssl-sdk-v1'
            or not re.fullmatch(r'3\.0\.\d+', manifest.get('version', ''))
            or set(manifest.get('sonames', [])) != SONAMES
            or manifest.get('symbol_version_maximum') != SYMBOL_BASELINE):
        raise RuntimeError('Invalid OpenSSL 3.0 SDK manifest')
    source_pin = json.loads(SDK_CATALOG.read_text())
    if any(manifest.get(field) != source_pin[key] for field, key in
           [('version', 'version'), ('source_url', 'url'), ('source_sha256', 'sha256')]):
        raise RuntimeError('OpenSSL SDK source does not match pinned source')
    files = manifest.get('files', {})
    expected = {str(p.relative_to(root)) for p in (root / 'include/openssl').rglob('*.h')}
    expected |= {'lib/' + name for name in SONAMES}
    if not expected - {'lib/' + name for name in SONAMES} or set(files) != expected:
        raise RuntimeError('OpenSSL SDK file set mismatch')
    for relative, digest in files.items():
        if sha256(sdk_file(root, relative)) != digest:
            raise RuntimeError('OpenSSL SDK file hash mismatch: ' + relative)
    source = sdk_file(root, manifest.get('source_archive', ''))
    if sha256(source) != manifest.get('source_sha256'):
        raise RuntimeError('OpenSSL SDK source hash mismatch')
    if any(root.rglob('libssl.a')) or any(root.rglob('libcrypto.a')):
        raise RuntimeError('OpenSSL SDK unexpectedly contains static archives')
    return manifest


def dynamic_symbols(path):
    text = subprocess.check_output(['readelf', '--dyn-syms', '--wide', str(path)], text=True)
    result = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 8 or not re.fullmatch(r'\d+:', fields[0]):
            continue
        name, _, version = fields[7].partition('@')
        result.append((name, version.lstrip('@'), fields[6] == 'UND'))
    return result


def sdk_exports(root, manifest):
    """Intersect source ABI declarations with the actual SDK library exports."""
    baseline = set()
    with tarfile.open(sdk_file(root, manifest['source_archive'])) as archive:
        for library in ('libssl', 'libcrypto'):
            member = f'openssl-{manifest["version"]}/util/{library}.num'
            with archive.extractfile(member) as stream:
                for line in stream.read().decode().splitlines():
                    fields = line.split()
                    if len(fields) >= 4 and fields[2] == '3_0_0' and fields[3].startswith('EXIST:'):
                        baseline.add(fields[0])
    exports = {soname: {name for name, version, undefined in dynamic_symbols(root / 'lib' / soname)
                        if not undefined and name in baseline
                        and (not version or version == SYMBOL_BASELINE)} for soname in SONAMES}
    if not all(exports.values()):
        raise RuntimeError('OpenSSL SDK has no baseline library exports')
    return exports


def audit_openssl(path, needed, exports):
    """Accept only host OpenSSL 3 and imports present in its original 3.0 ABI."""
    unexpected = {name for name in needed if re.match(r'lib(?:ssl|crypto)\.so', name)} - SONAMES
    if unexpected:
        raise RuntimeError('Unexpected OpenSSL SONAME: ' + str(path) + ': ' + repr(sorted(unexpected)))
    required = {'Xkasmvnc': SONAMES, 'Xvnc': SONAMES,
                'kasmvncpasswd': {'libcrypto.so.3'}}.get(path.name, set())
    if not required <= set(needed):
        raise RuntimeError('Missing required host OpenSSL libraries: ' + str(path))
    baseline = set().union(*exports.values())
    imports = []
    for name, version, undefined in dynamic_symbols(path):
        is_openssl = version.startswith('OPENSSL_') or name in baseline or OPENSSL_NAME.match(name)
        if not is_openssl:
            continue
        if not undefined:
            if name in baseline:
                raise RuntimeError('Embedded OpenSSL implementation exported by ' + str(path) + ': ' + name)
            continue
        if version and version != SYMBOL_BASELINE:
            raise RuntimeError('OpenSSL symbol exceeds 3.0.0 ABI: ' + name + '@' + version)
        if name not in baseline:
            raise RuntimeError('OpenSSL symbol absent from 3.0.0 ABI: ' + name)
        providers = {soname for soname, names in exports.items() if name in names}
        if not providers & set(needed):
            raise RuntimeError('OpenSSL import has no direct host dependency: ' + name)
        imports.append({'symbol': name, 'version': version or None})
    return {'sonames': sorted(set(needed) & SONAMES), 'symbol_version_maximum': SYMBOL_BASELINE,
            'imports': sorted(imports, key=lambda item: item['symbol']),
            'unversioned_check': 'Compared with source-declared 3.0.0 symbols exported by the verified build SDK'}


def reject_static_openssl(path):
    if re.fullmatch(r'lib(?:ssl|crypto)(?:[-_.].*)?\.a', path.name):
        raise RuntimeError('Portable artifact unexpectedly embeds static OpenSSL: ' + str(path))


def reject_embedded_symbols(binary, exports):
    # Inspect the original, unstripped binary as well as link-map archive names:
    # this also catches hidden definitions in a renamed/repacked archive.
    symbols = subprocess.check_output(['nm', '--defined-only', '--format=posix', str(binary)], text=True)
    names = {line.split()[0].split('@')[0] for line in symbols.splitlines() if line.split()}
    if not names:
        raise RuntimeError('Unstripped binary required for OpenSSL implementation audit: ' + str(binary))
    embedded = names & set().union(*exports.values())
    if embedded:
        raise RuntimeError('Embedded OpenSSL definitions in ' + str(binary) + ': ' + ', '.join(sorted(embedded)))
