#!/usr/bin/env python3
"""Exercise cache keys and registry failures without credentials/network."""
import importlib.util
import io
from pathlib import Path
import tempfile
import tarfile
import urllib.error
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('cache', Path(__file__).with_name('dependency-cache.py'))
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

class Response(io.BytesIO):
    def __init__(self, headers=None):
        super().__init__(b'')
        self.headers = headers or {}

class Tests(unittest.TestCase):
    def test_complete_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = root / 'dependencies'
            context.mkdir()
            recipe = context / 'recipe'
            recipe.write_text('version one')
            original = cache.context_key(context, 'amd64')
            (root / 'application.cpp').write_text('new app revision')
            self.assertEqual(original, cache.context_key(context, 'amd64'))
            self.assertNotEqual(original, cache.context_key(context, 'arm64'))
            recipe.write_text('version two')
            self.assertNotEqual(original, cache.context_key(context, 'amd64'))
            recipe.write_text('version one')
            recipe.chmod(0o755)
            self.assertNotEqual(original, cache.context_key(context, 'amd64'))
            recipe.chmod(0o644)
            recipe.rename(context / 'renamed')
            self.assertNotEqual(original, cache.context_key(context, 'amd64'))
            (context / 'external').symlink_to(root / 'application.cpp')
            with self.assertRaises(ValueError):
                cache.context_key(context, 'amd64')

    def test_checkout_umask_does_not_change_key_or_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = root / 'context'
            context.mkdir()
            script = context / 'build'
            script.write_text('script')
            config = context / 'Dockerfile'
            config.write_text('config')
            script.chmod(0o755)
            config.chmod(0o644)
            nested = context / 'nested'
            nested.mkdir(mode=0o755)
            original = cache.context_key(context, 'arm64')
            archive = root / 'first.tar'
            cache.context_archive(context, archive)
            script.chmod(0o777)
            nested.chmod(0o777)
            config.chmod(0o666)
            self.assertEqual(original, cache.context_key(context, 'arm64'))
            second = root / 'second.tar'
            cache.context_archive(context, second)
            self.assertEqual(archive.read_bytes(), second.read_bytes())
            with tarfile.open(second) as contents:
                self.assertEqual(contents.getmember('build').mode, 0o755)
                self.assertEqual(contents.getmember('nested').mode, 0o755)
                self.assertEqual(contents.getmember('Dockerfile').mode, 0o644)
                self.assertTrue(all(m.uid == m.gid == m.mtime == 0 for m in contents))
            script.chmod(0o666)
            self.assertNotEqual(original, cache.context_key(context, 'arm64'))

    def lookup(self, status, digest=None):
        def opener(request, timeout):
            if request.full_url.endswith('/v2/'):
                return Response()
            if status != 200:
                raise urllib.error.HTTPError(request.full_url, status, 'test', {}, None)
            return Response({'Docker-Content-Digest': digest} if digest else {})
        return cache.registry_digest('registry.example/project/portable-deps:amd64-key', opener)

    def test_authenticated_lookup(self):
        calls = []
        def opener(request, timeout):
            calls.append(request)
            if request.full_url.endswith('/v2/'):
                raise urllib.error.HTTPError(request.full_url, 401, 'auth', {
                    'WWW-Authenticate': 'Bearer realm="https://gitlab.com/jwt/auth",service="container_registry"'}, None)
            if request.full_url.startswith('https://gitlab.com/jwt/auth?'):
                self.assertEqual(request.get_header('Authorization'), 'Basic dXNlcjpwYXNz')
                return io.BytesIO(b'{"token":"test-token"}')
            self.assertEqual(request.get_header('Authorization'), 'Bearer test-token')
            return Response({'Docker-Content-Digest': 'sha256:' + 'a' * 64})
        with patch.dict(cache.os.environ, {'CI_REGISTRY_USER': 'user', 'CI_REGISTRY_PASSWORD': 'pass'}):
            result = cache.registry_digest('registry.gitlab.com/project/deps:key', opener)
        self.assertEqual(len(calls), 3)
        self.assertIn('@sha256:', result)

    def test_untrusted_auth_realm(self):
        def opener(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 401, 'auth', {
                'WWW-Authenticate': 'Bearer realm="https://other.example/token"'}, None)
        with self.assertRaisesRegex(RuntimeError, 'Untrusted'):
            cache.registry_digest('registry.gitlab.com/project/deps:key', opener)

    def test_realm_boundaries_and_diagnostics(self):
        for realm in ('https://gitlab.com:8443/token', 'https://gitlab.com:abc/token',
                      'http://gitlab.com/token', 'https://user@gitlab.com/token',
                      'https://gitlab.com/token#fragment', 'https://evil.example/token'):
            def opener(request, timeout):
                raise urllib.error.HTTPError(request.full_url, 401, 'auth', {
                    'WWW-Authenticate': 'Bearer realm="' + realm + '"'}, None)
            with self.subTest(realm=realm), self.assertRaisesRegex(RuntimeError, 'Untrusted'):
                cache.registry_digest('registry.gitlab.com/project/deps:key', opener)
        def no_realm(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 401, 'auth', {
                'WWW-Authenticate': 'Bearer service="x"'}, None)
        with self.assertRaisesRegex(RuntimeError, 'no realm'):
            cache.registry_digest('registry.example/p/r:key', no_realm)
        def valid(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 401, 'auth', {
                'WWW-Authenticate': 'Bearer realm="https://registry.example/token"'}, None)
        for present, missing in (({}, 'CI_REGISTRY_USER'), ({'CI_REGISTRY_USER': 'dummy'}, 'CI_REGISTRY_PASSWORD')):
            with patch.dict(cache.os.environ, present, clear=True), self.assertRaisesRegex(RuntimeError, missing):
                cache.registry_digest('registry.example/p/r:key', valid)

    def test_redirects_cannot_forward_credentials_or_become_misses(self):
        handler = cache.NoRegistryRedirects()
        for method in ('GET', 'HEAD'):
            request = cache.urllib.request.Request('https://registry.example/v2/p/manifests/tag',
                headers={'Authorization': 'Bearer dummy'}, method=method)
            for code in (301, 302, 303, 307, 308):
                with self.subTest(method=method, code=code), self.assertRaisesRegex(RuntimeError, 'redirects'):
                    handler.redirect_request(request, None, code, 'redirect', {}, 'https://elsewhere.example/missing')

    def test_hit_is_immutable(self):
        digest = 'sha256:' + 'a' * 64
        self.assertEqual(self.lookup(200, digest), 'registry.example/project/portable-deps@' + digest)

    def test_only_missing_is_cache_miss(self):
        self.assertIsNone(self.lookup(404))
        for status in (401, 403, 429, 500, 503):
            with self.assertRaises(urllib.error.HTTPError):
                self.lookup(status)
        with self.assertRaises(RuntimeError):
            self.lookup(200)

if __name__ == '__main__':
    unittest.main()
