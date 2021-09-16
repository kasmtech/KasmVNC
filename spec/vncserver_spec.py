import os
import shutil
import subprocess
from path import Path
from mamba import description, context, it, fit, before, after
from expects import expect, equal

vncserver_cmd = 'vncserver :1 -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
running_xvnc = False


def clean_env():
    home_dir = os.environ['HOME']
    password_file = os.path.join(home_dir, ".kasmpasswd")
    Path(password_file).remove_p()

    vnc_dir = os.path.join(home_dir, ".vnc")
    Path(vnc_dir).rmtree(ignore_errors=True)


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


def select_de(de_name):
    try:
        extra_args = f'-select-de {de_name}'
        completed_process = start_xvnc(extra_args)
        expect(completed_process.returncode).to(equal(0))
    finally:
        kill_xvnc()


def check_de_was_setup_to_run(de_name):
    completed_process = run_cmd(f'grep -q {de_name} ~/.vnc/xstartup')
    expect(completed_process.returncode).to(equal(0))


with description('vncserver') as self:
    with before.each:
        clean_env()
    with after.each:
        kill_xvnc()

    with context('on the first run'):
        with before.each:
            add_kasmvnc_user_docker()

        with it('asks user to select a DE'):
            choose_cinnamon = "1\n"
            completed_process = start_xvnc(input=choose_cinnamon)
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('cinnamon')

        with it('asks to select a DE, when ran with -select-de'):
            choose_cinnamon = "1\n"
            completed_process = start_xvnc('-select-de', input=choose_cinnamon)
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('cinnamon')

        with it('selects passed DE with -s'):
            select_de('mate')
            check_de_was_setup_to_run('mate')

    with context('after DE was selected'):
        with before.each:
            add_kasmvnc_user_docker()

        with it("don't ask to choose DE by default"):
            select_de('mate')

            completed_process = start_xvnc()
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('mate')

        with it('asks to select a DE, when ran with -select-de'):
            select_de('mate')

            choose_cinnamon_and_answer_yes = "1\ny\n"
            completed_process = start_xvnc('-select-de',
                                           input=choose_cinnamon_and_answer_yes)
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('cinnamon')

        with it('selects passed DE with -s'):
            select_de('mate')

            completed_process = start_xvnc('-select-de cinnamon')
            expect(completed_process.returncode).to(equal(0))

            check_de_was_setup_to_run('cinnamon')
