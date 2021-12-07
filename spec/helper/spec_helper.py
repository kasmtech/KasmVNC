import os
import shutil
import subprocess
import pexpect
from path import Path
from expects import expect, equal

vncserver_cmd = 'vncserver :1 -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050 -log *:stderr:100'
running_xvnc = False


def clean_env():
    clean_kasm_users()

    home_dir = os.environ['HOME']
    vnc_dir = os.path.join(home_dir, ".vnc")
    Path(vnc_dir).rmtree(ignore_errors=True)


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
    running_xvnc = True

    return child


def start_xvnc(extra_args="", **kwargs):
    global running_xvnc
    completed_process = run_cmd(f'{vncserver_cmd} {extra_args}', **kwargs)
    if completed_process.returncode == 0:
        running_xvnc = True

    return completed_process


def run_cmd(cmd, **kwargs):
    completed_process = subprocess.run(cmd, shell=True, text=True,
                                       capture_output=True,
                                       executable='/bin/bash', **kwargs)
    if completed_process.returncode != 0:
        print(completed_process.stdout)
        print(completed_process.stderr)

    return completed_process


def add_kasmvnc_user_docker():
    completed_process = run_cmd('echo -e "password\\npassword\\n" | vncpasswd -u docker')
    expect(completed_process.returncode).to(equal(0))


def kill_xvnc():
    global running_xvnc
    if not running_xvnc:
        return

    run_cmd('vncserver -kill :1')
    running_xvnc = False
