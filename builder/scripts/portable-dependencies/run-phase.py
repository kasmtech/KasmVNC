#!/usr/bin/env python3
"""Keep full dependency logs while bounding BuildKit's console output."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def tail(path, lines, maximum_bytes=65536):
    with path.open('rb') as stream:
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        stream.seek(max(0, size - maximum_bytes))
        data = stream.read()
    # A compiler can emit megabyte-long lines. Bound both bytes read and each
    # emitted line; carriage-return progress updates count as separate lines.
    return '\n'.join(re.sub(r'[\x00-\x08\x0b-\x1f\x7f]', '', line)[:240]
                     for line in data.decode('utf-8', errors='replace').splitlines()[-lines:])


def signal_group(process, number):
    try:
        os.killpg(process.pid, number)
    except ProcessLookupError:
        pass  # The child exiting must not mask the original exception.


def run_phase(name, command, log_dir=Path('/opt/kasm-dependency-logs'), interval=60):
    if not re.fullmatch(r'[a-z0-9-]+', name):
        raise ValueError('Unsafe phase name')
    log_dir.mkdir(parents=True, exist_ok=True)
    log = log_dir / (name + '.log')
    started = time.monotonic()
    print(f'Dependency phase {name}: started; full log {log}', flush=True)
    with log.open('wb') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            while True:
                try:
                    result = process.wait(timeout=interval)
                    break
                except subprocess.TimeoutExpired:
                    elapsed = int(time.monotonic() - started)
                    print(f'Dependency phase {name}: running ({elapsed}s)', flush=True)
                    sample = tail(log, 1)
                    if sample:
                        print(sample, flush=True)
        except BaseException:
            signal_group(process, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                signal_group(process, signal.SIGKILL)
                process.wait()
            raise
    elapsed = int(time.monotonic() - started)
    print(f'Dependency phase {name}: exit {result} ({elapsed}s); full log {log}', flush=True)
    if result:
        print(tail(log, 200), flush=True)
    return result if result >= 0 else 128 - result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error('A command is required')
    raise SystemExit(run_phase(args.phase, args.command))
