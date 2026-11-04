#!/usr/bin/env python3
"""Require native PIE, musl interpreter and an explicit host library boundary."""
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'portable'))
from openssl_boundary import SONAMES, read_sdk, sdk_path, sdk_exports, audit_openssl

MUSL_BASELINE = '1.2.5'


def require_baseline(interpreter):
    # musl has no GLIBC-style versioned symbols. Qualify in the baseline runtime,
    # rather than label an arbitrary builder's binaries as baseline-compatible.
    try:
        result = subprocess.run([interpreter], capture_output=True, text=True)
    except OSError as error:
        raise RuntimeError("Baseline musl loader unavailable: " + interpreter) from error
    match = re.search(r'^Version (\S+)$', result.stdout + result.stderr, re.MULTILINE)
    if not match or match[1] != MUSL_BASELINE:
        raise RuntimeError('ELF audit requires musl ' + MUSL_BASELINE + ': ' + interpreter)


def audit(root, arch):
    machine, native = {'amd64': ('Advanced Micro Devices X86-64', 'x86_64'),
                       'arm64': ('AArch64', 'aarch64')}[arch]
    libc = 'libc.musl-' + native + '.so.1'
    allowed = {libc, 'libGL.so.1', 'libgbm.so.1'} | SONAMES
    exports = sdk_exports(sdk_path(), read_sdk())
    expected_interpreter = '/lib/ld-musl-' + native + '.so.1'
    require_baseline(expected_interpreter)
    records = []
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as f:
            if f.read(4) != b'\x7fELF':
                continue
        def readelf(option):
            return subprocess.check_output(['readelf', option, str(path)], text=True)
        header, segments, dynamic, symbols = [readelf(x) for x in ('-hW', '-lW', '-dW', '-sDW')]
        if machine not in header or not re.search(r'Type:\s+DYN', header):
            raise RuntimeError('Expected native PIE: ' + str(path))
        if '[Requesting program interpreter: ' + expected_interpreter + ']' not in segments:
            raise RuntimeError('Unexpected ELF interpreter: ' + str(path))
        needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
        if libc not in needed or set(needed) - allowed:
            raise RuntimeError('Unexpected host library boundary: ' + str(path) + ': ' + repr(needed))
        if any(x in dynamic for x in ('TEXTREL', 'RPATH', 'RUNPATH')) or 'GLIBC_' in symbols:
            raise RuntimeError('Nonportable relocations/search path/glibc symbols: ' + str(path))
        openssl = audit_openssl(path, needed, exports)
        records.append({'path': '/' + str(path.relative_to(root)), 'needed': needed,
                        'musl_minimum': MUSL_BASELINE, 'interpreter': expected_interpreter,
                        'pie': True, 'openssl': openssl})
    if len(records) != 5:
        raise RuntimeError('Expected five ELF executables, found ' + str(len(records)))
    return records


if __name__ == '__main__':
    Path(sys.argv[3]).write_text(json.dumps(audit(Path(sys.argv[1]), sys.argv[2]), indent=2) + '\n')
