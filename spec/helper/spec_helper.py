import os
import re
import sys
import shutil
import subprocess
import pexpect
from path import Path
from expects import expect, equal

vncserver_cmd = 'vncserver :1'
running_xvnc = False
debug_output = False
config_dir = "spec/tmp"
config_filename = os.path.join(config_dir, "config.yaml")

if os.getenv('KASMVNC_SPEC_DEBUG_OUTPUT'):
    debug_output = True

def run_vncserver_to_print_xvnc_cli_options(env=os.environ):
    return run_cmd(f'vncserver -dry-run -config {config_filename}', env=env)

def pick_cli_option(cli_option, xvnc_cmd):
    cli_option_regex = re.compile(f'\'?-{cli_option}\'?(?:\s+[^-][^\s]*|$)')
    results = cli_option_regex.findall(xvnc_cmd)
    if len(results) == 0:
        return None

    return ' '.join(results)

def write_config(config_text):
    os.makedirs(config_dir, exist_ok=True)

    with open(config_filename, "w") as f:
        f.write(config_text)


def clean_locks():
    tmp = '/tmp'
    temporary_lock_file = os.path.join(tmp, '.X1-lock')
    if (os.path.exists(temporary_lock_file)):
        os.remove(temporary_lock_file)

    temporary_lock_file = os.path.join(tmp, '.X11-unix')
    temporary_lock_file = os.path.join(temporary_lock_file, 'X1')
    if (os.path.exists(temporary_lock_file)):
        os.remove(temporary_lock_file)


def clean_env():
    clean_kasm_users()

    home_dir = os.environ['HOME']
    vnc_dir = os.path.join(home_dir, ".vnc")
    Path(vnc_dir).rmtree(ignore_errors=True)

    clean_locks()

def clean_kasm_users():
    home_dir = os.environ['HOME']
    password_file = os.path.join(home_dir, ".kasmpasswd")
    Path(password_file).remove_p()


def start_xvnc_pexpect(extra_args="", **kwargs):
    global running_xvnc

    # ":;" is a hack. Without it, Xvnc doesn't run. No idea what happens, but
    # when I run top, Xvnc just isn't there. I suspect a race.
    child = pexpect.spawn('/bin/bash',
                          ['-ic', f':;{vncserver_cmd} {extra_args}'],
                          timeout=5, encoding='utf-8', **kwargs)
    if debug_output:
        child.logfile_read = sys.stderr

    running_xvnc = True

    return child


def start_xvnc(extra_args="", **kwargs):
    global running_xvnc
    completed_process = run_cmd(f'{vncserver_cmd} {extra_args}',
                                print_stderr=False, **kwargs)
    if completed_process.returncode == 0:
        running_xvnc = True

    return completed_process


def run_cmd(cmd, print_stderr=True, **kwargs):
    completed_process = subprocess.run(cmd, shell=True, text=True,
                                       capture_output=True,
                                       executable='/bin/bash', **kwargs)
    if debug_output:
        if len(completed_process.stderr) > 0:
            print(completed_process.stderr)
        if len(completed_process.stdout) > 0:
            print(completed_process.stdout)
    elif print_stderr:
        if len(completed_process.stderr) > 0:
            print(completed_process.stderr)

    return completed_process


def add_kasmvnc_user_docker():
    completed_process = run_cmd('echo -e "password\\npassword\\n" | vncpasswd -u docker -w')
    expect(completed_process.returncode).to(equal(0))


def kill_xvnc():
    global running_xvnc
    if not running_xvnc:
        return

    run_cmd('vncserver -kill :1', print_stderr=False)
    running_xvnc = False
    clean_locks()
