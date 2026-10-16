#!/usr/bin/env python3
"""Run the same exported closure on the core-images distro matrix.

Requires Docker, an extracted runtime+test-tools closure, and a Playwright
container prepared with browser.cjs at /tmp/browser/browser.cjs and /results
mounted to --results. No image packages are installed during the tests.
"""
import argparse
import concurrent.futures
import json
import pathlib
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--rootfs', required=True)
parser.add_argument('--package', required=True)
parser.add_argument('--tools', required=True)
parser.add_argument('--results', required=True)
parser.add_argument('--browser-container', default='vnc680-browser')
parser.add_argument('--only', nargs='*')
parser.add_argument('--jobs', type=int, default=1)
parser.add_argument('--modes', nargs='+', default=['software', 'gpu'])
args = parser.parse_args()
repo = pathlib.Path(__file__).resolve().parents[2]
results = pathlib.Path(args.results).resolve()
results.mkdir(parents=True, exist_ok=True)
rows = json.loads((repo/'nix/test-matrix.json').read_text())
reports = []

def command(argv, **kw):
    return subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)

def run_case(case):
    row, mode = case
    name = 'vnc680-' + row['name'] + '-' + mode
    report = dict(row, mode=mode, name=name, package=args.package, test_tools=args.tools, pass_=False)
    gpu = mode == 'gpu'
    cmd = ['docker', 'run', '-d', '--name', name, '--shm-size=512m',
           '--runtime=nvidia' if gpu else '--runtime=runc',
           '-p', '127.0.0.1::6901', '-v', str(pathlib.Path(args.rootfs).resolve()/'nix/store')+':/nix/store:ro',
           '-v', str(repo/'builder/nix-tests/session.sh')+':/test.sh:ro',
           '-e', 'PACKAGE='+args.package, '-e', 'TOOLS='+args.tools,
           '-e', 'GPU='+str(int(gpu)), '-e', 'NVIDIA_VISIBLE_DEVICES=all' if gpu else 'NVIDIA_VISIBLE_DEVICES=void',
           '-e', 'NVIDIA_DRIVER_CAPABILITIES=all', '--entrypoint', args.tools+'/bin/bash']
    if gpu:
        cmd += ['--gpus', 'all']
    cmd += [row['image'], '/test.sh']
    try:
        launched = command(cmd, timeout=90)
        (results/(name+'-launch.log')).write_text(launched.stdout)
        if launched.returncode:
            raise RuntimeError('container launch failed: '+launched.stdout)
        ready = False
        for _ in range(90):
            probe = command(['docker','exec',name,args.tools+'/bin/test','-f','/tmp/session-ready'])
            if probe.returncode == 0:
                ready = True
                break
            state = command(['docker','inspect','--format','{{.State.Running}}',name]).stdout.strip()
            if state != 'true':
                break
            time.sleep(1)
        if not ready:
            raise RuntimeError('session did not pass X11/render/encode readiness checks')
        port = command(['docker','port',name,'6901/tcp']).stdout.strip().rsplit(':',1)[1]
        browser = command(['docker','exec','-e','TEST_PASSWORD=kasmRulez',args.browser_container,'node','/tmp/browser/browser.cjs',
                           'http://127.0.0.1:'+port, mode,name], timeout=75)
        (results/(name+'-browser-command.log')).write_text(browser.stdout)
        if browser.returncode:
            raise RuntimeError('browser/codec test failed')
        server_log = command(['docker', 'exec', name, args.tools+'/bin/cat', '/tmp/xvnc.log']).stdout
        marker = 'FFMPEGHWEncoder: Frame sent to codec successfully' if gpu else 'SoftwareEncoder: Screen id 0, codec 16, frame size:'
        if marker not in server_log:
            raise RuntimeError('Server did not confirm the requested encoder produced frames')
        report['kernel'] = command(['docker', 'exec', name, args.tools+'/bin/uname', '-r']).stdout.strip()
        report['pass_'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        for filename in ['xvnc.log','renderer.txt','ffmpeg.log','xdpyinfo.txt','openbox.log','xterm.log','gears.log']:
            command(['docker','cp',name+':/tmp/'+filename,str(results/(name+'-'+filename))])
        (results/(name+'-container.log')).write_text(command(['docker','logs',name]).stdout)
        report['image_id'] = command(['docker','image','inspect','--format','{{.Id}}',row['image']]).stdout.strip()
        command(['docker','rm','-f',name])
        return report

cases = [(row, mode) for row in rows if not args.only or row["name"] in args.only for mode in args.modes]
with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for report in pool.map(run_case, cases):
        reports.append(report)
        (results/"matrix-results.json").write_text(json.dumps(reports, indent=2)+"\n")
        print(json.dumps(report), flush=True)
raise SystemExit(0 if all(x['pass_'] for x in reports) else 1)
