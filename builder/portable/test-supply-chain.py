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
    (base / 'test.cdx.json').write_text('{}')
    scanner = base / 'grype'
    scanner.write_text('#!/bin/sh\nexit 17\n'); scanner.chmod(0o755)
    env = dict(os.environ, PATH=tmp + ':' + os.environ['PATH'])
    result = subprocess.run([str(Path(__file__).with_name('scan')), str(base / 'test')], env=env)
    assert result.returncode == 17, result.returncode
    scanner.write_text('''#!/usr/bin/env python3
import json,sys
from pathlib import Path
Path(sys.argv[sys.argv.index('--file')+1]).write_text(json.dumps({"matches":[{"vulnerability":{"severity":"Critical"}}],"descriptor":{"db":{"status":{"valid":True}}}}))
''')
    result = subprocess.run([str(Path(__file__).with_name('scan')), str(base / 'test')], env=env)
    assert result.returncode == 0
    assert json.loads((base / 'test.vulnerability-summary.json').read_text())['match_counts_by_severity'] == {'Critical': 1}
print('PASS: frontend asset/source/notice integrity, nested and header-only notices, path traversal rejection, vendored headers, unknown actual link input, scanner errors, advisory critical findings')
