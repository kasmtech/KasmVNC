import os
import shutil
import subprocess
from path import Path
from mamba import description, context, it, before, after
from expects import expect, equal


def clean_env():
    home_dir = os.environ['HOME']
    password_file = os.path.join(home_dir, ".kasmpasswd")
    Path(password_file).remove_p()

    vnc_dir = os.path.join(home_dir, ".vnc")
    Path(vnc_dir).rmtree(ignore_errors=True)


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
    run_cmd('vncserver -kill :1')


def check_de_was_setup_to_run(de_name):
    completed_process = run_cmd(f'grep -q {de_name} ~/.vnc/xstartup')
    expect(completed_process.returncode).to(equal(0))


with description('vncserver') as self:
    with before.each:
        clean_env()

    with it('selects passed DE with -s'):
        add_kasmvnc_user_docker()

        cmd = 'vncserver :1 -select-de mate -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
        try:
            completed_process = run_cmd(cmd)
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('mate')
        finally:
            kill_xvnc()

    with it('asks to select a DE, when ran with -select-de'):
        add_kasmvnc_user_docker()

        cmd = 'vncserver :1 -select-de -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
        try:
            completed_process = run_cmd(cmd, input="1\ny\n")
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('cinnamon')
        finally:
            kill_xvnc()
