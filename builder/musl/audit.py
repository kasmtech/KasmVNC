#!/usr/bin/env python3
"""Require native PIE, musl interpreter and an explicit host library boundary."""
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'portable'))
from openssl_boundary import SONAMES, read_sdk, sdk_path, sdk_exports, audit_openssl
from host_libraries import sonames, audit_host_libraries

MUSL_BASELINE = '1.2.5'


def audit_compiler_compatibility_aliases(symbols, arch, needed, version_info):
    # Alpine's native AArch64 libgcc retains these versioned compatibility
    # aliases, also referenced weakly by GCC's crtbeginS.o. They are exported
    # by libgcc_s.so.1 on every supported Alpine release (3.21 through 3.24);
    # they are not glibc dependencies. No other GLIBC_ symbol is permitted.
    # Resolve version indexes through .gnu.version_r, not DT_NEEDED alone:
    # another needed DSO must not impersonate libgcc's GLIBC_2.0 node.
    providers = {}
    in_needs, provider = False, None
    for line in version_info.splitlines():
        if line.startswith('Version '):
            in_needs = line.startswith('Version needs section ')
            provider = None
        if not in_needs:
            continue
        if 'File:' in line:
            match = re.fullmatch(
                r'\s*(?:0x)?[0-9a-fA-F]+:\s+Version:\s+\d+\s+File:\s+(\S+)\s+Cnt:\s+\d+\s*', line)
            provider = match[1] if match else None
        match = re.fullmatch(
            r'\s*(?:0x)?[0-9a-fA-F]+:\s+Name:\s+(\S+)\s+Flags:\s+\S+\s+Version:\s+(\d+)\s*', line)
        if match:
            providers.setdefault(match[2], []).append((provider, match[1]))
    aliases = []
    for line in symbols.splitlines():
        if 'GLIBC_' not in line:
            continue
        match = re.fullmatch(
            r'\s*\d+:\s+0+\s+0\s+FUNC\s+WEAK\s+DEFAULT\s+UND\s+'
            r'(__register_frame_info|__deregister_frame_info)@GLIBC_2\.0'
            r'\s+\((\d+)\)\s*', line)
        if (arch != 'arm64' or 'libgcc_s.so.1' not in needed or not match or
                providers.get(match[2]) != [('libgcc_s.so.1', 'GLIBC_2.0')]):
            raise RuntimeError('Nonportable glibc symbol: ' + line.strip())
        aliases.append({'symbol': match[1], 'version': 'GLIBC_2.0',
                        'binding': 'WEAK', 'provider': 'libgcc_s.so.1'})
    return aliases


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
    allowed = {libc, 'libGL.so.1', 'libgbm.so.1'} | SONAMES | sonames('musl')
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
        if any(x in dynamic for x in ('TEXTREL', 'RPATH', 'RUNPATH')):
            raise RuntimeError('Nonportable relocations/search path/glibc symbols: ' + str(path))
        compatibility_aliases = audit_compiler_compatibility_aliases(
            symbols, arch, needed, readelf('-VW') if 'GLIBC_' in symbols else '')
        openssl = audit_openssl(path, needed, exports)
        host = audit_host_libraries(path, needed, 'musl')
        records.append({'path': '/' + str(path.relative_to(root)), 'needed': needed,
                        'musl_minimum': MUSL_BASELINE, 'interpreter': expected_interpreter,
                        'pie': True, 'openssl': openssl, 'host_libraries': host,
                        'compiler_compatibility_aliases': compatibility_aliases})
    if len(records) != 5:
        raise RuntimeError('Expected five ELF executables, found ' + str(len(records)))
    return records


if __name__ == '__main__':
    Path(sys.argv[3]).write_text(json.dumps(audit(Path(sys.argv[1]), sys.argv[2]), indent=2) + '\n')
