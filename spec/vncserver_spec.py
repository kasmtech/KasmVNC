import os
import subprocess
from mamba import description, context, it
from expects import expect, equal

with description('vncserver') as self:
    with it('selects passed DE with -s'):
        subprocess.run('echo -e "password\\npassword\\n" | vncpasswd -u docker',
                       shell=True, text=True, executable='/bin/bash')

        cmd = 'vncserver :1 -select-de mate -cert /etc/ssl/certs/ssl-cert-snakeoil.pem -key /etc/ssl/private/ssl-cert-snakeoil.key -sslOnly -FrameRate=24 -interface 0.0.0.0 -httpd /usr/share/kasmvnc/www -depth 24 -geometry 1280x1050'
        try:
            completed_process = subprocess.run(cmd, shell=True, capture_output=True,
                                            text=True, timeout=3)
            if completed_process.returncode != 0:
                print(completed_process.stdout)
                print(completed_process.stderr)
            expect(completed_process.returncode).to(equal(0))
            expect(completed_process.returncode).to(equal(0))

            exitcode = os.system('grep -q mate ~/.vnc/xstartup')
            expect(exitcode).to(equal(0))
        finally:
            subprocess.run('vncserver -kill :1', capture_output=True, shell=True, timeout=3)
