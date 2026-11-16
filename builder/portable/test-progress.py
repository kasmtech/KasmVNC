#!/usr/bin/env python3
"""Exercise live heartbeat and carriage-return progress through real pipes."""
from pathlib import Path
import selectors
import subprocess
import sys
import unittest

SCRIPT = Path(__file__).with_name("progress.py")


class ProgressTests(unittest.TestCase):
    def test_silent_and_carriage_return_progress(self):
        proc = subprocess.Popen([sys.executable, str(SCRIPT), "--interval", "0.05"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(5), "No heartbeat during silence")
            self.assertIn(b"Waiting for build output", proc.stdout.readline())
            proc.stdin.write(b"old progress\rnew progress\r")
            proc.stdin.flush()
            for _ in range(20):
                self.assertTrue(selector.select(5), "No heartbeat after CR output")
                line = proc.stdout.readline()
                if b"new progress" in line:
                    break
            else:
                self.fail("Carriage-return progress was buffered")
            proc.stdin.close()
            self.assertEqual(proc.wait(timeout=5), 0)
            proc.stdout.close()

    def test_invalid_intervals(self):
        for value in ("0", "-1", "nan", "inf"):
            result = subprocess.run([sys.executable, str(SCRIPT), "--interval", value],
                                    input=b"", capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
