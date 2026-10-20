#!/usr/bin/env python3
"""Disposable real-process launcher lifecycle tests with an instrumented fake Xvnc."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import threading
import time

from parity import ROOT, HOME, WORK, reset_fixture, setup_helpers, snapshot, normalized_diagnostics, normalize_pid, parse_command

FAKE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
  const char *base = "/tmp/kasm-launcher-parity";
  char path[256];
  snprintf(path,sizeof(path),"%s/attempts",base);
  FILE *f=fopen(path,"r"); int count=0;
  if(f){fscanf(f,"%d",&count);fclose(f);} ++count;
  f=fopen(path,"w");fprintf(f,"%d",count);fclose(f);
  snprintf(path,sizeof(path),"%s/argv-%d",base,count);
  f=fopen(path,"w");for(int i=0;i<argc;i++)fwrite(argv[i],1,strlen(argv[i])+1,f);fclose(f);
  const char *mode=getenv("PARITY_XVNC_MODE");
  if(mode && (!strcmp(mode,"fail") || (!strcmp(mode,"retry") && count==1)))return 7;
  snprintf(path,sizeof(path),"%s/ready",base);f=fopen(path,"w");fclose(f);
  puts("FAKE XVNC READY");fflush(stdout);
  for(;;) pause();
}
'''.replace('#include <stdlib.h>', '#include <stdlib.h>\n#include <string.h>')


def install_fake():
    source = Path('/tmp/parity-fake-xvnc.c')
    source.write_text(FAKE)
    subprocess.run(['cc', '-Wall', '-Wextra', str(source), '-o', '/usr/bin/Xvnc'], check=True)
    ready = Path('/usr/local/bin/xdpyinfo')
    ready.write_text('#!/bin/sh\ntest -f /tmp/kasm-launcher-parity/ready\n')
    ready.chmod(0o755)
    xinit = Path('/etc/X11/xinit/xinitrc')
    xinit.parent.mkdir(parents=True, exist_ok=True)
    xinit.write_text('#!/bin/sh\nprintf \"%s\\n%s\\n\" \"$DISPLAY\" \"$VNCDESKTOP\" > \"$HOME/startup-env\"\n')
    xinit.chmod(0o755)


def live(pid):
    try:
        text = Path(f'/proc/{pid}/stat').read_text()
        return text[text.rfind(')') + 2] != 'Z'
    except FileNotFoundError:
        return False


def run_one(command, name):
    config = {'command_line': {'prompt': 'false'}, 'network': {'interface': '127.0.0.1', 'websocket_port': '8534'}}
    configs = reset_fixture({'config': config})
    directory = HOME / '.vnc'
    directory.mkdir()
    (directory / '.de-was-selected').touch()
    (HOME / '.kasmpasswd').write_text('root:fixture-hash:w\n')
    startup = directory / 'xstartup'
    startup.write_text('#!/bin/sh\nprintf "%s\\n%s\\n" "$DISPLAY" "$VNCDESKTOP" > "$HOME/startup-env"\nsleep 0.15\n')
    startup.chmod(0o755)
    if name == 'generated-startup':
        startup.unlink()
    if name == 'custom-startup':
        startup.rename(HOME / 'custom-startup')
    pidfile = directory / (socket.gethostname() + ':91.pid')
    (directory / (socket.gethostname() + ':91.log')).write_text('OLD LOG MUST BE REPLACED\n')
    env = {**os.environ, 'HOME': str(HOME), 'USER': 'root', 'LOGNAME': 'root',
           'PERL5LIB': str(ROOT / 'unix'), 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
           'PERL_HASH_SEED': '0', 'PERL_PERTURB_KEYS': '0',
           'PARITY_XVNC_MODE': 'retry' if name == 'font-retry' else ('fail' if name == 'startup-failure' else 'success')}
    extras = {'start-no-apps': ['-noxstartup'], 'unicode-cli': ['-noxstartup', '-desktop', 'desk 日本語'], 'startup-failure': ['-noxstartup'],
              'font-retry': ['-noxstartup', '-fp', '/invalid-font-path'],
              'foreground': ['-fg'], 'generated-startup': ['-fg'],
              'custom-startup': ['-fg', '-xstartup', str(HOME / 'custom-startup')], 'autokill': ['-autokill'], 'background-apps': []}[name]
    args = [*command, ':91', '-config', ','.join(configs), *extras]
    stop_reaping = threading.Event()
    observed_pids = set()
    def reap_orphans():
        # Docker runs this Python as PID1. Reap only this fixture's Xvnc, never
        # the wrapper Popen child. This models the host init process correctly.
        while not stop_reaping.is_set():
            try:
                pid = int(pidfile.read_text().strip())
                observed_pids.add(pid)
            except (ValueError, FileNotFoundError):
                pass
            for pid in tuple(observed_pids):
                try:
                    os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    pass
            time.sleep(0.01)
    reaper = threading.Thread(target=reap_orphans)
    reaper.start()
    proc = subprocess.Popen(args, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=12)
        status = proc.returncode
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        status = 'timeout'
    if name == 'autokill':
        deadline = time.monotonic() + 3
        while pidfile.exists() and time.monotonic() < deadline:
            time.sleep(0.025)
    else:
        time.sleep(0.3)
    try:
        observed_pids.add(int(pidfile.read_text().strip()))
    except (ValueError, FileNotFoundError):
        pass
    state = {'status': status, 'stdout': stdout, 'stderr': stderr, 'pidfile_exists': pidfile.exists(),
             'server_alive': any(live(pid) for pid in observed_pids), 'attempts': (WORK / 'attempts').read_text() if (WORK / 'attempts').exists() else '0',
             'startup_env': (HOME / 'startup-env').read_text() if (HOME / 'startup-env').exists() else None,
             'files': snapshot(), 'argv': []}
    # Compare actual argv received by the fake binary, not launcher rendering.
    for record in sorted(WORK.glob('argv-*')):
        words = record.read_bytes().rstrip(b'\0').decode().split('\0')
        import shlex
        try:
            state['argv'].append(parse_command(shlex.join(words), [])['options'])
        except ValueError as error:
            state['argv'].append({'parse_error': str(error), 'raw': words})
    log = directory / (socket.gethostname() + ':91.log')
    state['log'] = log.read_text() if log.exists() else None
    for pid in observed_pids:
        if live(pid):
            os.kill(pid, signal.SIGTERM)
    time.sleep(0.1)
    stop_reaping.set()
    reaper.join()
    for pid in observed_pids:
        state['stdout'] = normalize_pid(state['stdout'], pid)
        state['stderr'] = normalize_pid(state['stderr'], pid)
    # PID/log content includes varying PIDs; normalize those bytes separately.
    for relative, value in state['files'].items():
        if relative.endswith('.pid') or relative.endswith('.log'):
            value.pop('sha256', None)
            value.pop('size', None)
    if state['log'] is not None:
        for pid in observed_pids:
            state['log'] = normalize_pid(state['log'], pid)
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', default='/src/unix/vncserver-native/vncserver-native')
    parser.add_argument('--oracle', default='/src/unix/vncserver')
    parser.add_argument('--report', default='/tmp/launcher-startup.json')
    parser.add_argument('--filter', default='')
    args = parser.parse_args()
    candidate_hash = hashlib.sha256(Path(args.candidate).read_bytes()).hexdigest()
    if not Path('/.dockerenv').exists():
        parser.error('only run in the disposable launcher Docker container')
    setup_helpers()
    install_fake()
    rows = []
    for name in ('start-no-apps', 'startup-failure', 'font-retry', 'foreground', 'autokill', 'background-apps', 'generated-startup', 'custom-startup', 'unicode-cli'):
        if args.filter not in name:
            continue
        left = run_one(['/usr/bin/perl', args.oracle], name)
        right = run_one([args.candidate], name)
        problems = []
        for key in ('status', 'pidfile_exists', 'server_alive', 'attempts', 'startup_env', 'files', 'argv', 'log'):
            if left[key] != right[key]:
                problems.append(key)
        for key in ('stdout', 'stderr'):
            if normalized_diagnostics(left[key]) != normalized_diagnostics(right[key]):
                problems.append(key)
        rows.append({'name': name, 'problems': problems, 'oracle': left, 'candidate': right})
        print('FAIL' if problems else 'PASS', name, ','.join(problems), flush=True)
    report = {'summary': {'candidate_sha256': candidate_hash, 'cases': len(rows), 'failed': sum(bool(row['problems']) for row in rows)}, 'results': rows}
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary']))
    return int(report['summary']['failed'] > 0)


if __name__ == '__main__':
    raise SystemExit(main())
