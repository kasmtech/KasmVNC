#!/usr/bin/env python3
"""Regression tests for the evidence comparator; run in its disposable container."""
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from parity import parse_command
from safety import rejected

class Comparison(unittest.TestCase):
    def test_option_name_can_be_a_value(self):
        command = parse_command('Xvnc :91 -desktop -FrameRate -FrameRate 30', [])
        self.assertEqual(command['options'], {'-desktop': ['-FrameRate'], '-FrameRate': ['30']})
    def test_missing_value_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing argument'):
            parse_command('Xvnc :91 -desktop', [])
        self.assertEqual(parse_command('Xvnc :91 -hw3d', [])['options'], {'-hw3d': [None]})
    def test_only_clean_errors_are_rejections(self):
        for status in (0, -11, -6, 'timeout', None, True):
            self.assertFalse(rejected(status))
        self.assertTrue(rejected(1))
    def test_recompare_preserves_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'raw.json'; source.write_text('original evidence')
            alias = Path(tmp)/'alias'; alias.hardlink_to(source)
            for target in (source, alias):
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('recompare.py')), str(source), str(target)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'must not overwrite', result.stderr)
                self.assertEqual(source.read_text(), 'original evidence')

if __name__ == '__main__': unittest.main()
