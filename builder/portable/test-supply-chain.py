#!/usr/bin/env python3
"""Failure-boundary checks; run inside the portable builder, never on the host."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile

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


with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp)
    front, www = base / 'front', base / 'www'
    front.mkdir(); www.mkdir()
    (www / 'index.html').write_text('original')
    (front / 'sources.tar.gz').write_bytes(b'source fixture')
    manifest = {'assets': {'index.html': m.sha(www / 'index.html')},
                'source_sha256': m.sha(front / 'sources.tar.gz')}
    (front / 'manifest.json').write_text(json.dumps(manifest))
    m.load_frontend(front, www)
    (www / 'index.html').write_text('tampered')
    rejects(lambda: m.load_frontend(front, www), 'asset does not match')
    (www / 'index.html').write_text('original')
    (www / 'extra.js').write_text('extra')
    rejects(lambda: m.load_frontend(front, www), 'asset set does not match')
    (www / 'extra.js').unlink()
    (front / 'sources.tar.gz').write_bytes(b'tampered sources')
    rejects(lambda: m.load_frontend(front, www), 'source hash mismatch')
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
print('PASS: frontend byte/set/source integrity, unknown actual link input, scanner execution failure, advisory critical finding')
