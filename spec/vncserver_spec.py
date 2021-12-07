import os
import sys
import pexpect
from mamba import description, context, it, fit, before, after
from expects import expect, equal

from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect

# WIP. Plan to move to start_xvnc_pexpect(), because pexpect provides a way to
# wait for vncserver output. start_xvnc() just blindly prints input to vncserver
# without knowing what it prints back.


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

    with context('guided user creation'):
        with fit('asks to create a user if none exist'):
            clean_kasm_users()

            child = start_xvnc_pexpect('-select-de cinnamon')
            child.expect('Enter username')
            child.sendline()
            child.expect('Password:')
            child.sendline('password')
            child.expect('Verify:')
            child.sendline('password')
            child.expect(pexpect.EOF)
            child.close()
            expect(child.exitstatus).to(equal(0))

            home_dir = os.environ['HOME']
            user = os.environ['USER']
            completed_process = run_cmd(f'grep -qw {user} {home_dir}/.kasmpasswd')
            expect(completed_process.returncode).to(equal(0))

        with fit('specify custom username'):
            custom_username = 'custom_username'
            child = start_xvnc_pexpect('-select-de cinnamon')
            child.logfile_read = sys.stderr
            child.expect('Enter username')
            child.sendline(custom_username)
            child.expect('Password:')
            child.sendline('password')
            child.expect('Verify:')
            child.sendline('password')
            child.expect(pexpect.EOF)
            child.wait()
            # expect(child.exitstatus).to(equal(0))

            home_dir = os.environ['HOME']
            completed_process = run_cmd(f'grep -qw {custom_username} {home_dir}/.kasmpasswd')
            expect(completed_process.returncode).to(equal(0))

            # os.system(f'pgrep Xvnc >/dev/null || cat {home_dir}/.vnc/*.log')
