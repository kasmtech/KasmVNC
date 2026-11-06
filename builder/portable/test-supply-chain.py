#!/usr/bin/env python3
"""Failure-boundary checks; run inside the portable builder, never on the host."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import tarfile
import io

spec = importlib.util.spec_from_file_location('supply', Path(__file__).with_name('supply-chain.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def rejects(call, message):
    try:
        call()
    except RuntimeError as error:
        assert message in str(error), str(error)
    else:
        raise AssertionError('Expected rejection: ' + message)


# RPMs may tag license texts as %doc; never borrow unrelated RPM notices.
original_run = m.run
rpm_fixture = {
    ('rpm', '-qa', '--qf', '%{NAME}\t%{SOURCERPM}\n'): 'compiler\tcompiler.src.rpm\ncompiler-libs\tcompiler.src.rpm\nunrelated\tother.src.rpm',
    ('rpm', '-q', '--licensefiles', 'compiler'): '',
    ('rpm', '-ql', 'compiler'): '/opt/compiler/share/doc/COPYING.RUNTIME\n/opt/compiler/bin/gcc',
    ('rpm', '-q', '--licensefiles', 'compiler-libs'): '/opt/compiler/licenses/LICENSE',
    ('rpm', '-ql', 'compiler-libs'): '/opt/compiler/lib/libstdc++.a',
}
try:
    m.run = lambda *args: rpm_fixture[args]
    assert m.rpm_notice_paths('compiler.src.rpm') - {''} == {
        '/opt/compiler/share/doc/COPYING.RUNTIME', '/opt/compiler/licenses/LICENSE'}
finally:
    m.run = original_run


with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp)
    source, cache = base / 'source', base / 'cache'
    catalog_path = source / 'builder/portable/native-sources.json'
    catalog_path.parent.mkdir(parents=True)
    catalog = json.loads(Path(__file__).with_name('native-sources.json').read_text())
    catalog_path.write_text(json.dumps(catalog))
    entry = next(item for item in catalog if item['name'] == 'zlib')
    cache.mkdir()
    # A previous release can remain in a reused source cache. The current
    # inventory must select the pinned source and omit its retired backport.
    for stem in ('zlib-1.3.2', entry['source']):
        with tarfile.open(cache / (stem + '.tar.gz'), 'w:gz') as tar:
            info = tarfile.TarInfo(stem + '/LICENSE')
            info.size = len(b'fixture license')
            tar.addfile(info, io.BytesIO(b'fixture license'))
    patch_dir = source / 'builder/scripts/portable-dependencies/patches'
    patch_dir.mkdir(parents=True)
    historical = patch_dir / 'zlib-CVE-2026-85091.patch'
    historical.write_bytes(b'Historical backport evidence')
    saved_source = m.SRC
    try:
        m.SRC = source

        def inventory(index):
            bundle, doc = base / ('bundle-' + index), base / ('doc-' + index)
            bundle.mkdir(); doc.mkdir()
            inv = m.Inventory(bundle, doc, 'amd64')
            inv.bases = [cache]
            return inv

        inv = inventory('current')
        current = inv.upstream(entry)
        assert current['version'] == '1.4'
        assert current['purl'] == 'pkg:generic/zlib@1.4'
        assert not any(prop['name'] == 'kasmvnc:source-patch' for prop in current['properties'])
        assert list(inv.sources) == ['upstream/zlib-1.4.tar.gz']
        assert historical.read_bytes() == b'Historical backport evidence'
        # Existing applied-patch discovery remains intact for other entries,
        # while explicit lists must name real, component-owned patch files.
        default = {key: value for key, value in entry.items() if key != 'patches'}
        for index, selected in [('default', default),
                                ('explicit', dict(entry, patches=[historical.name]))]:
            component = inventory(index).upstream(selected)
            patches = [json.loads(prop['value']) for prop in component['properties']
                       if prop['name'] == 'kasmvnc:source-patch']
            assert patches == [{'file': 'upstream/' + historical.name, 'sha256': m.sha(historical)}]
        rejects(lambda: inventory('invalid').upstream(dict(entry, patches=['../outside.patch'])),
                'Invalid source patch list')
        rejects(lambda: inventory('missing').upstream(dict(entry, patches=['zlib-missing.patch'])),
                'Missing source patch')
    finally:
        m.SRC = saved_source


with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp)
    adapter = base / 'adapter'; adapter.mkdir()
    (adapter / 'sources').mkdir()
    families = []
    header_hashes = {}
    for major in range(58, 64):
        for name in ('libavcodec/avcodec.h', 'libavcodec/version.h', 'libavutil/avconfig.h'):
            header = adapter / f'{major}/include/{name}'
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_bytes(b'isolated headers')
            header_hashes[str(header.relative_to(adapter))] = m.sha(header)
        archive_name = f'ffmpeg-{major}.tar.xz'
        archive = adapter / 'sources' / archive_name
        archive.write_bytes(str(major).encode())
        families.append({'avcodec_major': major, 'archive': archive_name, 'sha256': m.sha(archive)})
    adapter_manifest = {'format': 'kasmvnc-ffmpeg-adapter-headers-v1', 'families': families,
                        'headers': header_hashes}
    (adapter / 'manifest.json').write_text(json.dumps(adapter_manifest))
    assert m.load_adapter_headers(adapter)['families'] == families
    header.write_bytes(b'tampered')
    rejects(lambda: m.load_adapter_headers(adapter), 'header hash mismatch')
    header.write_bytes(b'isolated headers')
    archive.write_bytes(b'tampered')
    rejects(lambda: m.load_adapter_headers(adapter), 'source hash mismatch')
    archive.write_bytes(b'63')
    adapter_manifest['headers']['../../outside.h'] = 'unused'
    (adapter / 'manifest.json').write_text(json.dumps(adapter_manifest))
    rejects(lambda: m.load_adapter_headers(adapter), 'Invalid FFmpeg adapter header path')
    del adapter_manifest['headers']['../../outside.h']
    adapter_manifest['headers']['64/include/libavcodec/avcodec.h'] = 'unused'
    (adapter / 'manifest.json').write_text(json.dumps(adapter_manifest))
    rejects(lambda: m.load_adapter_headers(adapter), 'Invalid FFmpeg adapter header path')
    del adapter_manifest['headers']['64/include/libavcodec/avcodec.h']
    duplicate = dict(families[-1])
    adapter_manifest['families'] = families + [duplicate]
    (adapter / 'manifest.json').write_text(json.dumps(adapter_manifest))
    rejects(lambda: m.load_adapter_headers(adapter), 'Expected FFmpeg adapter families')
    adapter_manifest['families'] = families[:1]
    (adapter / 'manifest.json').write_text(json.dumps(adapter_manifest))
    rejects(lambda: m.load_adapter_headers(adapter), 'Expected FFmpeg adapter families')
    cache = base / 'rpm-cache'; cache.mkdir()
    srpm = 'example-1-1.src.rpm'
    (cache / srpm).write_bytes(b'source archive')
    (cache / 'manifest.json').write_text(json.dumps({srpm: {'sha256': m.sha(cache / srpm), 'url': 'https://example.invalid/source'}}))
    target = base / 'copied.src.rpm'
    assert m.cached_rpm_source(cache, srpm, target) == 'https://example.invalid/source'
    assert target.read_bytes() == b'source archive'
    (cache / srpm).write_bytes(b'tampered')
    rejects(lambda: m.cached_rpm_source(cache, srpm, target), 'hash mismatch')
    rejects(lambda: m.cached_rpm_source(cache, 'missing.src.rpm', target), 'missing')
    rejects(lambda: m.cached_rpm_source(cache, '../outside.src.rpm', target), 'Invalid')
    front, www = base / 'front', base / 'www'
    front.mkdir(); www.mkdir()
    (front / 'notices').mkdir()
    (front / 'notices/LICENSE.txt').write_text('License notice')
    (www / 'index.html').write_text('original')
    (front / 'sources.tar.gz').write_bytes(b'source fixture')
    manifest = {'assets': {'index.html': m.sha(www / 'index.html')},
                'source_sha256': m.sha(front / 'sources.tar.gz'),
                'notices': {'LICENSE.txt': m.sha(front / 'notices/LICENSE.txt')}}
    (front / 'manifest.json').write_text(json.dumps(manifest))
    m.load_frontend(front, www)
    (front / 'notices/LICENSE.txt').write_text('tampered license')
    rejects(lambda: m.load_frontend(front, www), 'notice does not match')
    (front / 'notices/LICENSE.txt').write_text('License notice')
    (front / 'notices/extra.txt').write_text('unlisted notice')
    rejects(lambda: m.load_frontend(front, www), 'notice set does not match')
    (front / 'notices/extra.txt').unlink()
    (www / 'index.html').write_text('tampered')
    rejects(lambda: m.load_frontend(front, www), 'asset does not match')
    (www / 'index.html').write_text('original')
    (www / 'extra.js').write_text('extra')
    rejects(lambda: m.load_frontend(front, www), 'asset set does not match')
    (www / 'extra.js').unlink()
    (front / 'sources.tar.gz').write_bytes(b'tampered sources')
    rejects(lambda: m.load_frontend(front, www), 'source hash mismatch')
    archive = base / 'source.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        for name in ['project/COPYING', 'project/vendor/LICENSE.txt']:
            info = tarfile.TarInfo(name); info.size = 7
            tar.addfile(info, io.BytesIO(b'License'))
    m.archived_notices(archive, base / 'extracted-notices')
    assert (base / 'extracted-notices/project/vendor/LICENSE.txt').read_text() == 'License'
    with tarfile.open(archive, 'w:gz') as tar:
        content = b'/* Copyright holder; permission notice */\nint function(void);'
        info = tarfile.TarInfo('project/header.h'); info.size = len(content)
        tar.addfile(info, io.BytesIO(content))
    m.archived_notices(archive, base / 'header-notices')
    assert (base / 'header-notices/project/header.h').read_bytes() == content
    with tarfile.open(archive, 'w:gz') as tar:
        info = tarfile.TarInfo('../LICENSE.txt'); info.size = 7
        tar.addfile(info, io.BytesIO(b'License'))
    rejects(lambda: m.archived_notices(archive, base / 'bad-notices'), 'Unsafe license notice')
    source = base / 'vendored'; source.mkdir()
    (source / 'header.h').write_text('/* MIT license header */')
    (source / 'LICENSE.txt').write_text('MIT')
    assert m.vendored_notices(source, base / 'vendored-notices', ['header.h']) == ['LICENSE.txt', 'header.h']
    assert (base / 'vendored-notices/header.h').read_text() == '/* MIT license header */'
    bundle, doc = base / 'bundle', base / 'doc'
    bundle.mkdir(); doc.mkdir()
    inv = m.Inventory(bundle, doc, 'amd64')
    unknown = base / 'libunknown-dependency.a'
    unknown.write_bytes(b'unknown archive')
    binary = base / 'test-binary'
    Path(str(binary) + '.static-link.json').write_text(json.dumps({'cwd': tmp, 'private_archive_copies': {}}))
    Path(str(binary) + '.link-map').write_text(f'Archive member included to satisfy reference by file (symbol)\n{unknown}(object.o)\nDiscarded input sections\n')
    inputs = m.link_inputs(binary)
    assert inputs == [unknown]
    map_path = Path(str(binary) + '.link-map')
    original_map = map_path.read_text()
    map_path.write_text(original_map + 'LOAD /tmp/ccABC123.debug.temp.o\nLOAD /tmp/ccABC123.ltrans0.ltrans.o\n')
    assert m.link_inputs(binary) == [unknown]
    for missing in ['/tmp/real-library.o', '/tmp/ccABC123.unrecognized.o',
                    str(base / 'ccABC123.debug.temp.o')]:
        map_path.write_text(original_map + 'LOAD ' + missing + '\n')
        rejects(lambda: m.link_inputs(binary), 'Missing link input')
    map_path.write_text(original_map)
    rejects(lambda: inv.archive(inputs[0]), 'Uncatalogued static archive')
    # The restored host boundary applies after resolving private-copy aliases,
    # but must not reject Xorg's unrelated, same-named server-side archives.
    desktop = base / 'libXext.a'
    desktop.write_bytes(b'old cached desktop archive')
    map_path.write_text(f'{desktop}(object.o)\nDiscarded input sections\n')
    rejects(lambda: m.link_inputs(binary), 'embeds a host library')
    rejects(lambda: inv.archive(desktop), 'embeds a host library')
    saved_source = m.SRC
    try:
        m.SRC = base
        # Preserve another external archive so the function's nonempty evidence
        # invariant still holds while internal Xorg archives are excluded.
        external = base.parent / (base.name + '-external.a')
        external.write_bytes(b'other input')
        map_path.write_text(f'{desktop}(server.o)\n{external}(object.o)\nDiscarded input sections\n')
        assert m.link_inputs(binary) == [external]
        external.unlink()
    finally:
        m.SRC = saved_source
    map_path.write_text(original_map)
    # Reject a TLS archive even when a cached builder has it available, and
    # before treating archive ownership as a legitimate embedded component.
    for name in ('libssl.a', 'libcrypto.a'):
        tls = base / name
        tls.write_bytes(b'cached static OpenSSL')
        map_path.write_text(f'{tls}(tls.o)\nDiscarded input sections\n')
        rejects(lambda: m.link_inputs(binary), 'static OpenSSL')
        rejects(lambda: inv.archive(tls), 'static OpenSSL')
    # The private-copy metadata must not conceal the original archive name.
    Path(str(binary) + '.static-link.json').write_text(json.dumps({
        'cwd': tmp, 'private_archive_copies': {str(tls): str(unknown)}}))
    map_path.write_text(original_map)
    rejects(lambda: m.link_inputs(binary), 'static OpenSSL')
    (base / 'test.cdx.json').write_text(json.dumps({
        'metadata': {'component': {'name': 'KasmVNC', 'properties': [
            {'name': 'kasmvnc:architecture', 'value': 'amd64'},
            {'name': 'kasmvnc:revision', 'value': 'fixture'}]}},
        'components': [{'name': 'fixture', 'version': '1.0', 'bom-ref': 'fixture@1.0'}]}))
    scanner = base / 'grype'
    scanner.write_text('#!/bin/sh\nexit 17\n'); scanner.chmod(0o755)
    env = dict(os.environ, PATH=tmp + ':' + os.environ['PATH'])
    result = subprocess.run([str(Path(__file__).with_name('scan')), str(base / 'test')], env=env)
    assert result.returncode == 17, result.returncode
    scanner.write_text('''#!/usr/bin/env python3
import json,sys
from pathlib import Path
Path(sys.argv[sys.argv.index('--file')+1]).write_text(json.dumps({
    "matches": [{"vulnerability": {"id": "CVE-2026-12345", "severity": "Critical"},
                 "artifact": {"name": "fixture", "version": "1.0", "type": "binary"}}],
    "descriptor": {"version": "0.104.4", "db": {"status": {"valid": True}}}}))
''')
    result = subprocess.run([str(Path(__file__).with_name('scan')), str(base / 'test')], env=env)
    assert result.returncode == 0
    assert json.loads((base / 'test.vulnerability-summary.json').read_text())['match_counts_by_severity'] == {'Critical': 1}
    gitlab_report = json.loads((base / 'gl-dependency-scanning-report.json').read_text())
    assert gitlab_report['scan']['status'] == 'success'
    assert len(gitlab_report['vulnerabilities']) == 1
    assert gitlab_report['vulnerabilities'][0]['severity'] == 'Critical'
    assert (base / 'gl-sbom.cdx.json').read_bytes() == (base / 'test.cdx.json').read_bytes()
print('PASS: frontend asset/source/notice integrity, nested and header-only notices, path traversal rejection, vendored headers, unknown actual link input, scanner errors, advisory critical findings')
