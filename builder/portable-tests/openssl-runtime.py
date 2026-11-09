#!/usr/bin/env python3
"""Prove the running packaged server uses distribution OpenSSL 3 libraries."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Leap 15.6 ships Python 3.6; probes must work with its stock interpreter.


def inspect_runtime(pid):
    executable = Path('/proc', pid, 'exe').resolve()
    if executable != Path('/usr/bin/Xkasmvnc'):
        raise RuntimeError('Expected the installed packaged server: ' + str(executable))
    libraries = {}
    for line in Path('/proc', pid, 'maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6:
            continue
        path = fields[5]
        match = re.search(r'/lib(ssl|crypto)\.so\.3(?:\.[0-9]+)*$', path)
        if match:
            libraries.setdefault(match[1], set()).add(path)
    if set(libraries) != {'ssl', 'crypto'} or any(len(paths) != 1 for paths in libraries.values()):
        raise RuntimeError('Expected one mapped system libssl.so.3 and libcrypto.so.3: ' + repr(libraries))
    records = []
    for name, paths in sorted(libraries.items()):
        path = next(iter(paths))
        if not path.startswith(('/usr/lib/', '/usr/lib64/', '/lib/', '/lib64/')):
            raise RuntimeError('OpenSSL was loaded outside system library directories: ' + path)
        if shutil.which('dpkg-query'):
            # dpkg ownership may retain /lib even on a merged-/usr filesystem.
            candidates = [path]
            if path.startswith('/usr/lib/'):
                candidates.append(path[len('/usr'):])
            elif path.startswith('/lib/'):
                candidates.append('/usr' + path)
            result = None
            for candidate in candidates:
                result = subprocess.run(['dpkg-query', '-S', candidate], universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if result.returncode == 0:
                    break
        elif shutil.which('rpm'):
            result = subprocess.run(['rpm', '-qf', path], universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        elif shutil.which('apk'):
            result = subprocess.run(['apk', 'info', '--who-owns', path], universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        else:
            raise RuntimeError('No supported package manager for OpenSSL ownership verification')
        if result.returncode or not result.stdout.strip():
            raise RuntimeError('Mapped OpenSSL library is not owned by a distro package: ' + path)
        records.append({'library': name, 'path': path, 'package_owner': result.stdout.strip()})
    return {'pid': int(pid), 'executable': str(executable), 'libraries': records, 'pass': True}


if __name__ == '__main__':
    if len(sys.argv) != 2 or not sys.argv[1].isdigit():
        raise SystemExit('Usage: openssl-runtime.py SERVER_PID')
    print(json.dumps(inspect_runtime(sys.argv[1]), indent=2))
