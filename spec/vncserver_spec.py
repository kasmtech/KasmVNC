import os
import subprocess
from mamba import description, context, it
from expects import expect, equal


def run_cmd(cmd, **kwargs):
    completed_process = subprocess.run(cmd, shell=True, text=True,
                                       capture_output=True,
                                       executable='/bin/bash', **kwargs)
    if completed_process.returncode != 0:
        print(completed_process.stdout)
        print(completed_process.stderr)

    return completed_process


with description('vncserver') as self:
    with it('selects passed DE with -s'):
        completed_process = run_cmd('echo -e "password\\npassword\\n" | vncpasswd -u docker')
        expect(completed_process.returncode).to(equal(0))

        cmd = 'vncserver :1 -select-de mate -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
        try:
            completed_process = run_cmd(cmd)
            expect(completed_process.returncode).to(equal(0))

            completed_process = run_cmd('grep -q mate ~/.vnc/xstartup')
            expect(completed_process.returncode).to(equal(0))
        finally:
            run_cmd('vncserver -kill :1')

    with it('asks to select a DE, when ran with -select-de'):
        completed_process = run_cmd('echo -e "password\\npassword\\n" | vncpasswd -u docker')
        expect(completed_process.returncode).to(equal(0))

        cmd = 'vncserver :1 -select-de -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
        try:
            completed_process = run_cmd(cmd, input="1\ny\n")
            expect(completed_process.returncode).to(equal(0))

            completed_process = run_cmd('grep -q cinnamon ~/.vnc/xstartup')
            expect(completed_process.returncode).to(equal(0))
        finally:
            run_cmd('vncserver -kill :1')
