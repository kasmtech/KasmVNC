#!/usr/bin/env python3
"""Build a real tiny RPM with the production scriptlet directive (in Docker)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class RPMScriptlet(unittest.TestCase):
    def test_preun_survives_old_rpmbuild(self):
        directive = next(line for line in (ROOT / 'package').read_text().splitlines()
                         if line.startswith('%preun '))
        with tempfile.TemporaryDirectory(prefix='kasm-rpm-scriptlet-') as tmp:
            top = Path(tmp)
            for name in ('BUILD', 'RPMS', 'SOURCES', 'SPECS', 'SRPMS'):
                (top / name).mkdir()
            shutil.copy2(ROOT / 'rpm-preun', top / 'BUILD/rpm-preun')
            spec = top / 'SPECS/test.spec'
            spec.write_text('Name: kasmvnc-scriptlet-test\nVersion: 1\nRelease: 1\n'
                            'Summary: Scriptlet regression\nLicense: GPL-2.0-or-later\n'
                            'BuildArch: noarch\n%description\nScriptlet regression\n'
                            + directive + '\n%files\n')
            result = subprocess.run(['rpmbuild', '--define', '_topdir ' + str(top),
                                     '-bb', str(spec)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            rpm = next((top / 'RPMS').rglob('*.rpm'))
            script = subprocess.check_output(['rpm', '-qp', '--qf', '%{PREUN}', str(rpm)], text=True)
            self.assertEqual(script.strip(), (ROOT / 'rpm-preun').read_text().strip())


if __name__ == '__main__':
    unittest.main()
