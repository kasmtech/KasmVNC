#!/usr/bin/env python3
"""Temporary Perl/C++ launcher differential tests; runs only in Docker.

The frozen inventory is independent of C++ metadata. No mismatch is waived.
JSON reports include raw results so differences in errors are reviewable.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time

import yaml

class FixtureDumper(yaml.SafeDumper):
    pass


FixtureDumper.add_representer(type(None),
    lambda dumper, value: dumper.represent_scalar('tag:yaml.org,2002:null', ''))


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HOME = Path('/tmp/kasm-launcher-parity/home')
WORK = HOME.parent


def set_key(config, key, value, delete=False):
    node = config
    parts = key.split('.')
    for part in parts[:-1]:
        if not isinstance(node.get(part), dict):
            node[part] = {}
        node = node[part]
    if delete:
        node.pop(parts[-1], None)
    else:
        node[parts[-1]] = value


def case(name, config=None, **kw):
    return {'name': name, 'config': config, **kw}


def cases(inventory, defaults):
    yield case('defaults', defaults)
    yield case('defaults-source', raw=(ROOT / 'unix/kasmvnc_defaults.yaml').read_text())
    yield case('empty-config', {})
    for row in inventory:
        key = row['key']
        for label, value in [('absent', None), ('null', None), ('empty', ''),
                             ('invalid', 'INVALID VALUE'), ('mapping', {'unexpected': 'value'})]:
            config = copy.deepcopy(defaults)
            set_key(config, key, value, delete=label == 'absent')
            yield case(f'key:{key}:{label}', config, key=key)
        for i, value in enumerate(row['values']):
            config = copy.deepcopy(defaults)
            set_key(config, key, value)
            # Fill the complete optional clumps, while separate absent/null cases
            # exercise partial clumps. Horizontal scrolling has an explicit prerequisite.
            if key.startswith('network.unix_relay.'):
                set_key(config, 'network.unix_relay.name', 'relay')
                set_key(config, 'network.unix_relay.path', '/tmp/relay.sock')
                set_key(config, key, value)
            if key.startswith('data_loss_prevention.visible_region.') and '.concealed_region.' not in key:
                for edge in ('top', 'left', 'right', 'bottom'):
                    set_key(config, 'data_loss_prevention.visible_region.' + edge, '10')
                set_key(config, key, value)
            if key == 'encoding.scrolling.detect_horizontal_scrolling':
                set_key(config, 'encoding.scrolling.detect_vertical_scrolling', 'true')
            yield case(f'key:{key}:value-{i}', config, key=key)
        config = copy.deepcopy(defaults)
        set_key(config, 'server.allow_environment_variables_to_override_config_settings', 'true')
        value = row['values'][0]
        if isinstance(value, (str, int)):
            yield case(f'env:{key}', config, key=key,
                       env={'KVNC_' + key.replace('.', '_').upper(): str(value)})
    for row in inventory:
        value = row['values'][0]
        scalar = value[0] if isinstance(value, list) and value else value
        for label, sequence in [('single', [scalar]), ('empty', []), ('two', ['true', 'false'] if row['type'] == 'BOOLEAN' else [scalar, scalar]),
                                ('nested', [[scalar]]), ('mapping', [{'unexpected': scalar}])]:
            config = copy.deepcopy(defaults)
            set_key(config, row['key'], sequence)
            yield case('sequence:' + row['key'] + ':' + label, config, key=row['key'])
    for name, overrides in [
        ('disconnect-true', {'user_session.new_session_disconnects_existing_exclusive_session': 'true'}),
        ('watermark-conflict', {'data_loss_prevention.watermark.image': '/tmp/image.png',
                                'data_loss_prevention.watermark.text.template': 'text'}),
        ('horizontal-requires-vertical', {'encoding.scrolling.detect_horizontal_scrolling': 'true',
                                          'encoding.scrolling.detect_vertical_scrolling': 'false'}),
    ]:
        config = copy.deepcopy(defaults)
        for key, value in overrides.items():
            set_key(config, key, value)
        yield case('interaction:' + name, config)
    # Exercise suppression of every generated option independently of the C++
    # metadata; duplicate pass-through order is covered separately below.
    option_names = sorted({name for row in inventory for name in row['options']} - {'prompt'})
    for name in option_names:
        value = {'geometry': '640x480', 'fp': '/tmp/fixture-fonts',
                 'interface': '127.0.0.1', 'websocketPort': '12345'}.get(name, '37')
        args = ['-' + name] if name == 'hw3d' else ['-' + name, value]
        prefix = [] if name in {'geometry', 'fp', 'interface', 'websocketPort'} else args
        yield case('option:' + name, defaults, args=args, prefix=prefix)
    for key, value in [('unknown', 'x'), ('desktop.unknown', 'x')]:
        config = copy.deepcopy(defaults)
        set_key(config, key, value)
        yield case('unsupported:' + key, config)
    for enabled in ('true', 'false'):
        config = copy.deepcopy(defaults)
        set_key(config, 'server.allow_environment_variables_to_override_config_settings', enabled)
        yield case('unknown-env:' + enabled, config, env={'KVNC_NOT_SUPPORTED': 'x'})
    for label, value, env in [
        ('known', '${PARITY_VALUE}', {'PARITY_VALUE': 'desk 日本語'}),
        ('missing', '${PARITY_UNDEFINED}', {}),
        ('repeated', '${PARITY_VALUE}/${PARITY_VALUE}', {'PARITY_VALUE': 'desk'}),
        ('nested', '${PARITY_VALUE}', {'PARITY_VALUE': '${PARITY_NEXT}', 'PARITY_NEXT': 'nested'}),
    ]:
        config = copy.deepcopy(defaults)
        set_key(config, 'legacy.desktop_name', value)
        yield case('interpolation:' + label, config, env=env)
    for name, args, prefix in [
        ('duplicate', ['-FrameRate', '17', '-FrameRate', '19'], ['-FrameRate', '17', '-FrameRate', '19']),
        ('negative', ['-JpegVideoQuality', '-1'], ['-JpegVideoQuality', '-1']),
        ('quoted', ['-desktop', 'quoted desktop 日本語'], ['-desktop', 'quoted desktop 日本語']),
        ('shell-literal', ['-desktop', 'quote\' dollar$ semi; star*'], ['-desktop', 'quote\' dollar$ semi; star*']),
        ('interface', ['-interface', '127.0.0.1'], []),
        ('port', ['-websocketPort', '12345'], []),
        ('geometry', ['-geometry', '800x600'], []),
        ('debug', ['-debug'], []),
    ]:
        yield case('cli:' + name, defaults, args=args, prefix=prefix)
    yield case('layering', defaults, layers=[{'desktop': {'allow_resize': 'false'}}, {'desktop': {'pixel_depth': '16'}}])
    yield case('empty-section-layer', defaults, layers=[{'desktop': None}])
    for name, text in [
        ('invalid-indent', 'desktop:\n width: 1\n  height: 2\n'),
        ('duplicate', 'desktop:\n  allow_resize: true\n  allow_resize: false\n'),
        ('multiple-documents', 'desktop:\n  allow_resize: true\n---\ndesktop:\n  allow_resize: false\n'),
        ('flow', 'desktop: {allow_resize: true}\n'),
        ('alias', 'desktop: &d\n  allow_resize: true\ncopy: *d\n'),
        ('no-final-newline', 'desktop:\n  allow_resize: true'),
        ('null-word', 'pointer:\n  enabled: null\n'),
        ('null-tilde', 'pointer:\n  enabled: ~\n'),
        ('boolean-yes', 'pointer:\n  enabled: yes\n'),
    ]:
        yield case('yaml:' + name, raw=text)


def snapshot():
    result = {}
    for file in sorted(HOME.rglob('*')):
        if file.is_symlink():
            value = {'symlink': os.readlink(file)}
        elif file.is_file():
            # Xauthority contains random cookies; inspect record existence/mode,
            # not its random bytes. Its semantics require separate real-X smoke.
            value = {'size': file.stat().st_size}
            if file.name != '.Xauthority':
                value['sha256'] = hashlib.sha256(file.read_bytes()).hexdigest()
        else:
            value = {'directory': True}
        value['mode'] = oct(file.lstat().st_mode & 0o777)
        result[str(file.relative_to(HOME))] = value
    return result


def reset_fixture(item):
    shutil.rmtree(WORK, ignore_errors=True)
    HOME.mkdir(parents=True)
    config_files = []
    configs = [item.get('config') or {}] + item.get('layers', [])
    for i, config in enumerate(configs):
        file = WORK / f'config-{i}.yaml'
        file.write_text(item['raw'] if i == 0 and 'raw' in item else (yaml.dump(config, Dumper=FixtureDumper, allow_unicode=True, sort_keys=False) if config else '---\n'))
        config_files.append(str(file))
    return config_files


def capture(command, item, display, timeout):
    files = reset_fixture(item)
    env = {'HOME': str(HOME), 'USER': 'root', 'LOGNAME': 'root',
           'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
           'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'PERL5LIB': str(ROOT / 'unix'),
           'PERL_HASH_SEED': '0', 'PERL_PERTURB_KEYS': '0', **item.get('env', {})}
    args = [*command, f':{display}', '-dry-run', '-config', ','.join(files), *item.get('args', [])]
    proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        status = proc.returncode
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
        status = 'timeout'
    return {'status': status, 'stdout': stdout, 'stderr': stderr, 'files': snapshot()}


def normalize_pid(text, pid):
    # Replace only actual PID fields, not coincidentally equal digits inside a
    # hostname, path, port, display number or user-authored value.
    text = re.sub(r'(\bprocess ID )' + re.escape(str(pid)) + r'\b', r'\1<pid>', text)
    return re.sub(r'(?m)(^:\d+[ \t]+)' + re.escape(str(pid)) + r'(?=[ \t]*$)', r'\1<pid>', text)


def normalized_diagnostics(text):
    # Locations differ between languages; keep error text and every line.
    text = re.sub(r' at /[^\n]+ line \d+\.?', '', text)
    text = re.sub(r'HASH\(0x[0-9a-f]+\)', '<mapping>', text)
    text = re.sub(r'(?<![\w])0x[0-9a-f]+', '<address>', text)
    return sorted(line.strip() for line in text.splitlines() if line.strip())


def parse_command(stdout, prefix):
    lines = []
    for line in stdout.splitlines():
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        if len(tokens) > 1 and Path(tokens[0]).name in ('Xvnc', 'Xkasmvnc') and re.fullmatch(r':\d+', tokens[1]):
            lines.append(line)
    if len(lines) != 1:
        raise ValueError(f'expected one Xvnc command, found {len(lines)}')
    words = shlex.split(lines[0])
    executable, display, *words = words
    redirection = []
    if '>>' in words:
        index = words.index('>>')
        redirection, words = words[index:], words[:index]
    if len(words) < len(prefix):
        raise ValueError('CLI pass-through arguments missing')
    observed_prefix = words[:len(prefix)]
    words = words[len(prefix):]
    # Known generated flags only. Each option owns its next argument even when
    # negative. Preserve repeated same-option order (notably HTTP headers).
    flag_only = {'-hw3d'}
    known = {'-' + name for row in json.loads((HERE / 'inventory.json').read_text())['keys'] for name in row['options']} | {'-rfbauth', '-rfbport', '-rfbwait'}
    grouped = {}
    i = 0
    while i < len(words):
        option = words[i]
        if not option.startswith('-'):
            raise ValueError('unrecognized option boundary: ' + repr(words[i:]))
        if option not in known:
            raise ValueError('unknown generated option: ' + option)
        if option in flag_only:
            value = None
            i += 1
        else:
            if i + 1 >= len(words):
                raise ValueError('missing argument: ' + option)
            value = words[i + 1]
            i += 2
        grouped.setdefault(option, []).append(value)
    return {'executable': Path(executable).name, 'display': display, 'prefix': observed_prefix,
            'options': grouped, 'redirection': redirection}


def compare(left, right, item):
    problems = []
    if left['status'] != right['status']:
        problems.append('exit-status')
    if normalized_diagnostics(left['stderr']) != normalized_diagnostics(right['stderr']):
        problems.append('diagnostics')
    if left['files'] != right['files']:
        problems.append('filesystem')
    if left['status'] == 0 and right['status'] == 0:
        for label, result in [('perl', left), ('native', right)]:
            try:
                result['command'] = parse_command(result['stdout'], item.get('prefix', []))
            except ValueError as error:
                result['command_error'] = str(error)
                problems.append(label + '-command-parse')
        if left.get('command') != right.get('command'):
            problems.append('argv')
    return problems


def setup_helpers():
    # --network none does not always provision hostname resolution. Both the
    # legacy shell helper and native getaddrinfo must see the same local host.
    with Path('/etc/hosts').open('a') as hosts:
        hosts.write('\n127.0.0.1 ' + socket.gethostname() + '\n')
    # These helpers never start a server; production commands are not run by the
    # fast config differential layer. Native and Perl see the same helper paths.
    for name in ('Xvnc', 'vncpasswd', 'kasmvncpasswd'):
        path = Path('/usr/bin') / name
        path.write_text('#!/bin/sh\nexit 0\n')
        path.chmod(0o755)
    # Deterministic cookie permits stable Xauthority size; content isn't compared.
    path = Path('/usr/local/bin/mcookie')
    path.write_text('#!/bin/sh\nprintf "%s\\n" 0123456789abcdef0123456789abcdef\n')
    path.chmod(0o755)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', default='/src/unix/vncserver-native/vncserver-native')
    parser.add_argument('--oracle', default='/src/unix/vncserver')
    parser.add_argument('--report', default='/tmp/launcher-parity.json')
    parser.add_argument('--filter', default='')
    parser.add_argument('--limit', type=int)
    parser.add_argument('--timeout', type=float, default=8)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--oracle-only', action='store_true', help='characterize corpus without claiming parity')
    args = parser.parse_args()
    if not Path('/.dockerenv').exists():
        parser.error('run only in the dedicated disposable Docker container')
    inventory = json.loads((HERE / 'inventory.json').read_text())['keys']
    source_keys = set(re.findall(r'KasmVNC::ConfigKey->new\(\{\s*name\s*=>\s*[\'"]([^\'"]+)', Path(args.oracle).read_text()))
    frozen_keys = {row['key'] for row in inventory}
    if source_keys != frozen_keys:
        parser.error('Perl/inventory key drift: missing=' + repr(sorted(source_keys - frozen_keys)) +
                     ' stale=' + repr(sorted(frozen_keys - source_keys)))
    defaults = yaml.load((ROOT / 'unix/kasmvnc_defaults.yaml').read_text(), Loader=yaml.BaseLoader)
    def perl_scalars(value):
        if value == '':
            return None
        if isinstance(value, dict):
            return {k: perl_scalars(v) for k, v in value.items()}
        if isinstance(value, list):
            return [perl_scalars(v) for v in value]
        return value
    defaults = perl_scalars(defaults)
    selected = [item for item in cases(inventory, defaults) if args.filter in item['name']]
    if args.limit is not None:
        selected = selected[:args.limit]
    if args.list:
        print('\n'.join(item['name'] for item in selected))
        return 0
    if not args.oracle_only and not Path(args.candidate).is_file():
        parser.error('candidate does not exist: ' + args.candidate)
    setup_helpers()
    rows = []
    if not args.oracle_only:
        # Freeze one candidate for the run; rebuilding in another terminal must
        # not silently mix different native versions in one report.
        candidate_bytes = Path(args.candidate).read_bytes()
        candidate_hash = hashlib.sha256(candidate_bytes).hexdigest()
        frozen = Path('/tmp/kasm-parity-candidate/unix/vncserver-native/vncserver-native')
        frozen.parent.mkdir(parents=True, exist_ok=True)
        frozen.write_bytes(candidate_bytes)
        frozen.chmod(0o755)
        candidate_command = str(frozen)
    else:
        candidate_hash = None
    started = time.monotonic()
    for item in selected:
        left = capture(['/usr/bin/perl', args.oracle], item, 91, args.timeout)
        right = None if args.oracle_only else capture([candidate_command], item, 91, args.timeout)
        problems = [] if args.oracle_only else compare(left, right, item)
        if item['name'] == 'defaults' and left['status'] != 0:
            problems.append('oracle-baseline-must-succeed')
        rows.append({'name': item['name'], 'key': item.get('key'), 'input': item, 'problems': problems,
                     'oracle': left, 'candidate': right})
        if problems:
            print('FAIL', item['name'], ','.join(problems), flush=True)
        elif len(rows) % 50 == 0:
            print(f'completed {len(rows)}/{len(selected)}', flush=True)
    summary = {'cases': len(rows), 'failed': sum(bool(r['problems']) for r in rows),
               'semantic_failures': sum(any(p != 'diagnostics' for p in r['problems']) for r in rows),
               'diagnostic_only_failures': sum(r['problems'] == ['diagnostics'] for r in rows),
               'inventory_keys': len(inventory), 'keys_exercised': len({r['key'] for r in rows if r['key']}),
               'corpus_sha256': hashlib.sha256(json.dumps(selected, sort_keys=True).encode()).hexdigest(),
               'candidate_sha256': candidate_hash, 'oracle_only': args.oracle_only, 'seconds': round(time.monotonic() - started, 2),
               'oracle_timeouts': sum(r['oracle']['status'] == 'timeout' for r in rows),
               'oracle_accepted': sum(r['oracle']['status'] == 0 for r in rows)}
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    coverage = {row['key']: {'cases': sum(r['key'] == row['key'] for r in rows),
                  'accepted_value_probes': sum(r['key'] == row['key'] and ':value-' in r['name'] and r['oracle']['status'] == 0 for r in rows)} for row in inventory}
    Path(args.report).write_text(json.dumps({'summary': summary, 'coverage': coverage, 'results': rows}, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary), flush=True)
    return int(summary['failed'] > 0 or summary['oracle_timeouts'] > 0)


if __name__ == '__main__':
    sys.exit(main())
