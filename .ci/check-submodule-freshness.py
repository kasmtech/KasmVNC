#!/usr/bin/env python3
"""Compare committed gitlinks with remote tips without changing the checkout."""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, urlopen

SUBMODULES = ('kasmvnc-unit-tests', 'kasmvnc-functional-tests', 'kasmweb')


class CheckError(Exception):
    pass


def git(*args, env=None):
    try:
        result = subprocess.run(['git', *args], text=True, capture_output=True,
                                timeout=60, env=env)
    except subprocess.TimeoutExpired:
        raise CheckError('Git lookup timed out') from None
    if result.returncode:
        # Do not echo remote diagnostics, which can contain credentials.
        raise CheckError('Git lookup failed; check repository access and configuration')
    return result.stdout.strip()


def mr_targets(env):
    """Branch pipelines do not have CI_MERGE_REQUEST_TARGET_BRANCH_NAME."""
    targets = set()
    page = 1
    while True:
        query = urlencode({'state': 'opened', 'source_branch': env['CI_COMMIT_BRANCH'],
                           'per_page': 100, 'page': page})
        url = (f"{env['CI_API_V4_URL']}/projects/{env['CI_PROJECT_ID']}"
               f'/merge_requests?{query}')
        request = Request(url, headers={'JOB-TOKEN': env['CI_JOB_TOKEN']})
        try:
            with urlopen(request, timeout=30) as response:
                rows = json.load(response)
                next_page = response.headers.get('X-Next-Page')
        except Exception:
            raise CheckError('Cannot determine MR target from GitLab API') from None
        for row in rows:
            # Exclude MRs from forks with an identically named source branch.
            if str(row['source_project_id']) == env['CI_PROJECT_ID']:
                targets.add(row['target_branch'])
        if not next_page:
            return targets
        page = int(next_page)


def target_branch(env):
    if env.get('CI_MERGE_REQUEST_TARGET_BRANCH_NAME'):
        return env['CI_MERGE_REQUEST_TARGET_BRANCH_NAME']
    if env.get('CI_COMMIT_TAG'):
        return ''
    branch = env.get('CI_COMMIT_BRANCH', '')
    if not branch:
        raise CheckError('No branch context; pass the target branch as an argument locally')
    targets = mr_targets(env)
    if len(targets) > 1:
        raise CheckError('Multiple MR targets: ' + ', '.join(sorted(targets)))
    if targets:
        return targets.pop()
    target = branch if branch.startswith('release/') else env.get('CI_DEFAULT_BRANCH', 'master')
    print(f'No open MR: checking against {target}')
    return target


def remote_refs(url):
    env = os.environ.copy()
    env['GIT_TERMINAL_PROMPT'] = '0'
    # This job skips submodule initialization. Authenticate read-only Git requests
    # using the job token, scoped to the CI server, without storing it on disk.
    host = env.get('CI_SERVER_HOST')
    if host and env.get('CI_JOB_TOKEN'):
        prefix = f'git@{host}:'
        if url.startswith(prefix):
            url = f"{env['CI_SERVER_URL']}/{url[len(prefix):]}"
        if urlsplit(url).hostname == host and urlsplit(url).scheme == 'https':
            auth = base64.b64encode(f"gitlab-ci-token:{env['CI_JOB_TOKEN']}".encode()).decode()
            index = int(env.get('GIT_CONFIG_COUNT', '0'))
            env['GIT_CONFIG_COUNT'] = str(index + 1)
            env[f'GIT_CONFIG_KEY_{index}'] = f"http.{env['CI_SERVER_URL']}/.extraHeader"
            env[f'GIT_CONFIG_VALUE_{index}'] = f'Authorization: Basic {auth}'
    output = git('ls-remote', '--symref', url, 'HEAD', 'refs/heads/*', env=env)
    refs = {}
    default = None
    for line in output.splitlines():
        value, ref = line.split('\t', 1)
        if value.startswith('ref: '):
            if ref == 'HEAD':
                default = value[5:]
        else:
            refs[ref] = value
    return default, refs


def expected_refs(path, target, default, refs):
    if target == 'master':
        if not default or not default.startswith('refs/heads/'):
            raise CheckError('Remote did not advertise its default branch')
        names = [default]
    elif path == 'kasmweb':
        names = sorted(ref for ref in refs if ref.startswith('refs/heads/release/'))
        if not names:
            raise CheckError('No remote release/* branches exist')
    else:
        names = ['refs/heads/' + target]
    for name in names:
        if name not in refs:
            raise CheckError(f'Missing remote branch: {name.removeprefix("refs/heads/")}')
    return {name.removeprefix('refs/heads/'): refs[name] for name in names}


def check_submodule(path, target):
    entry = git('ls-tree', 'HEAD', '--', path).split()
    if len(entry) != 4 or entry[:2] != ['160000', 'commit']:
        raise CheckError('Missing committed submodule gitlink')
    recorded = entry[2]
    print(f'  Recorded: {recorded}')
    # Read committed config too; an initialized/modified working tree is irrelevant.
    url = git('config', '--blob', 'HEAD:.gitmodules', '--get', f'submodule.{path}.url')
    default, refs = remote_refs(url)
    expected = expected_refs(path, target, default, refs)
    matches = []
    for branch, tip in expected.items():
        print(f'  Remote {branch}: {tip}')
        if tip == recorded:
            matches.append(branch)
    if not matches:
        print('  FAIL: recorded commit is not at an eligible branch tip')
        return False
    print('  PASS: matches ' + ', '.join(matches))
    return True


def main():
    os.chdir(Path(__file__).resolve().parents[1])
    try:
        target = sys.argv[1] if len(sys.argv) == 2 else target_branch(os.environ)
    except CheckError as error:
        print(f'FAIL: {error}')
        return 1
    if target != 'master' and not target.startswith('release/'):
        print(f'SKIP: target {target!r} is neither master nor release/*')
        return 0
    print(f'Submodule freshness for target {target} (remote tips at job execution time)')
    failures = 0
    for path in SUBMODULES:
        print(f'\n{path}:')
        try:
            passed = check_submodule(path, target)
        except CheckError as error:
            print(f'  FAIL: {error}')
            passed = False
        failures += not passed
    print(f'\n{len(SUBMODULES) - failures}/{len(SUBMODULES)} submodules match eligible branch tips.')
    print('Advisory only: no submodule revisions were changed. Retry before merging to refresh.')
    return int(failures != 0)


if __name__ == '__main__':
    sys.exit(main())
