#!/usr/bin/env python3
"""Verify packaged ELF dependencies resolve to distribution-owned libraries."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Leap 15.6 ships Python 3.6; probes must work with its stock interpreter.


EXECUTABLES = ('Xkasmvnc', 'kasmvncserver', 'kasmvncconfig',
               'kasmvncpasswd', 'kasmxproxy')


def output(command):
    return subprocess.check_output(command, universal_newlines=True, stderr=subprocess.STDOUT)


def owner(path):
    candidates = [path]
    if path.startswith('/usr/lib/'):
        candidates.append(path[len('/usr'):])
    elif path.startswith('/lib/'):
        candidates.append('/usr' + path)
    if shutil.which('dpkg-query'):
        commands = [['dpkg-query', '-S', p] for p in candidates]
    elif shutil.which('rpm'):
        commands = [['rpm', '-qf', path]]
    elif shutil.which('apk'):
        commands = [['apk', 'info', '--who-owns', path]]
    else:
        raise RuntimeError('No supported package manager for library ownership')
    for command in commands:
        result = subprocess.run(command, universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    raise RuntimeError('Library has no distribution package owner: ' + path)


def inspect_runtime(pid, expected):
    if Path('/proc', pid, 'exe').resolve() != Path('/usr/bin/Xkasmvnc'):
        raise RuntimeError('Expected the installed packaged Xkasmvnc process')
    mapped = set()
    for line in Path('/proc', pid, 'maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and fields[5].startswith('/'):
            mapped.add(str(Path(fields[5]).resolve()))
    expected = set(expected)
    if not expected:
        raise RuntimeError('Host-shared package must declare host libraries')
    observed = set()
    binaries = []
    ownership = {}
    for name in EXECUTABLES:
        executable = Path('/usr/bin', name)
        needed = set(re.findall(r'\(NEEDED\).*\[(.*?)\]',
                                output(['readelf', '-dW', str(executable)])))
        linkage = output(['ldd', str(executable)])
        if 'not found' in linkage:
            raise RuntimeError('Unresolved dependencies for ' + name + ': ' + linkage)
        providers = dict(re.findall(r'^\s*(\S+)\s+=>\s+(/\S+)', linkage, re.M))
        records = []
        for soname in sorted(needed & expected):
            if soname not in providers:
                raise RuntimeError('No resolved provider for ' + name + ': ' + soname)
            path = str(Path(providers[soname]).resolve())
            if not path.startswith(('/usr/lib/', '/usr/lib64/', '/lib/', '/lib64/')):
                raise RuntimeError('Host library resolved outside system directories: ' + path)
            if path not in ownership:
                ownership[path] = owner(path)
            if name == 'Xkasmvnc' and path not in mapped:
                raise RuntimeError('Resolved library is absent from the live server: ' + path)
            records.append({'soname': soname, 'path': path,
                            'package_owner': ownership[path],
                            'mapped_in_server': path in mapped})
            observed.add(soname)
        binaries.append({'name': name, 'sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                         'needed': sorted(needed), 'host_libraries': records})
    if observed != expected:
        raise RuntimeError('Provenance host libraries not found in installed binaries: '
                           + repr(sorted(expected - observed)))
    return {'pid': int(pid), 'executables': binaries, 'pass': True}


if __name__ == '__main__':
    if len(sys.argv) != 3 or not sys.argv[1].isdigit():
        raise SystemExit('Usage: host-runtime.py SERVER_PID EXPECTED_SONAMES_JSON')
    print(json.dumps(inspect_runtime(sys.argv[1], json.loads(Path(sys.argv[2]).read_text())), indent=2))
