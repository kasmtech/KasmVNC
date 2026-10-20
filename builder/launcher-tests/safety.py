#!/usr/bin/env python3
"""Candidate-only safety checks. Unsafe historical Perl behavior is never run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

from parity import HOME, WORK, reset_fixture, setup_helpers
from startup import install_fake, live


def prepare(config=None):
    files = reset_fixture({'config': config or {'command_line': {'prompt': 'false'},
                                               'network': {'interface': '127.0.0.1', 'websocket_port': '8534'}}})
    directory = HOME / '.vnc'
    directory.mkdir()
    (directory / '.de-was-selected').touch()
    (HOME / '.kasmpasswd').write_text('root:fixture:w\n')
    return files[0], directory / (socket.gethostname() + ':91.pid')


def invoke(candidate, args, extra_env=None):
    env = {**os.environ, 'HOME': str(HOME), 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'USER': 'root', 'LOGNAME': 'root', **(extra_env or {})}
    proc = subprocess.Popen([candidate, *args], env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, start_new_session=True)
    started = time.monotonic()
    try:
        stdout, stderr = proc.communicate(timeout=4)
        status = proc.returncode
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        status = 'timeout'
    return {'status': status, 'stdout': stdout, 'stderr': stderr,
            'seconds': round(time.monotonic() - started, 3)}


def cleanup(pidfile):
    try:
        pid = int(pidfile.read_text().strip())
        executable = Path(Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')[0].decode()).name
        if executable == 'Xvnc' and live(pid):
            os.kill(pid, signal.SIGTERM)
            time.sleep(0.05)
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
    except (FileNotFoundError, ValueError):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', default='/src/unix/vncserver-native/vncserver-native')
    parser.add_argument('--report', default='/tmp/launcher-safety.json')
    args = parser.parse_args()
    candidate_hash = hashlib.sha256(Path(args.candidate).read_bytes()).hexdigest()
    if not Path('/.dockerenv').exists():
        parser.error('run only in the disposable launcher container')
    setup_helpers()
    install_fake()
    rows = []
    def record(name, result, checks):
        result['checks'] = checks
        result['name'] = name
        result['passed'] = all(checks.values())
        rows.append(result)
        print('PASS' if result['passed'] else 'FAIL', name, flush=True)
    for which in ('directory-symlink', 'directory-symlink-list', 'directory-symlink-kill', 'passwd-symlink', 'passwd-hardlink', 'pid-symlink', 'pid-hardlink'):
        config, pidfile = prepare()
        target = WORK / 'untouched-target'
        target.write_text('DO NOT CHANGE\n')
        before = target.read_bytes()
        if which.startswith('directory-symlink'):
            directory = HOME / '.vnc'
            (directory / '.de-was-selected').unlink()
            directory.rmdir()
            destination = WORK / 'untouched-directory'
            destination.mkdir()
            directory.symlink_to(destination, target_is_directory=True)
        else:
            path = pidfile if which.startswith('pid') else HOME / '.vnc/passwd'
            if which.endswith('symlink'):
                path.symlink_to(target)
            else:
                os.link(target, path)
        options = [':91', '-config', config, '-noxstartup']
        if not which.startswith('pid'):
            options.append('-dry-run')
        if which.endswith('-list'):
            options = ['-list']
        elif which.endswith('-kill'):
            options = ['-kill', ':91']
        result = invoke(args.candidate, options)
        checks = {'rejected': isinstance(result['status'], int) and result['status'] != 0,
                  'target_untouched': target.read_bytes() == before,
                  'bounded': result['status'] != 'timeout'}
        if which.startswith('directory-symlink'):
            checks['directory_untouched'] = not list(destination.iterdir())
            checks['unsafe_directory_diagnosed'] = 'Unsafe session directory' in result['stderr']
        record(which, result, checks)
        cleanup(pidfile)
    for kind in ('symlink', 'hardlink'):
        config, pidfile = prepare()
        child = subprocess.Popen(['/usr/bin/Xvnc', ':91'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        target = WORK / 'pid-read-target'
        target.write_text(str(child.pid) + '\n')
        before = target.read_bytes()
        if kind == 'symlink':
            pidfile.symlink_to(target)
        else:
            os.link(target, pidfile)
        try:
            result = invoke(args.candidate, ['-kill', ':91'])
            record('pid-read-' + kind, result,
                   {'rejected': isinstance(result['status'], int) and result['status'] != 0,
                    'target_untouched': target.read_bytes() == before,
                    'process_unsignaled': child.poll() is None,
                    'bounded': result['status'] != 'timeout'})
        finally:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=3)
    config, pidfile = prepare()
    unrelated = subprocess.Popen(['/bin/sleep', '30'])
    try:
        pidfile.write_text(str(unrelated.pid) + '\n')
        result = invoke(args.candidate, ['-kill', ':91'])
        record('unrelated-process', result, {'rejected': result['status'] != 0,
                                            'process_unsignaled': unrelated.poll() is None,
                                            'bounded': result['status'] != 'timeout'})
    finally:
        unrelated.terminate()
        unrelated.wait(timeout=3)
    for name in ('interpolation-cycle', 'yaml-oversized', 'yaml-depth'):
        config, pidfile = prepare({'legacy': {'desktop_name': '${CYCLE}'}} if name == 'interpolation-cycle' else None)
        if name == 'yaml-oversized':
            Path(config).write_text('#' + 'x' * (4 * 1024 * 1024) + '\n')
        if name == 'yaml-depth':
            Path(config).write_text(''.join('  ' * n + 'nested:\n' for n in range(70)))
        result = invoke(args.candidate, [':91', '-config', config, '-dry-run'], {'CYCLE': '${CYCLE}'})
        record(name, result, {'rejected': isinstance(result['status'], int) and result['status'] != 0,
                              'bounded': result['status'] != 'timeout'})
        cleanup(pidfile)
    marker = WORK / 'shell-was-executed'
    literal = "quote';touch " + str(marker) + ";echo '$(touch " + str(marker) + ')`touch ' + str(marker) + '`'
    config, pidfile = prepare({'command_line': {'prompt': 'false'},
                               'network': {'interface': '127.0.0.1', 'websocket_port': '8534'},
                               'legacy': {'desktop_name': literal}})
    result = invoke(args.candidate, [':91', '-config', config, '-noxstartup'])
    argv_file = WORK / 'argv-1'
    words = argv_file.read_bytes().rstrip(b'\0').decode().split('\0') if argv_file.exists() else []
    received = words[words.index('-desktop') + 1] if '-desktop' in words else None
    record('literal-shell-metacharacters', result, {'started': result['status'] == 0,
                                                    'literal_argv': received == literal,
                                                    'no_shell_execution': not marker.exists()})
    cleanup(pidfile)
    report = {'summary': {'candidate_sha256': candidate_hash, 'cases': len(rows), 'failed': sum(not row['passed'] for row in rows)}, 'results': rows}
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary']))
    return int(report['summary']['failed'] > 0)


if __name__ == '__main__':
    raise SystemExit(main())
