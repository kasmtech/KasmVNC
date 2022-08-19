from mamba import description, context, fit, it, before, after
from expects import expect, equal, contain

from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect, \
    config_filename, write_config

with description('YAML validation') as self:
    with it("produces error message for an incomplete data clump"):
        write_config('''
        desktop:
            resolution:
                width: 1024
        ''')
        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain('desktop.resolution.width, desktop.resolution.height: either all keys or none must be present'))

    with it("produces error message if int key was set to a string"):
        write_config('''
        desktop:
            resolution:
                width: 1024
                height: none
        ''')
        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("desktop.resolution.height 'none': must be an integer"))

    with it("produces no error for valid boolean values"):
        write_config('''
        network:
            use_ipv4: true
            use_ipv6: false
        ''')
        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}')
        expect(completed_process.stderr).to(equal(''))

    with it("produces an error for invalid boolean values"):
        write_config('''
        desktop:
            allow_resize: 10
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("desktop.allow_resize '10': must be true or false"))

    with it("produces an error for invalid enum value"):
        write_config('''
        desktop:
            pixel_depth: none
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("desktop.pixel_depth 'none': must be one of [16, 24, 32]"))

    with it("produces an error for invalid pattern enum value"):
        write_config('''
        desktop:
            pixel_depth: 16|24|32
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("desktop.pixel_depth '16|24|32': must be one of [16, 24, 32]"))

    with it("produces an error fo partially present enum value"):
        write_config('''
        network:
            protocol: vnc2
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("network.protocol 'vnc2': must be one of [http, vnc]"))

    with it("is silent for a valid enum value"):
        write_config('''
        desktop:
            pixel_depth: 16
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}')
        expect(completed_process.stderr).to(equal(""))

    with it("produces an error for an array value"):
        write_config('''
        keyboard:
            remap_keys:
                - 0xzz->0x40
                - 0x24->0x40
        ''')

        completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
        expect(completed_process.stderr).to(contain("keyboard.remap_keys '0xzz->0x40, 0x24->0x40': must be in the format 0x<hex_number>->0x<hex_number>"))

    with context("unsupported keys"):
        with it("produces an error for an unsupported top-level key"):
            write_config('''
            foo: 1
            ''')

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
            expect(completed_process.stderr).to(
                contain("Unsupported config keys found:\nfoo"))

        with it("produces an error for an unsupported 2nd-level key"):
            write_config('''
            bar:
                baz: 1
            ''')

            completed_process = run_cmd(f'vncserver -dry-run -test-output-topic validation -config {config_filename}', print_stderr=False)
            expect(completed_process.stderr).to(
                contain("Unsupported config keys found:\nbar.baz"))
