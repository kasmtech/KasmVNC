import os
from mamba import description, context, fcontext, it, fit, _it, before, after
from expects import expect, equal, contain, match

from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect, \
    write_config, config_filename, pick_cli_option, \
    run_vncserver_to_print_xvnc_cli_options

with description('Env var config override') as self:
    with context("env var override is turned off"):
        with it("doesn't override, when setting is defined in config"):
            write_config('''
            desktop:
                allow_resize: true
            server:
                allow_environment_variables_to_override_config_settings: false
            ''')
            env = os.environ.copy()
            env["KVNC_DESKTOP_ALLOW_RESIZE"] = "false"

            completed_process = run_vncserver_to_print_xvnc_cli_options(env=env)
            cli_option = pick_cli_option('AcceptSetDesktopSize',
                                        completed_process.stdout)
            expect(cli_option).to(equal("-AcceptSetDesktopSize '1'"))

        with it("doesn't override, when setting is not defined in config"):
            write_config('''
            desktop:
                allow_resize: true
            ''')
            env = os.environ.copy()
            env["KVNC_DESKTOP_ALLOW_RESIZE"] = "false"

            completed_process = run_vncserver_to_print_xvnc_cli_options(env=env)
            cli_option = pick_cli_option('AcceptSetDesktopSize',
                                        completed_process.stdout)
            expect(cli_option).to(equal("-AcceptSetDesktopSize '1'"))

    with context("env var override is turned on"):
        with it("converts env var to CLI option"):
            write_config('''
            desktop:
                allow_resize: true
            server:
                allow_environment_variables_to_override_config_settings: true
            ''')
            env = os.environ.copy()
            env["KVNC_DESKTOP_ALLOW_RESIZE"] = "false"

            completed_process = run_vncserver_to_print_xvnc_cli_options(env=env)
            cli_option = pick_cli_option('AcceptSetDesktopSize',
                                        completed_process.stdout)
            expect(cli_option).to(equal("-AcceptSetDesktopSize '0'"))

        with it("produces error message if env var has invalid value"):
            write_config('''
            desktop:
                allow_resize: true
            server:
                allow_environment_variables_to_override_config_settings: true
            ''')
            env = os.environ.copy()
            env["KVNC_DESKTOP_ALLOW_RESIZE"] = "none"

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False, env=env)
            expect(completed_process.stderr).to(contain("desktop.allow_resize 'none': must be true or false"))

        with it("produces error message and exits if env var name is unsupported"):
            write_config('''
            foo: true
            desktop:
                allow_resize: true
            server:
                allow_environment_variables_to_override_config_settings: true
            ''')
            env = os.environ.copy()
            env["KVNC_FOO"] = "none"

            completed_process = run_cmd(f'vncserver -test-output-topic validation -config {config_filename}', print_stderr=False, env=env)
            expect(completed_process.stderr).to(contain("Unsupported config env vars found:\nKVNC_FOO"))

    with context("config setting server.allow_environment_variables_to_override_config_settings"):
        with it("produces error message if config has invalid value"):
            write_config('''
            server:
                allow_environment_variables_to_override_config_settings: none
            ''')

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
            expect(completed_process.stderr).to(contain("server.allow_environment_variables_to_override_config_settings 'none': must be true or false"))

        with it("doesn't interpolate env vars into value"):
            write_config('''
            server:
                allow_environment_variables_to_override_config_settings: ${ALLOW_OVERRIDE}
            ''')

            env = os.environ.copy()
            env["ALLOW_OVERRIDE"] = "true"

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}',\
                                        print_stderr=False, env=env)
            expect(completed_process.stderr).\
                to(contain("server.allow_environment_variables_to_override_config_settings '${ALLOW_OVERRIDE}': must be true or false"))

        with it("doesn't allow to override it with the corresponding env var if set to false"):
            write_config('''
            server:
                allow_environment_variables_to_override_config_settings: false
            ''')

            env = os.environ.copy()
            env["KVNC_SERVER_ALLOW_ENVIRONMENT_VARIABLES_TO_OVERRIDE_CONFIG_SETTINGS"] = "${ALLOW_OVERRIDE}"

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}',\
                                        print_stderr=False, env=env)
            expect(completed_process.stderr).\
                not_to(contain("server.allow_environment_variables_to_override_config_settings '${ALLOW_OVERRIDE}': must be true or false"))

        with it("doesn't allow to override it with the env var if set to true"):
            write_config('''
            server:
                allow_environment_variables_to_override_config_settings: true
            ''')

            env = os.environ.copy()
            env["KVNC_SERVER_ALLOW_ENVIRONMENT_VARIABLES_TO_OVERRIDE_CONFIG_SETTINGS"] = "${ALLOW_OVERRIDE}"

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}',\
                                        print_stderr=False, env=env)
            expect(completed_process.stderr).\
                not_to(contain("server.allow_environment_variables_to_override_config_settings '${ALLOW_OVERRIDE}': must be true or false"))
