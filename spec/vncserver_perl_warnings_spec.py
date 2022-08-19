from mamba import description, context, it, fit, before, after
from expects import expect, equal, contain, match
from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect, \
    write_config, config_filename

with description("Perl warnings"):
    with it("treats Perl warning as error"):
        completed_process = run_cmd("vncserver -dry-run")
        expect(completed_process.stderr).not_to(match(r'line \d+\.$'))
