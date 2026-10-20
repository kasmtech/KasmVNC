#!/usr/bin/env python3
"""Build a native-link-evidence SBOM and corresponding-source artifact.

Fail closed on unknown native link inputs, missing source or mismatched frontend.
This describes supplied components, not a legal certification of compliance.
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import quote

SRC = Path('/src')
BINARIES = ['unix/xserver/hw/vnc/Xvnc', 'unix/vncconfig/vncconfig',
            'unix/kasmvncpasswd/kasmvncpasswd', 'unix/kasmxproxy/kasmxproxy']


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
        if not path.is_absolute():
            path = Path(metadata['cwd']) / path
        path = path.resolve()
        path = Path(reverse.get(str(path), str(path)))
        # GCC's temporary LTO objects are generated from already catalogued inputs.
        if re.fullmatch(r'cc[^/]+\.ltrans[0-9]+\.ltrans\.o', path.name) and path.parent == Path('/tmp'):
            continue
        if SRC not in path.parents:
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
        (bundle / 'evidence').mkdir()
        (doc / 'third-party-licenses/embedded').mkdir(parents=True, exist_ok=True)

    def preserve(self, path, dest):
        target = self.bundle / dest
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

    def archive(self, path):
        # Package ownership wins over an upstream name (EL backports matter).
        if subprocess.run(['rpm', '-qf', str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
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
        for name in BINARIES:
            binary = SRC / name
            for path in link_inputs(binary):
                deps.add(inv.archive(path))
            for suffix in ['.static-link.json', '.link-map']:
                if Path(str(binary) + suffix).exists():
                    shutil.copy2(str(binary) + suffix, bundle / 'evidence' / (binary.name + suffix))
        xorg = inv.upstream({'name': 'xorg-server', 'source': 'xorg-server-*', 'cpe': 'x.org:x_server'})
        deps.add(xorg['bom-ref'])
        # Header inputs/generators are supplied for rebuilding, but aren't
        # represented as shipped static libraries merely because they were used.
        for pattern in ['xcb-proto-*.tar.*']:
            for base in inv.bases:
                for p in base.glob(pattern):
                    inv.preserve(p, 'upstream/' + p.name)
        headers = Path(os.environ.get('KASMVNC_CODEC_SOURCES', '/tmp/static-sources')) / 'nv-codec-headers'
        if not (headers / '.git').exists():
            raise RuntimeError('Missing NVIDIA codec header sources')
        revision = run('git', '-C', headers, 'rev-parse', 'HEAD')
        subprocess.run(['git', '-C', str(headers), 'archive', '--format=tar', '-o', str(bundle / 'upstream/nv-codec-headers.tar'), revision], check=True)
        inv.sources['upstream/nv-codec-headers.tar'] = {'file': 'upstream/nv-codec-headers.tar',
            'sha256': sha(bundle / 'upstream/nv-codec-headers.tar'), 'revision': revision, 'purpose': 'build headers'}
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
                'other_requirements': ['glibc >= 2.34', 'Perl and package-declared Perl modules',
                    'XKB tools/data', 'desktop/session applications', 'GPU vendor drivers/loaders when enabled'],
                'versions': 'Supplied and patched by the host; deliberately not assigned builder versions.'}
        inv.limitations += ['Link evidence records archive members selected by ld; a library entry does not imply every upstream feature is enabled.',
            'Vendored source components without upstream release versions may not be matched by vulnerability databases.',
            'Packages marked embedded-startup-object contribute CRT objects only, not a full static libc or compiler.',
            'An SBOM and source bundle are not a legal certification; review license obligations and recorded source limitations before public release.']
        manifest = {'format': 'kasmvnc-corresponding-sources-v1', 'revision': os.environ['KASMVNC_COMMIT_ID'],
                    'architecture': args.arch, 'sources': list(inv.sources.values()), 'link_inputs': inv.records,
                    'source_binary_sha256': {name: sha(SRC / name) for name in BINARIES},
                    'host_requirements': host, 'limitations': inv.limitations}
        write(bundle / 'manifest.json', manifest)
        (bundle / 'README.txt').write_text('KasmVNC portable source inputs\n\nExtract kasmvnc-source.tar for source, patches and builder scripts.\n'
            'upstream/ contains exact release archives and Git snapshots; rpm/ contains signed exact source RPMs with distro patches/specs.\n'
            'frontend-sources.tar.gz contains the web client source inventory.\n'
            'Build entry point: builder/build-portable; see builder/portable/README.md.\n'
            'It downloads dependencies and is not an offline or bit-reproducible rebuild command.\n'
            'For exact input reproduction restore these archives and recorded Git revisions, rather than following floating branches.\n'
            'Read manifest.json limitations before claiming source-compliance completeness.\n')
        root = component('KasmVNC', os.environ['KASMVNC_PORTABLE_VERSION'], kind='application',
                         properties=[prop('revision', os.environ['KASMVNC_COMMIT_ID']), prop('architecture', args.arch),
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
