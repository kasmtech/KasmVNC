from mamba import description, context, fcontext, it, fit, _it, before, after
from expects import expect, equal, contain, match

from helper.spec_helper import start_xvnc, kill_xvnc, run_cmd, clean_env, \
    add_kasmvnc_user_docker, clean_kasm_users, start_xvnc_pexpect, \
    write_config, config_filename, pick_cli_option, \
    run_vncserver_to_print_xvnc_cli_options

with description('YAML to CLI') as self:
    with context("convert a boolean key"):
        with it("convert true to 1"):
            write_config('''
            desktop:
                allow_resize: true
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('AcceptSetDesktopSize',
                                         completed_process.stdout)
            expect(cli_option).to(equal("-AcceptSetDesktopSize '1'"))

        with it("convert false to 0"):
            write_config('''
            desktop:
                allow_resize: false
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('AcceptSetDesktopSize',
                                         completed_process.stdout)
            expect(cli_option).to(equal("-AcceptSetDesktopSize '0'"))

    with it("converts a numeric key to a CLI arg"):
        write_config('''
        security:
            brute_force_protection:
                blacklist_threshold: 2
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('BlacklistThreshold',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-BlacklistThreshold '2'"))

    with it("converts an ANY key to a CLI arg"):
        write_config('''
        network:
            ssl:
               pem_certificate: /etc/ssl/certs/ssl-cert-snakeoil.pem
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('cert',
                                     completed_process.stdout)
        expect(cli_option).to(
            equal("-cert '/etc/ssl/certs/ssl-cert-snakeoil.pem'"))

    with it("converts an array key to a CLI arg"):
        write_config('''
        keyboard:
            remap_keys:
                - 0x22->0x40
                - 0x24->0x40
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('RemapKeys',
                                     completed_process.stdout)
        expect(cli_option).to(
            equal("-RemapKeys '0x22->0x40,0x24->0x40'"))

    with it("converts a constant value to the corresponding numeric value"):
        write_config('''
        data_loss_prevention:
            clipboard:
                server_to_client:
                    size: 20
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('DLP_ClipSendMax',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-DLP_ClipSendMax '20'"))

    with context("websocketPort"):
        with it("converts 'auto' value to calculated value"):
            write_config('''
            network:
                websocket_port: auto
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('websocketPort',
                                         completed_process.stdout)
            expect(["-websocketPort '8444'", "-websocketPort '8445'"]). \
                to(contain(cli_option))

        with it("passes numeric value to CLI option"):
            write_config('''
            network:
                websocket_port: 8555
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('websocketPort',
                                         completed_process.stdout)
            expect(cli_option).to(equal("-websocketPort '8555'"))

        with it("no key - no CLI option"):
            write_config('''
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('websocketPort',
                                         completed_process.stdout)
            expect(cli_option).to(equal(None))

    with context("option that can yield nothing"):
        with it("converts a config value that yields nothing"):
            write_config('''
            network:
                protocol: http
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('noWebsocket',
                                         completed_process.stdout)
            expect(cli_option).to(equal(None))

        with it("converts a config value that yields CLI option"):
            write_config('''
            network:
                protocol: vnc
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('noWebsocket',
                                         completed_process.stdout)
            expect(cli_option).to(equal("-noWebsocket '1'"))

    with it("interpolates env variable"):
        write_config('''
        server:
            advanced:
                kasm_password_file: ${HOME}/.kasmpasswd
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('KasmPasswordFile',
                                        completed_process.stdout)
        expect(cli_option).to(equal("-KasmPasswordFile '/home/docker/.kasmpasswd'"))

    with it("converts logging options into one -Log"):
        write_config('''
        logging:
            log_writer_name: all
            log_dest: logfile
            level: 40
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('Log',
                                        completed_process.stdout)
        expect(cli_option).to(equal("-Log '*:stdout:40'"))

    with it("converts DLP region options into one -DLP_Region"):
        write_config('''
        data_loss_prevention:
            visible_region:
                top: -10
                left: 10
                right: 40%
                bottom: 40
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('DLP_Region',
                                        completed_process.stdout)
        expect(cli_option).to(equal("-DLP_Region '10,-10,40%,40'"))

    with context("converts x_font_path"):
        with it("auto"):
            write_config('''
            server:
                advanced:
                    x_font_path: auto
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('fp',
                                        completed_process.stdout)
            expect(cli_option).to(match(r'/usr/share/fonts'))

        with it("none specified"):
            write_config('''
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('fp',
                                        completed_process.stdout)
            expect(cli_option).to(match(r'/usr/share/fonts'))

        with it("path specified"):
            write_config('''
            server:
                advanced:
                    x_font_path: /src
            ''')
            completed_process = run_vncserver_to_print_xvnc_cli_options()
            cli_option = pick_cli_option('fp', completed_process.stdout)
            expect(cli_option).to(equal("-fp '/src'"))

        with it("CLI override"):
            write_config('''
            server:
                advanced:
                    x_font_path: /src
            ''')
            completed_process = \
                run_cmd(f'vncserver -dry-run -fp /override -config {config_filename}')
            cli_option = pick_cli_option('fp', completed_process.stdout)
            expect(cli_option).to(equal("-fp '/override'"))

    with it("converts network.interface to -interface"):
        write_config('''
        network:
            interface: 0.0.0.0
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('interface',
                                    completed_process.stdout)
        expect(cli_option).to(equal("-interface '0.0.0.0'"))

    with it("CLI option directly passed, overrides config"):
        write_config('''
        encoding:
            video_encoding_mode:
                jpeg_quality: -1
        ''')
        completed_process = \
            run_cmd(f'vncserver -dry-run -JpegVideoQuality 8 -config {config_filename}')
        cli_option = pick_cli_option("JpegVideoQuality",
                                    completed_process.stdout)
        expect(cli_option).to(equal("'-JpegVideoQuality' '8'"))

    with it("converts 2 keys into a single CLI option"):
        write_config('''
        desktop:
            resolution:
                width: 1024
                height: 768
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('geometry',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-geometry '1024x768'"))

    with it("allows wide utf characters"):
        write_config('''
        data_loss_prevention:
            watermark:
                text:
                    template: "星街すいせい"
        ''')
        completed_process = run_vncserver_to_print_xvnc_cli_options()
        cli_option = pick_cli_option('DLP_WatermarkText',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-DLP_WatermarkText '星街すいせい'"))

    with it("ignores empty section override"):
        write_config('''
        security:
        ''')
        completed_process = \
            run_cmd(f'vncserver -dry-run -config spec/fixtures/global_config.yaml,{config_filename}')
        cli_option = pick_cli_option('BlacklistThreshold',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-BlacklistThreshold '6'"))

    with it("overrides default config value with global config value"):
        completed_process = run_cmd("vncserver -dry-run -config spec/fixtures/defaults_config.yaml,spec/fixtures/global_config.yaml")
        cli_option = pick_cli_option('BlacklistThreshold',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-BlacklistThreshold '6'"))

    with it("uses default config value even if section was overriden"):
        completed_process = run_cmd("vncserver -dry-run -config spec/fixtures/defaults_config.yaml,spec/fixtures/global_config.yaml")
        cli_option = pick_cli_option('BlacklistTimeout',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-BlacklistTimeout '10'"))

    with it("overrides global config with user config value"):
        completed_process = run_cmd("vncserver -dry-run -config spec/fixtures/defaults_config.yaml,spec/fixtures/global_config.yaml,spec/fixtures/user_config.yaml")
        cli_option = pick_cli_option('BlacklistThreshold',
                                     completed_process.stdout)
        expect(cli_option).to(equal("-BlacklistThreshold '7'"))
