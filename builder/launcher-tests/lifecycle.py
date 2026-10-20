#!/usr/bin/env python3
"""Temporary isolated session-list and graceful-kill differential cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading

from parity import ROOT, HOME, WORK, reset_fixture, setup_helpers, snapshot, normalized_diagnostics, normalize_pid


def run_one(command, name):
    reset_fixture({})
    vncdir = HOME / '.vnc'
    vncdir.mkdir()
    pidfile = vncdir / (socket.gethostname() + ':91.pid')
    child = None
    waiter = None
    if name in ('list-live', 'kill-live', 'kill-screen-suffix'):
        source = WORK / 'fake-xvnc.c'
        binary = WORK / 'Xvnc'
        source.write_text('#include <unistd.h>\nint main(void) { sleep(60); return 0; }\n')
        subprocess.run(['cc', str(source), '-o', str(binary)], check=True)
        child = subprocess.Popen([str(binary)])
        pidfile.write_text(str(child.pid) + '\n')
        # Reap promptly so kill(0) doesn't treat the terminated child as live.
        waiter = threading.Thread(target=child.wait)
        waiter.start()
    elif name in ('list-stale', 'kill-stale'):
        pidfile.write_text('2147483647\n')
    option = ['-list'] if name.startswith('list') else ['-kill', ':91.0' if name == 'kill-screen-suffix' else ':91']
    env = {**os.environ, 'HOME': str(HOME), 'USER': 'root', 'LOGNAME': 'root',
           'PERL5LIB': str(ROOT / 'unix'), 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}
    proc = subprocess.Popen([*command, *option], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=8)
        status = proc.returncode
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        status = 'timeout'
    state = {'status': status, 'stdout': stdout, 'stderr': stderr,
             'pidfile_exists': pidfile.exists(), 'child_alive': child is not None and child.poll() is None,
             'files': snapshot()}
    if child:
        state['stdout'] = normalize_pid(state['stdout'], child.pid)
        state['stderr'] = normalize_pid(state['stderr'], child.pid)
        # PID bytes deliberately differ; compare their existence/mode instead.
        for value in state['files'].values():
            value.pop('sha256', None)
            value.pop('size', None)
        if child.poll() is None:
            child.terminate()
        waiter.join(timeout=3)
        if child.poll() is None:
            child.kill()
            waiter.join()
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', default='/src/unix/vncserver-native/vncserver-native')
    parser.add_argument('--oracle', default='/src/unix/vncserver')
    parser.add_argument('--report', default='/tmp/launcher-lifecycle.json')
    args = parser.parse_args()
    candidate_hash = hashlib.sha256(Path(args.candidate).read_bytes()).hexdigest()
    if not Path('/.dockerenv').exists():
        parser.error('only run in the disposable launcher Docker container')
    setup_helpers()
    rows = []
    for name in ('list-empty', 'list-live', 'list-stale', 'kill-missing', 'kill-live', 'kill-screen-suffix', 'kill-stale'):
        left = run_one(['/usr/bin/perl', args.oracle], name)
        right = run_one([args.candidate], name)
        issues = []
        for key in ('status', 'pidfile_exists', 'child_alive', 'files'):
            if left[key] != right[key]:
                issues.append(key)
        for key in ('stdout', 'stderr'):
            if normalized_diagnostics(left[key]) != normalized_diagnostics(right[key]):
                issues.append(key)
        rows.append({'name': name, 'problems': issues, 'oracle': left, 'candidate': right})
        print('FAIL' if issues else 'PASS', name, ','.join(issues), flush=True)
    report = {'summary': {'candidate_sha256': candidate_hash, 'cases': len(rows), 'failed': sum(bool(row['problems']) for row in rows)}, 'results': rows}
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary']))
    return int(report['summary']['failed'] > 0)


if __name__ == '__main__':
    raise SystemExit(main())
