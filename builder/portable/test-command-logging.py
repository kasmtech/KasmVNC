#!/usr/bin/env python3
"""Exercise the runtime harness command wrapper without Docker orchestration."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

source = Path(__file__).resolve().parents[1] / 'test-portable'
function = next(node for node in ast.parse(source.read_text()).body
                if isinstance(node, ast.FunctionDef) and node.name == 'run')
namespace = {'subprocess': subprocess, 'Path': Path}
exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), 'exec'), namespace)
run = namespace['run']


class CommandLoggingTests(unittest.TestCase):
    def test_success_and_failure_output(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'command.log'
            result = run([sys.executable, '-c', 'print("complete")'], log=log)
            self.assertEqual(log.read_text(), result.stdout)
            with self.assertRaisesRegex(RuntimeError, 'failed-output'):
                run([sys.executable, '-c', 'print("failed-output"); exit(1)'], log=log)
            self.assertIn('failed-output', log.read_text())

    def test_timeout_retains_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'install.log'
            with self.assertRaisesRegex(RuntimeError, 'install-progress'):
                run([sys.executable, '-c',
                     'import os,time; os.write(1, b"install-progress\\xff"); time.sleep(60)'],
                    log=log, timeout=0.2)
            self.assertIn('install-progress', log.read_text())


unittest.main()
