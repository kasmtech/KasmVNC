#!/usr/bin/env python3
"""Exercise noisy/silent subprocesses and retained failure diagnostics."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('phases', Path(__file__).resolve().parents[1] / 'scripts/portable-dependencies/run-phase.py')
phases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(phases)

class Tests(unittest.TestCase):
    def execute(self, name, script, directory):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = phases.run_phase(name, [sys.executable, '-c', script], Path(directory), interval=0.05)
        return status, output.getvalue()

    def test_disappeared_group_preserves_original_exception(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(phases.subprocess, 'Popen') as spawn, \
                patch.object(phases.os, 'killpg', side_effect=ProcessLookupError):
            spawn.return_value.wait.side_effect = [KeyboardInterrupt(), 0]
            with self.assertRaises(KeyboardInterrupt):
                phases.run_phase('interrupted', ['unused'], Path(directory))

    def test_silent_heartbeat(self):
        with tempfile.TemporaryDirectory() as directory:
            status, output = self.execute('silent', 'import time; time.sleep(0.2)', directory)
            self.assertEqual(status, 0)
            self.assertIn('running', output)
            self.assertIn('exit 0', output)

    def test_large_output_retained_console_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            status, output = self.execute('noisy', "import sys; sys.stdout.write('x' * 3000000); sys.stderr.write('error-end'); sys.exit(7)", directory)
            self.assertEqual(status, 7)
            self.assertLess(len(output), 2000)
            self.assertEqual((Path(directory) / 'noisy.log').stat().st_size, 3000009)
            self.assertTrue((Path(directory) / 'noisy.log').read_bytes().endswith(b'error-end'))

    def test_failure_tail_contains_final_error(self):
        with tempfile.TemporaryDirectory() as directory:
            status, output = self.execute('failed', "print('prior\\n' * 300); print('final error'); raise SystemExit(2)", directory)
            self.assertEqual(status, 2)
            self.assertIn('final error', output)
            self.assertLessEqual(output.count('prior'), 199)

if __name__ == '__main__':
    unittest.main()
