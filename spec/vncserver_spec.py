import os
import stat
import tempfile
from mamba import description, context, fcontext, it, fit, before, after
from expects import expect, equal, contain, match

from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect, \
    write_config, config_filename


def run_vncserver():
    return start_xvnc(f'-config {config_filename}')


def temp_file_name():
    return f'/tmp/vncserver.{next(tempfile._get_candidate_names())}'


with description('vncserver') as self:
    with before.each:
        clean_env()
    with after.each:
        kill_xvnc()

    with context("SSL certs"):
        with before.each:
            add_kasmvnc_user_docker()

        with it("complains if SSL certs don't exist"):
            non_existent_file_name = temp_file_name()

            write_config(f'''
            network:
                ssl:
                    pem_certificate: {non_existent_file_name}
            ''')
            completed_process = run_vncserver()
            expect(completed_process.returncode).to(equal(1))
            expect(completed_process.stderr).to(
                match(r'certificate file doesn\'t exist'))

        with it("complains if SSL cert not available"):
            cert_file_name = temp_file_name()
            with open(cert_file_name, 'w') as f:
                f.write('test')
            os.chmod(cert_file_name, stat.S_IXUSR)

            write_config(f'''
            network:
                ssl:
                    pem_certificate: {cert_file_name}
            ''')
            completed_process = run_vncserver()
            expect(completed_process.returncode).to(equal(1))
            expect(completed_process.stderr).to(
                match(r'certificate isn\'t readable'))
            expect(completed_process.stderr).to(
                match(r'addgroup \$USER'))
