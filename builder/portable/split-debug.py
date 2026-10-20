#!/usr/bin/env python3
"""Split and verify symbols before packaging the identical runtime ELFs."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

root, debugroot, manifest = map(Path, sys.argv[1:])
def output(*args):
    return subprocess.check_output(args, text=True)
def build_id(path):
    match = re.search(r"Build ID: ([a-f0-9]+)", output('readelf', '-n', str(path)))
    if not match:
        raise SystemExit(f'Missing ELF build ID: {path}')
    return match[1]
records = []
for name in ('Xkasmvnc', 'kasmvncconfig', 'kasmvncpasswd', 'kasmxproxy'):
    binary = root / 'usr/bin' / name
    identity = build_id(binary)
    debug = debugroot / 'usr/lib/debug/.build-id' / identity[:2] / (identity[2:] + '.debug')
    debug.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['objcopy', '--only-keep-debug', str(binary), str(debug)], check=True)
    # Fail the build if symbols exist but source/line information was lost.
    symbols = output('nm', '-n', str(debug))
    main = re.search(r'^([0-9a-f]+) [Tt] main$', symbols, re.M)
    if not main:
        raise SystemExit(f'Missing main symbol: {name}')
    line = output('addr2line', '-e', str(debug), main[1]).strip()
    if '??' in line or not re.search(r':\d+', line):
        raise SystemExit(f'Missing source/line information: {name}: {line}')
    subprocess.run(['strip', '--strip-unneeded', str(binary)], check=True)
    subprocess.run(['objcopy', '--add-gnu-debuglink=' + str(debug), str(binary)], check=True)
    if build_id(binary) != identity or build_id(debug) != identity:
        raise SystemExit(f'Build ID mismatch: {name}')
    lookup = output('gdb', '-nx', '-batch',
                    '-ex', 'set debug-file-directory ' + str(debugroot / 'usr/lib/debug'),
                    '-ex', 'file ' + str(binary), '-ex', 'info line main')
    if not re.search(r'Line \d+ of ', lookup):
        raise SystemExit(f'GDB could not discover separate symbols: {name}: {lookup}')
    records.append({'binary': '/usr/bin/' + name, 'build_id': identity,
                    'debug_file': '/' + str(debug.relative_to(debugroot)),
                    'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'md5': hashlib.md5(binary.read_bytes()).hexdigest(),
                    'main_source': line})
manifest.write_text(json.dumps(records, indent=2) + '\n')
