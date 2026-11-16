#!/usr/bin/env python3
"""Materialize runtime-only signing credentials; never print their contents."""
import os
from pathlib import Path
import subprocess
import sys


def materialize(directory):
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    private = directory / 'kasmvnc_signing_key.rsa'
    public = directory / 'kasmvnc_signing_key.rsa.pub'
    values = [os.environ.get('KASMVNC_ALPINE_' + k + '_KEY', '') for k in ('PRIVATE', 'PUBLIC')]
    if not any(values) and os.environ.get('KASMVNC_APK_ALLOW_TEST_KEY') == '1' and not os.environ.get('CI'):
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:3072', '-out', str(private)], check=True, stderr=subprocess.DEVNULL)
        subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)], check=True)
        (directory / 'TEST-KEY.txt').write_text('Ephemeral local test signing key. Not a release trust anchor.\n')
    elif all(values):
        for path, value, kind in zip((private, public), values, ('PRIVATE', 'PUBLIC')):
            value = value.replace('\\n', '\n').strip()
            if not value.startswith('-----BEGIN '):
                value = '-----BEGIN ' + kind + ' KEY-----\n' + value + '\n-----END ' + kind + ' KEY-----'
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
            with os.fdopen(os.open(path, flags, 0o600 if kind == 'PRIVATE' else 0o644), 'w') as stream:
                stream.write(value + '\n')
    else:
        raise RuntimeError('Both Alpine signing keys are required; local test key requires explicit non-CI opt-in')
    private.chmod(0o600)
    public.chmod(0o644)
    derived = subprocess.check_output(['openssl', 'pkey', '-in', str(private), '-pubout', '-outform', 'DER'], stderr=subprocess.DEVNULL)
    supplied = subprocess.check_output(['openssl', 'pkey', '-pubin', '-in', str(public), '-outform', 'DER'], stderr=subprocess.DEVNULL)
    if derived != supplied:
        raise RuntimeError('Alpine signing key pair does not match')


if __name__ == '__main__':
    materialize(Path(sys.argv[1]))
