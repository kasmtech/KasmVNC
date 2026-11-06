#!/usr/bin/env python3
"""Build a native-link-evidence SBOM and corresponding-source artifact.

Fail closed on unknown native link inputs, missing source or mismatched frontend.
This describes supplied components, not a legal certification of compliance.
"""
import argparse
import datetime
import fnmatch
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import quote
from openssl_boundary import (read_sdk, sdk_path, sdk_exports, reject_static_openssl,
                              reject_embedded_symbols, audit_openssl)
from host_libraries import (components as host_components, reject_static_host_library,
                            audit_host_libraries, COMPILER_MAXIMUM, compiler_exceptions)
from glibc_crt import audit_link as audit_glibc_startup

SRC = Path('/src')
BINARIES = ['unix/xserver/hw/vnc/Xvnc', 'unix/vncconfig/vncconfig',
            'unix/kasmvncpasswd/kasmvncpasswd', 'unix/kasmxproxy/kasmxproxy',
            'unix/vncserver-native/vncserver-native']


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def rpm_notice_paths(srpm):
    siblings = [row.split('\t')[0] for row in run('rpm', '-qa', '--qf', '%{NAME}\t%{SOURCERPM}\n').splitlines()
                if row.split('\t')[1] == srpm]
    license_paths = set()
    for sibling in siblings:
        license_paths.update(run('rpm', '-q', '--licensefiles', sibling).splitlines())
        # GCC toolsets label COPYING texts as %doc instead of %license.
        # Keep the same exact-SRPM ownership boundary for these files.
        license_paths.update(p for p in run('rpm', '-ql', sibling).splitlines()
                             if re.match(r'(?i)^(license|copying|copyright|notice)', Path(p).name))
    return license_paths


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def load_adapter_headers(root):
    """Verify the isolated header inputs independently of cached static codecs."""
    manifest = json.loads((root / 'manifest.json').read_text())
    if manifest.get('format') != 'kasmvnc-ffmpeg-adapter-headers-v1':
        raise RuntimeError('Invalid FFmpeg adapter header manifest')
    families = manifest.get('families', [])
    catalog = json.loads((Path(__file__).resolve().parents[1] / 'ffmpeg-adapters/sources.json').read_text())
    expected_families = {item['avcodec_major'] for item in catalog}
    if sorted(item['avcodec_major'] for item in families) != sorted(expected_families):
        raise RuntimeError('Expected FFmpeg adapter families ' + ', '.join(map(str, sorted(expected_families))))
    if not manifest.get('headers'):
        raise RuntimeError('Missing FFmpeg adapter header hashes')
    # version.h is present in every supported family. The separate
    # version_major.h does not exist in the FFmpeg 4.4 headers.
    required = {f'{major}/include/{name}' for major in expected_families
                for name in ('libavcodec/avcodec.h', 'libavcodec/version.h', 'libavutil/avconfig.h')}
    if not required <= manifest['headers'].keys():
        raise RuntimeError('Missing required FFmpeg adapter headers')
    for item in families:
        archive = item['archive']
        if Path(archive).name != archive or sha(root / 'sources' / archive) != item['sha256']:
            raise RuntimeError('FFmpeg adapter source hash mismatch')
    for relative, digest in manifest['headers'].items():
        match = re.fullmatch(r'([0-9]+)/include/(?:[A-Za-z0-9_]+/)*[A-Za-z0-9_]+\.h', relative)
        if not match or int(match[1]) not in expected_families:
            raise RuntimeError('Invalid FFmpeg adapter header path')
        if sha(root / relative) != digest:
            raise RuntimeError('FFmpeg adapter header hash mismatch: ' + relative)
    if set(manifest['headers']) != {str(p.relative_to(root)) for p in root.glob('*/include/**/*.h')}:
        raise RuntimeError('FFmpeg adapter header set mismatch')
    return manifest


def cached_rpm_source(cache, srpm, target):
    if not re.fullmatch(r'[a-zA-Z0-9_.+~-]+\.src\.rpm', srpm):
        raise RuntimeError('Invalid cached source RPM name')
    manifest = json.loads((cache / 'manifest.json').read_text())
    record = manifest.get(srpm)
    source = cache / srpm
    if not record or not source.is_file() or sha(source) != record['sha256']:
        raise RuntimeError('Cached source RPM missing or hash mismatch: ' + srpm)
    shutil.copyfile(source, target)
    return record['url']


def write(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2) + '\n')


def component(name, version, kind='library', **extra):
    return {'type': kind, 'bom-ref': name + '@' + version, 'name': name,
            'version': version, 'scope': 'required', **extra}


def prop(name, value):
    return {'name': 'kasmvnc:' + name, 'value': str(value)}


def link_inputs(binary):
    """Read members selected by ld, rather than every archive in the builder."""
    meta = Path(str(binary) + '.static-link.json')
    if meta.exists():
        metadata = json.loads(meta.read_text())
    elif binary.name == 'kasmvncpasswd':
        metadata = {'cwd': str(binary.parent), 'private_archive_copies': {}}
    else:
        raise RuntimeError(f'Missing link metadata: {meta}')
    text = Path(str(binary) + '.link-map').read_text()
    # GNU ld records archive members before its discarded-section listing.
    selected = text.split('Discarded input sections')[0]
    paths = set(re.findall(r'([^\s()]+\.a)\(', selected))
    # Compiler CRT objects are embedded too, despite dynamic glibc.
    paths.update(re.findall(r'^LOAD\s+(\S+\.o)$', text, re.M))
    reverse = {v: k for k, v in metadata['private_archive_copies'].items()}
    result = []
    for name in sorted(paths):
        path = Path(name)
        reject_static_openssl(path)
        if not path.is_absolute():
            path = Path(metadata['cwd']) / path
        path = path.resolve()
        path = Path(reverse.get(str(path), str(path)))
        reject_static_openssl(path)
        # GCC's LTO code/debug objects derive from already catalogued inputs.
        # lto-wrapper.cc debug_objcopy copies .gnu.debuglto sections into
        # make_temp_file(".debug.temp.o"); it is not another source dependency.
        if re.fullmatch(r'cc[^/]+\.(?:ltrans[0-9]+\.ltrans|debug\.temp)\.o', path.name) and path.parent == Path('/tmp'):
            continue
        if SRC not in path.parents:
            reject_static_host_library(path)
            if not path.is_file():
                raise RuntimeError(f'Missing link input {path}')
            result.append(path)
    if not result:
        raise RuntimeError(f'No archive evidence for {binary}')
    return result


def load_frontend(frontdir, www):
    frontend = json.loads((frontdir / 'manifest.json').read_text())
    actual_assets = {str(p.relative_to(www)) for p in (www).rglob('*') if p.is_file()}
    if actual_assets != set(frontend['assets']):
        raise RuntimeError('Frontend asset set does not match provenance')
    for name, digest in frontend['assets'].items():
        if sha(www / name) != digest:
            raise RuntimeError(f'Frontend asset does not match provenance: {name}')
    if sha(frontdir / 'sources.tar.gz') != frontend['source_sha256']:
        raise RuntimeError('Frontend source hash mismatch')
    notices = frontdir / 'notices'
    actual_notices = {str(p.relative_to(notices)) for p in notices.rglob('*') if p.is_file()}
    if not frontend.get('notices') or actual_notices != set(frontend['notices']):
        raise RuntimeError('Frontend notice set does not match provenance')
    for name, digest in frontend['notices'].items():
        if sha(notices / name) != digest:
            raise RuntimeError(f'Frontend notice does not match provenance: {name}')
    return frontend


def archived_notices(archive, destination):
    """Copy notice text from the actual source input, never unpack archive paths."""
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        notices = [m for m in members if m.isfile() and Path(m.name).name.upper().startswith(
            ('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE'))]
        # Some projects (notably libdrm) put their licenses only in source-file
        # headers. Retain those complete files rather than truncate legal text.
        if not notices:
            for member in members:
                if not member.isfile() or Path(member.name).suffix not in ['.c', '.h', '.cc', '.cpp', '.hpp']:
                    continue
                with tar.extractfile(member) as stream:
                    header = stream.read(65536).lower()
                if b'copyright' in header or b'spdx-license-identifier' in header:
                    notices.append(member)
        if not notices:
            raise RuntimeError(f'No license notices found in source archive: {archive}')
        for member in notices:
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or member.size > 10 * 1024 * 1024:
                raise RuntimeError(f'Unsafe license notice member: {member.name}')
            target = destination / path
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as src, target.open('wb') as out:
                shutil.copyfileobj(src, out)


def vendored_notices(source, destination, files):
    """Keep embedded copyright/license headers and any sibling notice texts."""
    destination.mkdir(parents=True, exist_ok=True)
    copied = set()
    for name in files:
        path = source / name
        # Preserve paths to avoid collisions between nested copies.
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        copied.add(name)
        for notice in path.parent.iterdir():
            if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                relative = str(notice.relative_to(source))
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(notice, target)
                copied.add(relative)
    return sorted(copied)


class Inventory:
    def __init__(self, bundle, doc, arch):
        self.bundle, self.doc, self.arch = bundle, doc, arch
        self.components, self.sources, self.records = {}, {}, []
        self.limitations = []
        self.catalog = json.loads((SRC / 'builder/portable/native-sources.json').read_text())
        self.bases = list(dict.fromkeys(Path(os.environ.get(env, default)) for env, default in [
            ('KASMVNC_RUNTIME_SOURCES', '/tmp/static-sources'),
            ('KASMVNC_CODEC_SOURCES', '/tmp/static-sources'),
            ('KASMVNC_DESKTOP_SOURCES', '/tmp/static-desktop-sources'),
            ('KASMVNC_GCC_RUNTIME_BUILD_DIR', '/tmp/kasm-gcc-runtime')])) + [Path('/tmp')]
        (bundle / 'upstream').mkdir()
        (bundle / 'rpm').mkdir()
        (bundle / 'apk').mkdir()
        self.musl = os.environ.get('KASMVNC_LIBC') == 'musl'
        if self.musl:
            spec = importlib.util.spec_from_file_location('apk_sources', SRC / 'builder/musl/dependencies/cache-apk-sources.py')
            self.apk_sources = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.apk_sources)
            self.apk_packages = self.apk_sources.installed()
        (bundle / 'evidence').mkdir()
        (doc / 'third-party-licenses/embedded').mkdir(parents=True, exist_ok=True)

    def preserve(self, path, dest):
        target = self.bundle / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            shutil.copy2(path, target)
        record = {'file': dest, 'sha256': sha(target)}
        self.sources[dest] = record
        return record

    def notices(self, directory, name):
        target = self.doc / 'third-party-licenses/embedded' / name
        target.mkdir(exist_ok=True)
        for path in directory.iterdir():
            if path.is_file() and path.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                shutil.copy2(path, target / path.name)

    def upstream(self, entry):
        name = entry['name']
        if name in self.components:
            return self.components[name]
        pattern = entry['source']
        archives = sorted({p for base in self.bases for p in base.glob(pattern + '.tar.*') if p.is_file()})
        repos = [base / pattern for base in self.bases if (base / pattern / '.git').exists()]
        properties = [prop('delivery', 'embedded-static')]
        if repos:
            directory = repos[0]
            revision = run('git', '-C', directory, 'rev-parse', 'HEAD')
            origin = run('git', '-C', directory, 'remote', 'get-url', 'origin')
            dest = 'upstream/' + name + '-' + revision + '.tar'
            subprocess.run(['git', '-C', str(directory), 'archive', '--format=tar',
                            '-o', str(self.bundle / dest), revision], check=True)
            record = {'file': dest, 'sha256': sha(self.bundle / dest), 'repository': origin, 'revision': revision}
            self.sources[dest] = record
            version = revision
            if entry.get('pc'):
                pcs = sorted({p.resolve() for base in [os.environ.get('KASMVNC_STATIC_RUNTIME_PREFIX', '/opt/kasm-runtime'),
                                                    os.environ.get('KASMVNC_CODEC_PREFIX', '/opt/kasm-codecs'),
                                                    os.environ.get('KASMVNC_STATIC_DESKTOP_PREFIX', '/opt/kasm-desktop')]
                       for p in Path(base).glob('lib*/pkgconfig/' + entry['pc'] + '.pc')})
                versions = {re.search(r'^Version:\s*(.+)', pc.read_text(), re.M)[1] for pc in pcs}
                if len(versions) != 1:
                    raise RuntimeError(f'Expected unique pkg-config version for {name}: {pcs}')
                version = versions.pop()
            if not re.fullmatch(r'\d+(?:\.\d+)+', version):
                self.limitations.append(f'{name}: version {version} is not an upstream release number; advisory matching may be incomplete. Exact Git revision is retained.')
            properties += [prop('source-revision', revision), prop('source-repository', origin)]
        else:
            if len(archives) != 1:
                raise RuntimeError(f'Expected unique source archive for {name}: {archives}')
            archive = archives[0]
            record = self.preserve(archive, 'upstream/' + archive.name)
            stem = archive.name.split('.tar.')[0]
            version = re.search(r'([0-9]+(?:[._][0-9]+)+)$', stem)
            if not version:
                raise RuntimeError(f'Cannot identify source version: {archive}')
            version = version[1].replace('_', '.')
            dirs = [p for p in archive.parent.glob(pattern) if p.is_dir()]
            # Legacy helpers unpack to a fixed name, and systemd uses stable-.
            dirs += [archive.parent / n for n in (name, 'cpuid' if name == 'libcpuid' else name,
                                                'tbb' if name == 'onetbb' else name)
                     if (archive.parent / n).is_dir()]
            directory = dirs[0] if dirs else None
        c = component(name, version, purl=f'pkg:generic/{quote(name)}@{quote(version)}',
                      properties=properties + [prop('source-file', record['file']), prop('source-sha256', record['sha256'])])
        # Backports keep the upstream version truthful. Preserve the exact patch
        # bytes alongside upstream sources so consumers can reproduce/review them.
        patch_dir = SRC / 'builder/scripts/portable-dependencies/patches'
        # An explicit list can retire applied backports without deleting their
        # historical assessment evidence from the repository. In particular,
        # zlib 1.4 already contains the fix that was backported onto 1.3.2.
        patch_names = entry.get('patches')
        if patch_names is None:
            patches = sorted(patch_dir.glob(name + '-*.patch'))
        else:
            if not isinstance(patch_names, list) or any(
                    not isinstance(p, str) or Path(p).name != p or
                    not p.startswith(name + '-') or not p.endswith('.patch')
                    for p in patch_names):
                raise RuntimeError('Invalid source patch list for ' + name)
            patches = [patch_dir / p for p in patch_names]
        for patch in patches:
            if not patch.is_file():
                raise RuntimeError('Missing source patch: ' + str(patch))
            patch_record = self.preserve(patch, 'upstream/' + patch.name)
            c['properties'].append(prop('source-patch', json.dumps(patch_record, sort_keys=True)))
        if entry.get('cpe') and re.fullmatch(r'\d+(?:\.\d+)+', version):
            c['cpe'] = f'cpe:2.3:a:{entry["cpe"]}:{version}:*:*:*:*:*:*:*'
        if directory:
            self.notices(directory, name)
        archived_notices(self.bundle / record['file'], self.doc / 'third-party-licenses/embedded' / name)
        # Full upstream archives retain all license texts, including nested ones.
        c['properties'].append(prop('license-notices', 'Corresponding source archive and third-party-licenses/embedded/' + name))
        inv_license = entry.get('license')
        if inv_license:
            c['licenses'] = [{'expression': inv_license}]
        else:
            self.limitations.append(f'{name}: license expression not normalized; inspect included upstream license notices.')
        self.components[name] = c
        return c

    def rpm(self, path):
        qf = '%{NAME}\t%{EPOCHNUM}\t%{VERSION}\t%{RELEASE}\t%{ARCH}\t%{LICENSE}\t%{SOURCERPM}'
        name, epoch, version, release, arch, license_, srpm = run('rpm', '-qf', '--qf', qf, path).split('\t')
        key = name + '-' + version + '-' + release
        if key not in self.components:
            target = self.bundle / 'rpm' / srpm
            if not target.exists():
                cache = Path('/opt/kasm-rpm-sources')
                if cache.exists():
                    url = cached_rpm_source(cache, srpm, target)
                else:
                    machine = 'x86_64' if self.arch == 'amd64' else 'aarch64'
                    for repo in ['baseos/latest', 'appstream', 'codeready/builder', 'developer/EPEL']:
                        url = f'https://yum.oracle.com/repo/OracleLinux/OL9/{repo}/{machine}/getPackageSource/{srpm}'
                        result = subprocess.run(['curl', '-fLsS', '--retry', '3', '--connect-timeout', '30', '--max-time', '600', url, '-o', str(target)])
                        if result.returncode == 0:
                            break
                    else:
                        raise RuntimeError(f'Exact corresponding SRPM unavailable: {srpm}')
                identity = run('rpm', '-qp', '--qf', '%{NAME}-%{VERSION}-%{RELEASE}.src.rpm', target)
                if identity != srpm:
                    raise RuntimeError(f'SRPM identity mismatch: {identity} != {srpm}')
                # Reject unsigned or unknown-key source RPMs; the builder already
                # trusts Oracle's packaged signing keys for its binary archives.
                signature = run('rpmkeys', '--checksig', target)
                if 'signatures OK' not in signature:
                    raise RuntimeError('SRPM signature check failed: ' + signature)
                self.sources['rpm/' + srpm] = {'file': 'rpm/' + srpm, 'sha256': sha(target), 'url': url,
                                             'signature': signature.split(': ', 1)[-1]}
            ver = (epoch + ':' if epoch != '0' else '') + version + '-' + release
            c = component(name, ver, purl=f'pkg:rpm/oracle/{quote(name)}@{quote(ver)}?arch={arch}&distro=oraclelinux-9',
                          licenses=[{'license': {'name': license_}}], properties=[prop('delivery', 'embedded-static'),
                          prop('source-rpm', srpm), prop('rpm-epoch', epoch), prop('source-sha256', sha(target))])
            self.components[key] = c
            # Static subpackages often inherit notices from an installed sibling
            # package. Select only siblings built from this exact source RPM.
            license_paths = rpm_notice_paths(srpm)
            found_notice = False
            for p in sorted(license_paths):
                if Path(p).is_file():
                    found_notice = True
                    dest = self.doc / 'third-party-licenses/embedded' / name
                    target = dest / Path(p).relative_to('/')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, target)
            if not found_notice:
                raise RuntimeError(f'No installed license notices for source RPM {srpm}; inspect its source notices')
        return self.components[key]

    def apk(self, package):
        key = package['P'] + '-' + package['V']
        if key not in self.components:
            archive, record = self.apk_sources.checked_source(Path('/opt/kasm-apk-sources'), package)
            dest = 'apk/' + archive.name
            self.preserve(archive, dest)
            self.sources[dest].update({k: record[k] for k in ['url', 'aports_commit', 'checksums']})
            c = component(package['P'], package['V'],
                purl=f"pkg:apk/alpine/{quote(package['P'])}@{quote(package['V'])}?arch={package['A']}&distro=alpine-3.21",
                licenses=[{'license': {'name': package['L']}}], properties=[
                    prop('delivery', 'embedded-static'), prop('apk-origin', package['o']),
                    prop('aports-commit', package['c']), prop('apk-package-checksum', package['C']),
                    prop('source-file', dest), prop('source-sha256', record['sha256'])])
            notice_dir = self.doc / 'third-party-licenses/embedded' / package['P']
            notice_dir.mkdir(exist_ok=True)
            # The exact APKBUILD source bundle contains nested upstream release
            # archives. Extract only into a disposable directory with data filter.
            with tempfile.TemporaryDirectory(prefix='kasm-apk-notices-') as tmp:
                stage = Path(tmp)
                with tarfile.open(archive) as tf:
                    tf.extractall(stage, filter='data')
                for source in (stage / 'upstream').iterdir():
                    if source.is_file() and tarfile.is_tarfile(source):
                        archived_notices(source, notice_dir / source.name)
                for notice in stage.rglob('*'):
                    if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                        target = notice_dir / notice.relative_to(stage)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(notice, target)
            if not any(p.is_file() for p in notice_dir.rglob('*')):
                raise RuntimeError('Missing corresponding APK license notices: ' + key)
            self.components[key] = c
        return self.components[key]

    def archive(self, path):
        reject_static_openssl(path)
        reject_static_host_library(path)
        # Package ownership wins over an upstream name (EL backports matter).
        package = self.apk_sources.owner(path, self.apk_packages) if self.musl else None
        if package:
            c = self.apk(package)
        elif not self.musl and subprocess.run(['rpm', '-qf', str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            c = self.rpm(path)
        else:
            lib = re.sub(r'^lib|\.a$', '', path.name)
            entries = [e for e in self.catalog if any(fnmatch.fnmatch(lib, pat) for pat in e['libraries'])]
            if len(entries) != 1:
                raise RuntimeError(f'Uncatalogued static archive {path}')
            c = self.upstream(entries[0])
        kinds = {r['kind'] for r in self.records if r['component'] == c['bom-ref']}
        kind = 'startup-object' if path.suffix == '.o' else 'static-archive'
        kinds.add(kind)
        c['properties'] = [p for p in c.get('properties', []) if p['name'] not in ['kasmvnc:delivery', 'kasmvnc:native-input-kinds']]
        c['properties'] += [prop('delivery', 'embedded-startup-object' if kinds == {'startup-object'} else 'embedded-static'),
                            prop('native-input-kinds', ','.join(sorted(kinds)))]
        self.records.append({'path': str(path), 'kind': kind, 'sha256': sha(path), 'component': c['bom-ref']})
        return c['bom-ref']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('doc', type=Path)
    parser.add_argument('arch', choices=['amd64', 'arm64'])
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='kasm-sources-') as tmp:
        bundle = Path(tmp)
        inv = Inventory(bundle, args.doc, args.arch)
        shutil.copy2(SRC / 'builder/portable/kasmvnc-source.tar', bundle / 'kasmvnc-source.tar')
        inv.sources['kasmvnc-source.tar'] = {'file': 'kasmvnc-source.tar', 'sha256': sha(bundle / 'kasmvnc-source.tar'),
                                           'revision': os.environ['KASMVNC_COMMIT_ID']}
        deps = set()
        openssl_root = sdk_path()
        openssl_sdk = read_sdk(openssl_root)
        openssl_exports = sdk_exports(openssl_root, openssl_sdk)
        for name in BINARIES:
            binary = SRC / name
            if not inv.musl:
                audit_glibc_startup(binary)
            selected_inputs = link_inputs(binary)
            if binary.name == 'Xvnc' and not any(path.name == 'libz.a' for path in selected_inputs):
                raise RuntimeError('Xvnc is missing its private static zlib link input')
            reject_embedded_symbols(binary, openssl_exports, selected_inputs)
            needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', run('readelf', '-dW', binary))
            audit_openssl(binary, needed, openssl_exports)
            audit_host_libraries(binary, needed, 'musl' if inv.musl else 'glibc')
            for path in selected_inputs:
                deps.add(inv.archive(path))
            for suffix in ['.static-link.json', '.link-map']:
                if Path(str(binary) + suffix).exists():
                    shutil.copy2(str(binary) + suffix, bundle / 'evidence' / (binary.name + suffix))
        xorg = inv.upstream({'name': 'xorg-server', 'source': 'xorg-server-*', 'cpe': 'x.org:x_server'})
        deps.add(xorg['bom-ref'])
        # Header inputs/generators are supplied for rebuilding, but aren't
        # represented as shipped static libraries merely because they were used.
        inv.preserve(openssl_root / 'share/kasmvnc/openssl-sdk.json', 'openssl-build-sdk.json')
        sdk_source = openssl_root / openssl_sdk['source_archive']
        sdk_record = inv.preserve(sdk_source, 'build-only/' + sdk_source.name)
        sdk_record.update({'url': openssl_sdk['source_url'],
                           'purpose': 'Build-only OpenSSL 3.0 SDK; runtime libraries supplied by host'})
        archived_notices(bundle / sdk_record['file'], args.doc / 'third-party-licenses/build-only/openssl-sdk')
        host_openssl = {'type': 'library', 'bom-ref': 'host:openssl3', 'name': 'OpenSSL',
                        'scope': 'required', 'properties': [prop('delivery', 'host-runtime'),
                            prop('required-sonames', 'libssl.so.3,libcrypto.so.3'),
                            prop('symbol-version-maximum', 'OPENSSL_3.0.0'),
                            prop('version-source', 'Installed host package; no runtime version assigned from builder SDK')]}
        inv.components['host:openssl3'] = host_openssl
        deps.add(host_openssl['bom-ref'])
        for pattern in ['xcb-proto-*.tar.*']:
            for base in inv.bases:
                for p in base.glob(pattern):
                    inv.preserve(p, 'upstream/' + p.name)
        ffmpeg_mode = os.environ.get('KASMVNC_FFMPEG_MODE', 'static')
        adapter_headers = None
        if ffmpeg_mode == 'adapters':
            # A cached builder may contain these archives, but adapter packages
            # must never describe or ship them as part of their static closure.
            if {'ffmpeg', 'x264', 'x265', 'svt-av1'} & inv.components.keys():
                raise RuntimeError('Runtime FFmpeg adapter build unexpectedly embeds codec libraries')
            header_root = Path(os.environ.get('KASMVNC_FFMPEG_HEADERS_ROOT', '/opt/kasm-ffmpeg-headers'))
            adapter_headers = load_adapter_headers(header_root)
            inv.preserve(header_root / 'manifest.json', 'ffmpeg-adapter-headers.json')
            for item in adapter_headers['families']:
                record = inv.preserve(header_root / 'sources' / item['archive'], 'upstream/' + item['archive'])
                record.update({'url': item['url'], 'purpose': 'Runtime ABI adapter build headers',
                               'avcodec_major': item['avcodec_major']})
                name = 'ffmpeg-headers-' + str(item['avcodec_major'])
                archived_notices(bundle / record['file'], args.doc / 'third-party-licenses/embedded' / name)
                c = component('ffmpeg-headers', item['version'],
                    licenses=[{'expression': 'LGPL-2.1-or-later'}],
                    properties=[prop('delivery', 'build-headers'), prop('avcodec-major', item['avcodec_major']),
                        prop('source-sha256', item['sha256']), prop('source-file', record['file']),
                        prop('runtime-implementation', 'Host-supplied FFmpeg; no FFmpeg or encoder library embedded')])
                inv.components[name] = c
                deps.add(c['bom-ref'])
        elif ffmpeg_mode == 'static':
            headers = Path(os.environ.get('KASMVNC_CODEC_SOURCES', '/tmp/static-sources')) / 'nv-codec-headers'
            if not (headers / '.git').exists():
                raise RuntimeError('Missing NVIDIA codec header sources')
            revision = run('git', '-C', headers, 'rev-parse', 'HEAD')
            subprocess.run(['git', '-C', str(headers), 'archive', '--format=tar', '-o', str(bundle / 'upstream/nv-codec-headers.tar'), revision], check=True)
            inv.sources['upstream/nv-codec-headers.tar'] = {'file': 'upstream/nv-codec-headers.tar',
                'sha256': sha(bundle / 'upstream/nv-codec-headers.tar'), 'revision': revision, 'purpose': 'build headers'}
        else:
            raise RuntimeError('Unsupported FFmpeg mode: ' + ffmpeg_mode)
        # Vendored code has exact file hashes even when upstream version is absent.
        for name, files in [('qoi', ['common/rfb/qoi.h']), ('xxhash', ['common/rfb/xxhash.h', 'common/rfb/xxhash.c']),
                            ('tinyxml2', ['third_party/tinyxml2/tinyxml2.h', 'third_party/tinyxml2/tinyxml2.cpp']),
                            ('websocket', ['common/network/websocket.c', 'common/network/websocket.h', 'common/network/websockify.c']),
                            ('cjson', ['common/network/cJSON.h', 'common/network/cJSON.c']),
                            ('d3des', ['common/rfb/d3des.c', 'common/rfb/d3des.h']),
                            ('x11-vendored', ['common/rfb/keysymdef.h', 'unix/xserver/hw/vnc/xvnc.c'] +
                             sorted(str(p.relative_to(SRC)) for p in (SRC / 'common/Xregion').iterdir()
                                    if p.is_file() and p.suffix in ['.c', '.h'])),
                            ('webudp', sorted(str(p.relative_to(SRC)) for p in (SRC / 'common/network/webudp').iterdir()
                                              if p.is_file() and p.suffix in ['.cpp', '.h', '.hpp']))]:
            version = 'git-' + os.environ['KASMVNC_COMMIT_ID']
            text = (SRC / files[0]).read_text()
            if name == 'xxhash':
                version = '.'.join(re.search(r'#define XXH_VERSION_' + part + r'\s+(\d+)', text)[1]
                                   for part in ['MAJOR', 'MINOR', 'RELEASE'])
            elif name == 'tinyxml2':
                version = '.'.join(re.search(r'#define TINYXML2_' + part + r'_VERSION\s+(\d+)', text)[1]
                                   for part in ['MAJOR', 'MINOR', 'PATCH'])
            elif name == 'cjson':
                version = '.'.join(re.search(r'#define CJSON_VERSION_' + part + r'\s+(\d+)', text)[1]
                                   for part in ['MAJOR', 'MINOR', 'PATCH'])
            elif name == 'd3des':
                version = re.search(r'D3DES \(V([0-9.]+)\)', text)[1]
            c = component(name, version, properties=[prop('delivery', 'vendored-source'),
                prop('file-sha256', json.dumps({p: sha(SRC / p) for p in files}, sort_keys=True))],
                licenses=[{'expression': {'qoi': 'MIT', 'xxhash': 'BSD-2-Clause', 'tinyxml2': 'Zlib',
                                         'websocket': 'LGPL-3.0-only', 'webudp': 'MIT', 'cjson': 'MIT',
                                         'd3des': 'LicenseRef-D3DES-VNC', 'x11-vendored': 'LicenseRef-X11-Vendored'}[name]}])
            if name in ['d3des', 'x11-vendored']:
                c['licenses'] = [{'license': {'name': {'d3des': 'Public-domain D3DES with VNC modification notices',
                    'x11-vendored': 'X11/Open Group, DEC and X Consortium permissive notices'}[name]}}]
                c['properties'].append(prop('modified-source', 'Inherited VNC fork code; source hashes identify the supplied variant'))
            if name in ['tinyxml2', 'xxhash', 'cjson']:
                vendor = {'tinyxml2': 'tinyxml2_project', 'xxhash': 'xxhash_project', 'cjson': 'davegamble'}[name]
                c['cpe'] = f'cpe:2.3:a:{vendor}:{name}:{version}:*:*:*:*:*:*:*'
                c['purl'] = f'pkg:generic/{name}@{version}'
            notice_inputs = files + (['builder/portable/licenses/LGPL-3.0.txt', 'builder/portable/licenses/GPL-3.0.txt']
                                     if name == 'websocket' else [])
            notices = vendored_notices(SRC, args.doc / 'third-party-licenses/embedded' / name, notice_inputs)
            c['properties'].append(prop('license-notice-files', json.dumps(notices)))
            inv.components['vendored-' + name] = c
            deps.add(c['bom-ref'])
        frontdir = SRC / 'builder/www-supply-chain'
        frontend = load_frontend(frontdir, SRC / 'builder/www')
        shutil.copytree(frontdir / 'notices', args.doc / 'third-party-licenses/frontend', dirs_exist_ok=True)
        inv.preserve(frontdir / 'sources.tar.gz', 'frontend-sources.tar.gz')
        inv.preserve(frontdir / 'manifest.json', 'frontend-manifest.json')
        inv.limitations.extend(frontend.get('limitations', []))
        for name, digest in frontend['assets'].items():
            if name.endswith('.wasm'):
                c = {'type': 'file', 'bom-ref': 'wasm:' + digest, 'name': name,
                     'hashes': [{'alg': 'SHA-256', 'content': digest}],
                     'properties': [prop('source-status', 'Version, license and corresponding source unverified; see frontend limitations')]}
                inv.components['wasm-' + digest] = c
                deps.add(c['bom-ref'])
        front = component(frontend['name'], frontend['revision'], properties=[prop('delivery', 'bundled-web-client')])
        inv.components['frontend'] = front
        deps.add(front['bom-ref'])
        for item in frontend['components']:
            c = component(item['name'], item['version'], purl=item['purl'],
                licenses=[{'license': {'name': item.get('license') or 'Unknown'}}],
                properties=[prop('delivery', 'bundled-web-dependency'), prop('source', item['source'])])
            inv.components['npm-' + c['bom-ref']] = c
            deps.add(c['bom-ref'])
        for item in frontend.get('vendored_components', []):
            c = component(item['name'], item['version'],
                licenses=[{'license': {'name': item['license']}}],
                hashes=[{'alg': 'SHA-256', 'content': item['file_sha256']}],
                properties=[prop('delivery', 'vendored-web-source'), prop('source', item['source'])])
            inv.components['web-vendored-' + c['bom-ref']] = c
            deps.add(c['bom-ref'])
        audit = json.loads((args.doc / 'elf-audit.json').read_text())
        host = {'shared_libraries': sorted({lib for elf in audit for lib in elf['needed']}),
                'other_requirements': ['musl >= 1.2.5' if inv.musl else 'glibc >= 2.34', 'Bash and host timezone data',
                    'XKB tools/data', 'desktop/session applications', 'GPU vendor drivers/loaders when enabled'],
                'versions': 'Supplied and patched by the host; deliberately not assigned builder versions.'}
        host['desktop_mode'] = 'host-shared'
        host['compiler_runtime'] = {
            'selection': 'Host libstdc++.so.6 and libgcc_s.so.1; selected startup/builtin and toolset nonshared compatibility inputs remain embedded.',
            'symbol_version_maximum': COMPILER_MAXIMUM,
            'symbol_exceptions': compiler_exceptions(),
            'musl_qualification': 'Unversioned C++ imports require native baseline builds and runtime-matrix execution.',
        }
        host['private_dependency_closure'] = 'The host desktop DSOs own their transitive dependencies; versions and security fixes are determined by the deployed distro.'
        host['zlib'] = 'KasmVNC embeds a hidden private zlib for compression performance; host PNG/font DSOs retain their own distro zlib.'
        for item in host_components('musl' if inv.musl else 'glibc'):
            required = sorted(set(item['sonames']) & set(host['shared_libraries']))
            if not required:
                continue
            ref = 'host:' + item['name']
            c = {'type': 'library', 'bom-ref': ref, 'name': item['name'], 'scope': 'required',
                 'properties': [prop('delivery', 'host-runtime'),
                                prop('required-sonames', ','.join(required)),
                                prop('version-source', 'Installed host package; no runtime version assigned from builder') ]}
            inv.components[ref] = c
            deps.add(ref)
        host['runtime_openssl'] = {
            'sonames': ['libssl.so.3', 'libcrypto.so.3'], 'abi_baseline': 'OPENSSL_3.0.0',
            'selection': 'Required system shared libraries; no embedded runtime or private shared-library fallback.',
            'configuration': 'Host providers, OpenSSL configuration and distribution crypto policy.',
            'updates': 'Update host packages and restart Xvnc sessions; refresh/recreate immutable containers.',
        }
        if adapter_headers:
            host['runtime_ffmpeg'] = {
                'selection': 'Optional runtime loading; video unavailable without a supported FFmpeg family and encoder.',
                'families': [item['libraries'] for item in adapter_headers['families']],
                'libavformat': 'Required only for file-based benchmarks.',
                'encoders': 'Host FFmpeg build and host encoder libraries determine codec availability and performance.',
            }
        inv.limitations += ['Link evidence records archive members selected by ld; a library entry does not imply every upstream feature is enabled.',
            'Vendored source components without upstream release versions may not be matched by vulnerability databases.',
            'Packages marked embedded-startup-object contribute CRT objects only, not a full static libc or compiler.',
            'An SBOM and source bundle are not a legal certification; review license obligations and recorded source limitations before public release.']
        manifest = {'format': 'kasmvnc-corresponding-sources-v1', 'revision': os.environ['KASMVNC_COMMIT_ID'],
                    'architecture': args.arch, 'sources': list(inv.sources.values()), 'link_inputs': inv.records,
                    'source_binary_sha256': {name: sha(SRC / name) for name in BINARIES},
                    'ffmpeg_mode': ffmpeg_mode, 'openssl_mode': 'host-shared-3',
                    'desktop_mode': 'host-shared', 'compiler_runtime_mode': 'host-shared',
                    'build_dependencies': {'openssl_sdk': openssl_sdk},
                    'host_requirements': host, 'limitations': inv.limitations}
        write(bundle / 'manifest.json', manifest)
        (bundle / 'README.txt').write_text('KasmVNC portable source inputs\n\nExtract kasmvnc-source.tar for source, patches and builder scripts.\n'
            'upstream/ contains exact release archives and Git snapshots; rpm/ contains signed exact source RPMs; apk/ contains exact aports recipes/patches and checksum-verified upstream inputs.\n'
            'frontend-sources.tar.gz contains the web client source inventory.\n'
            'build-only/ contains the OpenSSL SDK source used to compile/link; its runtime is not bundled.\n'
            f'Build entry point: builder/build-portable{" --libc musl" if inv.musl else ""} --ffmpeg-mode {ffmpeg_mode}; see builder documentation.\n'
            'It downloads dependencies and is not an offline or bit-reproducible rebuild command.\n'
            'For exact input reproduction restore these archives and recorded Git revisions, rather than following floating branches.\n'
            'Read manifest.json limitations before claiming source-compliance completeness.\n')
        root = component('KasmVNC', os.environ['KASMVNC_PORTABLE_VERSION'], kind='application',
                         properties=[prop('revision', os.environ['KASMVNC_COMMIT_ID']), prop('architecture', args.arch), prop('libc', 'musl' if inv.musl else 'glibc'),
                                     prop('ffmpeg-mode', ffmpeg_mode), prop('openssl-mode', 'host-shared-3'),
                                     prop('desktop-mode', 'host-shared'), prop('compiler-runtime-mode', 'host-shared'),
                                     prop('host-requirements', json.dumps(host)), prop('limitations', json.dumps(inv.limitations))])
        bom = {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
               'metadata': {'timestamp': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'component': root},
               'components': sorted(inv.components.values(), key=lambda c: c['bom-ref']),
               'dependencies': [{'ref': root['bom-ref'], 'dependsOn': sorted(deps)}]}
        write(str(args.output) + '.cdx.json', bom)
        write(str(args.output) + '.sources.json', manifest)
        # No recursive /src or /tmp snapshots: only explicit source inputs.
        subprocess.run(['tar', '--sort=name', '--owner=0', '--group=0', '--numeric-owner', '-czf',
                        str(args.output) + '.sources.tar.gz', '-C', str(bundle), '.'], check=True)
        shutil.copy2(str(args.output) + '.cdx.json', args.doc / 'sbom.cdx.json')
        shutil.copy2(str(args.output) + '.sources.json', args.doc / 'sources.json')


if __name__ == '__main__':
    main()
