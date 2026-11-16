#!/usr/bin/env python3
"""Exercise source transport failures without network access, inside the builder."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

FETCH = Path(__file__).resolve().parents[1] / 'scripts/portable-dependencies/fetch-archive'
CANONICAL = 'https://upstream.invalid/release.tar.xz'
MIRROR = 'https://mirror.invalid/release.tar.xz'
SOURCE = b'exact pinned upstream archive\n'
DIGEST = hashlib.sha256(SOURCE).hexdigest()


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.destination = self.root / 'source.tar.xz'
        self.log = self.root / 'requests.jsonl'
        self.responses = self.root / 'responses.json'
        self.responses.write_text('{}')
        fake = self.root / 'curl'
        fake.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
url = next(value for value in args if value.startswith('https://'))
output = Path(args[args.index('-o') + 1])
assert args[args.index('--proto') + 1] == '=https'
assert args[args.index('--proto-redir') + 1] == '=https'
assert int(args[args.index('--connect-timeout') + 1]) > 0
assert int(args[args.index('--max-time') + 1]) > 0
with open(os.environ['FETCH_TEST_LOG'], 'a') as log:
    log.write(json.dumps({'url': url, 'output': str(output),
        'final_exists': Path(os.environ['FETCH_TEST_DESTINATION']).exists()}) + '\\n')
response = json.loads(Path(os.environ['FETCH_TEST_RESPONSES']).read_text())[url]
output.write_bytes(bytes.fromhex(response['bytes']))
raise SystemExit(response['status'])
''')
        fake.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH'],
                        FETCH_TEST_LOG=str(self.log), FETCH_TEST_RESPONSES=str(self.responses),
                        FETCH_TEST_DESTINATION=str(self.destination))

    def run_fetch(self, responses, digest=DIGEST):
        self.responses.write_text(json.dumps({url: {'status': status, 'bytes': data.hex()}
                                             for url, (status, data) in responses.items()}))
        result = subprocess.run([str(FETCH), str(self.destination), digest, CANONICAL, MIRROR],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(list(self.root.glob('*.part.*')), [], result.stderr)
        return result

    def requests(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_canonical_success_promotes_only_verified_temporary(self):
        result = self.run_fetch({CANONICAL: (0, SOURCE)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.destination.read_bytes(), SOURCE)
        request, = self.requests()
        self.assertEqual(request['url'], CANONICAL)
        self.assertNotEqual(request['output'], str(self.destination))
        self.assertFalse(request['final_exists'])

    def test_partial_transport_failure_uses_verified_mirror(self):
        result = self.run_fetch({CANONICAL: (28, b'partial'), MIRROR: (0, SOURCE)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.destination.read_bytes(), SOURCE)
        self.assertEqual([r['url'] for r in self.requests()], [CANONICAL, MIRROR])
        self.assertTrue(all(not r['final_exists'] for r in self.requests()))

    def test_all_transport_failures_leave_no_archive(self):
        result = self.run_fetch({CANONICAL: (28, b'partial'), MIRROR: (22, b'')})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.destination.exists())
        self.assertEqual([r['url'] for r in self.requests()], [CANONICAL, MIRROR])

    def test_wrong_canonical_bytes_do_not_fall_back(self):
        result = self.run_fetch({CANONICAL: (0, b'corrupt'), MIRROR: (0, SOURCE)})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.destination.exists())
        self.assertEqual([r['url'] for r in self.requests()], [CANONICAL])

    def test_wrong_mirror_bytes_are_rejected(self):
        result = self.run_fetch({CANONICAL: (28, b''), MIRROR: (0, b'corrupt')})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.destination.exists())

    def test_valid_cached_archive_avoids_transport(self):
        self.destination.write_bytes(SOURCE)
        self.assertEqual(self.run_fetch({}).returncode, 0)
        self.assertEqual(self.requests(), [])

    def test_corrupt_cached_archive_fails_without_replacement(self):
        self.destination.write_bytes(b'corrupt cache')
        self.assertNotEqual(self.run_fetch({}).returncode, 0)
        self.assertEqual(self.destination.read_bytes(), b'corrupt cache')
        self.assertEqual(self.requests(), [])

    def test_malformed_checksum_is_rejected_before_transport(self):
        self.assertEqual(self.run_fetch({}, digest='not-a-sha256').returncode, 2)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.requests(), [])


if __name__ == '__main__':
    unittest.main()
